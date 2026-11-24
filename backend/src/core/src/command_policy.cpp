#include <coffeecnc/core/command_policy.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace coffeecnc {
namespace {

auto allow(std::optional<TaskMode> mode = {}, std::optional<JogSpace> space = {}) -> Decision {
  return {true, mode, {}, space};
}
auto reject(std::string reason, RefusalMessage message) -> Decision {
  return {false, {}, std::move(reason), {}, std::move(message)};
}
auto between(double value, double low, double high) -> bool {
  return std::isfinite(value) && value >= low && value <= high;
}
auto homed(const ControllerState& s) -> bool {
  return s.any_enabled_axis([](const auto&) { return true; }) &&
         !s.any_enabled_axis([](const auto& a) { return !a.homed || a.homing; });
}
auto homing(const ControllerState& s) -> bool {
  return s.any_enabled_axis([](const auto& a) { return a.homing; });
}
auto valid_text(const std::string& text, std::size_t max_size) -> bool {
  return !text.empty() && text.size() <= max_size &&
         text.find_first_of("\r\n") == std::string::npos && text.find('\0') == std::string::npos &&
         text.find_first_not_of(" \t") != std::string::npos;
}
constexpr auto pending_tool_reason = "Confirm the pending tool change or press Stop first.";

auto idle(const ControllerState& s, TaskMode mode, bool require_homed = false) -> Decision {
  if (s.requested_tool)
    return reject(pending_tool_reason, RefusalMessage{"messages.policy.pendingToolChange"});
  if (s.machine != MachineState::on)
    return reject("Machine power is off or E-stop is active.",
                  RefusalMessage{"messages.policy.machineOffOrEstop"});
  if (s.interp != InterpState::idle || !s.in_position || homing(s))
    return reject("Wait for the interpreter and motion to become idle.",
                  RefusalMessage{"messages.policy.waitForIdle"});
  if (require_homed && !homed(s))
    return reject("Home all configured axes first.",
                  RefusalMessage{"messages.policy.homeAllFirst"});
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

auto CommandPolicy::evaluate(const Command& command, const ControllerState& s,
                             bool increment_in_progress) const -> Decision {
  return std::visit(
      [&](const auto& c) -> Decision {
        using T = std::decay_t<decltype(c)>;
        // Read-only app requests need no connection. Stop requests must not be gated by stale
        // telemetry: the adapter still has to attempt the stop and report its actual outcome.
        if constexpr (std::is_same_v<T, Ready> || std::is_same_v<T, ListDirectory>)
          return allow();
        // A backup only reads the machine's files and writes an archive elsewhere.
        if constexpr (std::is_same_v<T, Backup>) {
          if (!valid_text(c.directory, 4096) || !c.directory.starts_with('/'))
            return reject("Choose a folder for the backup.",
                          RefusalMessage{"messages.policy.backupFolder"});
          return allow();
        }
        if constexpr (std::is_same_v<T, CheckRestore> || std::is_same_v<T, RestoreBackup>) {
          if (!valid_text(c.path, 4096) || !c.path.starts_with('/'))
            return reject("Choose a backup file.", RefusalMessage{"messages.policy.backupFile"});
        }
        if constexpr (std::is_same_v<T, ManageProgramFile>)
          return allow();
        if constexpr (std::is_same_v<T, Abort> || std::is_same_v<T, JogStopAll>)
          return allow();
        // A new file: nothing of the machine's state decides about it. Replacing the loaded
        // program is refused by the app, which resolves the path. A save over the loaded program
        // (`stamp`) is an edit of it: the checks below decide, as for EditProgram.
        if constexpr (std::is_same_v<T, SaveProgram>) {
          if (!valid_text(c.path, 254))
            return reject("Program path is empty, too long or contains control characters.",
                          RefusalMessage{"messages.policy.programPathInvalid"});
          if (c.text.empty() || c.text.size() > max_saved_program_bytes ||
              c.text.find('\0') != std::string::npos)
            return reject("The program text is empty, longer than 512 KiB or contains a NUL byte.",
                          RefusalMessage{"messages.policy.programTextInvalid"});
          if (!c.stamp)
            return allow();
          if (!valid_text(*c.stamp, 64))
            return reject("The program stamp is not valid.",
                          RefusalMessage{"messages.policy.programStampInvalid"});
        }
        // Visual preferences of the panel: nothing of the machine's state decides about them,
        // and nothing of them reaches the controller.
        if constexpr (std::is_same_v<T, SetToolModelDefaults> ||
                      std::is_same_v<T, SetToolModelAssignment> ||
                      std::is_same_v<T, SetOperatorSettings>)
          return allow();
        // Reading HAL and starting a HAL tool: neither commands the controller, and both are
        // wanted most when it is in E-stop or its link is gone.
        if constexpr (std::is_same_v<T, WatchDiagnostics> || std::is_same_v<T, LaunchHalTool>)
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
            return reject("Unknown axis.", RefusalMessage{"messages.policy.unknownAxis"});
          if (!c.held)
            return allow();
        }
        if (s.link != Link::live)
          return reject("Controller status is unavailable or stale.",
                        RefusalMessage{"messages.policy.statusUnavailable"});
        // Tool I/O can be waiting at M6 even when the interpreter and trajectory are idle.
        if constexpr (std::is_same_v<T, Quit> || std::is_same_v<T, SetSpindle> ||
                      std::is_same_v<T, Jog> || std::is_same_v<T, JogStep> ||
                      std::is_same_v<T, JogPausedZ> || std::is_same_v<T, OpenProgram> ||
                      std::is_same_v<T, EditProgram> || std::is_same_v<T, SaveProgram>) {
          if (s.requested_tool)
            return reject(pending_tool_reason, RefusalMessage{"messages.policy.pendingToolChange"});
        }

        if (s.paused_z && s.paused_z->spindle == PausedSpindle::recover &&
            !std::is_same_v<T, SetMachine> && !std::is_same_v<T, Quit> &&
            !std::is_same_v<T, OpenProgram>)
          return reject("Press Stop before continuing after an interrupted spindle hold.",
                        RefusalMessage{"messages.policy.spindleHoldInterrupted"});

        if (s.paused_z && !s.paused_z->feedback_available && !std::is_same_v<T, SetMachine> &&
            !std::is_same_v<T, Quit> && !std::is_same_v<T, OpenProgram> &&
            !std::is_same_v<T, EditProgram> && !std::is_same_v<T, SaveProgram> &&
            !std::is_same_v<T, SetTool> && !std::is_same_v<T, RemoveTool> &&
            !std::is_same_v<T, SetOverride> && !std::is_same_v<T, SetProgramOption>)
          return reject("Wait for paused Z feedback before commanding machine motion.",
                        RefusalMessage{"messages.policy.waitPausedZFeedback"});
        if (s.paused_z && s.paused_z->displaced && s.interp != InterpState::paused &&
            !std::is_same_v<T, SetMachine> && !std::is_same_v<T, Quit>)
          return reject(
              "Z is still lifted. Turn machine power off and on to clear the offset here.",
              RefusalMessage{"messages.policy.zStillLifted"});

        if constexpr (std::is_same_v<T, Run> || std::is_same_v<T, Step> ||
                      std::is_same_v<T, Resume> || std::is_same_v<T, PlanRestart> ||
                      std::is_same_v<T, StartRestart>) {
          if (s.unresolved_legacy_offset)
            return reject("Use the previous CoffeeCNC version to clear its temporary Z offset, "
                          "then restart CoffeeCNC.",
                          RefusalMessage{"messages.policy.legacyOffset"});
        }

        if constexpr (std::is_same_v<T, Quit>) {
          if (s.interp != InterpState::idle)
            return reject("Stop the program before quitting.",
                          RefusalMessage{"messages.policy.stopBeforeQuit"});
          return allow();
        }

        // LinuxCNC must not run anything while its files are replaced, and the machine stays
        // off until it has started again with them.
        if constexpr (std::is_same_v<T, CheckRestore> || std::is_same_v<T, RestoreBackup>) {
          if (s.machine == MachineState::on)
            return reject("Switch the machine off or press E-stop before a restore.",
                          RefusalMessage{"messages.policy.restoreMachineOn"});
          if (s.interp != InterpState::idle || homing(s))
            return reject("Stop the program before a restore.",
                          RefusalMessage{"messages.policy.restoreProgram"});
          return allow();
        }

        if constexpr (std::is_same_v<T, SetMachine>) {
          if (c.state != MachineState::on)
            return reject("Unknown machine state.",
                          RefusalMessage{"messages.policy.unknownMachineState"});
          if (s.machine == MachineState::estop)
            return reject("Clear E-stop before enabling power.",
                          RefusalMessage{"messages.policy.clearEstopFirst"});
          return allow();
        } else if constexpr (std::is_same_v<T, Home>) {
          if (c.axis && !axis_state(s, *c.axis))
            return reject("Axis is not configured.",
                          RefusalMessage{"messages.policy.axisNotConfigured"});
          if (!std::ranges::any_of(s.axes, [](const auto& a) -> auto { return a.enabled; }))
            return reject("No axes are configured.",
                          RefusalMessage{"messages.policy.noAxesConfigured"});
          auto decision = idle(s, TaskMode::manual);
          if (!decision.allowed)
            return decision;
          // The home move would search for a mark it cannot find from where the axis stands.
          for (const Axis axis : {Axis::x, Axis::y, Axis::z}) {
            const auto* a = axis_state(s, axis);
            if (a != nullptr && (!c.axis || *c.axis == axis) && a->reference_mark.has_value() &&
                !*a->reference_mark)
              return reject(
                  std::format("Move {} across its reference mark first: homing cannot find it yet.",
                              letter(axis)),
                  RefusalMessage{"messages.policy.crossReferenceMark",
                                 {{"axis", std::string(1, letter(axis))}}});
          }
          return decision;
        } else if constexpr (std::is_same_v<T, Jog> || std::is_same_v<T, JogStep>) {
          const auto* a = axis_state(s, c.axis);
          if (!a)
            return reject("Axis is not configured.",
                          RefusalMessage{"messages.policy.axisNotConfigured"});
          if (!between(c.velocity, 0, limits_.max_jog_velocity) || c.velocity == 0)
            return reject("Jog velocity is outside the configured range.",
                          RefusalMessage{"messages.policy.jogVelocityRange"});
          if (const double ceiling =
                  limits_.max_axis_jog_velocity.at(axis_index(c.axis).value_or(0));
              c.velocity > ceiling)
            return reject(std::format("Jog velocity is above the {} axis limit of {} mm/min.",
                                      letter(c.axis), ceiling),
                          RefusalMessage{"messages.policy.jogVelocityAxisLimit",
                                         {{"axis", std::string(1, letter(c.axis))},
                                          {"limit", std::format("{}", ceiling)}}});
          if (s.machine != MachineState::on || s.interp != InterpState::idle || homing(s))
            return reject("Jogging needs machine power, an idle interpreter and no active homing.",
                          RefusalMessage{"messages.policy.jogNeedsIdle"});
          if (a->on_hard_limit)
            return reject("A hard limit is active; limit recovery is not supported yet.",
                          RefusalMessage{"messages.policy.hardLimitActive"});
          if constexpr (std::is_same_v<T, Jog>) {
            if (c.direction != -1 && c.direction != 1)
              return reject("Invalid jog direction.",
                            RefusalMessage{"messages.policy.invalidJogDirection"});
          } else {
            if (!std::isfinite(c.distance) || c.distance == 0)
              return reject("Invalid jog distance.",
                            RefusalMessage{"messages.policy.invalidJogDistance"});
            if (!s.in_position)
              return reject("Wait for motion to stop before an incremental jog.",
                            RefusalMessage{"messages.policy.waitBeforeIncrement"});
          }
          if (a->homed) {
            if (!std::isfinite(a->machine) || !std::isfinite(a->min_limit) ||
                !std::isfinite(a->max_limit) || a->min_limit >= a->max_limit)
              return reject("Axis position or limits are invalid.",
                            RefusalMessage{"messages.policy.axisLimitsInvalid"});
            if constexpr (std::is_same_v<T, Jog>) {
              const double limit = c.direction < 0 ? a->min_limit : a->max_limit;
              if ((a->machine - limit) * c.direction > 0 ||
                  (!increment_in_progress && a->machine == limit))
                return reject("Jog points beyond the configured travel.",
                              RefusalMessage{"messages.policy.jogBeyondTravel"});
            } else {
              if (!between(a->machine + c.distance, a->min_limit, a->max_limit))
                return reject("Incremental jog exceeds configured travel.",
                              RefusalMessage{"messages.policy.incrementBeyondTravel"});
            }
          }
          if (const int motors = limits_.axis_motors.at(axis_index(c.axis).value_or(0));
              !homed(s) && motors > 1)
            return reject(
                std::format("Home the machine before jogging {}: {} motors drive it.",
                            letter(c.axis), motors == 2 ? "two" : std::to_string(motors)),
                RefusalMessage{"messages.policy.homeBeforeGantryJog",
                               {{"axis", std::string(1, letter(c.axis))},
                                {"motors", motors == 2 ? "two" : std::to_string(motors)}}});
          return allow(TaskMode::manual, homed(s) ? JogSpace::axis : JogSpace::joint);
        } else if constexpr (std::is_same_v<T, Mdi>) {
          if (!valid_text(c.text, 254))
            return reject("MDI requires one nonempty block of at most 254 bytes.",
                          RefusalMessage{"messages.policy.mdiInvalid"});
          return idle(s, TaskMode::mdi, limits_.homing_required);
        } else if constexpr (std::is_same_v<T, OpenProgram>) {
          if (!valid_text(c.path, 254))
            return reject("Program path is empty, too long or contains control characters.",
                          RefusalMessage{"messages.policy.programPathInvalid"});
          if (s.interp != InterpState::idle || !s.in_position || homing(s))
            return reject("Stop execution before replacing the program.",
                          RefusalMessage{"messages.policy.stopBeforeReplace"});
          return allow(); // File containment/existence checks belong to the app's program store.
        } else if constexpr (std::is_same_v<T, Run>) {
          if (!s.program_loaded)
            return reject("Load a program first.",
                          RefusalMessage{"messages.policy.loadProgramFirst"});
          if (s.breakpoint_program)
            return reject("Wait for the original program to reopen after the breakpoint run.",
                          RefusalMessage{"messages.policy.waitBreakpointReopen"});
          return idle(s, TaskMode::automatic, limits_.homing_required);
        } else if constexpr (std::is_same_v<T, EditProgram>) {
          if (!s.program_loaded)
            return reject("Load a program first.",
                          RefusalMessage{"messages.policy.loadProgramFirst"});
          if (!valid_text(c.path, 254) || c.line < 1 || c.before.size() > 4096 || c.after.size() > 4096)
            return reject("The program edit is not valid.",
                          RefusalMessage{"messages.policy.editInvalid"});
          if (s.interp != InterpState::idle || !s.in_position || homing(s))
            return reject("Stop execution before editing the program.",
                          RefusalMessage{"messages.policy.stopBeforeEdit"});
          return allow();
        } else if constexpr (std::is_same_v<T, SaveProgram>) {
          if (!s.program_loaded)
            return reject("Load a program first.",
                          RefusalMessage{"messages.policy.loadProgramFirst"});
          if (s.interp != InterpState::idle || !s.in_position || homing(s))
            return reject("Stop the program before saving it.",
                          RefusalMessage{"messages.policy.stopBeforeSave"});
          return allow();
        } else if constexpr (std::is_same_v<T, PlanRestart> || std::is_same_v<T, StartRestart>) {
          if (s.breakpoint_program)
            return reject("Wait for the breakpoint run to finish.",
                          RefusalMessage{"messages.policy.waitBreakpointFinish"});
          if (!s.program_loaded)
            return reject("Load a program first.",
                          RefusalMessage{"messages.policy.loadProgramFirst"});
          const auto decision = idle(s, TaskMode::mdi, limits_.homing_required);
          return decision.allowed ? allow() : decision;
        } else if constexpr (std::is_same_v<T, Pause>) {
          if (s.machine != MachineState::on || s.mode == TaskMode::manual ||
              (s.interp != InterpState::reading && s.interp != InterpState::waiting))
            return reject("There is no executing program or MDI command to pause.",
                          RefusalMessage{"messages.policy.nothingToPause"});
          return allow();
        } else if constexpr (std::is_same_v<T, JogPausedZ>) {
          if (!s.paused_z)
            return reject("Paused Z jog is unavailable on this machine.",
                          RefusalMessage{"messages.policy.pausedZUnavailable"});
          if (s.machine != MachineState::on || s.interp != InterpState::paused ||
              s.mode != TaskMode::automatic)
            return reject("Pause a running program before jogging Z.",
                          RefusalMessage{"messages.policy.pauseBeforeZJog"});
          if (!homed(s) || homing(s))
            return reject("Home all axes before jogging Z while paused.",
                          RefusalMessage{"messages.policy.homeBeforePausedZ"});
          if (s.any_enabled_axis([](const auto& axis) { return axis.on_hard_limit; }))
            return reject("A travel limit blocks paused Z motion.",
                          RefusalMessage{"messages.policy.travelLimitBlocksZ"});
          const bool stopping_spindle =
              c.stop_spindle && c.distance > 0 && s.paused_z->spindle == PausedSpindle::can_stop;
          if (c.stop_spindle && !stopping_spindle)
            return reject("The paused spindle state changed. Review the controls and try again.",
                          RefusalMessage{"messages.policy.pausedSpindleChanged"});
          if (!stopping_spindle && !s.paused_z->reason.empty())
            return {false, {}, s.paused_z->reason, {}, s.paused_z->localization};
          if (!std::isfinite(s.paused_z->max_lift) ||
              !between(std::abs(c.distance), 0, s.paused_z->max_lift) || c.distance == 0)
            return reject("The paused Z jog distance is out of range.",
                          RefusalMessage{"messages.policy.pausedZDistance"});
          return allow();
        } else if constexpr (std::is_same_v<T, Resume>) {
          if (s.machine != MachineState::on || s.interp != InterpState::paused ||
              s.mode == TaskMode::manual || s.requested_tool)
            return reject("There is no paused execution ready to resume.",
                          RefusalMessage{"messages.policy.nothingToResume"});
          return allow();
        } else if constexpr (std::is_same_v<T, Step>) {
          if (s.paused_z && s.paused_z->spindle == PausedSpindle::stopping)
            return reject("Wait for the spindle to stop before continuing.",
                          RefusalMessage{"messages.policy.waitSpindleStop"});
          if (s.paused_z && s.paused_z->spindle == PausedSpindle::restore)
            return reject("Use Resume to restore the program spindle before continuing.",
                          RefusalMessage{"messages.policy.resumeRestoresSpindle"});
          if (s.paused_z && s.paused_z->displaced)
            return reject("Use Resume to return Z before continuing.",
                          RefusalMessage{"messages.policy.resumeReturnsZ"});
          if (s.breakpoint_program)
            return reject("Use Resume to continue the breakpoint run.",
                          RefusalMessage{"messages.policy.resumeBreakpoint"});
          if (!s.program_loaded || s.machine != MachineState::on || s.requested_tool)
            return reject("Single block needs a loaded program and no pending tool change.",
                          RefusalMessage{"messages.policy.singleBlockNeeds"});
          if (limits_.homing_required && !homed(s))
            return reject("Home all configured axes first.",
                          RefusalMessage{"messages.policy.homeAllFirst"});
          // Not needing homing never means stepping while an axis is homing.
          if (homing(s))
            return reject("Wait for homing to finish.",
                          RefusalMessage{"messages.policy.waitHoming"});
          if (s.interp == InterpState::paused && s.mode == TaskMode::automatic)
            return allow();
          return idle(s, TaskMode::automatic, limits_.homing_required);
        } else if constexpr (std::is_same_v<T, SetOverride>) {
          switch (c.which) {
          case Override::feed:
            return between(c.value, 0, limits_.max_feed_override)
                       ? allow()
                       : reject("Feed override is out of range.",
                                RefusalMessage{"messages.policy.feedOverrideRange"});
          case Override::rapid:
            return between(c.value, 0, 1)
                       ? allow()
                       : reject("Rapid override is out of range.",
                                RefusalMessage{"messages.policy.rapidOverrideRange"});
          case Override::velocity_limit:
            if (!limits_.max_trajectory_velocity)
              return reject("Maximum velocity control is unavailable on this machine.",
                            RefusalMessage{"messages.policy.velocityLimitUnavailable"});
            return between(c.value, 0, 1)
                       ? allow()
                       : reject("Maximum velocity limit is out of range.",
                                RefusalMessage{"messages.policy.velocityLimitRange"});
          case Override::spindle:
            return between(c.value, limits_.min_spindle_override, limits_.max_spindle_override)
                       ? allow()
                       : reject("Spindle override is out of range.",
                                RefusalMessage{"messages.policy.spindleOverrideRange"});
          }
          return reject("Unknown override.", RefusalMessage{"messages.policy.unknownOverride"});
        } else if constexpr (std::is_same_v<T, SetSpindle>) {
          if (c.direction != SpindleDirection::forward && c.direction != SpindleDirection::reverse)
            return reject("Unknown spindle direction.",
                          RefusalMessage{"messages.policy.unknownSpindleDirection"});
          if (!between(c.rpm, 0, limits_.max_spindle_rpm) || c.rpm == 0)
            return reject("Spindle speed is out of range.",
                          RefusalMessage{"messages.policy.spindleSpeedRange"});
          if (s.machine != MachineState::on || s.interp != InterpState::idle || homing(s))
            return reject(
                "Spindle start needs machine power, an idle interpreter and no active homing.",
                RefusalMessage{"messages.policy.spindleStartNeeds"});
          return allow(); // Spindle control does not require stopping an ongoing manual jog.
        } else if constexpr (std::is_same_v<T, SetCoolant>) {
          if (s.machine != MachineState::on)
            return reject("Machine power is off.",
                          RefusalMessage{"messages.policy.machinePowerOff"});
          return allow();
        } else if constexpr (std::is_same_v<T, SetProgramOption>) {
          if (c.option == ProgramOption::block_delete && s.breakpoint_program)
            return reject("Stop the breakpoint run before changing block delete.",
                          RefusalMessage{"messages.policy.stopBreakpointBlockDelete"});
          if (c.option != ProgramOption::optional_stop && c.option != ProgramOption::block_delete)
            return reject("Unknown program option.",
                          RefusalMessage{"messages.policy.unknownProgramOption"});
          if (c.option == ProgramOption::block_delete && s.interp != InterpState::idle)
            return reject("Stop execution before changing block delete.",
                          RefusalMessage{"messages.policy.stopBeforeBlockDelete"});
          return allow();
        } else if constexpr (std::is_same_v<T, SelectWcs>) {
          if (c.wcs < 1 || c.wcs > 9)
            return reject("Work coordinate system must be G54 through G59.3.",
                          RefusalMessage{"messages.policy.wcsRange"});
          return idle(s, TaskMode::mdi, limits_.homing_required);
        } else if constexpr (std::is_same_v<T, SetWorkOffset>) {
          if (s.requested_tool)
            return reject("Finish the tool change before editing work origins.",
                          RefusalMessage{"messages.policy.finishToolChangeOrigins"});
          if (c.wcs < 1 || c.wcs > 9 ||
              !std::ranges::all_of(c.offset, [](double value) -> bool { return std::isfinite(value); }) ||
              !axis_state(s, Axis::x) || !axis_state(s, Axis::y) || !axis_state(s, Axis::z))
            return reject("Provide G54 through G59.3 and finite XYZ offsets.",
                          RefusalMessage{"messages.policy.workOffsetInvalid"});
          return idle(s, TaskMode::mdi, limits_.homing_required);
        } else if constexpr (std::is_same_v<T, SetWorkRotation>) {
          if (s.requested_tool)
            return reject("Finish the tool change before editing work origins.",
                          RefusalMessage{"messages.policy.finishToolChangeOrigins"});
          if (c.wcs < 1 || c.wcs > 9 || !std::isfinite(c.degrees) ||
              std::abs(c.degrees) > max_work_rotation)
            return reject("Provide G54 through G59.3 and an XY rotation from -360 to 360 degrees.",
                          RefusalMessage{"messages.policy.rotationInvalid"});
          return idle(s, TaskMode::mdi, limits_.homing_required);
        } else if constexpr (std::is_same_v<T, ClearG92>) {
          if (s.requested_tool)
            return reject("Finish the tool change before clearing the G92/G52 shift.",
                          RefusalMessage{"messages.policy.finishToolChangeShift"});
          // G92.1 clears the A, B and C shift as well, which the panel neither shows in the
          // review nor records in the offset history: not offered on such a machine.
          if (std::ranges::any_of(s.rotary_axes, [](const AxisState& a) -> bool { return a.enabled; }))
            return reject("On a machine with A, B or C axes, clear the G92/G52 shift with G92.1 "
                          "in MDI: it clears their shift too.",
                          RefusalMessage{"messages.policy.clearShiftRotary"});
          return idle(s, TaskMode::mdi, limits_.homing_required);
        } else if constexpr (std::is_same_v<T, RestoreOffset>) {
          // G10 L2, G52 or G10 L1 through MDI: the conditions of the other offset edits.
          if (s.requested_tool)
            return reject("Finish the tool change before restoring offsets.",
                          RefusalMessage{"messages.policy.finishToolChangeRestore"});
          if (c.entry == 0)
            return reject("Choose an offset history entry.",
                          RefusalMessage{"messages.policy.chooseHistoryEntry"});
          return idle(s, TaskMode::mdi, limits_.homing_required);
        } else if constexpr (std::is_same_v<T, TouchOff>) {
          if (!axis_state(s, c.axis) || !std::isfinite(c.value))
            return reject("Invalid touch-off axis or value.",
                          RefusalMessage{"messages.policy.touchOffInvalid"});
          return idle(s, TaskMode::mdi, limits_.homing_required);
        } else if constexpr (std::is_same_v<T, ToolTouchOff>) {
          if (s.tool_in_spindle <= 0 || !std::isfinite(c.value))
            return reject("Load a tool and provide a finite touch-off value.",
                          RefusalMessage{"messages.policy.toolTouchOffInvalid"});
          return idle(s, TaskMode::mdi, limits_.homing_required);
        } else if constexpr (std::is_same_v<T, FixedSetterRequest>) {
          if (c.action == SetterAction::status)
            return allow();
          if (s.spindle_active || s.requested_tool || s.tool_in_spindle <= 0 ||
              c.tool != s.tool_in_spindle)
            return reject("Fixed setter requires the confirmed tool and a stopped spindle.",
                          RefusalMessage{"messages.policy.fixedSetterNeeds"});
          if (!std::ranges::all_of(c.start, [](double value) { return std::isfinite(value); }) ||
              !std::isfinite(c.gauge_width_mm))
            return reject("Probe positions and dimensions must be finite.",
                          RefusalMessage{"messages.policy.probeValuesFinite"});
          if (c.measure_only && (!is_material_probe(c.action) || c.action == SetterAction::ball ||
                                 c.action == SetterAction::apply_angle ||
                                 c.action == SetterAction::apply_measurement))
            return reject("This probe action always writes its result.",
                          RefusalMessage{"messages.policy.probeAlwaysWrites"});
          if (!std::ranges::all_of(s.axes, &AxisState::enabled) ||
              s.any_enabled_axis([](const auto& axis) { return axis.on_hard_limit; }))
            return reject("The probe requires configured axes and no active hard limit.",
                          RefusalMessage{"messages.policy.probeNeedsAxes"});
          return idle(s, TaskMode::mdi, limits_.homing_required);
        } else if constexpr (std::is_same_v<T, SetTool> || std::is_same_v<T, RemoveTool>) {
          if (s.interp != InterpState::idle || !s.in_position || s.requested_tool || homing(s))
            return reject("Stop execution and finish tool changes before editing tools.",
                          RefusalMessage{"messages.policy.stopBeforeToolEdit"});
          if constexpr (std::is_same_v<T, SetTool>) {
            if (c.tool.number <= 0 || c.tool.pocket < 0 || !std::isfinite(c.tool.length) ||
                !std::isfinite(c.tool.diameter) || c.tool.diameter < 0 ||
                c.tool.comment.size() > max_tool_comment_bytes ||
                c.tool.comment.find_first_of("\r\n") != std::string::npos ||
                c.tool.comment.find('\0') != std::string::npos)
              return reject("Invalid tool table row.",
                            RefusalMessage{"messages.policy.invalidToolRow"});
          } else if (c.number <= 0)
            return reject("Invalid tool number.",
                          RefusalMessage{"messages.policy.invalidToolNumber"});
          return allow();
        } else if constexpr (std::is_same_v<T, ToolChanged>) {
          if (s.machine != MachineState::on || !s.requested_tool || *s.requested_tool < 0)
            return reject("No manual tool change is awaiting confirmation.",
                          RefusalMessage{"messages.policy.noToolChangePending"});
          if (c.measure_request &&
              (*c.measure_request != s.tool_request || *s.requested_tool == 0 ||
               s.mode != TaskMode::automatic || !s.tool_change_line || s.breakpoint_program ||
               s.spindle_active || !s.in_position))
            return reject(
                "The program tool change or its stationary position could not be verified.",
                RefusalMessage{"backend.toolChangeMeasurement.verifyStationaryChange"});
          return allow(); // Tool 0 may be requested to unload the spindle.
        }
        return reject("Unsupported command.", RefusalMessage{"messages.policy.unsupportedCommand"});
      },
      command);
}

} // namespace coffeecnc
