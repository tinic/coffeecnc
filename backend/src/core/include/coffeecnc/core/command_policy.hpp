#ifndef COFFEECNC_CORE_COMMAND_POLICY_HPP
#define COFFEECNC_CORE_COMMAND_POLICY_HPP

#include <coffeecnc/core/diagnostics.hpp>
#include <coffeecnc/core/fixed_setter.hpp>
#include <coffeecnc/core/operator_settings.hpp>
#include <coffeecnc/core/tool_models.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <variant>

namespace coffeecnc {

enum class Axis : std::uint8_t { x, y, z };
enum class Link : std::uint8_t { live, stale, down };
enum class MachineState : std::uint8_t { estop, off, on };
enum class TaskMode : std::uint8_t { manual, mdi, automatic };
enum class InterpState : std::uint8_t { idle, reading, paused, waiting };
enum class SpindleDirection : std::uint8_t { stopped, forward, reverse };
enum class Override : std::uint8_t { feed, rapid, spindle, velocity_limit };
enum class ProgramOption : std::uint8_t { optional_stop, block_delete };
enum class JogSpace : std::uint8_t { joint, axis };

struct AxisState {
  bool enabled{};
  bool homed{};
  bool homing{};
  bool on_hard_limit{};
  double machine{};
  double min_limit{std::numeric_limits<double>::quiet_NaN()};
  double max_limit{std::numeric_limits<double>::quiet_NaN()};
  // Whether homing can find this axis's reference mark, where the machine's HAL reports it: a
  // scale with one mark says nothing about the side the axis is on until it has crossed the
  // mark. Nothing on a machine without that signal.
  std::optional<bool> reference_mark{};
  // The axis stands at that mark now. Nothing on a machine without the signal.
  std::optional<bool> at_reference_mark{};
};

// Input from the controller adapter, never reconstructed from pending UI actions.
// The first machine profile is a three-joint XYZ mill with trivial kinematics.
// The unit the interpreter reads length words in (G21 / G20).
enum class LengthUnit : std::uint8_t { mm, inch };

enum class PausedSpindle : std::uint8_t { none, can_stop, stopping, restore, recover };

struct PausedZStatus {
  double offset{}; // applied Z offset, mm
  double max_lift{};
  bool displaced{};
  bool feedback_available{};
  std::string reason;
  PausedSpindle spindle{};
  friend auto operator==(const PausedZStatus&, const PausedZStatus&) -> bool = default;
};

struct ControllerState {
  Link link{Link::down};
  MachineState machine{MachineState::estop};
  TaskMode mode{TaskMode::manual};
  InterpState interp{InterpState::idle};
  std::array<AxisState, 3> axes{};
  std::array<AxisState, 3> rotary_axes{}; // A, B, C, in degrees.
  [[nodiscard]] auto any_enabled_axis(const auto& matches) const -> bool {
    const auto check = [&](const AxisState& axis) { return axis.enabled && matches(axis); };
    return std::ranges::any_of(axes, check) || std::ranges::any_of(rotary_axes, check);
  }
  bool in_position{true};
  bool program_loaded{};
  int tool_in_spindle{};
  std::optional<int> requested_tool;
  // Which request of the controller `requested_tool` is: another request, also one for the same
  // tool, has another number. A confirmation names it, so that it cannot answer a later request.
  std::uint64_t tool_request{};
  // Physical line in the original AUTO program waiting for the manual tool change.
  std::optional<int> tool_change_line{};
  std::array<double, 3> tool_change_position{}; // commanded machine XYZ at that line, in mm
  // Protocol values are millimetres; composed G-code must be written in this unit.
  LengthUnit program_units{LengthUnit::mm};
  std::string program_file;
  std::string active_mdi; // LinuxCNC's active input block while MDI waits at a tool exchange.
  bool mdi_queued{};
  bool spindle_active{};
  bool unresolved_legacy_offset{};
  bool breakpoint_program{};
  std::optional<PausedZStatus> paused_z{};
};

struct Limits {
  Limits() = delete;
  // Every axis may jog at the global maximum and homing is required, as on a LinuxCNC machine
  // that configures neither.
  Limits(double jog_velocity, double feed_override, double spindle_override_min,
         double spindle_override_max, double spindle_rpm)
      : Limits(jog_velocity, {jog_velocity, jog_velocity, jog_velocity}, feed_override,
               spindle_override_min, spindle_override_max, spindle_rpm, true) {}
  Limits(double jog_velocity, const std::array<double, 3>& axis_jog_velocity, double feed_override,
         double spindle_override_min, double spindle_override_max, double spindle_rpm, bool homing)
      : max_jog_velocity(jog_velocity), max_axis_jog_velocity(axis_jog_velocity),
        max_feed_override(feed_override), min_spindle_override(spindle_override_min),
        max_spindle_override(spindle_override_max), max_spindle_rpm(spindle_rpm),
        homing_required(homing) {}
  // All values must be supplied by validated machine configuration or an explicit simulator
  // profile.
  double max_jog_velocity; // mm/min, any axis
  // mm/min for X, Y, Z. A jog must be within both this and the global maximum.
  std::array<double, 3> max_axis_jog_velocity;
  double max_feed_override;
  double min_spindle_override;
  double max_spindle_override;
  double max_spindle_rpm;
  // LinuxCNC refuses MDI and program runs on a machine that is not homed. False for a machine
  // configured to accept them ([TRAJ] NO_FORCE_HOMING): MDI, run, single block, work coordinate
  // selection and touch-off are then allowed unhomed. Jog rules do not depend on it.
  bool homing_required;
  // The controller's configured linear trajectory ceiling in mm/min, when available.
  std::optional<double> max_trajectory_velocity{};
  // Joints (motors) that drive X, Y, Z: 2 for the Y of a gantry. Before homing jogs move joints,
  // and an axis with more than one is not jogged then: one motor alone would rack it.
  std::array<int, 3> axis_motors{1, 1, 1};
};

// Typed bodies of protocol v1 requests. Wire parsing and request IDs belong to the app layer.
struct Ready {};
struct SetMachine {
  MachineState state;
};
struct Home {
  std::optional<Axis> axis;
}; // null means all
struct Jog {
  Axis axis;
  int direction;
  double velocity;
  bool held;
};
struct JogStep {
  Axis axis;
  double distance;
  double velocity;
};
struct JogStopAll {};
struct JogPausedZ {
  double distance; // mm
  bool stop_spindle{}; // explicitly confirmed by the operator
};
struct Mdi {
  std::string text;
};
struct ListDirectory {
  std::optional<std::string> path;
};
enum class ProgramFileOperation : std::uint8_t { create_folder, copy, rename, remove };
// Copy uses the current file's name in Programs. Rename changes only its leaf name.
struct ManageProgramFile {
  ProgramFileOperation operation;
  std::string path;
  std::string name{};
};
struct OpenProgram {
  std::string path;
};
struct BreakpointRequest {
  int line{};
  std::string text;
};

struct Run {
  std::optional<std::uint64_t> preview_context{};
  std::optional<LengthUnit> display_units{};
  bool confirmed{};
  std::optional<BreakpointRequest> breakpoint{};
};
struct PlanRestart {
  int line{};
  std::uint64_t preview_context{};
};
struct StartRestart {
  std::uint64_t plan_id{};
};
// Changes the number of one F or S word on `line` of the loaded program, then reloads it.
struct EditProgram {
  std::string path;
  int line{};
  std::string before;
  std::string after;
};
// Writes a new program file below the program directory ([DISPLAY] PROGRAM_PREFIX). An existing
// file is replaced only when `replace` is set, and never the loaded program.
//
// With `stamp` (from the `program` message) it saves the program editor's text over the loaded
// program instead: only while nothing runs, only when the file still has that stamp, and the
// program is then opened again. `replace` does not apply.
struct SaveProgram {
  std::string path;
  std::string text;
  bool replace{};
  std::optional<std::string> stamp{};
};
// The longest program text `save_program` writes.
inline constexpr std::size_t max_saved_program_bytes = std::size_t{512} * 1024;
struct Pause {};
struct Resume {};
struct Step {};
struct Abort {};
struct SetOverride {
  Override which;
  double value;
};
struct SetSpindle {
  SpindleDirection direction;
  double rpm;
};
struct SetCoolant {
  bool flood;
  bool mist;
};
struct SetProgramOption {
  ProgramOption option;
  bool on;
};
struct SelectWcs {
  int wcs;
};
struct SetWorkOffset {
  int wcs;
  std::array<double, 3> offset; // XYZ origin in machine coordinates, millimetres
};
// The XY rotation of one work system (G10 L2 P<wcs> R<degrees>); 0 clears it.
inline constexpr double max_work_rotation = 360; // degrees, either way
struct SetWorkRotation {
  int wcs;
  double degrees;
};
// Clears the G92/G52 shift (G92.1): one offset in LinuxCNC, #5211 ... #5219.
struct ClearG92 {};
// What the operator used to set the coordinate: only for the offset history.
enum class TouchOrigin : std::uint8_t { typed, zero, half };
struct TouchOff {
  Axis axis;
  double value;
  TouchOrigin origin{TouchOrigin::typed};
};
struct ToolTouchOff {
  double value;
};
inline constexpr std::size_t max_tool_comment_bytes = 255;
struct Tool {
  int number{};
  int pocket{};
  double length{std::numeric_limits<double>::quiet_NaN()};
  double diameter{std::numeric_limits<double>::quiet_NaN()};
  std::string comment;
  friend auto operator==(const Tool&, const Tool&) -> bool = default;
};
struct SetTool {
  Tool tool;
};
struct RemoveTool {
  int number;
};
struct ToolChanged {
  std::optional<std::uint64_t> measure_request{};
};
// Writes back the earlier values of one offset history entry (OffsetHistoryStore).
struct RestoreOffset {
  std::uint64_t entry{};
};
// Switch the machine off (an E-stop stays set) and close CoffeeCNC; LinuxCNC then shuts down.
struct Quit {};
// Write a backup archive of the machine's data into this folder.
struct Backup {
  std::string directory;
};
// What a restore of this archive would write; nothing is written.
struct CheckRestore {
  std::string path;
};
// Restore the archive checked with this token: staged, then CoffeeCNC and LinuxCNC stop and the
// files are written.
struct RestoreBackup {
  std::string path;
  std::string token;
};
// The diagnostics view opens (`on`) or closes: HAL inputs are read and sent only while it is open.
struct WatchDiagnostics {
  bool on{};
};
// Start halshow, halmeter or halscope as a program of its own.
struct LaunchHalTool {
  HalTool tool{HalTool::halshow};
};

using Command =
    std::variant<Ready, SetMachine, Home, Jog, JogStep, JogStopAll, Mdi, ListDirectory, OpenProgram,
                 Run, Pause, Resume, Step, Abort, SetOverride, SetSpindle, SetCoolant,
                 SetProgramOption, SelectWcs, TouchOff, ToolTouchOff, SetTool, RemoveTool,
                 ToolChanged, SetToolModelDefaults, SetToolModelAssignment, Quit,
                 SetOperatorSettings, SetWorkOffset, FixedSetterRequest, PlanRestart, StartRestart,
                 EditProgram, JogPausedZ, SaveProgram, RestoreOffset, SetWorkRotation, ClearG92,
                 WatchDiagnostics, LaunchHalTool, Backup, CheckRestore, RestoreBackup,
                 ManageProgramFile>;

struct Decision {
  bool allowed{};
  // The app must request and observe this mode before submitting the action.
  std::optional<TaskMode> required_mode;
  std::string reason;
  // Joint jog before all axes home; axis jog afterward requires teleop in the adapter.
  std::optional<JogSpace> jog_space;
};

class CommandPolicy {
public:
  explicit CommandPolicy(const Limits& limits);
  // Permission to attempt an action, NOT a protocol CommandResult/acknowledgement.
  [[nodiscard]] auto evaluate(const Command&, const ControllerState&,
                              bool increment_in_progress = false) const -> Decision;

private:
  Limits limits_;
};

[[nodiscard]] auto axis_index(Axis axis) -> std::optional<std::size_t>;

} // namespace coffeecnc

#endif // COFFEECNC_CORE_COMMAND_POLICY_HPP
