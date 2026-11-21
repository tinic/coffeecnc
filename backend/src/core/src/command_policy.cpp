#include <coffeecnc/core/command_policy.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace coffeecnc {
namespace {

auto allow(std::optional<TaskMode> mode = {}, std::optional<JogSpace> space = {}) -> Decision {
  return {true, mode, {}, space};
}
auto reject(std::string reason) -> Decision { return {false, {}, std::move(reason), {}}; }
auto between(double value, double low, double high) -> bool {
  return std::isfinite(value) && value >= low && value <= high;
}
auto homed(const ControllerState& s) -> bool {
  return std::ranges::any_of(s.axes, [](const auto& a) -> auto { return a.enabled; }) &&
         std::ranges::all_of(
             s.axes, [](const auto& a) -> auto { return !a.enabled || (a.homed && !a.homing); });
}
auto homing(const ControllerState& s) -> bool {
  return std::ranges::any_of(s.axes, [](const auto& a) -> auto { return a.enabled && a.homing; });
}
auto valid_text(const std::string& text, std::size_t max_size) -> bool {
  return !text.empty() && text.size() <= max_size &&
         text.find_first_of("\r\n") == std::string::npos && text.find('\0') == std::string::npos &&
         text.find_first_not_of(" \t") != std::string::npos;
}
auto idle(const ControllerState& s, TaskMode mode, bool require_homed = false) -> Decision {
  if (s.machine != MachineState::on)
    return reject("Machine power is off or E-stop is active.");
  if (s.interp != InterpState::idle || !s.in_position || homing(s))
    return reject("Wait for the interpreter and motion to become idle.");
  if (require_homed && !homed(s))
    return reject("Home all configured axes first.");
  return allow(mode);
}
auto letter(Axis axis) -> char {
  switch (axis) {
  case Axis::x:
    return 'X';
  case Axis::y:
    return 'Y';
  case Axis::z:
    return 'Z';
  }
  return '?';
}
auto axis_state(const ControllerState& s, Axis axis) -> const AxisState* {
  const auto index = axis_index(axis);
  if (!index || !s.axes[*index].enabled)
    return nullptr;
  return &s.axes[*index];
}

} // namespace

auto axis_index(Axis axis) -> std::optional<std::size_t> {
  switch (axis) {
  case Axis::x:
    return 0;
  case Axis::y:
    return 1;
  case Axis::z:
    return 2;
  }
  return {};
}

CommandPolicy::CommandPolicy(const Limits& limits) : limits_(limits) {
  if (!std::isfinite(limits.max_jog_velocity) || limits.max_jog_velocity <= 0 ||
      !std::ranges::all_of(limits.max_axis_jog_velocity,
                           [](double v) -> bool { return std::isfinite(v) && v > 0; }) ||
      !std::isfinite(limits.max_feed_override) || limits.max_feed_override < 1 ||
      !between(limits.min_spindle_override, 0, 1) || !std::isfinite(limits.max_spindle_override) ||
      limits.max_spindle_override < 1 || !std::isfinite(limits.max_spindle_rpm) ||
      limits.max_spindle_rpm <= 0)
    throw std::invalid_argument("Invalid machine command limits");
}

auto CommandPolicy::evaluate(const Command& command, const ControllerState& s) const -> Decision {
  return std::visit(
      [&](const auto& c) -> Decision {
        using T = std::decay_t<decltype(c)>;
        // Read-only app requests need no connection. Stop requests must not be gated by stale
        // telemetry: the adapter still has to attempt the stop and report its actual outcome.
        if constexpr (std::is_same_v<T, Ready> || std::is_same_v<T, ListDirectory>)
          return allow();
        if constexpr (std::is_same_v<T, Abort> || std::is_same_v<T, JogStopAll>)
          return allow();
        // Visual preferences of the panel: nothing of the machine's state decides about them,
        // and nothing of them reaches the controller.
        if constexpr (std::is_same_v<T, SetToolModelDefaults> ||
                      std::is_same_v<T, SetToolModelAssignment> ||
                      std::is_same_v<T, SetOperatorSettings>)
          return allow();
        if constexpr (std::is_same_v<T, SetMachine>) {
          if (c.state == MachineState::estop || c.state == MachineState::off)
            return allow();
        }
        if constexpr (std::is_same_v<T, SetSpindle>) {
          if (c.direction == SpindleDirection::stopped)
            return allow();
        }
        if constexpr (std::is_same_v<T, SetCoolant>) {
          if (!c.flood && !c.mist)
            return allow();
        }
        if constexpr (std::is_same_v<T, Jog>) {
          if (!axis_index(c.axis))
            return reject("Unknown axis.");
          if (!c.held)
            return allow();
        }
        if (s.link != Link::live)
          return reject("Controller status is unavailable or stale.");

        if (s.paused_z && !s.paused_z->feedback_available && !std::is_same_v<T, SetMachine> &&
            !std::is_same_v<T, Quit> && !std::is_same_v<T, OpenProgram>)
          return reject("Wait for paused Z feedback before commanding machine motion.");
        if (s.paused_z && s.paused_z->displaced && s.interp != InterpState::paused &&
            !std::is_same_v<T, SetMachine> && !std::is_same_v<T, Quit>)
          return reject(
              "Z is still lifted. Turn machine power off and on to clear the offset here.");

        // A controller that requires homing may refuse the MDI cleanup until it is homed.
        if (s.air_offset_recovery && !std::is_same_v<T, SetMachine> && !std::is_same_v<T, Quit> &&
            !(std::is_same_v<T, Home> && limits_.homing_required && !homed(s)))
          return reject("Wait for the temporary air-cut offset to clear.");

        if constexpr (std::is_same_v<T, Quit>) {
          if (s.interp != InterpState::idle)
            return reject("Stop the program before quitting.");
          return allow();
        }

        if constexpr (std::is_same_v<T, SetMachine>) {
          if (c.state != MachineState::on)
            return reject("Unknown machine state.");
          if (s.machine == MachineState::estop)
            return reject("Clear E-stop before enabling power.");
          return allow();
        } else if constexpr (std::is_same_v<T, Home>) {
          if (c.axis && !axis_state(s, *c.axis))
            return reject("Axis is not configured.");
          if (!std::ranges::any_of(s.axes, [](const auto& a) -> auto { return a.enabled; }))
            return reject("No axes are configured.");
          return idle(s, TaskMode::manual);
        } else if constexpr (std::is_same_v<T, Jog> || std::is_same_v<T, JogStep>) {
          const auto* a = axis_state(s, c.axis);
          if (!a)
            return reject("Axis is not configured.");
          if (!between(c.velocity, 0, limits_.max_jog_velocity) || c.velocity == 0)
            return reject("Jog velocity is outside the configured range.");
          if (const double ceiling =
                  limits_.max_axis_jog_velocity.at(axis_index(c.axis).value_or(0));
              c.velocity > ceiling)
            return reject(std::format("Jog velocity is above the {} axis limit of {} mm/min.",
                                      letter(c.axis), ceiling));
          if (s.machine != MachineState::on || s.interp != InterpState::idle || homing(s))
            return reject("Jogging needs machine power, an idle interpreter and no active homing.");
          if (a->on_hard_limit)
            return reject("A hard limit is active; limit recovery is not supported yet.");
          if constexpr (std::is_same_v<T, Jog>) {
            if (c.direction != -1 && c.direction != 1)
              return reject("Invalid jog direction.");
          } else {
            if (!std::isfinite(c.distance) || c.distance == 0)
              return reject("Invalid jog distance.");
            if (!s.in_position)
              return reject("Wait for motion to stop before an incremental jog.");
          }
          if (a->homed) {
            if (!std::isfinite(a->machine) || !std::isfinite(a->min_limit) ||
                !std::isfinite(a->max_limit) || a->min_limit >= a->max_limit)
              return reject("Axis position or limits are invalid.");
            if constexpr (std::is_same_v<T, Jog>) {
              if ((c.direction < 0 && a->machine <= a->min_limit) ||
                  (c.direction > 0 && a->machine >= a->max_limit))
                return reject("Jog points beyond the configured travel.");
            } else {
              if (!between(a->machine + c.distance, a->min_limit, a->max_limit))
                return reject("Incremental jog exceeds configured travel.");
            }
          }
          return allow(TaskMode::manual, homed(s) ? JogSpace::axis : JogSpace::joint);
        } else if constexpr (std::is_same_v<T, Mdi>) {
          if (!valid_text(c.text, 254))
            return reject("MDI requires one nonempty block of at most 254 bytes.");
          return idle(s, TaskMode::mdi, limits_.homing_required);
        } else if constexpr (std::is_same_v<T, OpenProgram>) {
          if (!valid_text(c.path, 254))
            return reject("Program path is empty, too long or contains control characters.");
          if (s.interp != InterpState::idle || !s.in_position || homing(s))
            return reject("Stop execution before replacing the program.");
          return allow(); // File containment/existence checks belong to the app's program store.
        } else if constexpr (std::is_same_v<T, Run>) {
          if (!s.program_loaded)
            return reject("Load a program first.");
          if (c.air_z != s.air_z)
            return reject("The air-cut selection changed. Review the current preview.");
          if (s.breakpoint_program)
            return reject("Wait for the original program to reopen after the breakpoint run.");
          if (c.breakpoint && c.air_z != 0)
            return reject("Turn off air cut before running to a breakpoint.");
          const auto decision = idle(s, TaskMode::automatic, limits_.homing_required);
          return decision.allowed && c.air_z > 0 ? allow() : decision;
        } else if constexpr (std::is_same_v<T, SetAirCut>) {
          if (!between(c.air_z, 0, 1000) || (c.air_z > 0 && c.air_z < 0.001))
            return reject(
                "Air-cut lift must be between 0.001 and 1,000 mm, or zero to turn it off.");
          if (c.air_z > 0 && !s.program_loaded)
            return reject("Load a program first.");
          if (s.breakpoint_program)
            return reject("Wait for the breakpoint run to finish.");
          if (s.interp != InterpState::idle || !s.in_position || s.air_offset_owned)
            return reject("Stop the program before changing the air-cut lift.");
          return allow();
        } else if constexpr (std::is_same_v<T, EditProgram>) {
          if (!s.program_loaded)
            return reject("Load a program first.");
          if (!valid_text(c.path, 254) || c.line < 1 || c.before.size() > 4096 || c.after.size() > 4096)
            return reject("The program edit is not valid.");
          if (s.interp != InterpState::idle || !s.in_position || homing(s))
            return reject("Stop execution before editing the program.");
          return allow();
        } else if constexpr (std::is_same_v<T, PlanRestart> || std::is_same_v<T, StartRestart>) {
          if (s.breakpoint_program)
            return reject("Wait for the breakpoint run to finish.");
          if (s.air_z > 0 || s.air_offset_owned)
            return reject("Turn off air cut before restarting from a line.");
          if (!s.program_loaded)
            return reject("Load a program first.");
          const auto decision = idle(s, TaskMode::mdi, limits_.homing_required);
          return decision.allowed ? allow() : decision;
        } else if constexpr (std::is_same_v<T, Pause>) {
          if (s.machine != MachineState::on || s.mode == TaskMode::manual ||
              (s.interp != InterpState::reading && s.interp != InterpState::waiting))
            return reject("There is no executing program or MDI command to pause.");
          return allow();
        } else if constexpr (std::is_same_v<T, JogPausedZ>) {
          if (!s.paused_z)
            return reject("Paused Z jog is unavailable on this machine.");
          const bool stopping_spindle =
              c.stop_spindle && c.distance > 0 && s.paused_z->spindle == PausedSpindle::can_stop;
          if (c.stop_spindle && !stopping_spindle)
            return reject("The paused spindle state changed. Review the controls and try again.");
          if (!stopping_spindle && !s.paused_z->reason.empty())
            return reject(s.paused_z->reason);
          if (!std::isfinite(c.distance) || c.distance == 0 ||
              std::abs(c.distance) > s.paused_z->max_lift)
            return reject("The paused Z jog distance is out of range.");
          return allow();
        } else if constexpr (std::is_same_v<T, Resume>) {
          if (s.paused_z && s.paused_z->spindle == PausedSpindle::recover)
            return reject("Press Stop before continuing after an interrupted spindle hold.");
          if (s.machine != MachineState::on || s.interp != InterpState::paused ||
              s.mode == TaskMode::manual || s.requested_tool)
            return reject("There is no paused execution ready to resume.");
          return allow();
        } else if constexpr (std::is_same_v<T, Step>) {
          if (s.paused_z && s.paused_z->spindle == PausedSpindle::recover)
            return reject("Press Stop before continuing after an interrupted spindle hold.");
          if (s.paused_z && s.paused_z->spindle == PausedSpindle::restore)
            return reject("Use Resume to restore the program spindle before continuing.");
          if (s.paused_z && s.paused_z->displaced)
            return reject("Use Resume to return Z before continuing.");
          if (s.breakpoint_program)
            return reject("Use Resume to continue the breakpoint run.");
          if (s.air_z > 0 || s.air_offset_owned)
            return reject("Turn off air cut before using single block.");
          if (!s.program_loaded || s.machine != MachineState::on || s.requested_tool)
            return reject("Single block needs a loaded program and no pending tool change.");
          if (limits_.homing_required && !homed(s))
            return reject("Home all configured axes first.");
          // Not needing homing never means stepping while an axis is homing.
          if (homing(s))
            return reject("Wait for homing to finish.");
          if (s.interp == InterpState::paused && s.mode == TaskMode::automatic)
            return allow();
          return idle(s, TaskMode::automatic, limits_.homing_required);
        } else if constexpr (std::is_same_v<T, SetOverride>) {
          switch (c.which) {
          case Override::feed:
            return between(c.value, 0, limits_.max_feed_override)
                       ? allow()
                       : reject("Feed override is out of range.");
          case Override::rapid:
            return between(c.value, 0, 1) ? allow() : reject("Rapid override is out of range.");
          case Override::spindle:
            return between(c.value, limits_.min_spindle_override, limits_.max_spindle_override)
                       ? allow()
                       : reject("Spindle override is out of range.");
          }
          return reject("Unknown override.");
        } else if constexpr (std::is_same_v<T, SetSpindle>) {
          if (c.direction != SpindleDirection::forward && c.direction != SpindleDirection::reverse)
            return reject("Unknown spindle direction.");
          if (!between(c.rpm, 0, limits_.max_spindle_rpm) || c.rpm == 0)
            return reject("Spindle speed is out of range.");
          if (s.machine != MachineState::on || s.interp != InterpState::idle || homing(s))
            return reject(
                "Spindle start needs machine power, an idle interpreter and no active homing.");
          return allow(); // Spindle control does not require stopping an ongoing manual jog.
        } else if constexpr (std::is_same_v<T, SetCoolant>) {
          if (s.machine != MachineState::on)
            return reject("Machine power is off.");
          return allow();
        } else if constexpr (std::is_same_v<T, SetProgramOption>) {
          if (c.option == ProgramOption::block_delete && s.breakpoint_program)
            return reject("Stop the breakpoint run before changing block delete.");
          if (c.option == ProgramOption::block_delete && s.air_offset_owned)
            return reject("Stop the air cut before changing block delete.");
          if (c.option != ProgramOption::optional_stop && c.option != ProgramOption::block_delete)
            return reject("Unknown program option.");
          if (c.option == ProgramOption::block_delete && s.interp != InterpState::idle)
            return reject("Stop execution before changing block delete.");
          return allow();
        } else if constexpr (std::is_same_v<T, SelectWcs>) {
          if (c.wcs < 1 || c.wcs > 9)
            return reject("Work coordinate system must be G54 through G59.3.");
          return idle(s, TaskMode::mdi, limits_.homing_required);
        } else if constexpr (std::is_same_v<T, SetWorkOffset>) {
          if (s.requested_tool)
            return reject("Finish the tool change before editing work origins.");
          if (c.wcs < 1 || c.wcs > 9 ||
              !std::ranges::all_of(c.offset, [](double value) -> bool { return std::isfinite(value); }) ||
              !axis_state(s, Axis::x) || !axis_state(s, Axis::y) || !axis_state(s, Axis::z))
            return reject("Provide G54 through G59.3 and finite XYZ offsets.");
          return idle(s, TaskMode::mdi, limits_.homing_required);
        } else if constexpr (std::is_same_v<T, TouchOff>) {
          if (!axis_state(s, c.axis) || !std::isfinite(c.value))
            return reject("Invalid touch-off axis or value.");
          return idle(s, TaskMode::mdi, limits_.homing_required);
        } else if constexpr (std::is_same_v<T, ToolTouchOff>) {
          if (s.tool_in_spindle <= 0 || !std::isfinite(c.value))
            return reject("Load a tool and provide a finite touch-off value.");
          return idle(s, TaskMode::mdi, limits_.homing_required);
        } else if constexpr (std::is_same_v<T, FixedSetterRequest>) {
          if (c.action == SetterAction::status)
            return allow();
          if (s.spindle_active || s.requested_tool || s.tool_in_spindle <= 0 ||
              c.tool != s.tool_in_spindle)
            return reject("Fixed setter requires the confirmed tool and a stopped spindle.");
          return idle(s, TaskMode::mdi,
                      c.action != SetterAction::material && c.action != SetterAction::corner &&
                          c.action != SetterAction::ball &&
                          limits_.homing_required);
        } else if constexpr (std::is_same_v<T, SetTool> || std::is_same_v<T, RemoveTool>) {
          if (s.interp != InterpState::idle || !s.in_position || s.requested_tool || homing(s))
            return reject("Stop execution and finish tool changes before editing tools.");
          if constexpr (std::is_same_v<T, SetTool>) {
            if (c.tool.number <= 0 || c.tool.pocket < 0 || !std::isfinite(c.tool.length) ||
                !std::isfinite(c.tool.diameter) || c.tool.diameter < 0 ||
                c.tool.comment.find_first_of("\r\n") != std::string::npos ||
                c.tool.comment.find('\0') != std::string::npos)
              return reject("Invalid tool table row.");
          } else if (c.number <= 0)
            return reject("Invalid tool number.");
          return allow();
        } else if constexpr (std::is_same_v<T, ToolChanged>) {
          if (s.machine != MachineState::on || !s.requested_tool || *s.requested_tool < 0)
            return reject("No manual tool change is awaiting confirmation.");
          return allow(); // Tool 0 may be requested to unload the spindle.
        }
        return reject("Unsupported command.");
      },
      command);
}

} // namespace coffeecnc
