#ifndef COFFEECNC_SHELL_PANEL_WINDOW_HPP
#define COFFEECNC_SHELL_PANEL_WINDOW_HPP

#include <coffeecnc/app/session.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace coffeecnc {

struct PanelOptions {
  // The built panel: the directory with index.html.
  std::string ui_directory;
  // A decorated window of window_width x window_height instead of the undecorated full screen.
  bool windowed{};
  // WebKit's inspector can be opened.
  bool developer_tools{};
  int window_width{1280};
  int window_height{800};
};

// What the window tells the application, always on the GTK thread. None may wait for the
// controller.
struct PanelEvents {
  // A new document has committed: Runtime::begin_page.
  std::function<PageToken()> begin_page;
  // The document of this page is gone: replaced, its load failed, its web process ended, or the
  // window closed. Runtime::end_page.
  std::function<void(PageToken)> end_page;
  // One message the page posted, with the timestamp of the frame visible when it was sent.
  std::function<void(PageToken, std::string_view, std::chrono::milliseconds)> message;
  // The current document's animation-frame heartbeat. It is outside the command stream.
  std::function<void(PageToken, std::chrono::milliseconds)> frame;
  // The program LinuxCNC has open: only its STEP model is served (resolve_program_part).
  std::function<std::string()> loaded_program;
};

// Opens the display. False when there is none.
[[nodiscard]] auto initialise_display() -> bool;

// The one window of the application: a GTK 3 window that holds one WebKitGTK web view showing
// the panel from coffeecnc://panel/index.html. The only code that includes GTK or WebKit.
//
// Serving. The scheme `coffeecnc` is answered from the panel directory by resolve_asset
// (panel_assets.hpp): GET only, the file's exact media type, a status of its own for everything
// refused. It is registered as secure and CORS-enabled, so module workers, fetch and
// WebAssembly streaming work from it. Every response carries a Content-Security-Policy that
// allows the panel's own origin and nothing on any network.
//
// Page generations come from WebKit's load events, on the GTK thread:
//
//   load-changed, committed       end_page for the document that was showing, then begin_page
//   load-failed                   end_page, and the panel is loaded again a moment later. Not
//                                 for a navigation the policy refused or that was cancelled:
//                                 the document that shows stays
//   web-process-terminated        end_page, and the panel is loaded again a moment later
//   the window is destroyed       end_page
//
// To the page. post() may be called on any thread; delivery happens on the GTK thread, where
// the token is compared with the page that shows at that moment, and messages of another page
// are dropped. The script that delivers them also carries the identity the document gave
// itself when it began (a document-start script; see panel_window.cpp), so that a delivery
// already on its way to the web process when a new document commits there finds another
// identity and does nothing. Messages reach the page as objects: window.coffeecncReceive(m).
//
// From the page. The script message handler `coffeecnc` exists before any document loads. A
// message is accepted as a string only, and only from the document that shows: the
// document-start script makes every message leave the page with its document's identity in
// front, and a message with another identity, or none, is dropped before the session sees it.
// So a message of a replaced document is never taken for the present page's, whenever it
// arrives. The rest is handed on with the token of the page that shows.
//
// Refused: context menu, back and forward gestures, new windows, every permission request, and
// navigation anywhere but the panel's origin.
//
// SIGINT, SIGTERM and SIGHUP close the window while run() is in progress.
//
// Every member but post() and close_after_posts() is called on the thread that called initialise_display.
class PanelWindow {
public:
  PanelWindow(PanelOptions options, PanelEvents events);
  ~PanelWindow();
  PanelWindow(const PanelWindow&) = delete;
  auto operator=(const PanelWindow&) -> PanelWindow& = delete;

  // Shows the window, loads the panel and runs the GTK main loop until the window is closed.
  void run();
  // Closes the window; run() returns.
  void close();
  // Any thread, never blocks: messages for the page with this token, in order.
  void post(PageToken page, std::vector<std::string> messages);
  // Any thread, never blocks: closes only this page, after WebKit finishes delivering
  // posted messages. A reload or failed delivery cancels this close request.
  void close_after_posts(PageToken page);

  struct Impl;
  // For the test hooks (shell_test_hooks.hpp) only.
  [[nodiscard]] auto internals() -> Impl& { return *impl_; }

private:
  std::unique_ptr<Impl> impl_;
};

} // namespace coffeecnc

#endif // COFFEECNC_SHELL_PANEL_WINDOW_HPP
