#ifndef COFFEECNC_LINUXCNC_HAL_DIAGNOSTICS_HPP
#define COFFEECNC_LINUXCNC_HAL_DIAGNOSTICS_HPP

#include <coffeecnc/core/diagnostics.hpp>
#include <coffeecnc/linuxcnc/machine_config.hpp>

#include <array>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <sys/types.h>

namespace coffeecnc {

// The rows of the diagnostics view for a machine, in order: per joint the minimum and maximum
// limit switch, the home switch and the drive fault; the E-stop chain and LinuxCNC's own enable;
// the probe input; the spindle; then each [COFFEECNC] DIAGNOSTIC entry. Pure.
[[nodiscard]] auto diagnostic_inputs(const MachineConfig& config) -> std::vector<DiagnosticInput>;

// What HAL holds under one name. A pin always has a value: a pin linked to no signal reads its
// own, which `setp` sets.
struct PinReading {
  DiagnosticValue value;
  bool linked{};
};
struct SignalReading {
  DiagnosticValue value;
  bool driven{}; // a pin writes the signal
};

// Read-only lookups in HAL by name. Neither creates, links nor sets anything.
class HalLookup {
public:
  HalLookup() = default;
  virtual ~HalLookup() = default;
  [[nodiscard]] virtual auto pin(const std::string& name) -> std::optional<PinReading> = 0;
  [[nodiscard]] virtual auto signal(const std::string& name) -> std::optional<SignalReading> = 0;

protected:
  HalLookup(const HalLookup&) = default;
  HalLookup(HalLookup&&) = default;
  auto operator=(const HalLookup&) -> HalLookup& = default;
  auto operator=(HalLookup&&) -> HalLookup& = default;
};

// LinuxCNC's own lookups (hal_get_pin_value_by_name, hal_get_signal_value_by_name): each takes
// HAL's mutex for the lookup, and the value is read with one atomic load. Needs a HAL component
// of this process (hal_init) for as long as it is used. Any thread; not the control thread.
class HalApiLookup final : public HalLookup {
public:
  [[nodiscard]] auto pin(const std::string& name) -> std::optional<PinReading> override;
  [[nodiscard]] auto signal(const std::string& name) -> std::optional<SignalReading> override;
};

// The signal each linked pin is on, from `halcmd -s show sig`: one line per signal, `type value
// name`, then pairs of an arrow (<==, ==>, <=>) and a pin. HAL's C API tells whether a pin is
// linked, not to which signal; without the signal a pin on a signal nothing writes would read
// as a switch that is open. Lines without that form are skipped.
using PinSignals = std::map<std::string, std::string, std::less<>>;
[[nodiscard]] auto pin_signals(std::string_view shown) -> PinSignals;

// One reading of every row. A configured name is looked up as a signal first, then as a pin.
// A linked pin whose signal `wiring` names is `undriven` while nothing writes that signal, else
// `live`; a linked pin `wiring` does not know is `live`.
[[nodiscard]] auto read_diagnostics(std::span<const DiagnosticInput> inputs, HalLookup& hal,
                                    const PinSignals& wiring) -> std::vector<DiagnosticReading>;

// Starts halshow, halmeter and halscope, at most one of each, as programs of their own: in a
// session of their own (setsid), standard input from /dev/null, no descriptor of this process
// but standard output and error, and the environment given here (that of LinuxCNC's session,
// which CoffeeCNC was started with). It never waits for them. A tool that has ended is noticed
// by waitpid on its own process, never on any other child. A tool still open when this is
// destroyed is sent SIGTERM: it was opened for the session that ends with CoffeeCNC.
// Thread-safe.
class HalToolLauncher {
public:
  // `search_path` is where the programs are looked for, as in PATH.
  HalToolLauncher(std::string search_path, std::vector<std::string> environment);
  HalToolLauncher(const HalToolLauncher&) = delete;
  HalToolLauncher(HalToolLauncher&&) = delete;
  auto operator=(const HalToolLauncher&) -> HalToolLauncher& = delete;
  auto operator=(HalToolLauncher&&) -> HalToolLauncher& = delete;
  ~HalToolLauncher();

  // A reason for the operator when it is not started: not installed, already open, or the start
  // failed.
  [[nodiscard]] auto launch(HalTool tool) -> std::optional<std::string>;
  // Each tool: installed, and open. Notes the tools that have ended.
  [[nodiscard]] auto states() -> std::vector<HalToolState>;
  // The program of a tool: the first executable regular file of that name in the search path.
  [[nodiscard]] auto program(HalTool tool) const -> std::optional<std::string>;

private:
  void reap(); // mutex_ held

  std::string search_path_;
  std::vector<std::string> environment_;
  std::mutex mutex_;
  std::array<pid_t, hal_tools.size()> children_{};
};

// The environment for the HAL tools: this process's, with INI_FILE_NAME set to `ini_file`
// where LinuxCNC's start script has not set it. Call before another thread changes the
// environment.
[[nodiscard]] auto hal_tool_environment(const std::string& ini_file) -> std::vector<std::string>;

} // namespace coffeecnc

#endif // COFFEECNC_LINUXCNC_HAL_DIAGNOSTICS_HPP
