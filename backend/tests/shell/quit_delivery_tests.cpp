// WebKit completion is asynchronous: a queued quit must not destroy its receiver early.
#include "panel_window_impl.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace coffeecnc;

namespace {
auto scenario(const std::filesystem::path& directory, bool broken, bool wrong_page) -> bool {
  std::filesystem::create_directories(directory);
  {
    std::ofstream html{directory / "index.html"};
    html << "<html><body><script src=\"test.js\"></script></body></html>";
    std::ofstream script{directory / "test.js"};
    script << "window.coffeecncReceive = (m) => {"
         << (broken ? "throw new Error('test receiver failure');" :
             "const until=performance.now()+100;while(performance.now()<until){};"
             "window.webkit.messageHandlers.coffeecnc.postMessage('received');")
         << "};window.webkit.messageHandlers.coffeecnc.postMessage('ready');";
  }
  bool received = false;
  bool timed_out = false;
  PanelWindow* active = nullptr;
  PanelWindow window({.ui_directory = directory.string(), .windowed = true}, {
    .begin_page = []() -> PageToken { return {1}; },
    .end_page = {},
    .message = [&](PageToken page, std::string_view text) -> void {
      if (text == "received") received = true;
      if (text == "ready") {
        active->post(page, {R"({"type":"result","id":27,"outcome":"accepted","reason":""})"});
        active->close_after_posts(wrong_page ? PageToken{2} : page);
      }
    },
  });
  active = &window;
  struct Timeout { PanelWindow& window; bool& expired; } timeout{window, timed_out};
  const guint timer = g_timeout_add(1500, [](gpointer data) -> gboolean {
    auto& state = *static_cast<Timeout*>(data);
    state.expired = true;
    state.window.close();
    return G_SOURCE_REMOVE;
  }, &timeout);
  window.run();
  if (!timed_out) g_source_remove(timer);
  const auto delivered = window.internals().delivered;
  const bool passed = broken ? timed_out && delivered == 0 && !received
                    : wrong_page ? timed_out && delivered == 1 && received
                                 : !timed_out && delivered == 1 && received;
  std::cout << "check=quit_delivery_" << (broken ? "failure" : wrong_page ? "page" : "completion")
            << " result=" << (passed ? "pass" : "fail") << " delivered=" << delivered
            << " received=" << received << " timeout=" << timed_out << '\n';
  return passed;
}
} // namespace

auto main(int argc, char** argv) -> int {
  if (argc != 2 || !initialise_display()) return 1;
  const std::filesystem::path directory{argv[1]};
  bool passed = scenario(directory, false, false);
  passed = scenario(directory, true, false) && passed;
  passed = scenario(directory, false, true) && passed;
  return passed ? 0 : 1;
}
