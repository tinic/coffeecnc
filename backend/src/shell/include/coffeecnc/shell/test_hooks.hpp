#ifndef COFFEECNC_SHELL_TEST_HOOKS_HPP
#define COFFEECNC_SHELL_TEST_HOOKS_HPP

#include <coffeecnc/shell/panel_window.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace coffeecnc {

// What a test does to the window that the application never does: run script in the page,
// press the mouse, take pictures, answer the file chooser. In a library of its own
// (coffeecnc_shell_test_hooks) that only tests link; the `coffeecnc` executable has none of it.
//
// install() is called on the GTK thread before PanelWindow::run. Every other member may be
// called from any thread but the GTK thread: it hands its work to that thread and waits for it,
// so the GTK main loop must be running.
class ShellTestHooks {
public:
  struct State {
    std::uint64_t page{};    // the token of the document that shows; 0 for none
    std::uint64_t commits{}; // documents committed so far
    bool identified{};       // the document's first script has reported to the shell
    // The `coffeecnc` handler was there when that script ran; nothing before it has reported.
    std::optional<bool> handler_at_start;
    std::uint64_t delivered{};
    std::uint64_t dropped{};
    std::string uri;
    int width{};
    int height{};
    // Every resource the view began to load, in order, and those not on the panel's origin
    // (data: and blob: URLs aside).
    std::vector<std::string> resources;
    std::vector<std::string> foreign_resources;
    std::uint64_t process_terminations{};
    std::uint64_t file_choosers{}; // requests of the page for a file chooser
  };

  // Records resource loads and web process terminations, writes the page's console to stdout,
  // answers file chooser requests itself (no dialog opens), and adds `script` to every
  // document, run before the page's own scripts.
  static auto install(PanelWindow& window, const std::string& document_start_script)
      -> std::unique_ptr<ShellTestHooks>;
  ~ShellTestHooks();
  ShellTestHooks(const ShellTestHooks&) = delete;
  auto operator=(const ShellTestHooks&) -> ShellTestHooks& = delete;

  [[nodiscard]] auto state() -> State;
  // Runs `body` as the body of an async function in the page and waits for what it returns,
  // which comes back as JSON text (`null` for nothing). The error text when it throws.
  [[nodiscard]] auto evaluate(const std::string& body,
                              std::chrono::milliseconds timeout = std::chrono::seconds{10})
      -> std::expected<std::string, std::string>;
  // A PNG of what the view shows.
  [[nodiscard]] auto snapshot(const std::string& file) -> std::expected<void, std::string>;
  // The left mouse button at a point of the view, as GDK events the view handles like the
  // operator's own.
  void mouse_move(double x, double y);
  void mouse_press(double x, double y);
  void mouse_release(double x, double y);
  // The files the next file chooser request is answered with; none cancels it.
  void answer_file_chooser(std::vector<std::string> files);
  void reload();
  void load(const std::string& uri);
  void close();
  // What WebKit says about its graphics (the text of its webkit://gpu page), from a view of
  // the test's own beside the panel: the panel's window never navigates there.
  [[nodiscard]] auto graphics_report() -> std::expected<std::string, std::string>;
  // PanelWindow::post from the calling thread.
  void post(std::uint64_t page, std::vector<std::string> messages);

  struct Impl;

private:
  explicit ShellTestHooks(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

} // namespace coffeecnc

#endif // COFFEECNC_SHELL_TEST_HOOKS_HPP
