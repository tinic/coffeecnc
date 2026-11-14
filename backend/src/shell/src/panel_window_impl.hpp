#ifndef COFFEECNC_SHELL_SRC_PANEL_WINDOW_IMPL_HPP
#define COFFEECNC_SHELL_SRC_PANEL_WINDOW_IMPL_HPP

// The window's state, for panel_window.cpp and the test hooks. Not a public header: it needs
// GTK's and WebKit's.

#include <coffeecnc/shell/panel_window.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

namespace coffeecnc {

// Messages on their way from other threads to the GTK thread.
struct Outbox {
  std::mutex mutex;
  std::vector<std::pair<PageToken, std::vector<std::string>>> waiting;
  // A call on the GTK thread is on its way to take what waits.
  bool scheduled{};
  // The window is gone: nothing is taken any more.
  bool closed{};
  // GTK thread only.
  PanelWindow::Impl* window{};
};

struct PanelWindow::Impl {
  PanelOptions options;
  PanelEvents events;
  WebKitWebContext* context{};
  GtkWidget* window{};
  WebKitWebView* view{};
  std::shared_ptr<Outbox> outbox{std::make_shared<Outbox>()};
  // The document that shows, once it has committed.
  std::optional<PageToken> page;
  // The identity that document gave itself; empty until it has said so.
  std::string identity;
  std::array<guint, 3> signal_sources{};
  guint reload_source{};
  bool running{};

  // What tests ask about.
  std::uint64_t commits{};
  std::uint64_t delivered{}; // messages evaluated in a page
  std::uint64_t dropped{};   // messages of a page that no longer showed
  // The document that shows found the `coffeecnc` handler when its first script ran.
  std::optional<bool> handler_at_start;

  void begin_page();
  void end_page();
  void reload_later();
  void deliver(PageToken token, const std::vector<std::string>& messages);
};

} // namespace coffeecnc

#endif // COFFEECNC_SHELL_SRC_PANEL_WINDOW_IMPL_HPP
