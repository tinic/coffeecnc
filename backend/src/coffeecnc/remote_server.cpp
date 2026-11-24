#include "remote_server.hpp"

#include <coffeecnc/app/panel_assets.hpp>
#include <coffeecnc/app/program_store.hpp>
#include <coffeecnc/core/remote_hosts.hpp>

#include "viewer_boost.hpp"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <concepts>
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
constexpr auto upload_timeout = 30s;
constexpr auto interval = 75ms;
constexpr auto host_refresh = 5s;
constexpr std::string_view upload_path = "/program-upload";

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

struct DiscoveredHost {
  std::string query;
  std::string name;
  std::vector<address> local_addresses;
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

auto viewer_hello(const std::string& message, bool upload) -> std::string {
  auto hello = nlohmann::json::parse(message);
  hello.erase("program_places");
  hello.erase("macros");
  hello.erase("operator_settings_storage");
  hello.erase("tool_model_storage");
  hello.erase("quit_supported");
  if (upload)
    hello["program_upload"] = true;
  hello.erase("diagnostics");
  return hello.dump();
}

} // namespace

struct RemoteServer::Impl {
  class Client;

  Impl(const std::string& directory, std::uint16_t port, std::span<const std::string> aliases,
       Upload receive);
  ~Impl();
  void stop();
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
  const Upload upload;
  std::optional<net::thread_pool> upload_worker;
  bool upload_active{};
  Snapshot staged{};
  std::atomic<std::shared_ptr<const Snapshot>> latest{};
  std::mutex events_mutex;
  std::deque<Notification> events;
  std::uint64_t event_sequence{};
  std::size_t clients{};
  std::size_t viewers{};
  std::vector<std::string> allowed_hosts;
  std::vector<DiscoveredHost> discovered_hosts;
  std::vector<DiscoveredHost> resolving_hosts;
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
    request_.body_limit(ProgramStore::max_program_bytes);
  }

  ~Client() {
    if (ws_)
      --owner_.viewers;
    if (uploading_)
      owner_.upload_active = false;
    --owner_.clients;
  }

  void start() {
    socket_.expires_after(io_timeout);
    http::async_read_header(socket_, request_buffer_, request_,
      [self = shared_from_this()](Error error, std::size_t) -> void {
        if (!error)
          self->serve();
        else if (error == http::error::body_limit)
          self->upload_reply(http::status::payload_too_large, upload_too_large());
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
           std::ranges::find(owner_.discovered_hosts, *name, &DiscoveredHost::name) !=
               owner_.discovered_hosts.end();
  }

  struct FileTransfer {
    explicit FileTransfer(http::response<http::file_body> response)
        : message(std::move(response)), serializer(message) {}
    http::response<http::file_body> message;
    http::response_serializer<http::file_body> serializer;
  };

  void write_file(const std::shared_ptr<FileTransfer>& transfer) {
    // The stream's timeout is per write. Progress keeps a slow LAN download alive.
    socket_.expires_after(io_timeout);
    http::async_write_some(socket_, transfer->serializer,
      [self = shared_from_this(), transfer](Error error, std::size_t) -> void {
        if (error || transfer->serializer.is_done())
          self->close();
        else
          self->write_file(transfer);
      });
  }

  template <class Body> void respond(http::response<Body> response, std::string_view host = {},
                                     bool rejected_upload = false) {
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
    if constexpr (std::same_as<Body, http::file_body>) {
      write_file(std::make_shared<FileTransfer>(std::move(response)));
    } else {
      socket_.expires_after(io_timeout);
      auto outgoing = std::make_shared<http::response<Body>>(std::move(response));
      http::async_write(socket_, *outgoing,
        [self = shared_from_this(), outgoing, rejected_upload](Error error, std::size_t) -> void {
          static_cast<void>(outgoing);
          if (!error && rejected_upload)
            self->finish_rejected_upload();
          else
            self->close();
        });
    }
  }

  void refuse(http::status status) {
    respond(http::response<http::empty_body>{status, request_.get().version()});
  }

  void serve() {
    const auto& request = request_.get();
    const std::string_view host = request[http::field::host];
    const std::string_view target = request.target();
    const bool uploading = request.method() == http::verb::post && target == upload_path;
    if (!host_allowed(host) || !target.starts_with('/') || target.starts_with("//")) {
      if (uploading)
        return upload_reply(http::status::forbidden);
      return refuse(http::status::forbidden);
    }
    if (uploading) {
      if (owner_.upload_worker)
        return begin_upload(host, *owner_.upload_worker);
      return upload_reply(http::status::method_not_allowed);
    }
    if (request.method() != http::verb::get && request.method() != http::verb::head)
      return refuse(http::status::method_not_allowed);
    if (!request_.is_done())
      return refuse(http::status::bad_request);
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

  static auto upload_too_large() -> LocalizedReason {
    return {"The program is longer than 16 MiB.", MessageKey{"backend.programFiles.size"}};
  }

  void upload_reply(http::status status, LocalizedReason reason = {}) {
    http::response<http::string_body> response{status, request_.get().version()};
    response.set(http::field::content_type, "application/json");
    nlohmann::json payload{{"reason", std::move(reason.text)}};
    if (const auto& key = reason.localization)
      payload["localization"] = {{"key", key->key}, {"params", key->params}};
    response.body() = payload.dump();
    response.prepare_payload();
    respond(std::move(response), {}, !request_.is_done());
  }

  void finish_rejected_upload() {
    // Send the complete response and FIN before closing the receive side, which may
    // still have body bytes waiting. A full close with unread bytes can reset the reply.
    Error error;
    std::ignore = socket_.socket().shutdown(tcp::socket::shutdown_send, error);
    if (error)
      return close();
    socket_.expires_after(io_timeout);
    drain_rejected_upload(ProgramStore::max_program_bytes);
  }

  void drain_rejected_upload(std::size_t remaining) {
    if (remaining == 0)
      return close();
    socket_.async_read_some(net::buffer(upload_discard_.data(), std::min(upload_discard_.size(), remaining)),
      [self = shared_from_this(), remaining](Error error, std::size_t consumed) -> void {
        if (error || consumed == 0)
          self->close();
        else
          self->drain_rejected_upload(remaining - consumed);
      });
  }

  void begin_upload(std::string_view host, net::thread_pool& worker) {
    const auto& request = request_.get();
    if (request[http::field::origin] != "http://" + std::string(host))
      return upload_reply(http::status::forbidden, {"Upload from the CoffeeCNC LAN screen.",
                                                    MessageKey{"backend.programFiles.sameOrigin"}});
    if (request[http::field::content_type] != "application/octet-stream")
      return upload_reply(http::status::unsupported_media_type);
    auto name = program_upload_name(request["X-CoffeeCNC-Filename"]);
    if (!name || request_.is_done())
      return upload_reply(http::status::bad_request, {"Choose a program file to upload.",
                                                      MessageKey{"backend.programFiles.choose"}});
    if (owner_.upload_active)
      return upload_reply(
          http::status::service_unavailable,
          {"Another program upload is in progress.", MessageKey{"backend.programFiles.busy"}});
    owner_.upload_active = uploading_ = true;
    const bool replace = request["X-CoffeeCNC-Replace"] == "1";
    read_upload(worker, std::move(*name), replace);
  }

  void read_upload(net::thread_pool& worker, std::string name, bool replace) {
    socket_.expires_after(upload_timeout);
    http::async_read_some(socket_, request_buffer_, request_,
      [self = shared_from_this(), &worker, name = std::move(name), replace](Error error, std::size_t) mutable -> void {
        if (error == http::error::body_limit)
          return self->upload_reply(http::status::payload_too_large, upload_too_large());
        if (error)
          return self->close();
        if (self->request_.is_done())
          self->write_upload(worker, std::move(name), replace);
        else
          self->read_upload(worker, std::move(name), replace);
      });
  }

  void write_upload(net::thread_pool& worker, std::string name, bool replace) {
    auto& body = request_.get().body();
    if (body.empty() || body.find('\0') != std::string::npos)
      return upload_reply(http::status::bad_request, {"The upload must be a program text file.",
                                                      MessageKey{"backend.programFiles.text"}});
    net::post(worker, [self = shared_from_this(), name = std::move(name), replace,
                                     bytes = std::move(body)]() mutable -> void {
      auto status = http::status::created;
      LocalizedReason reason;
      try {
        if (auto saved = self->owner_.upload(name, bytes, replace); !saved) {
          status = http::status::conflict;
          reason = std::move(saved.error());
        }
      } catch (const std::exception&) {
        status = http::status::internal_server_error;
        reason = {"The upload could not be saved.",
                  MessageKey{"backend.programFiles.uploadFailed"}};
      }
      auto& io = self->owner_.io;
      net::post(io, [self = std::move(self), status, reason = std::move(reason)]() mutable -> void {
        self->upload_reply(status, std::move(reason));
      });
    });
  }

  void serve_part(std::string_view target, std::string_view host) {
    const auto snapshot = owner_.latest.load();
    if (!snapshot)
      return refuse(http::status::not_found);
    const auto& selected = snapshot->program;
    if (!selected)
      return refuse(http::status::not_found);
    const auto& program = *selected;
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
  std::array<char, 4096> upload_discard_{};
  http::request_parser<http::string_body> request_;
  std::optional<websocket::stream<Stream>> ws_;
  std::size_t input_start_{}, pong_bytes_{};
  std::optional<std::chrono::steady_clock::time_point> partial_since_;
  std::array<std::weak_ptr<const std::string>, record_count> sent_;
  std::shared_ptr<const Snapshot> next_;
  std::vector<Record> writing_;
  std::vector<net::const_buffer> buffers_;
  std::uint64_t last_event_{};
  bool closed_{};
  bool uploading_{};
};

RemoteServer::Impl::Impl(const std::string& directory, std::uint16_t port,
                         std::span<const std::string> aliases, Upload receive)
    : ui_directory(directory), upload(std::move(receive)) {
  if (upload)
    upload_worker.emplace(1);
  Error error;
  std::ignore = listener.open(tcp::v6(), error);
  if (!error)
    std::ignore = listener.set_option(net::ip::v6_only(false), error);
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

RemoteServer::Impl::~Impl() { stop(); }

void RemoteServer::Impl::stop() {
  if (!thread.joinable())
    return;
  io.stop();
  thread.join();
  if (upload_worker)
    upload_worker->join();
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
    [this, query, search_candidate]
    (Error error, const tcp::resolver::results_type& entries) -> void {
      // Validate against the addresses held now, including DHCP changes since startup.
      const auto local = local_ip_addresses();
      const auto is_local = [&local](const address& ip) -> bool {
        return std::ranges::find(local, ip) != local.end();
      };
      if (error) {
        const auto previous = std::ranges::find(discovered_hosts, query, &DiscoveredHost::query);
        // A failed refresh doesn't supersede a successful lookup while one of its addresses
        // still belongs to the machine. A new answer or a withdrawn address can revoke it.
        if (previous != discovered_hosts.end() && std::ranges::any_of(previous->local_addresses, is_local))
          resolving_hosts.push_back(*previous);
      } else {
        std::vector<address> matched;
        for (const auto& entry : entries) {
          const auto ip = comparable(entry.endpoint().address());
          if (is_local(ip))
            matched.push_back(ip);
        }
        if (!matched.empty())
          if (auto name = normalize_remote_host(search_candidate ? query : entries.begin()->host_name()))
            resolving_hosts.push_back({query, std::move(*name), std::move(matched)});
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
                           std::span<const std::string> aliases, Upload upload)
    : impl_(std::make_unique<Impl>(ui_directory, port, aliases, std::move(upload))) {}
RemoteServer::~RemoteServer() = default;
auto RemoteServer::port() const -> std::uint16_t { return impl_->listening_port; }
void RemoteServer::stop() { impl_->stop(); }

void RemoteServer::publish(std::span<const std::string> messages,
                           std::span<const PanelRecord> records, const Status& status,
                           bool has_program) noexcept {
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
            slot = std::make_shared<const std::string>(viewer_hello(text, static_cast<bool>(impl_->upload)));
        } else {
          slot = std::make_shared<const std::string>(text);
        }
      }
    }
    if (!has_program)
      impl_->staged.records[index(PanelRecord::program)].reset();
    // The viewer asks for the path in controller status. ProgramText may use a canonical
    // spelling of the same file, such as a program directory reached through a symlink.
    const auto& program_file = status.program.file;
    if (has_program && status.link == Link::live && program_file)
      impl_->staged.program = ProgramIdentity{*program_file, status.program.revision};
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
