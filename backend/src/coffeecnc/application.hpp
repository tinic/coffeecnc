#ifndef COFFEECNC_APPLICATION_HPP
#define COFFEECNC_APPLICATION_HPP

#include <coffeecnc/linuxcnc/machine_config.hpp>
#include <coffeecnc/shell/panel_window.hpp>

#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace coffeecnc {

struct Options {
  std::string ini_file;
  std::optional<std::string> ui_directory;
  bool windowed{};
  bool developer_tools{};
  bool help{};
  bool check{}; // --check <file>: report on the INI file and exit; no window, no LinuxCNC
};

// `coffeecnc --check <ini>`: loads the machine's INI file as start-up does and tells, on standard
// output, whether CoffeeCNC starts with it and which optional functions are on or off and why.
// Exit status 0 when it starts, 1 when it does not. Reads files only; LinuxCNC need not run.
[[nodiscard]] auto check_configuration(const Options& options) -> int;

// The command line after the program name. LinuxCNC starts its DISPLAY program as
// `<program> -ini <file>`. Fails with a sentence for the terminal.
[[nodiscard]] auto parse_options(std::span<const std::string_view> arguments)
    -> std::expected<Options, std::string>;
[[nodiscard]] auto usage() -> std::string_view;

// For tests that drive the application from inside. The `coffeecnc` executable passes none.
struct ApplicationHooks {
  // On the GTK thread, with everything running and the window made but not yet shown or
  // loaded.
  std::function<void(PanelWindow&, const MachineConfig&)> before_load;
};

// The application from start to end; the exit code. Start-up, each step only when the one
// before it succeeded, each failure said on stderr as `coffeecnc: <reason>`, exit code 1:
//
//   1  read the INI file (load_machine_config): an unsupported machine ends here
//   2  find the panel (see panel_directory)
//   3  open the display
//   4  create the HAL component `coffeecnc`
//   5  run [HAL] POSTGUI_HALFILE through `halcmd -i <ini> -f <file>`, in the INI's directory
//   6  connect to LinuxCNC: command and error channel, and the status thread's status channel
//   7  start the runtime (control, status and worker threads) and the publisher thread
//   8  make the window, show it, load the panel
//
// Until step 7 there is one thread. A failure at or before step 6 leaves no thread; the HAL
// component is removed again (hal_exit) by its destructor. What step 5 loaded into LinuxCNC's
// realtime side stays until LinuxCNC shuts down, which it does when its DISPLAY program exits.
//
// It ends when the window closes: by the operator, or by SIGINT, SIGTERM or SIGHUP. Then, in
// this order: the last page ends (jogs stop), the publisher thread ends, the runtime shuts down
// safely (the jog-stop level high, written last by the control thread; see Runtime), the
// channels close, the HAL component is removed. The exit code is 0; LinuxCNC then shuts down.
[[nodiscard]] auto run_application(const Options& options, const ApplicationHooks& hooks = {})
    -> int;

// Settings of the process WebKit reads from the environment, each only when the environment
// does not say otherwise. Call before any thread starts.
//
//   WEBKIT_DISABLE_DMABUF_RENDERER=1   WebKit's DMA-BUF renderer needs a GPU buffer path the
//                                      panel's hosts do not all have (none under Xvfb or over
//                                      remote desktop); the older path renders everywhere
void configure_environment();

// The built panel: `--ui`, else `<prefix>/share/coffeecnc/ui`, where `<prefix>/bin` is the
// directory of the running executable (/proc/self/exe), whatever the prefix is; else, only in a
// build with COFFEECNC_DEVELOPMENT_PATHS, the source tree's `ui/dist` this build was made in.
// Fails when the directory found has no index.html.
[[nodiscard]] auto panel_directory(const Options& options) -> std::expected<std::string, std::string>;

} // namespace coffeecnc

#endif // COFFEECNC_APPLICATION_HPP
