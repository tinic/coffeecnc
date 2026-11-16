#ifndef COFFEECNC_SHELL_SRC_PANEL_WINDOW_IMPL_HPP
#define COFFEECNC_SHELL_SRC_PANEL_WINDOW_IMPL_HPP

// The window's state, for panel_window.cpp and the test hooks. Not a public header: it needs
// GTK's and WebKit's.

#include <coffeecnc/shell/panel_window.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

namespace coffeecnc {

// What other threads hand to the GTK thread: messages for the page and, for the test hooks,
// work to do there. Everything in it is touched under its mutex only, by whatever thread. The
// GTK main loop is used to wake the GTK thread and for nothing else: the wake-up carries no
// data, the GTK thread finds this object itself and takes what waits under the mutex. So the
// order between the thread that hands something over and the GTK thread that takes it is made
// by this mutex, not by GLib's own locking, which a sanitizer cannot see into.
struct Outbox {
  std::mutex mutex;
  std::vector<std::pair<PageToken, std::vector<std::string>>> waiting;
  std::vector<std::function<void()>> work;
  // A wake-up of the GTK thread is on its way.
  bool scheduled{};
  // The window is gone: nothing is taken any more.
  bool closed{};
  // Close the window once the messages posted before have been delivered.
  bool close_requested{};
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
  std::uint64_t rejected{};  // messages from a document that is not the one that shows
  // The document that shows found the `coffeecnc` handler when its first script ran.
  std::optional<bool> handler_at_start;

  // One message as the `coffeecnc` handler delivers it.
  void receive(std::string_view raw);
  void begin_page();
  void end_page();
  void reload_later();
  void deliver(PageToken token, const std::vector<std::string>& messages);
  // Any thread: the pieces of `work` run on the GTK thread, in the main loop, in this order
  // and in one drain: they are handed over under one hold of the outbox's mutex. False, and
  // nothing is kept, when the window is closed. Work that still waits when the window closes
  // (also work behind the piece that closes it) is dropped without being run. For the test
  // hooks.
  [[nodiscard]] auto on_gtk_thread(std::vector<std::function<void()>> work) -> bool;
  // GTK thread: takes what waits in the outbox.
  void drain();
};

} // namespace coffeecnc

#endif // COFFEECNC_SHELL_SRC_PANEL_WINDOW_IMPL_HPP
