#include "remote_server.hpp"

#include <coffeecnc/app/panel_assets.hpp>
#include <coffeecnc/core/remote_hosts.hpp>

#include "viewer_boost.hpp"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <resolv.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace coffeecnc {
namespace {

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using net::ip::address;
using net::ip::address_v4;
using net::ip::address_v6;
using net::ip::tcp;
using Error = boost::system::error_code;
using namespace std::chrono_literals;

constexpr std::size_t record_count = static_cast<std::size_t>(PanelRecord::message);
constexpr std::size_t max_connections = 32;
constexpr std::size_t max_viewers = 8;
constexpr std::size_t max_notifications = 64;
constexpr std::size_t max_notification_bytes = 8192;
constexpr std::size_t max_record_bytes = 48uz * 1024 * 1024;
constexpr std::uint64_t max_step_bytes = 25uz * 1024 * 1024; // UI importer limit
constexpr auto io_timeout = 5s;
constexpr auto interval = 75ms;
constexpr auto host_refresh = 5s;

using Record = std::shared_ptr<const std::string>;

struct ProgramIdentity {
  std::string file;
  std::uint64_t revision;
  friend auto operator==(const ProgramIdentity&, const ProgramIdentity&) -> bool = default;
};

struct Snapshot {
  std::array<Record, record_count> records{};
  std::optional<ProgramIdentity> program;
};

struct Notification {
  std::uint64_t sequence;
  Record json;
};

auto index(PanelRecord record) -> std::size_t { return static_cast<std::size_t>(record); }

// Counts the bytes a client sends, without a rate limit. A viewer only answers keep-alive
// pings, so the server can tell when a client sends frames that Beast does not report, such
// as empty non-final fragments.
class InboundBytes {
public:
  [[nodiscard]] auto received() const noexcept -> std::size_t { return received_; }

private:
  friend class beast::rate_policy_access;
  static constexpr auto all = std::numeric_limits<std::size_t>::max();
  [[nodiscard]] static auto available_read_bytes() noexcept -> std::size_t { return all; }
  [[nodiscard]] static auto available_write_bytes() noexcept -> std::size_t { return all; }
  void transfer_read_bytes(std::size_t bytes) noexcept { received_ += bytes; }
  static void transfer_write_bytes(std::size_t) noexcept {}
  static void on_timer() noexcept {}

  std::size_t received_{};
};

using Stream = beast::basic_stream<tcp, net::any_io_executor, InboundBytes>;

// A client frame of a pong: two header bytes, the four-byte mask and the payload.
constexpr std::size_t client_control_header = 6;
// A control frame that is still arriving, at most 125 payload bytes.
constexpr std::size_t max_partial_control = client_control_header + 125;
// How long part of a frame may wait for the rest. TCP can split a pong or a close.
constexpr auto partial_input_timeout = 2s;

auto comparable(address value) -> address {
  if (value.is_v4())
    return value;
  const auto ipv6 = value.to_v6();
  if (ipv6.is_v4_mapped())
    return net::ip::make_address_v4(net::ip::v4_mapped, ipv6);
  // DNS and HTTP address literals do not carry a local interface index.
  return address_v6{ipv6.to_bytes()};
}

auto local_ip_addresses() -> std::vector<address> {
  ifaddrs* interfaces = nullptr;
  if (getifaddrs(&interfaces) != 0)
    return {};
  const std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> guard{interfaces, freeifaddrs};
  std::vector<address> addresses;
  for (auto* iface = interfaces; iface != nullptr; iface = iface->ifa_next) {
    if (iface->ifa_addr == nullptr)
      continue;
    if (iface->ifa_addr->sa_family == AF_INET) {
      const auto* socket_address = reinterpret_cast<const sockaddr_in*>(iface->ifa_addr);
      addresses.emplace_back(address_v4{ntohl(socket_address->sin_addr.s_addr)});
    } else if (iface->ifa_addr->sa_family == AF_INET6) {
      const auto* socket_address = reinterpret_cast<const sockaddr_in6*>(iface->ifa_addr);
      address_v6::bytes_type bytes{};
      std::ranges::copy(socket_address->sin6_addr.s6_addr, bytes.begin());
      addresses.emplace_back(comparable(address_v6{bytes}));
    }
  }
  return addresses;
}

auto dns_search_domains() -> std::vector<std::string> {
  struct ResolverState {
    struct __res_state value {};
    ~ResolverState() { res_nclose(&value); }
  } configuration;
  std::vector<std::string> domains;
  if (res_ninit(&configuration.value) == 0)
    for (const auto* domain : configuration.value.dnsrch) {
      if (domain == nullptr)
        break;
      if (auto normalized = normalize_remote_host(domain))
        domains.push_back(std::move(*normalized));
    }
  return domains;
}

auto viewer_hello(const std::string& message) -> std::string {
  auto hello = nlohmann::json::parse(message);
  hello.erase("program_places");
  hello.erase("macros");
  hello.erase("operator_settings_storage");
  hello.erase("tool_model_storage");
  hello.erase("quit_supported");
  return hello.dump();
}

} // namespace

struct RemoteServer::Impl {
  class Client;

  Impl(const std::string& directory, std::uint16_t port, std::span<const std::string> aliases);
  ~Impl();
  void accept();
  void discover_hosts(const std::string& name);
  void refresh_hosts(const std::string& name);
  void resolve_host(const std::string& query, bool search_candidate);

  void notify(const std::string& message) {
    if (message.size() > max_notification_bytes)
      return;
    const std::unique_lock lock{events_mutex, std::try_to_lock};
    if (!lock)
      return;
    events.push_back({++event_sequence, std::make_shared<const std::string>(message)});
    if (events.size() > max_notifications)
      events.pop_front();
  }

  auto notifications_after(std::uint64_t after) -> std::vector<Notification> {
    const std::scoped_lock lock{events_mutex};
    std::vector<Notification> result;
    std::ranges::copy_if(events, std::back_inserter(result),
                         [after](const Notification& event) { return event.sequence > after; });
    return result;
  }

  const std::string ui_directory;
  Snapshot staged{};
  std::atomic<std::shared_ptr<const Snapshot>> latest{};
  std::mutex events_mutex;
  std::deque<Notification> events;
  std::uint64_t event_sequence{};
  std::size_t clients{};
  std::size_t viewers{};
  std::vector<std::string> allowed_hosts;
  std::vector<std::string> discovered_hosts;
  std::vector<std::string> resolving_hosts;
  std::size_t pending_resolutions{};
  net::io_context io{1};
  tcp::resolver resolver{io};
  tcp::acceptor listener{io};
  net::steady_timer refresh{io};
  net::steady_timer accept_retry{io};
  std::uint16_t listening_port{};
  std::thread thread;
};

class RemoteServer::Impl::Client : public std::enable_shared_from_this<Client> {
public:
  Client(Impl& owner, tcp::socket stream)
      : owner_(owner), socket_(std::move(stream)), tick_(socket_.get_executor()),
        deadline_(socket_.get_executor()) {
    ++owner_.clients;
    request_.header_limit(8192);
    request_.body_limit(0);
  }

  ~Client() {
    if (ws_)
      --owner_.viewers;
    --owner_.clients;
  }

  void start() {
    socket_.expires_after(io_timeout);
    http::async_read(socket_, request_buffer_, request_,
      [self = shared_from_this()](Error error, std::size_t) -> void {
        if (!error)
          self->serve();
      });
  }

private:
  auto host_allowed(std::string_view host) -> bool {
    Error error;
    const auto local = socket_.socket().local_endpoint(error);
    if (error)
      return false;
    const auto port = ":" + std::to_string(local.port());
    if (host.ends_with(port))
      host.remove_suffix(port.size());
    else if (local.port() != 80)
      return false;
    const auto local_ip = comparable(local.address());
    if (host.starts_with('[') && host.ends_with(']')) {
      const auto literal = host.substr(1, host.size() - 2);
      if (!local_ip.is_v6() || literal.find('%') != std::string_view::npos)
        return false;
      const auto ip = net::ip::make_address_v6(literal, error);
      return !error && ip == local_ip.to_v6();
    }
    const auto name = normalize_remote_host(host);
    if (!name)
      return false;
    if (*name == "localhost")
      return local_ip.is_loopback();
    // Never resolve the request's Host. Foreign DNS names must not be able to rebind to this origin.
    return (local_ip.is_v4() && *name == local_ip.to_string()) ||
           std::ranges::find(owner_.allowed_hosts, *name) != owner_.allowed_hosts.end() ||
           std::ranges::find(owner_.discovered_hosts, *name) != owner_.discovered_hosts.end();
  }

  template <class Body> void respond(http::response<Body> response, std::string_view host = {}) {
    response.keep_alive(false);
    response.set(http::field::cache_control, "no-store");
    response.set("X-Content-Type-Options", "nosniff");
    response.set("Referrer-Policy", "no-referrer");
    if (!host.empty()) {
      response.set("Content-Security-Policy",
          "default-src 'none'; script-src 'self' 'wasm-unsafe-eval'; style-src 'self' 'unsafe-inline'; "
          "img-src 'self' data: blob:; font-src 'self'; worker-src 'self' blob:; "
          "connect-src 'self' ws://" + std::string(host) +
          "; object-src 'none'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'");
    }
    socket_.expires_after(io_timeout);
    auto outgoing = std::make_shared<http::response<Body>>(std::move(response));
    http::async_write(socket_, *outgoing,
      [self = shared_from_this(), outgoing](Error, std::size_t) -> void {
        static_cast<void>(outgoing);
        self->close();
      });
  }

  void refuse(http::status status) {
    respond(http::response<http::empty_body>{status, request_.get().version()});
  }

  void serve() {
    const auto& request = request_.get();
    const std::string_view host = request[http::field::host];
    const std::string_view target = request.target();
    if (!host_allowed(host) || !target.starts_with('/') || target.starts_with("//"))
      return refuse(http::status::forbidden);
    if (request.method() != http::verb::get && request.method() != http::verb::head)
      return refuse(http::status::method_not_allowed);
    const auto path = target.substr(0, target.find('?'));
    if (path == "/feed") {
      if (target == path && websocket::is_upgrade(request) &&
          request[http::field::origin] == "http://" + std::string(host) &&
          request_buffer_.size() == 0)
        return owner_.viewers < max_viewers ? upgrade() : refuse(http::status::service_unavailable);
      return refuse(http::status::forbidden);
    }
    if (websocket::is_upgrade(request))
      return refuse(http::status::not_found);
    if (path == program_part_path)
      return serve_part(target, host);
    if (path == "/" || path == "/index.html") {
      http::response<http::empty_body> redirect{http::status::temporary_redirect, request.version()};
      redirect.set(http::field::location, "/view");
      return respond(std::move(redirect), host);
    }
    const auto url = path == "/view" ? std::string(panel_url) : std::string(panel_origin) + std::string(path);
    const auto asset = resolve_asset(owner_.ui_directory, url);
    if (!asset)
      return refuse(static_cast<http::status>(asset.error()));
    Error error;
    http::file_body::value_type file;
    file.open(asset->file.c_str(), beast::file_mode::scan, error);
    if (error)
      return refuse(http::status::not_found);
    return send_file(std::move(file), asset->content_type, host);
  }

  void serve_part(std::string_view target, std::string_view host) {
    const auto snapshot = owner_.latest.load();
    if (!snapshot || !snapshot->program)
      return refuse(http::status::not_found);
    const auto& program = *snapshot->program;
    const auto prefix = std::string(program_part_path) + "?revision=" +
                        std::to_string(program.revision) + "&";
    if (!target.starts_with(prefix) || target.find('#') != std::string_view::npos)
      return refuse(http::status::not_found);
    const auto url = std::string(panel_origin) + std::string(program_part_path) + "?" +
                     std::string(target.substr(prefix.size()));
    const auto model = resolve_program_part(url, program.file, ProgramPartLinks::skip);
    if (!model)
      return refuse(http::status::not_found);

    // The selected sibling can be replaced while the request is in flight. Check the open
    // descriptor and refuse symlinks or special files even if the directory entry has changed.
    beast::file descriptor;
    descriptor.native_handle(open(model->c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    struct stat metadata {};
    if (!descriptor.is_open() || fstat(descriptor.native_handle(), &metadata) != 0 ||
        !S_ISREG(metadata.st_mode) || metadata.st_size <= 0)
      return refuse(http::status::not_found);
    if (static_cast<std::uint64_t>(metadata.st_size) > max_step_bytes)
      return refuse(http::status::payload_too_large);

    Error error;
    http::file_body::value_type file;
    file.reset(std::move(descriptor), error);
    if (error || file.size() == 0)
      return refuse(http::status::not_found);
    if (file.size() > max_step_bytes)
      return refuse(http::status::payload_too_large);
    const auto current = owner_.latest.load();
    if (!current || current->program != snapshot->program)
      return refuse(http::status::not_found);
    send_file(std::move(file), "model/step", host);
  }

  void send_file(http::file_body::value_type file, std::string_view content_type,
                 std::string_view host) {
    const auto& request = request_.get();
    if (request.method() == http::verb::head) {
      http::response<http::empty_body> headers{http::status::ok, request.version()};
      headers.set(http::field::content_type, content_type);
      headers.content_length(file.size());
      return respond(std::move(headers), host);
    }
    http::response<http::file_body> response{http::status::ok, request.version()};
    response.set(http::field::content_type, content_type);
    response.content_length(file.size());
    response.body() = std::move(file);
    respond(std::move(response), host);
  }

  void upgrade() {
    socket_.expires_never();
    ws_.emplace(std::move(socket_));
    ++owner_.viewers;
    ws_->set_option(websocket::stream_base::timeout{io_timeout, 30s, true});
    ws_->read_message_max(1024);
    ws_->text(true);
    // The browser sends no data frames and no pings: it only answers keep-alive pings and may
    // close. Any other input disconnects the client, so input cannot hold a viewer slot.
    input_start_ = ws_->next_layer().rate_policy().received();
    ws_->control_callback([this](websocket::frame_type kind, beast::string_view payload) -> void {
      if (kind == websocket::frame_type::ping)
        return close();
      if (kind == websocket::frame_type::pong)
        pong_bytes_ += client_control_header + payload.size();
    });
    ws_->async_accept(request_.get(), [self = shared_from_this()](Error error) -> void {
      if (error)
        return self->close();
      // Completes on the first data byte, an empty final frame, a close or an error.
      self->ws_->async_read_some(self->discard_, 1,
        [self](Error, std::size_t) -> void { self->close(); });
      self->send();
    });
  }

  // Input that pongs do not explain. Beast skips empty non-final fragments without completing a
  // read, and they reset its idle timeout. One control frame can still be arriving, but not for
  // longer than partial_input_timeout.
  [[nodiscard]] auto sent_input(const websocket::stream<Stream>& viewer) -> bool {
    const auto unexplained = viewer.next_layer().rate_policy().received() - input_start_ - pong_bytes_;
    if (unexplained == 0) {
      partial_since_.reset();
      return false;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!partial_since_)
      partial_since_ = now;
    return unexplained > max_partial_control ||
           now - partial_since_.value_or(now) > partial_input_timeout;
  }

  void send() {
    if (closed_ || !ws_)
      return;
    auto& viewer = *ws_;
    if (sent_input(viewer))
      return close();
    next_ = owner_.latest.load();
    for (std::size_t i = 0; next_ && i < next_->records.size(); ++i) {
      const auto& record = next_->records[i];
      if (record && sent_[i].lock() != record)
        writing_.push_back(record);
    }
    for (auto& entry : owner_.notifications_after(last_event_)) {
      last_event_ = entry.sequence;
      writing_.push_back(std::move(entry.json));
    }
    if (writing_.empty())
      return wait_for_next();
    buffers_.push_back(net::buffer("[", 1));
    for (std::size_t i = 0; i < writing_.size(); ++i) {
      if (i != 0)
        buffers_.push_back(net::buffer(",", 1));
      buffers_.push_back(net::buffer(*writing_[i]));
    }
    buffers_.push_back(net::buffer("]", 1));
    deadline_.expires_after(io_timeout);
    deadline_.async_wait([self = shared_from_this()](Error error) -> void {
      if (!error)
        self->close();
    });
    viewer.async_write(buffers_, [self = shared_from_this()](Error error, std::size_t) -> void {
      self->deadline_.cancel();
      if (error)
        return self->close();
      if (self->next_)
        for (std::size_t i = 0; i < self->sent_.size(); ++i)
          self->sent_[i] = self->next_->records[i];
      self->next_.reset();
      self->writing_.clear();
      self->buffers_.clear();
      self->wait_for_next();
    });
  }

  void wait_for_next() {
    tick_.expires_after(interval);
    tick_.async_wait([self = shared_from_this()](Error error) -> void {
      if (!error)
        self->send();
    });
  }

  void close() {
    if (std::exchange(closed_, true))
      return;
    tick_.cancel();
    deadline_.cancel();
    Error ignored;
    auto& stream = ws_ ? beast::get_lowest_layer(*ws_) : socket_;
    std::ignore = stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
    std::ignore = stream.socket().close(ignored);
  }

  Impl& owner_;
  Stream socket_;
  net::steady_timer tick_, deadline_;
  beast::flat_buffer request_buffer_{8192}, discard_{1024};
  http::request_parser<http::empty_body> request_;
  std::optional<websocket::stream<Stream>> ws_;
  std::size_t input_start_{}, pong_bytes_{};
  std::optional<std::chrono::steady_clock::time_point> partial_since_;
  std::array<std::weak_ptr<const std::string>, record_count> sent_;
  std::shared_ptr<const Snapshot> next_;
  std::vector<Record> writing_;
  std::vector<net::const_buffer> buffers_;
  std::uint64_t last_event_{};
  bool closed_{};
};

RemoteServer::Impl::Impl(const std::string& directory, std::uint16_t port,
                         std::span<const std::string> aliases)
    : ui_directory(directory) {
  Error error;
  listener.open(tcp::v6(), error);
  if (!error)
    listener.set_option(net::ip::v6_only(false), error);
  const auto protocol = error ? tcp::v4() : tcp::v6();
  if (error) {
    if (listener.is_open())
      listener.close();
    listener.open(protocol);
  }
  listener.set_option(tcp::acceptor::reuse_address(true));
  listener.bind({protocol, port});
  listener.listen();
  listening_port = listener.local_endpoint().port();
  for (const auto& name : aliases)
    if (auto normalized = normalize_remote_host(name))
      allowed_hosts.push_back(std::move(*normalized));
  std::array<char, 256> raw_name{};
  if (gethostname(raw_name.data(), raw_name.size() - 1) == 0) {
    if (const auto name = normalize_remote_host(raw_name.data())) {
      const auto short_name = name->substr(0, name->find('.'));
      allowed_hosts.insert(allowed_hosts.end(), {*name, short_name, short_name + ".local"});
      discover_hosts(*name);
    }
  }
  accept();
  thread = std::thread([this]() -> void {
    try {
      io.run();
    } catch (const std::exception&) {
      io.stop();
    }
  });
}

RemoteServer::Impl::~Impl() {
  io.stop();
  thread.join();
}

void RemoteServer::Impl::discover_hosts(const std::string& name) {
  const auto short_name = name.substr(0, name.find('.'));
  resolve_host(name, false);
  for (const auto& domain : dns_search_domains()) {
    auto qualified_name = short_name + '.';
    qualified_name += domain;
    if (auto candidate = normalize_remote_host(qualified_name))
      resolve_host(candidate->append("."), true);
  }
  refresh_hosts(name);
}

void RemoteServer::Impl::refresh_hosts(const std::string& name) {
  refresh.expires_after(host_refresh);
  refresh.async_wait([this, name](Error error) -> void {
    if (error)
      return;
    if (pending_resolutions == 0)
      discover_hosts(name);
    else
      refresh_hosts(name);
  });
}

void RemoteServer::Impl::resolve_host(const std::string& query, bool search_candidate) {
  // Use only the OS hostname and system DNS configuration as lookup input.
  ++pending_resolutions;
  resolver.async_resolve(query, "0", tcp::resolver::canonical_name,
    [this, candidate = search_candidate ? query : std::string{}]
    (Error error, const tcp::resolver::results_type& entries) -> void {
      if (!error) {
        // Validate against the addresses held now, including DHCP changes since startup.
        const auto local = local_ip_addresses();
        if (std::ranges::any_of(entries, [&local](const auto& entry) {
              return std::ranges::find(local, comparable(entry.endpoint().address())) != local.end();
            }))
          if (auto name = normalize_remote_host(candidate.empty() ? entries.begin()->host_name() : candidate))
            resolving_hosts.push_back(std::move(*name));
      }
      if (--pending_resolutions == 0) {
        discovered_hosts.swap(resolving_hosts);
        resolving_hosts.clear();
      }
    });
}

void RemoteServer::Impl::accept() {
  listener.async_accept([this](Error error, tcp::socket socket) -> void {
    if (!error) {
      if (clients < max_connections)
        std::make_shared<Client>(*this, std::move(socket))->start();
      accept();
    } else if (error != net::error::operation_aborted) {
      accept_retry.expires_after(250ms);
      accept_retry.async_wait([this](Error retry_error) -> void {
        if (!retry_error)
          accept();
      });
    }
  });
}

RemoteServer::RemoteServer(const std::string& ui_directory, std::uint16_t port,
                           std::span<const std::string> aliases)
    : impl_(std::make_unique<Impl>(ui_directory, port, aliases)) {}
RemoteServer::~RemoteServer() = default;
auto RemoteServer::port() const -> std::uint16_t { return impl_->listening_port; }

void RemoteServer::publish(std::span<const std::string> messages,
                           std::span<const PanelRecord> records, const ProgramText* program,
                           Link link) noexcept {
  try {
    if (messages.size() != records.size())
      return;
    for (std::size_t i = 0; i < records.size(); ++i) {
      const auto record = records[i];
      const auto& text = messages[i];
      if (record == PanelRecord::message) {
        impl_->notify(text);
      } else if (record != PanelRecord::offset_history) {
        auto& slot = impl_->staged.records[index(record)];
        if (text.size() > max_record_bytes) {
          slot.reset();
        } else if (record == PanelRecord::hello) {
          if (!slot)
            slot = std::make_shared<const std::string>(viewer_hello(text));
        } else {
          slot = std::make_shared<const std::string>(text);
        }
      }
    }
    if (program == nullptr)
      impl_->staged.records[index(PanelRecord::program)].reset();
    if (program != nullptr && link == Link::live)
      impl_->staged.program = ProgramIdentity{program->file, program->revision};
    else
      impl_->staged.program.reset();
    impl_->latest.store(std::make_shared<const Snapshot>(impl_->staged));
  } catch (const std::exception&) {
    // A failed mirror cannot prevent publishing to the local panel.
    return;
  }
}

void RemoteServer::notifications(std::span<const std::string> messages) noexcept {
  try {
    for (const auto& message : messages) {
      if (message.size() <= max_notification_bytes &&
          message.find("\"type\":\"message\"") != std::string::npos) {
        const auto parsed = nlohmann::json::parse(message);
        if (parsed.value("type", "") == "message")
          impl_->notify(message);
      }
    }
  } catch (const std::exception&) {
    // Local command and error delivery takes priority.
    return;
  }
}

} // namespace coffeecnc
