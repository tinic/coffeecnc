#ifndef COFFEECNC_CORE_COMMAND_POLICY_HPP
#define COFFEECNC_CORE_COMMAND_POLICY_HPP

#include <coffeecnc/core/fixed_setter.hpp>
#include <coffeecnc/core/operator_settings.hpp>
#include <coffeecnc/core/tool_models.hpp>

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
enum class Override : std::uint8_t { feed, rapid, spindle };
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
};

// Input from the controller adapter, never reconstructed from pending UI actions.
// The first machine profile is a three-joint XYZ mill with trivial kinematics.
// The unit the interpreter reads length words in (G21 / G20).
enum class LengthUnit : std::uint8_t { mm, inch };

struct ControllerState {
  Link link{Link::down};
  MachineState machine{MachineState::estop};
  TaskMode mode{TaskMode::manual};
  InterpState interp{InterpState::idle};
  std::array<AxisState, 3> axes{};
  bool in_position{true};
  bool program_loaded{};
  int tool_in_spindle{};
  std::optional<int> requested_tool;
  // Which request of the controller `requested_tool` is: another request, also one for the same
  // tool, has another number. A confirmation names it, so that it cannot answer a later request.
  std::uint64_t tool_request{};
  // Protocol values are millimetres; composed G-code must be written in this unit.
  LengthUnit program_units{LengthUnit::mm};
  std::string program_file;
  bool spindle_active{};
  double air_z{}; // selected lift, millimetres
  bool air_offset_owned{};
  bool air_offset_recovery{};
  bool breakpoint_program{};
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
struct Mdi {
  std::string text;
};
struct ListDirectory {
  std::optional<std::string> path;
};
struct OpenProgram {
  std::string path;
};
struct Run {
  std::optional<std::uint64_t> preview_context{};
  std::optional<LengthUnit> display_units{};
  bool confirmed{};
  double air_z{};
  std::optional<int> breakpoint{}; // pause before this original 1-based source line
};
struct SetAirCut {
  double air_z{}; // zero selects a normal run
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
struct TouchOff {
  Axis axis;
  double value;
};
struct ToolTouchOff {
  double value;
};
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
struct ToolChanged {};
// Switch the machine off (an E-stop stays set) and close CoffeeCNC; LinuxCNC then shuts down.
struct Quit {};

using Command =
    std::variant<Ready, SetMachine, Home, Jog, JogStep, JogStopAll, Mdi, ListDirectory, OpenProgram,
                 Run, Pause, Resume, Step, Abort, SetOverride, SetSpindle, SetCoolant,
                 SetProgramOption, SelectWcs, TouchOff, ToolTouchOff, SetTool, RemoveTool,
                 ToolChanged, SetToolModelDefaults, SetToolModelAssignment, Quit,
                 SetOperatorSettings, SetWorkOffset, FixedSetterRequest, PlanRestart, StartRestart,
                 EditProgram, SetAirCut>;

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
  [[nodiscard]] auto evaluate(const Command&, const ControllerState&) const -> Decision;

private:
  Limits limits_;
};

[[nodiscard]] auto axis_index(Axis axis) -> std::optional<std::size_t>;

} // namespace coffeecnc

#endif // COFFEECNC_CORE_COMMAND_POLICY_HPP
