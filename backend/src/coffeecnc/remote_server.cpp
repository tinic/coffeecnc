#include "remote_server.hpp"

#include <coffeecnc/app/panel_assets.hpp>
#include <coffeecnc/core/remote_hosts.hpp>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <resolv.h>
#include <unistd.h>

namespace coffeecnc {
namespace {

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using net::ip::address_v4;
using net::ip::tcp;
using Error = boost::system::error_code;
using namespace std::chrono_literals;

constexpr std::size_t record_count = static_cast<std::size_t>(PanelRecord::message);
constexpr std::size_t max_connections = 32;
constexpr std::size_t max_viewers = 8;
constexpr std::size_t max_notifications = 64;
constexpr std::size_t max_notification_bytes = 8192;
constexpr std::size_t max_record_bytes = 48 * 1024 * 1024;
constexpr auto io_timeout = 5s;
constexpr auto interval = 75ms;

using Record = std::shared_ptr<const std::string>;
using Snapshot = std::array<Record, record_count>;

struct Notification {
  std::uint64_t sequence;
  Record json;
};

auto index(PanelRecord record) -> std::size_t { return static_cast<std::size_t>(record); }


auto local_ipv4_addresses() -> std::vector<address_v4> {
  ifaddrs* interfaces = nullptr;
  if (getifaddrs(&interfaces) != 0)
    return {};
  const std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> guard{interfaces, freeifaddrs};
  std::vector<address_v4> addresses;
  for (auto* iface = interfaces; iface != nullptr; iface = iface->ifa_next)
    if (iface->ifa_addr && iface->ifa_addr->sa_family == AF_INET) {
      const auto* address = reinterpret_cast<const sockaddr_in*>(iface->ifa_addr);
      addresses.emplace_back(ntohl(address->sin_addr.s_addr));
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
  void add_host(std::string_view name);
  void discover_hosts();
  void resolve_host(std::string query, bool search_candidate);

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
    for (const auto& event : events)
      if (event.sequence > after)
        result.push_back(event);
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
  const std::vector<address_v4> local_addresses = local_ipv4_addresses();
  net::io_context io{1};
  tcp::resolver resolver{io};
  tcp::acceptor listener{io};
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
    const auto name = normalize_remote_host(host);
    if (!name)
      return false;
    if (*name == "localhost")
      return local.address().is_loopback();
    // Never resolve the request's Host. Foreign DNS names must not be able to rebind to this origin.
    return *name == local.address().to_string() ||
           std::ranges::find(owner_.allowed_hosts, *name) != owner_.allowed_hosts.end();
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
    if (request.method() == http::verb::head) {
      http::response<http::empty_body> headers{http::status::ok, request.version()};
      headers.set(http::field::content_type, asset->content_type);
      headers.content_length(file.size());
      return respond(std::move(headers), host);
    }
    http::response<http::file_body> response{http::status::ok, request.version()};
    response.set(http::field::content_type, asset->content_type);
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
    ws_->async_accept(request_.get(), [self = shared_from_this()](Error error) -> void {
      if (error)
        return self->close();
      // The browser sends no data frames. A client that tries to send one is disconnected.
      self->ws_->async_read(self->discard_,
        [self](Error, std::size_t) -> void { self->close(); });
      self->send();
    });
  }

  void send() {
    if (closed_)
      return;
    next_ = owner_.latest.load();
    for (std::size_t i = 0; next_ && i < next_->size(); ++i) {
      const auto& record = (*next_)[i];
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
    ws_->async_write(buffers_, [self = shared_from_this()](Error error, std::size_t) -> void {
      self->deadline_.cancel();
      if (error)
        return self->close();
      if (self->next_)
        for (std::size_t i = 0; i < self->sent_.size(); ++i)
          self->sent_[i] = (*self->next_)[i];
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
    stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
    stream.socket().close(ignored);
  }

  Impl& owner_;
  beast::tcp_stream socket_;
  net::steady_timer tick_, deadline_;
  beast::flat_buffer request_buffer_{8192}, discard_{1024};
  http::request_parser<http::empty_body> request_;
  std::optional<websocket::stream<beast::tcp_stream>> ws_;
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
  listener.open(tcp::v4());
  listener.set_option(tcp::acceptor::reuse_address(true));
  listener.bind({tcp::v4(), port});
  listener.listen();
  listening_port = listener.local_endpoint().port();
  for (const auto& name : aliases)
    add_host(name);
  discover_hosts();
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

void RemoteServer::Impl::add_host(std::string_view name) {
  if (auto normalized = normalize_remote_host(name);
      normalized && std::ranges::find(allowed_hosts, *normalized) == allowed_hosts.end())
    allowed_hosts.push_back(std::move(*normalized));
}

void RemoteServer::Impl::discover_hosts() {
  std::array<char, 256> raw_name{};
  if (gethostname(raw_name.data(), raw_name.size() - 1) != 0)
    return;
  const auto name = normalize_remote_host(raw_name.data());
  if (!name)
    return;
  const auto short_name = name->substr(0, name->find('.'));
  add_host(*name);
  add_host(short_name);
  add_host(short_name + ".local");

  resolve_host(*name, false);
  for (const auto& domain : dns_search_domains())
    if (auto candidate = normalize_remote_host(short_name + "." + domain))
      resolve_host(*candidate + ".", true);
}

void RemoteServer::Impl::resolve_host(std::string query, bool search_candidate) {
  // Use only the OS hostname and system DNS configuration as lookup input.
  resolver.async_resolve(tcp::v4(), query, "0", tcp::resolver::canonical_name,
    [this, candidate = search_candidate ? query : std::string{}]
    (Error error, const tcp::resolver::results_type& entries) -> void {
      if (error || entries.empty() || !std::ranges::all_of(entries, [this](const auto& entry) {
            const auto address = entry.endpoint().address().to_v4();
            return std::ranges::find(local_addresses, address) != local_addresses.end();
          }))
        return;
      add_host(candidate.empty() ? entries.begin()->host_name() : candidate);
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
                           std::span<const PanelRecord> records, bool has_program) noexcept {
  try {
    if (messages.size() != records.size())
      return;
    for (std::size_t i = 0; i < records.size(); ++i) {
      const auto record = records[i];
      const auto& text = messages[i];
      if (record == PanelRecord::message) {
        impl_->notify(text);
      } else if (record != PanelRecord::offset_history) {
        auto& slot = impl_->staged[index(record)];
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
    if (!has_program)
      impl_->staged[index(PanelRecord::program)].reset();
    impl_->latest.store(std::make_shared<const Snapshot>(impl_->staged));
  } catch (const std::exception&) {
    // A failed mirror cannot prevent publishing to the local panel.
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
  }
}

} // namespace coffeecnc
