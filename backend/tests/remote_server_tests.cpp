#include "remote_server.hpp"

#include "viewer_boost.hpp"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using net::ip::tcp;
using Json = nlohmann::json;
using coffeecnc::LocalizedReason;
using coffeecnc::MessageKey;
using coffeecnc::PanelRecord;
using coffeecnc::RemoteServer;
using namespace std::chrono_literals;

void require(bool condition, const char* why) {
  if (!condition)
    throw std::runtime_error(why);
}

auto get(net::io_context& io, std::uint16_t port, http::verb method, const char* path,
         const std::string& host, const std::string& origin = {},
         const net::ip::address& address = net::ip::address_v4::loopback())
    -> http::response<http::string_body> {
  tcp::socket tcp_socket{io};
  tcp_socket.connect({address, port});
  http::request<http::empty_body> request{method, path, 11};
  request.set(http::field::host, host);
  if (!origin.empty()) {
    request.set(http::field::origin, origin);
    request.set(http::field::upgrade, "websocket");
    request.set(http::field::connection, "upgrade");
    request.set(http::field::sec_websocket_version, "13");
    request.set(http::field::sec_websocket_key, "dGhlIHNhbXBsZSBub25jZQ==");
  }
  http::write(tcp_socket, request);
  beast::flat_buffer buffer;
  http::response_parser<http::string_body> parser;
  parser.skip(method == http::verb::head);
  http::read(tcp_socket, buffer, parser);
  return parser.release();
}

using Viewer = websocket::stream<tcp::socket>;

auto upload(net::io_context& io, std::uint16_t port, const std::string& host,
            const std::string& origin, const std::string& name, bool replace = false)
    -> http::response<http::string_body> {
  tcp::socket socket{io};
  socket.connect({net::ip::address_v4::loopback(), port});
  http::request<http::string_body> request{http::verb::post, "/program-upload", 11};
  request.set(http::field::host, host);
  request.set(http::field::origin, origin);
  request.set(http::field::content_type, "application/octet-stream");
  request.set("X-CoffeeCNC-Filename", name);
  request.set("X-CoffeeCNC-Replace", replace ? "1" : "0");
  request.body() = "G21\r\nM2\r\n";
  request.prepare_payload();
  http::write(socket, request);
  beast::flat_buffer buffer;
  http::response_parser<http::string_body> parser;
  http::read(socket, buffer, parser);
  return parser.release();
}

void program_uploads(const std::string& assets) {
  std::atomic<int> calls{};
  RemoteServer server{assets,
                      0,
                      {},
                      [&](std::string_view name, std::string_view bytes,
                          bool replace) -> std::expected<void, LocalizedReason> {
                        ++calls;
                        if (name != "Plate 2.ngc" || bytes != "G21\r\nM2\r\n" || replace)
                          return std::unexpected(LocalizedReason{
                              "A program named Plate 2.ngc exists already.",
                              MessageKey{"backend.programEdit.exists", {{"name", "Plate 2.ngc"}}}});
                        return {};
                      }};
  const auto host = "127.0.0.1:" + std::to_string(server.port());
  net::io_context io;
  const auto forbidden = upload(io, server.port(), host, "http://other.example", "Plate%202.ngc");
  require(forbidden.result() == http::status::forbidden &&
              upload(io, server.port(), "other.example", "http://other.example", "Plate%202.ngc")
                      .result() == http::status::forbidden,
          "Foreign origins and hosts cannot post a program");
  require(Json::parse(forbidden.body()) ==
              Json{{"reason", "Upload from the CoffeeCNC LAN screen."},
                   {"localization",
                    {{"key", "backend.programFiles.sameOrigin"}, {"params", Json::object()}}}},
          "The upload refusal retains its English reason and catalog key");
  require(upload(io, server.port(), host, "http://" + host, "..%2Fpart.ngc").result() ==
                  http::status::bad_request &&
              calls == 0,
          "An uploaded filename cannot name a path");
  const auto success = upload(io, server.port(), host, "http://" + host, "Plate%202.ngc");
  require(success.result() == http::status::created &&
              Json::parse(success.body()) == Json{{"reason", ""}} && calls == 1,
          "The opted-in route sends the original bytes and decoded name to the disk writer");
  const auto conflict = upload(io, server.port(), host, "http://" + host, "Plate%202.ngc", true);
  require(conflict.result() == http::status::conflict && calls == 2 &&
              Json::parse(conflict.body()) ==
                  Json{{"reason", "A program named Plate 2.ngc exists already."},
                       {"localization",
                        {{"key", "backend.programEdit.exists"},
                         {"params", {{"name", "Plate 2.ngc"}}}}}},
          "A writer refusal is reported to the browser");
}

void connect(Viewer& viewer, std::uint16_t port, const std::string& host,
             const net::ip::address& address = net::ip::address_v4::loopback()) {
  viewer.next_layer().connect({address, port});
  viewer.set_option(websocket::stream_base::decorator([&](websocket::request_type& request) -> void {
    request.set(http::field::origin, "http://" + host);
  }));
  viewer.handshake(host, "/feed");
}

auto read(Viewer& viewer) -> Json {
  beast::flat_buffer buffer;
  viewer.read(buffer);
  return Json::parse(beast::buffers_to_string(buffer.data()));
}

auto wait_for(Viewer& viewer, const char* key, int sequence) -> Json {
  for (int attempt = 0; attempt < 5; ++attempt) {
    const auto batch = read(viewer);
    if (std::ranges::any_of(batch, [key, sequence](const Json& record) {
          return record.value(key, -1) == sequence;
        }))
      return batch;
  }
  throw std::runtime_error("The current status did not arrive.");
}

void slow_model_download(net::io_context& io, std::uint16_t port, const std::string& host,
                         const std::string& url, std::size_t size) {
  tcp::socket socket{io};
  socket.connect({net::ip::address_v4::loopback(), port});
  socket.set_option(net::socket_base::receive_buffer_size(128 * 1024));
  http::request<http::empty_body> request{http::verb::get, url, 11};
  request.set(http::field::host, host);
  http::write(socket, request);
  beast::flat_buffer input;
  http::response_parser<http::empty_body> header;
  header.skip(true);
  http::read_header(socket, input, header);
  require(header.get().result() == http::status::ok && header.content_length() == size,
          "The slow STEP viewer receives the full length in the header.");
  std::vector<char> chunk(2uz * 1024 * 1024);
  for (std::size_t received = input.size(); received < size;) {
    // Slow, steady reads take longer than one five-second write deadline.
    std::this_thread::sleep_for(900ms);
    const auto count = std::min(chunk.size(), size - received);
    received += net::read(socket, net::buffer(chunk.data(), count));
  }
}

// True if the server ends the connection within `wait`. The viewer reads all updates
// meanwhile, so a connection that stays open is a live viewer, not a slow one.
auto server_closes(net::io_context& io, Viewer& viewer,
                   std::chrono::steady_clock::duration wait = 2s) -> bool {
  beast::flat_buffer buffer;
  bool closed = false;
  std::function<void()> next = [&] {
    viewer.async_read(buffer, [&](boost::system::error_code error, std::size_t) -> void {
      if (error) {
        closed = true;
        return;
      }
      buffer.clear();
      next();
    });
  };
  next();
  io.restart();
  io.run_for(wait);
  const bool by_server = closed;
  if (!by_server) {
    boost::system::error_code ignored;
    std::ignore = viewer.next_layer().close(ignored);
    io.restart();
    io.run();
  }
  return by_server;
}

// A browser viewer sends no input. Each kind of client frame must end the connection, so a
// client cannot keep one of the viewer slots with traffic other than the replies to keep-alives.
void input_frames_close(net::io_context& io, std::uint16_t port, const std::string& host) {
  {
    Viewer text{io};
    connect(text, port, host);
    text.write(net::buffer(std::string_view{R"({"type":"set_machine","state":"on"})"}));
    require(server_closes(io, text), "A text frame closes the read-only socket.");
  }
  {
    Viewer fragment{io};
    connect(fragment, port, host);
    fragment.write_some(false, net::buffer(std::string_view{"{"}));
    require(server_closes(io, fragment), "A data fragment closes the read-only socket.");
  }
  {
    // Empty non-final fragments never complete a message or a read, but they reset the idle
    // timeout. Without a check they hold the slot forever.
    Viewer empty{io};
    connect(empty, port, host);
    for (int i = 0; i < 64; ++i)
      empty.write_some(false, net::const_buffer{});
    require(server_closes(io, empty), "Empty fragments close the read-only socket.");
  }
  {
    Viewer ping{io};
    connect(ping, port, host);
    ping.ping({});
    require(server_closes(io, ping), "A client ping closes the read-only socket.");
  }
  {
    // The first byte of a pong header, then nothing: the rest of a frame is due within 2 s.
    Viewer stalled{io};
    connect(stalled, port, host);
    constexpr std::array<unsigned char, 1> header{0x8a};
    net::write(stalled.next_layer(), net::buffer(header));
    require(server_closes(io, stalled, 4s), "A partial frame that stalls closes the socket.");
  }
  {
    // A viewer that only answers keep-alives stays connected.
    Viewer quiet{io};
    connect(quiet, port, host);
    quiet.pong({});
    require(!server_closes(io, quiet), "A pong does not close the viewer.");
  }
}

void test(const std::filesystem::path& root, std::string_view discovered,
          std::string_view nonlocal, std::string_view loopback_v6, std::string_view delayed) {
  const auto assets = root / "remote-server-assets";
  std::filesystem::create_directories(assets);
  std::ofstream(assets / "index.html") << "<title>Remote test panel</title>";
  program_uploads(assets.string());
  std::ofstream(root / "remote-server-private.txt") << "outside the panel";
  const std::array aliases{std::string{"shop.mill.example"}, std::string{"Friendly.Example."}};
  RemoteServer server{assets.string(), 0, aliases};
  // Only the test constructs a server with port zero, to get a free TCP port.
  const auto port = server.port();
  const auto suffix = ":" + std::to_string(port);
  const auto host = "127.0.0.1" + suffix;
  const auto alias = aliases.front() + suffix;
  net::io_context io;
  require(upload(io, port, host, "http://" + host, "sample.ngc").result() ==
              http::status::method_not_allowed,
          "The default viewer has no upload route");
  const auto ipv6 = net::ip::address_v6::loopback();
  const auto host6 = "[::1]" + suffix;
  boost::system::error_code ipv6_unavailable;
  tcp::acceptor ipv6_probe{io};
  std::ignore = ipv6_probe.open(tcp::v6(), ipv6_unavailable);
  if (!ipv6_unavailable)
    std::ignore = ipv6_probe.bind({ipv6, 0}, ipv6_unavailable);
  const auto home = get(io, port, http::verb::get, "/", host);
  require(home.result() == http::status::temporary_redirect && home[http::field::location] == "/view",
          "The root redirects to the viewer entry.");
  const auto index = get(io, port, http::verb::get, "/view", host);
  require(index.result() == http::status::ok && index.body() == "<title>Remote test panel</title>" &&
              index["Content-Security-Policy"].find("frame-ancestors 'none'") != std::string_view::npos,
          "The panel bundle has a same-origin response.");
  require(get(io, port, http::verb::head, "/view", host).result() == http::status::ok,
          "Asset HEAD is supported.");
  require(get(io, port, http::verb::get, "/%2e%2e/remote-server-private.txt", host).result() ==
              http::status::forbidden,
          "Requests stay within the built panel assets.");
  for (const auto& name : {alias, "FRIENDLY.Example." + suffix, "localhost" + suffix})
    require(get(io, port, http::verb::get, "/view", name).result() == http::status::ok,
            "A configured alias or the loopback hostname can open the viewer.");
  if (!ipv6_unavailable) {
    for (const auto& name : {host6, "[0:0:0:0:0:0:0:1]" + suffix, "localhost" + suffix, alias})
      require(get(io, port, http::verb::get, "/view", name, {}, ipv6).result() == http::status::ok,
              "IPv6 accepts its numeric endpoint and the same allowed local names.");
    for (const auto& name : {"[::2]" + suffix, "::1" + suffix, "external.example" + suffix,
                             "[::1%25lo]" + suffix, "[::1]:" + std::to_string(port + 1)})
      require(get(io, port, http::verb::get, "/view", name, {}, ipv6).result() ==
                  http::status::forbidden,
              "IPv6 requires a matching endpoint and a valid Host.");
    require(get(io, port, http::verb::get, "/feed", host6, "http://" + host, ipv6).result() ==
                http::status::forbidden,
            "IPv6 feed still requires the requesting browser origin.");
  }

  std::array<char, 256> hostname{};
  require(gethostname(hostname.data(), hostname.size() - 1) == 0, "Read the system hostname.");
  const std::string machine{hostname.data()};
  const auto short_name = machine.substr(0, machine.find('.'));
  for (const auto& name : {machine, short_name, short_name + ".local"})
    require(get(io, port, http::verb::get, "/view", name + suffix).result() == http::status::ok,
            "The hostname, short name and .local name can open the viewer.");

  for (const auto& name : {"external.example" + suffix, "shop.mill.example.external" + suffix,
                           std::string{"shop.mill.example:0"}, aliases.front()})
    require(get(io, port, http::verb::get, "/view", name).result() == http::status::forbidden,
            "The Host must name an allowed host on the listener port.");
  require(get(io, port, http::verb::get, "/feed", alias, "http://friendly.example" + suffix).result() ==
              http::status::forbidden,
          "A separately allowed alias does not grant cross-origin feed access.");

  const auto wait_for_host = [&](std::string_view name, http::status expected,
                                 std::chrono::steady_clock::duration wait) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + wait;
    const auto authority = std::string(name) + suffix;
    do {
      if (get(io, port, http::verb::get, "/view", authority).result() == expected)
        return true;
      std::this_thread::sleep_for(30ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
  };
  // A DNS fixture can supply two search results, one local and one on a different machine.
  if (!discovered.empty()) {
    require(wait_for_host(discovered, http::status::ok, 3s),
            "A DNS search name that resolves locally is allowed.");
    require(get(io, port, http::verb::get, "/view", std::string(nonlocal) + suffix).result() ==
                http::status::forbidden,
            "A DNS search name that resolves off-machine is rejected.");
  }
  if (!loopback_v6.empty() && !ipv6_unavailable)
    require(wait_for_host(loopback_v6, http::status::ok, 3s),
            "A DNS search name with a local IPv6 address is allowed.");
  if (!delayed.empty()) {
    require(wait_for_host(delayed, http::status::forbidden, 1s),
            "The address-change fixture starts with the name unapproved.");
    // The shell binds this writable fixture to /etc/hosts inside the isolated namespace.
    const auto hosts_file = root / "hosts";
    const std::string workshop_entry = std::string(discovered) + ".";
    std::string other_entries;
    {
      std::ifstream hosts(hosts_file);
      for (std::string line; std::getline(hosts, line);)
        if (!line.ends_with(workshop_entry))
          other_entries += line + '\n';
    }
    require(!other_entries.empty(), "Read the DNS fixture.");
    std::ofstream(hosts_file, std::ios::trunc) << other_entries;
    // The DNS script enables this case only in a private network namespace.
    require(std::system("ip address add 192.0.2.39/32 dev lo") == 0,
            "Assign the delayed IPv4 address.");
    require(wait_for_host(delayed, http::status::ok, 8s),
            "The server allows the new local DNS address without a restart.");
    require(wait_for_host(discovered, http::status::ok, 1s),
            "A lookup failure retains a name whose previously verified address is still local.");
    std::ofstream(hosts_file, std::ios::app) << "192.0.2.36 " << workshop_entry << '\n';
    require(std::system("ip address del 192.0.2.39/32 dev lo") == 0,
            "Withdraw the delayed IPv4 address.");
    require(wait_for_host(delayed, http::status::forbidden, 8s),
            "The server retires the discovery after the local address is gone.");
    require(wait_for_host(discovered, http::status::forbidden, 1s),
            "A fresh answer pointing away from the machine retires the old discovery.");
  }
  require(get(io, port, http::verb::get, "/feed", host, "http://external.example").result() ==
              http::status::forbidden,
          "A different browser origin cannot subscribe.");
  require(get(io, port, http::verb::post, "/feed", host).result() == http::status::method_not_allowed,
          "HTTP cannot submit commands.");

  // The test drives the same publish-only interface as the panel feed. There is no runtime.
  constexpr std::array records{PanelRecord::hello, PanelRecord::program, PanelRecord::preview,
                               PanelRecord::status, PanelRecord::message};
  const std::array initial{
      std::string(R"({"type":"hello","machine_name":"Sim","macros":["private"],"program_places":["private"],"quit_supported":true})"),
      std::string(R"({"type":"program","lines":["M2"]})"),
      std::string(R"({"type":"preview","context":7})"),
      std::string(R"({"type":"status","seq":1,"preview_context":7})"),
      std::string(R"({"type":"message","text":"First message"})")};
  const auto programs = root / "programs-real";
  const auto program_alias = root / "programs-link";
  std::filesystem::create_directories(programs);
  std::filesystem::remove(program_alias);
  std::filesystem::create_directory_symlink(programs, program_alias);
  const auto job = program_alias / "plate.ngc";
  std::ofstream(job) << "M2";
  std::ofstream(job.parent_path() / "plate.stp") << "ISO-10303-21;";
  // The sample test directories contain ordinary absolute paths.
  const auto part_url = "/program-part?revision=7&program=" + job.string();
  auto part = [&]() { return get(io, port, http::verb::get, part_url.c_str(), host); };
  require(part().result() == http::status::not_found, "No model is available before publication.");
  coffeecnc::Status program_status;
  program_status.link = coffeecnc::Link::live;
  program_status.program.file = job.string();
  program_status.program.revision = 7;
  auto publication = initial;
  publication[1] = Json{{"type", "program"}, {"file", (programs / "plate.ngc").string()},
                        {"lines", {"M2"}}}.dump();
  server.publish(publication, records, program_status, true);
  const auto model = part();
  require(model.result() == http::status::ok && model.body() == "ISO-10303-21;" &&
              model[http::field::content_type] == "model/step",
          "The viewer downloads the STEP file using the raw controller path through a symlink.");
  const auto model_head = get(io, port, http::verb::head, part_url.c_str(), host);
  require(model_head.result() == http::status::ok && model_head.body().empty() &&
              model_head[http::field::content_length] == "13",
          "The viewer can inspect a STEP response without downloading the file.");
  require(get(io, port, http::verb::post, part_url.c_str(), host).result() ==
              http::status::method_not_allowed,
          "The model route is read-only.");
  const auto obsolete = "/program-part?revision=6&program=" + job.string();
  const auto other_part = "/program-part?revision=7&program=" + (job.parent_path() / "other.ngc").string();
  std::ofstream(job.parent_path() / "other.ngc") << "M2";
  std::ofstream(job.parent_path() / "other.step") << "ISO-10303-21;";
  for (const auto& path : {obsolete, other_part})
    require(get(io, port, http::verb::get, path.c_str(), host).result() == http::status::not_found,
            "The model route requires both the published path and revision.");
  std::filesystem::remove(job.parent_path() / "plate.stp");
  const auto sibling = job.parent_path() / "plate.step";
  std::filesystem::remove(sibling);
  std::filesystem::create_symlink(root / "remote-server-private.txt", sibling);
  require(part().result() == http::status::not_found, "The LAN route skips linked STEP files.");
  std::filesystem::remove(sibling);
  std::ofstream(sibling) << 'x';
  std::filesystem::resize_file(sibling, 25uz * 1024 * 1024 + 1);
  require(part().result() == http::status::payload_too_large, "The LAN route caps model size.");
  constexpr auto slow_size = 20uz * 1024 * 1024;
  std::filesystem::resize_file(sibling, slow_size);
  slow_model_download(io, port, host, part_url, slow_size);
  std::ofstream(sibling, std::ios::trunc) << "ISO-10303-21;";
  program_status.link = coffeecnc::Link::stale;
  server.publish({}, {}, program_status, true);
  require(part().result() == http::status::not_found, "Stale controller status withdraws the model.");
  program_status.link = coffeecnc::Link::live;
  server.publish({}, {}, program_status, true);
  const std::array runtime{
      std::string(R"({"type":"result","text":"not a notification"})"),
      std::string(R"({"type":"message","text":"Controller message"})")};
  server.notifications(runtime);
  Viewer first{io}, second{io};
  connect(first, port, alias);
  connect(second, port, host);
  const auto snapshot = read(first);
  require(snapshot == read(second) && snapshot.size() == 6 &&
              snapshot.at(0).at("machine_name") == "Sim" && !snapshot.at(0).contains("macros") &&
              !snapshot.at(0).contains("program_places") && !snapshot.at(0).contains("quit_supported") &&
              snapshot.at(1).at("type") == "program" && snapshot.at(2).at("context") == 7 &&
              snapshot.at(3).at("preview_context") == 7 &&
              snapshot.at(5).at("text") == "Controller message",
          "Both viewers receive the same coherent snapshot and only read-only notifications.");
  if (!ipv6_unavailable) {
    Viewer viewer6{io};
    connect(viewer6, port, host6, ipv6);
    require(read(viewer6) == snapshot, "The IPv6 socket receives the same read-only snapshot.");
  }

  constexpr std::string_view input = R"({"type":"set_machine","state":"on","id":9})";
  first.write(net::buffer(input));
  beast::flat_buffer no_reply;
  boost::system::error_code rejected;
  first.read(no_reply, rejected);
  require(static_cast<bool>(rejected), "An incoming application frame closes the read-only socket.");
  input_frames_close(io, port, host);

  Viewer slow{io};
  connect(slow, port, host);
  slow.next_layer().set_option(net::socket_base::receive_buffer_size(1024));
  const std::array large{
      std::string(R"({"type":"preview","context":8,"payload":")") + std::string(4uz * 1024 * 1024, 'x') + "\"}",
      std::string(R"({"type":"status","seq":2,"preview_context":8})")};
  constexpr std::array update{PanelRecord::preview, PanelRecord::status};
  const auto before = std::chrono::steady_clock::now();
  server.publish(large, update, program_status, false);
  require(part().result() == http::status::not_found, "Closing the program withdraws its model.");
  const auto latest = wait_for(second, "seq", 2);
  require(std::chrono::steady_clock::now() - before < 4s && latest.at(0).at("context") == 8,
          "A fast viewer still gets a coherent update while another viewer cannot read it.");
  Viewer late{io};
  connect(late, port, host);
  const auto current = wait_for(late, "seq", 2);
  for (const auto& record : current)
    require(record.at("type") != "program", "A late viewer does not see a program that was closed.");
}

} // namespace

auto main(int argc, char** argv) -> int {
  try {
    test(argc > 1 ? std::filesystem::path{argv[1]} : std::filesystem::temp_directory_path(),
         argc > 2 ? argv[2] : "", argc > 3 ? argv[3] : "", argc > 4 ? argv[4] : "",
         argc > 5 ? argv[5] : "");
    std::cout << "remote_server: all passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
