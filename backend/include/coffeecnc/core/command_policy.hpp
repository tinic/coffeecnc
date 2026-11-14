#pragma once

#include <array>
#include <optional>
#include <string>
#include <variant>

namespace coffeecnc {

enum class Axis { x, y, z };
enum class Link { live, stale, down };
enum class MachineState { estop, off, on };
enum class TaskMode { manual, mdi, automatic };
enum class InterpState { idle, reading, paused, waiting };
enum class SpindleDirection { stopped, forward, reverse };
enum class Override { feed, rapid, spindle };
enum class ProgramOption { optional_stop, block_delete };

struct AxisState {
  bool enabled{true};
  bool homed{};
  bool homing{};
  bool on_hard_limit{};
  double machine{};
  double min_limit{-1000};
  double max_limit{1000};
};

// Input from the controller adapter, never reconstructed from pending UI actions.
// The first machine profile is a three-joint XYZ mill with trivial kinematics.
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
};

struct Limits {
  double max_jog_velocity{3000}; // mm/min
  double max_feed_override{1.2};
  double min_spindle_override{0.5};
  double max_spindle_override{1.2};
  double max_spindle_rpm{24000}; // Must come from machine configuration for a live adapter.
};

// Typed bodies of protocol v1 requests. Wire parsing and request IDs belong to the app layer.
struct Ready {};
struct SetMachine { MachineState state; };
struct Home { std::optional<Axis> axis; }; // null means all
struct Jog { Axis axis; int direction; double velocity; bool held; };
struct JogStep { Axis axis; double distance; double velocity; };
struct JogStopAll {};
struct Mdi { std::string text; };
struct ListDirectory { std::optional<std::string> path; };
struct OpenProgram { std::string path; };
struct Run {};
struct Pause {};
struct Resume {};
struct Step {};
struct Abort {};
struct SetOverride { Override which; double value; };
struct SetSpindle { SpindleDirection direction; double rpm; };
struct SetCoolant { bool flood; bool mist; };
struct SetProgramOption { ProgramOption option; bool on; };
struct SelectWcs { int wcs; };
struct TouchOff { Axis axis; double value; };
struct ToolTouchOff { double value; };
struct Tool { int number; int pocket; double length; double diameter; std::string comment; };
struct SetTool { Tool tool; };
struct RemoveTool { int number; };
struct ToolChanged {};

using Command = std::variant<Ready, SetMachine, Home, Jog, JogStep, JogStopAll, Mdi,
    ListDirectory, OpenProgram, Run, Pause, Resume, Step, Abort, SetOverride,
    SetSpindle, SetCoolant, SetProgramOption, SelectWcs, TouchOff, ToolTouchOff,
    SetTool, RemoveTool, ToolChanged>;

struct Decision {
  bool allowed{};
  // The app must request and observe this mode before submitting the action.
  std::optional<TaskMode> required_mode;
  std::string reason;
};

class CommandPolicy {
 public:
  explicit CommandPolicy(Limits limits = {});
  // Permission to attempt an action, NOT a protocol CommandResult/acknowledgement.
  [[nodiscard]] Decision evaluate(const Command&, const ControllerState&) const;

 private:
  Limits limits_;
};

[[nodiscard]] std::optional<std::size_t> axis_index(Axis axis);

} // namespace coffeecnc
