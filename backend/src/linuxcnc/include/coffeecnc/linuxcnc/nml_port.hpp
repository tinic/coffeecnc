#ifndef COFFEECNC_LINUXCNC_NML_PORT_HPP
#define COFFEECNC_LINUXCNC_NML_PORT_HPP

#include <coffeecnc/core/command_policy.hpp>
#include <coffeecnc/core/controller_port.hpp>
#include <coffeecnc/core/panel_messages.hpp>
#include <coffeecnc/linuxcnc/hal_control.hpp>
#include <coffeecnc/linuxcnc/machine_config.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace coffeecnc {

// Everything LinuxCNC's task reports that the panel's status is made of, in protocol units:
// millimetres, mm/min, rpm, factors. Read from one EMC_STAT.
struct NmlStatus {
  ControllerState controller;
  Vec3 commanded_position{}; // trajectory's controlled point in machine coordinates, mm
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
  // The F word in effect, in the unit of feed_mode: mm/min, 1/min or mm/rev.
  double programmed_feed{};
  FeedMode feed_mode{FeedMode::per_minute};
  // Active modal codes in the order task reports them, G codes first: "G1", "G59.1", "M5".
  std::vector<std::string> active_codes;
  std::string program_file; // empty when none is loaded
  // Tool table index (not a tool number) prepared by a T word and not yet changed to.
  std::optional<int> prepared_pocket;
  int wcs{}; // 1 is G54 ... 9 is G59.3
  int current_line{};
  int motion_line{};
  int queued_mdi{};
  SpindleDirection spindle_direction{SpindleDirection::stopped};
  // The serial number of the last command task has taken up, and whether task reports that
  // command done (neither executing nor failed).
  int echo_serial{};
  bool command_done{};
  // Task has something in execution: its execution state is not "done" (it waits for motion,
  // for io, for a delay) or its overall status is "executing". A tool table load sent through
  // task by any client keeps this set until io has finished it, whatever command task took up
  // after it: the table in LinuxCNC's tool data memory may be half replaced meanwhile.
  bool task_executing{};
  bool task_failed{};
  // Motion is in teleop (world) mode: axis jogs are possible, joint jogs and homing are not.
  bool teleop{};
  bool flood{};
  bool mist{};
  bool optional_stop{};
  bool block_delete{};
  bool paused{};
};

// What LinuxCNC has loaded at this port's request, counted at the moment LinuxCNC accepted it.
// The publisher reads the counts: a load of the same file again is a load. Thread-safe.
struct NmlLoads {
  std::atomic<std::uint64_t> programs{};    // open_program accepted
  // An MDI block accepted (mdi, select_wcs, touch_off, tool_touch_off): task's line counters
  // are no longer those of the loaded program, also when the block is over before a status
  // shows it.
  std::atomic<std::uint64_t> mdi_blocks{};
  std::atomic<std::uint64_t> tool_tables{}; // the tool table loaded after set_tool, remove_tool
  std::atomic<std::uint64_t> aborts{};
};

struct FixedSetterMove {
  int tool;
  std::array<double, 3> start; // machine XYZ confirmed by the operator, millimetres
  double travel_mm;
  double feed_mm_min;
  std::optional<std::array<double, 3>> approach{}; // traverse at or above the taught clearance
  std::optional<double> retract_z{};
  double retract_above_contact_mm{};
  std::optional<ProbeDirection> material{};
};

struct FixedSetterContact {
  std::array<double, 3> machine; // latched machine XYZ, millimetres
  double stopping_distance;      // downward travel to settled machine Z before the retract, mm
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
// Blocking. The waits are polls with deadlines: echo_timeout for the echo, completion_timeout
// for completion, the windows below for errors and settling. A deadline is checked between
// libnml calls, and one libnml call (peek, read, write) can itself wait for the buffer's System V
// semaphore up to the timeout T of the `xemc` lines of the NML file, 10 s in LinuxCNC's
// linuxcnc.nml; so can the NML gate, while another thread's call waits in it. Bounds:
//
//   one libnml call          T, and as much again for the gate
//   one wait with deadline D D + 2T: its last pass makes at most two calls (status, error)
//   one NML command          write, echo, completion: at most 4 s + 5T, and about 0.1 s + 4T more
//                            when it fails and its error text is collected
//   an action                at most two commands (teleop then jog or home; flood then mist)
//                            and a settle wait of 1 s + 2T
//
// With a task that is stopped but does not hold a semaphore, which is what a stopped task was
// seen to be, a command returns after echo_timeout, 1 s. Typical times on a 1 ms task cycle are
// 2 to 30 ms.
//
// NOT thread-safe: one thread, the runtime's worker, calls every member. Other threads may use
// other NML objects (NmlStatusReader) at the same time: every call into LinuxCNC's NML library,
// which shares unlocked state between all its channels, goes through one process-wide gate
// (src/nml_gate.hpp), one call at a time. emcCommand is a queue: task executes commands in the
// order they were written, also those written while it was stopped.
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
  // no manual tool change. `loads` may be null: nothing is counted. Both must outlive the port.
  [[nodiscard]] static auto connect(const MachineConfig& config, HalShared* hal,
                                    NmlLoads* loads = nullptr)
      -> std::expected<std::unique_ptr<NmlPort>, std::string>;
  ~NmlPort() override;
  NmlPort(const NmlPort&) = delete;
  auto operator=(const NmlPort&) -> NmlPort& = delete;

  // One command as it left this port.
  struct Written {
    long type{};  // the NML message type
    int serial{}; // the serial number task echoes once it has taken the command up
    std::chrono::steady_clock::time_point at{}; // when libnml's write returned
    friend auto operator==(const Written&, const Written&) -> bool = default;
  };
  // Every command this port has written, in order, the last max_trace of them: for tests and
  // logs that must know what is, and what is not, in LinuxCNC's command queue. Any thread.
  static constexpr std::size_t max_trace = 4096;
  [[nodiscard]] auto writes() const -> std::vector<Written>;
  // The NML types of the commands tests ask about.
  struct Types {
    long jog_start;
    long jog_stop;
    long jog_increment;
    long teleop;
    long home;
    long run;
    long mdi;
    long spindle_on;
    long coolant_flood_on;
    long coolant_mist_on;
    long load_tool_table;
    long abort;
  };
  [[nodiscard]] static auto types() -> Types;

  [[nodiscard]] auto snapshot() -> ControllerState override;
  // The same reading with everything else the panel shows.
  [[nodiscard]] auto status() -> NmlStatus;
  // Also the regular status reading that keeps `link` meaningful between actions: call it at
  // least a few times per second.
  [[nodiscard]] auto take_messages() -> std::vector<ControllerMessage> override;
  void idle_maintenance(const Wanted& wanted) override;

  // Cancellation, as ControllerPort asks. Every action asks its `Wanted` after each wait of
  // its own (the collecting of earlier errors, the switch between world and joint mode, a
  // status it waits for) and a last time once it has the NML gate, immediately before each
  // write. The gate is taken first and the session's mutex is never held across a libnml call:
  // the session answers under its mutex and lets go of it before the write. So no other NML
  // call falls between the last question and the write, but the answer may still change in
  // that moment, and once libnml's write has begun the command is committed; see
  // ControllerPort for what ends a command that was written too late to withdraw. Actions that
  // write twice ask before each write: home and the jogs (leave or enter world mode, then the
  // command), coolant (flood, then mist). `wanted` is used during the call only and not kept.
  //
  // The tool table has two commits. Withdrawn before the file is replaced, the edit leaves the
  // table as it was and removes its temporary file. Withdrawn after that, before LinuxCNC is
  // told to load the table, it is `failed` with a reason that says so: the file is changed and
  // LinuxCNC still uses the old table. Nothing is rolled back: that could overwrite a later
  // edit. The tool change confirmation asks at the moment it is handed to the control thread.
  [[nodiscard]] auto set_machine_state(MachineState state, const Wanted& wanted)
      -> ActionResult override;
  [[nodiscard]] auto power_off() -> ActionResult override;
  [[nodiscard]] auto set_task_mode(TaskMode mode, const Wanted& wanted) -> ActionResult override;
  // Homing all axes fails, with nothing sent, unless MachineConfig::home_all_configured.
  [[nodiscard]] auto home(std::optional<Axis> axis, const Wanted& wanted) -> ActionResult override;
  [[nodiscard]] auto start_jog(Axis axis, int direction, double velocity, JogSpace space,
                               const Wanted& wanted) -> ActionResult override;
  // Sends nothing while the machine is observed off or outside manual mode, provided task has
  // echoed the last command this port wrote: task refuses jog commands then and no jog can be
  // running. A stop for the same axis that task has not echoed yet is not written again: the
  // call waits for that one.
  [[nodiscard]] auto stop_jog(Axis axis) -> ActionResult override;
  [[nodiscard]] auto jog_increment(Axis axis, double distance, double velocity, JogSpace space,
                                   const Wanted& wanted) -> ActionResult override;
  [[nodiscard]] auto mdi(std::string_view text, const Wanted& wanted) -> ActionResult override;
  // Two commands: the program that is open is closed, then this one opened. Withdrawn between
  // them, the result says that the old program was closed and the new one not opened.
  [[nodiscard]] auto open_program(std::string_view path, const Wanted& wanted)
      -> ActionResult override;
  [[nodiscard]] auto run(const Wanted& wanted) -> ActionResult override;
  [[nodiscard]] auto validate_restart(RestartPoint& point, const PreviewInputs& inputs)
      -> std::string override;
  [[nodiscard]] auto start_restart(const RestartPoint& point, const PreviewInputs& inputs,
                                   bool after_change, const Wanted& files_current,
                                   const Wanted& wanted) -> ActionResult override;
  [[nodiscard]] auto pause(const Wanted& wanted) -> ActionResult override;
  [[nodiscard]] auto resume(const Wanted& wanted) -> ActionResult override;
  [[nodiscard]] auto step(const Wanted& wanted) -> ActionResult override;
  [[nodiscard]] auto abort() -> ActionResult override;
  [[nodiscard]] auto set_override(Override which, double value, const Wanted& wanted)
      -> ActionResult override;
  [[nodiscard]] auto set_spindle(SpindleDirection direction, double rpm, const Wanted& wanted)
      -> ActionResult override;
  [[nodiscard]] auto set_coolant(bool flood, bool mist, const Wanted& wanted)
      -> ActionResult override;
  [[nodiscard]] auto set_program_option(ProgramOption option, bool on, const Wanted& wanted)
      -> ActionResult override;
  // Through the HAL component, not NML: asks the control thread to raise tool-changed for
  // exactly that request. Fails while the control thread does not renew its copy of the pins.
  [[nodiscard]] auto tool_changed(int tool, std::uint64_t request, const Wanted& wanted)
      -> ActionResult override;
  // Rewrite [EMCIO] TOOL_TABLE and have LinuxCNC load it again.
  [[nodiscard]] auto set_tool(const Tool& tool, const Wanted& wanted) -> ActionResult override;
  [[nodiscard]] auto remove_tool(int number, const Wanted& wanted) -> ActionResult override;

  // A fresh fixed-setter contact followed by a verified return to the starting Z. This is an
  // XYZ-machine primitive, independent of calibration and with no tool-table writes.
  [[nodiscard]] auto probe_fixed_setter(const FixedSetterMove& move, const Wanted& wanted)
      -> std::expected<FixedSetterContact, std::string>;
  [[nodiscard]] auto fixed_setter_available() const -> bool override;
  [[nodiscard]] auto fixed_setter(const FixedSetterRequest& request, SetterEpoch epoch,
                                  const Wanted& wanted)
      -> std::expected<SetterState, std::string> override;

private:
  struct Impl;
  explicit NmlPort(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

// Keeps the NML gate for as long as it lives: every NML call of every thread of the process
// waits. For tests of what a thread does that was held up on its way into LinuxCNC's library;
// nothing else has a reason to use it.
class NmlGateHold {
public:
  NmlGateHold();
  ~NmlGateHold();
  NmlGateHold(const NmlGateHold&) = delete;
  auto operator=(const NmlGateHold&) -> NmlGateHold& = delete;

private:
  std::unique_lock<std::mutex> lock_;
};

} // namespace coffeecnc

#endif // COFFEECNC_LINUXCNC_NML_PORT_HPP
