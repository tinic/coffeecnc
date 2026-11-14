#ifndef COFFEECNC_LINUXCNC_NML_PORT_HPP
#define COFFEECNC_LINUXCNC_NML_PORT_HPP

#include <coffeecnc/core/command_policy.hpp>
#include <coffeecnc/core/controller_port.hpp>
#include <coffeecnc/linuxcnc/hal_component.hpp>
#include <coffeecnc/linuxcnc/machine_config.hpp>

#include <array>
#include <chrono>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace coffeecnc {

// Everything LinuxCNC's task reports that the panel's status is made of, in protocol units:
// millimetres, mm/min, rpm, factors. Read from one EMC_STAT.
struct NmlStatus {
  ControllerState controller;
  // Motion is in teleop (world) mode: axis jogs are possible, joint jogs and homing are not.
  bool teleop{};
  std::array<double, 3> work{}; // machine position less G5x, G92 and tool offset, rotated
  std::array<double, 3> dtg{};
  int wcs{}; // 1 is G54 ... 9 is G59.3
  std::array<double, 3> g5x{};
  std::array<double, 3> g92{};
  std::array<double, 3> tool_offset{};
  double rotation_xy{}; // degrees
  double velocity{};    // mm/min
  double feed_override{};
  double rapid_override{};
  SpindleDirection spindle_direction{SpindleDirection::stopped};
  double spindle_rpm{}; // commanded, before override
  double spindle_override{};
  bool flood{};
  bool mist{};
  // Tool table index (not a tool number) prepared by a T word and not yet changed to.
  std::optional<int> prepared_pocket;
  std::string program_file; // empty when none is loaded
  int current_line{};
  int motion_line{};
  bool optional_stop{};
  bool block_delete{};
  bool paused{};
  double dwell_seconds_left{};
  int queued_mdi{};
};

// An operator error or message LinuxCNC published.
struct ControllerMessage {
  bool error{};
  std::string text;
};

// LinuxCNC's task controller over its NML channels: the one writer of commands, the reader of
// status, and the ONLY reader of the error channel (reading consumes; no second GUI may share the
// machine).
//
// Every action writes its NML message and waits for task to echo it, then for what the action's
// result needs: completion (state, mode, teleop, stop, abort, overrides, spindle, coolant, tool
// table), a short window for errors (jogs, homing, step) or the start of execution (MDI, run).
// When task rejects a command, its operator error text becomes the reason and is not reported a
// second time by take_messages().
//
// Blocking: a single NML command waits at most echo_timeout + completion_timeout. Actions that
// must first switch teleop (home, jogs) or send two commands (coolant) wait at most twice that.
// Typical times on a 1 ms task cycle are 2 to 30 ms. NOT thread-safe.
class NmlPort final : public ControllerPort {
public:
  static constexpr auto echo_timeout = std::chrono::milliseconds{1000};
  static constexpr auto completion_timeout = std::chrono::milliseconds{3000};
  // How long an accepted jog, home or step is watched for an operator error.
  static constexpr auto error_window = std::chrono::milliseconds{30};
  // How long an MDI block or program start is watched for an error before it counts as running.
  static constexpr auto start_window = std::chrono::milliseconds{50};
  // How long a state that follows completion (machine state, mode, pause) may take to show.
  static constexpr auto settle_timeout = std::chrono::milliseconds{1000};

  // Opens the channels named by the configuration's NML file. `hal` may be null: there is then
  // no manual tool change. It must outlive the port.
  [[nodiscard]] static auto connect(const MachineConfig& config, HalComponent* hal)
      -> std::expected<std::unique_ptr<NmlPort>, std::string>;
  ~NmlPort() override;
  NmlPort(const NmlPort&) = delete;
  auto operator=(const NmlPort&) -> NmlPort& = delete;

  [[nodiscard]] auto snapshot() -> ControllerState override;
  // The same reading with everything else the panel shows.
  [[nodiscard]] auto status() -> NmlStatus;
  // Operator messages published since the last call, oldest first. Call it every control cycle:
  // it is also the regular status reading that keeps `link` meaningful between actions.
  [[nodiscard]] auto take_messages() -> std::vector<ControllerMessage>;

  [[nodiscard]] auto set_machine_state(MachineState state) -> ActionResult override;
  [[nodiscard]] auto set_task_mode(TaskMode mode) -> ActionResult override;
  [[nodiscard]] auto home(std::optional<Axis> axis) -> ActionResult override;
  [[nodiscard]] auto start_jog(Axis axis, int direction, double velocity, JogSpace space)
      -> ActionResult override;
  // Sends nothing while the machine is observed off or outside manual mode: task refuses jog
  // commands then and no jog can be running.
  [[nodiscard]] auto stop_jog(Axis axis) -> ActionResult override;
  [[nodiscard]] auto jog_increment(Axis axis, double distance, double velocity, JogSpace space)
      -> ActionResult override;
  [[nodiscard]] auto mdi(std::string_view text) -> ActionResult override;
  [[nodiscard]] auto open_program(std::string_view path) -> ActionResult override;
  [[nodiscard]] auto run() -> ActionResult override;
  [[nodiscard]] auto pause() -> ActionResult override;
  [[nodiscard]] auto resume() -> ActionResult override;
  [[nodiscard]] auto step() -> ActionResult override;
  [[nodiscard]] auto abort() -> ActionResult override;
  [[nodiscard]] auto set_override(Override which, double value) -> ActionResult override;
  [[nodiscard]] auto set_spindle(SpindleDirection direction, double rpm) -> ActionResult override;
  [[nodiscard]] auto set_coolant(bool flood, bool mist) -> ActionResult override;
  [[nodiscard]] auto set_program_option(ProgramOption option, bool on) -> ActionResult override;
  // Through the HAL component, not NML.
  [[nodiscard]] auto tool_changed() -> ActionResult override;
  // Rewrite [EMCIO] TOOL_TABLE and have LinuxCNC load it again.
  [[nodiscard]] auto set_tool(const Tool& tool) -> ActionResult override;
  [[nodiscard]] auto remove_tool(int number) -> ActionResult override;

private:
  struct Impl;
  explicit NmlPort(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

} // namespace coffeecnc

#endif // COFFEECNC_LINUXCNC_NML_PORT_HPP
