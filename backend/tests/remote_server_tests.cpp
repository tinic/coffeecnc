#include "remote_server.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <unistd.h>

namespace {
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using net::ip::tcp;
using Json = nlohmann::json;
using coffeecnc::PanelRecord;
using coffeecnc::RemoteServer;
using namespace std::chrono_literals;

void require(bool condition, const char* why) {
  if (!condition)
    throw std::runtime_error(why);
}

auto get(net::io_context& io, std::uint16_t port, http::verb method, const char* path,
         const std::string& host, const std::string& origin = {}) -> http::response<http::string_body> {
  tcp::socket tcp_socket{io};
  tcp_socket.connect({net::ip::address_v4::loopback(), port});
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

void connect(Viewer& viewer, std::uint16_t port, const std::string& host) {
  viewer.next_layer().connect({net::ip::address_v4::loopback(), port});
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
    for (const auto& record : batch)
      if (record.value(key, -1) == sequence)
        return batch;
  }
  throw std::runtime_error("The current status did not arrive.");
}

void test(const std::filesystem::path& root, std::string_view discovered,
          std::string_view nonlocal) {
  const auto assets = root / "remote-server-assets";
  std::filesystem::create_directories(assets);
  std::ofstream(assets / "index.html") << "<title>Remote test panel</title>";
  std::ofstream(root / "remote-server-private.txt") << "outside the panel";
  const std::array aliases{std::string{"shop.mill.example"}, std::string{"Friendly.Example."}};
  RemoteServer server{assets.string(), 0, aliases};
  // Only the test constructs a server with port zero, to get a free TCP port.
  const auto port = server.port();
  const auto suffix = ":" + std::to_string(port);
  const auto host = "127.0.0.1" + suffix;
  const auto alias = aliases.front() + suffix;
  net::io_context io;
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

  // A DNS fixture can supply two search results, one local and one on a different machine.
  if (!discovered.empty()) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    auto status = http::status::forbidden;
    do {
      status = get(io, port, http::verb::get, "/view", std::string(discovered) + suffix).result();
      if (status != http::status::ok)
        std::this_thread::sleep_for(30ms);
    } while (status != http::status::ok && std::chrono::steady_clock::now() < deadline);
    require(status == http::status::ok, "A DNS search name that resolves locally is allowed.");
    require(get(io, port, http::verb::get, "/view", std::string(nonlocal) + suffix).result() ==
                http::status::forbidden,
            "A DNS search name that resolves off-machine is rejected.");
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
  server.publish(initial, records, true);
  const std::array runtime{
      std::string(R"({"type":"result","text":"not a notification"})"),
      std::string(R"({"type":"message","text":"Controller message"})")};
  server.notifications(runtime);
  Viewer first{io}, second{io};
  connect(first, port, discovered.empty() ? alias : std::string(discovered) + suffix);
  connect(second, port, host);
  const auto snapshot = read(first);
  require(snapshot == read(second) && snapshot.size() == 6 &&
              snapshot.at(0).at("machine_name") == "Sim" && !snapshot.at(0).contains("macros") &&
              !snapshot.at(0).contains("program_places") && !snapshot.at(0).contains("quit_supported") &&
              snapshot.at(1).at("type") == "program" && snapshot.at(2).at("context") == 7 &&
              snapshot.at(3).at("preview_context") == 7 &&
              snapshot.at(5).at("text") == "Controller message",
          "Both viewers receive the same coherent snapshot and only read-only notifications.");

  constexpr std::string_view input = R"({"type":"set_machine","state":"on","id":9})";
  first.write(net::buffer(input));
  beast::flat_buffer no_reply;
  boost::system::error_code rejected;
  first.read(no_reply, rejected);
  require(static_cast<bool>(rejected), "An incoming application frame closes the read-only socket.");

  Viewer slow{io};
  connect(slow, port, host);
  slow.next_layer().set_option(net::socket_base::receive_buffer_size(1024));
  const std::array large{
      std::string(R"({"type":"preview","context":8,"payload":")") + std::string(4 * 1024 * 1024, 'x') + "\"}",
      std::string(R"({"type":"status","seq":2,"preview_context":8})")};
  constexpr std::array update{PanelRecord::preview, PanelRecord::status};
  const auto before = std::chrono::steady_clock::now();
  server.publish(large, update, false);
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
         argc == 4 ? argv[2] : "", argc == 4 ? argv[3] : "");
    std::cout << "remote_server: all passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
