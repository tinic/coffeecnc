#ifndef COFFEECNC_LINUXCNC_NML_PORT_HPP
#define COFFEECNC_LINUXCNC_NML_PORT_HPP

#include <coffeecnc/core/command_policy.hpp>
#include <coffeecnc/core/controller_port.hpp>
#include <coffeecnc/linuxcnc/hal_control.hpp>
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
  std::array<double, 3> work{}; // machine position less G5x, G92 and tool offset, rotated
  std::array<double, 3> dtg{};
  std::array<double, 3> g5x{};
  std::array<double, 3> g92{};
  std::array<double, 3> tool_offset{};
  double rotation_xy{}; // degrees
  double velocity{};    // mm/min
  double feed_override{};
  double rapid_override{};
  double spindle_rpm{}; // commanded, before override
  double spindle_override{};
  // From the HAL component's optional input pins. Nothing when the machine does not connect the
  // pin ([COFFEECNC] ..._SIGNAL in the INI file) or there is no HAL component: never a zero that
  // was not measured.
  std::optional<double> spindle_actual_rpm; // magnitude
  std::optional<bool> spindle_at_speed;
  std::optional<bool> probe_input;
  double dwell_seconds_left{};
  std::string program_file; // empty when none is loaded
  // Tool table index (not a tool number) prepared by a T word and not yet changed to.
  std::optional<int> prepared_pocket;
  int wcs{}; // 1 is G54 ... 9 is G59.3
  int current_line{};
  int motion_line{};
  int queued_mdi{};
  SpindleDirection spindle_direction{SpindleDirection::stopped};
  // Motion is in teleop (world) mode: axis jogs are possible, joint jogs and homing are not.
  bool teleop{};
  bool flood{};
  bool mist{};
  bool optional_stop{};
  bool block_delete{};
  bool paused{};
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
// Typical times on a 1 ms task cycle are 2 to 30 ms.
//
// NOT thread-safe: one thread, the runtime's worker, calls every member. LinuxCNC's NML library
// keeps a list of all channels of the process without a lock (libnml/nml/nml.cc,
// NML_Main_Channel_List) and each channel has one read buffer, so: connect and destroy every NML
// object (NmlPort, NmlStatusReader) on one thread while no other thread is inside an NML call,
// and use each object from one thread only. Every read of a buffer takes that buffer's System V
// semaphore with the 10 s timeout of linuxcnc.nml (libnml/buffer/shmem.cc): a task stopped
// inside its own write blocks every reader that long. emcCommand is a queue: task executes
// commands in the order they were written, also those written while it was stopped.
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
  [[nodiscard]] static auto connect(const MachineConfig& config, HalShared* hal)
      -> std::expected<std::unique_ptr<NmlPort>, std::string>;
  ~NmlPort() override;
  NmlPort(const NmlPort&) = delete;
  auto operator=(const NmlPort&) -> NmlPort& = delete;

  [[nodiscard]] auto snapshot() -> ControllerState override;
  // The same reading with everything else the panel shows.
  [[nodiscard]] auto status() -> NmlStatus;
  // Also the regular status reading that keeps `link` meaningful between actions: call it at
  // least a few times per second.
  [[nodiscard]] auto take_messages() -> std::vector<ControllerMessage> override;

  [[nodiscard]] auto set_machine_state(MachineState state) -> ActionResult override;
  [[nodiscard]] auto set_task_mode(TaskMode mode) -> ActionResult override;
  // Homing all axes fails, with nothing sent, unless MachineConfig::home_all_configured.
  [[nodiscard]] auto home(std::optional<Axis> axis) -> ActionResult override;
  [[nodiscard]] auto start_jog(Axis axis, int direction, double velocity, JogSpace space)
      -> ActionResult override;
  // Sends nothing while the machine is observed off or outside manual mode, provided task has
  // echoed the last command this port wrote: task refuses jog commands then and no jog can be
  // running. A stop for the same axis that task has not echoed yet is not written again: the
  // call waits for that one.
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
  // Through the HAL component, not NML: asks the control thread to raise tool-changed.
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
