#ifndef COFFEECNC_CORE_CONTROLLER_PORT_HPP
#define COFFEECNC_CORE_CONTROLLER_PORT_HPP

#include <coffeecnc/core/command_policy.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace coffeecnc {

struct RestartPoint;
struct PreviewInputs;

enum class ActionStatus : std::uint8_t {
  accepted,  // the controller took the command; motion it starts is read from the snapshot
  failed,    // the controller reported an error
  timed_out, // the controller did not answer
  // The caller withdrew the action and nothing of it was written. An action withdrawn after one
  // of its commands went out is `failed`, with a reason that says what was sent and what not.
  cancelled,
};

struct ActionResult {
  ActionStatus status{ActionStatus::accepted};
  // In words for the operator. Empty when accepted.
  std::string reason;
  std::optional<MessageKey> localization{};
};

// The result of an E-stop request, by the machine state the controller reports after the
// command: the reported state decides, never the command's own status or which step of it
// failed. A state confirms or names anything only when it is evidence: from a status of a
// live link (`link`) that the controller published after it took the command
// (`after_command`). A status buffer keeps its last contents when the controller stops: an
// E-stop state still readable there says nothing. `reported` is empty when there is no status.
// Not confirmed, the reason says what the controller reports instead, and nothing about why
// (wiring is not visible in a status).
[[nodiscard]] inline auto estop_request_result(std::optional<MachineState> reported, Link link,
                                               bool after_command) -> ActionResult {
  if (!reported || link != Link::live || !after_command)
    return {ActionStatus::failed, "LinuxCNC's status cannot be read; E-stop was not confirmed.",
            MessageKey{"backend.controllerPort.estopStatus"}};
  switch (*reported) {
  case MachineState::estop:
    return {};
  case MachineState::off:
    return {ActionStatus::failed, "LinuxCNC reports machine off; E-stop was not confirmed.",
            MessageKey{"backend.controllerPort.estopMachineOff"}};
  case MachineState::on:
    break;
  }
  return {ActionStatus::failed, "LinuxCNC still reports machine on; E-stop was not confirmed.",
          MessageKey{"backend.controllerPort.estopMachineOn"}};
}

// The tool offset in effect and the table length of the tool in the spindle, as the controller
// reports them. LinuxCNC reports G43 for any nonzero tool offset and G49 for none
// (rs274ngc/interp_write.cc): G43.1 and G43.2 are not told apart from G43, and the H word of a
// G43 is not reported. The values are all there is.
struct ToolLengthInEffect {
  std::array<double, 3> offset{}; // mm, the X, Y, Z tool offset applied
  // mm, the tool table length of the tool in the spindle, when it has a row.
  std::optional<double> loaded_length;
};

// The MDI block that must follow a G10 L1 that writes a new length for tool `written`, so that
// the offset in effect follows the table; none when nothing must. LinuxCNC applies a table
// length only at G43: G10 L1 alone changes the table and leaves the offset in effect.
//
// `G43 H<tool>` follows when the tool is the one in the spindle and the offset in effect is its
// table length: a nonzero Z equal to it within 0.001 mm, and no X or Y offset. Any other offset
// is kept: G49 (zero), a G43 of another H, a G43.1 or G43.2 offset that is not that length. A
// G43.1 or G43.2 offset, or a G43 of another H, that equals the length is taken for a G43 of the
// loaded tool and follows the table. A zero length applied with G43 reads as G49 and is kept.
[[nodiscard]] inline auto after_tool_length_write(int written, int loaded,
                                                  const ToolLengthInEffect& effect)
    -> std::optional<std::string> {
  constexpr double same_length = 0.001; // mm
  const auto zero = [](double value) { return std::abs(value) <= 1e-9; };
  if (loaded <= 0 || written != loaded || !effect.loaded_length || zero(effect.offset[2]) ||
      !zero(effect.offset[0]) || !zero(effect.offset[1]) ||
      !(std::abs(effect.offset[2] - *effect.loaded_length) <= same_length))
    return std::nullopt;
  return "G43 H" + std::to_string(loaded);
}

// Asked by a port while it carries out an action: is the action still wanted? The session
// answers for the request behind it: false once its page has ended or an abort, E-stop or power
// off was asked for, and for a jog start or increment also once jogs were stopped. Cheap, and it
// may be asked while the port holds its own locks, so it must not call back into the port.
// Synchronous only: a port may call it during the action it was passed to, on the thread of that
// call, and must neither keep it nor call it after the action has returned.
using Wanted = std::function<bool()>;

// An operator error or message the controller published.
struct ControllerMessage {
  bool error{};
  std::string text;
  std::optional<MessageKey> localization{};
};

// The machine as the session acts on it: one call per kind of action CommandPolicy can allow.
// Callers must have an allowing Decision for a fresh snapshot first; the port validates nothing.
//
// A real implementation sends an NML message and blocks until LinuxCNC has answered or its own
// timeout has passed. Exactly one thread, the runtime's worker, calls every member: it is the
// only writer of commands. Implementations need not be thread-safe.
//
// Cancellation. Every action that is not a stop takes a `Wanted`. An implementation asks it
// after each step of its own that can block and, last, immediately before each command it
// writes; an action that writes several commands asks before each. Once the answer is no,
// nothing further is written; the result is `cancelled` when nothing was written at all and
// `failed`, saying what was sent, when the action was withdrawn half way. The actions that only
// ever stop something take none and cannot be withdrawn: stop_jog and abort; and the stopping
// values of the others are carried out whatever the answer: assert E-stop, spindle stop and
// both coolants off. The protocol's SetMachine off also serves as E-stop reset and checks Wanted.
//
// What the checks cannot do. The answer can change between the last question and the write:
// the session does not keep its state locked while a port writes, and its control thread is
// never made to wait for a port. And a command that is being written, or was written, cannot be
// recalled. What then ends such a command depends on the decision that came too late for it:
//
//   abort, E-stop, power off   also a command to the controller, written behind the late one;
//                              the controller executes commands in order, so it ends it.
//   jog release, stop-all,     stop jogs only: the session's jog-stop level covers a late jog
//   lease expiry, link loss,   start or increment. They are not meant to end anything else.
//   machine off, page end      In particular the end of a page is not an abort: a program, an
//                              MDI block, homing or a spindle start that was committed before
//                              it carries on, as a running job does across a page reload. What
//                              a page end withdraws is only what was not yet written.
//
// Lengths are millimetres, velocities mm/min, spindle speed rpm, overrides factors (1.0 is 100 %).
class ControllerPort {
public:
  ControllerPort() = default;
  virtual ~ControllerPort() = default;

  // Observed controller state, never reconstructed from commands sent through this port.
  [[nodiscard]] virtual auto snapshot() -> ControllerState = 0;
  // Operator messages published since the last call, oldest first; those that became the reason
  // of a failed action are not among them. Called whenever the worker has nothing else to do.
  [[nodiscard]] virtual auto take_messages() -> std::vector<ControllerMessage> = 0;
  // The worker may perform deferred startup work between operator requests.
  virtual void idle_maintenance(const Wanted&) {}

  [[nodiscard]] virtual auto set_machine_state(MachineState state, const Wanted& wanted)
      -> ActionResult = 0;
  // Stop-only power removal for Quit. Must never reset an E-stop, even if one arrives
  // during the call. Accepted only after live status confirms that machine power is off.
  [[nodiscard]] virtual auto power_off() -> ActionResult = 0;
  // Accepted only once the controller reports the mode; the next snapshot shows it.
  [[nodiscard]] virtual auto set_task_mode(TaskMode mode, const Wanted& wanted) -> ActionResult = 0;
  // No axis homes all configured axes.
  [[nodiscard]] virtual auto home(std::optional<Axis> axis, const Wanted& wanted)
      -> ActionResult = 0;

  // Runs until stop_jog. Axis space needs teleop; the port enables it.
  [[nodiscard]] virtual auto start_jog(Axis axis, int direction, double velocity, JogSpace space,
                                       const Wanted& wanted) -> ActionResult = 0;
  // Stops a jog in whichever space it was started. Valid with no jog running. Never withdrawn.
  [[nodiscard]] virtual auto stop_jog(Axis axis) -> ActionResult = 0;
  // Signed distance.
  [[nodiscard]] virtual auto jog_increment(Axis axis, double distance, double velocity,
                                           JogSpace space, const Wanted& wanted)
      -> ActionResult = 0;

  // One block. Accepted once a following block may be submitted.
  [[nodiscard]] virtual auto mdi(std::string_view text, const Wanted& wanted) -> ActionResult = 0;
  [[nodiscard]] virtual auto open_program(std::string_view path, const Wanted& wanted)
      -> ActionResult = 0;
  // From the first line of the loaded program.
  [[nodiscard]] virtual auto run(const Wanted& wanted) -> ActionResult = 0;
  [[nodiscard]] virtual auto validate_program_inputs(const PreviewInputs&) -> LocalizedReason {
    return {};
  }
  // Starts only the checked file, even if task has an older descriptor for the same path.
  [[nodiscard]] virtual auto run_checked(const PreviewInputs&, const Wanted& current,
                                         const Wanted& wanted) -> ActionResult {
    if (!current())
      return {ActionStatus::failed, "The program check changed. Review the current results.",
              MessageKey{"messages.session.programCheckChanged"}};
    return run(wanted);
  }
  [[nodiscard]] virtual auto run_to_breakpoint(const PreviewInputs&, std::string_view, int,
                                               const Wanted&, const Wanted&) -> ActionResult {
    return {ActionStatus::failed, "Program breakpoints are unavailable on this controller.",
            MessageKey{"backend.controllerPort.breakpointsUnavailable"}};
  }
  // Validates the approach without sending controller commands and updates its clearance.
  [[nodiscard]] virtual auto validate_restart(RestartPoint&, const PreviewInputs&)
      -> LocalizedReason {
    return {"Restart from line is unavailable on this controller.",
            MessageKey{"backend.controllerPort.restartUnavailable"}};
  }
  // First call may stop at the normal tool-change prompt. The second follows its confirmation.
  [[nodiscard]] virtual auto start_restart(const RestartPoint&, const PreviewInputs&, bool,
                                           const Wanted&, const Wanted&) -> ActionResult {
    return {ActionStatus::failed, "Restart from line is unavailable on this controller.",
            MessageKey{"backend.controllerPort.restartUnavailable"}};
  }
  [[nodiscard]] virtual auto pause(const Wanted& wanted) -> ActionResult = 0;
  [[nodiscard]] virtual auto jog_paused_z(double, bool, const Wanted&) -> ActionResult {
    return {ActionStatus::failed, "Paused Z jog is unavailable on this controller.",
            MessageKey{"backend.controllerPort.pausedZUnavailable"}};
  }
  [[nodiscard]] virtual auto resume(const Wanted& wanted) -> ActionResult = 0;
  [[nodiscard]] virtual auto step(const Wanted& wanted) -> ActionResult = 0;
  [[nodiscard]] virtual auto abort() -> ActionResult = 0;

  [[nodiscard]] virtual auto set_override(Override which, double value, const Wanted& wanted)
      -> ActionResult = 0;
  // The speed is ignored for SpindleDirection::stopped.
  [[nodiscard]] virtual auto set_spindle(SpindleDirection direction, double rpm,
                                         const Wanted& wanted) -> ActionResult = 0;
  [[nodiscard]] virtual auto set_coolant(bool flood, bool mist, const Wanted& wanted)
      -> ActionResult = 0;
  [[nodiscard]] virtual auto set_program_option(ProgramOption option, bool on, const Wanted& wanted)
      -> ActionResult = 0;

  // The operator has put this tool in the spindle, as the controller asked with the request
  // that a snapshot showed as ControllerState::tool_request. Fails when that request is gone or
  // was replaced, also by a new one for the same tool.
  [[nodiscard]] virtual auto tool_changed(int tool, std::uint64_t request, const Wanted& wanted)
      -> ActionResult = 0;
  // Stops the exact AUTO tool-change request without acknowledging it and registers the
  // installed tool in idle MDI. This does not move the machine or apply compensation.
  [[nodiscard]] virtual auto prepare_tool_measurement(int, std::uint64_t, int, const RestartPoint&,
                                                      const PreviewInputs&, const Wanted&) -> ActionResult {
    return {ActionStatus::failed, "Measuring during a program tool change is unavailable.",
            MessageKey{"backend.toolChangeMeasurement.unavailable"}};
  }
  // The tool length offset in effect, read from the controller's status and tool table. A port
  // that cannot tell reports none.
  [[nodiscard]] virtual auto tool_length_in_effect()
      -> std::expected<ToolLengthInEffect, LocalizedReason> {
    return ToolLengthInEffect{};
  }
  // Replaces the row with this tool number, or adds it.
  [[nodiscard]] virtual auto set_tool(const Tool& tool, const Wanted& wanted) -> ActionResult = 0;
  [[nodiscard]] virtual auto remove_tool(int number, const Wanted& wanted) -> ActionResult = 0;

  [[nodiscard]] virtual auto fixed_setter_available() const -> bool { return false; }
  [[nodiscard]] virtual auto fixed_setter(const FixedSetterRequest&, SetterEpoch, const Wanted&)
      -> std::expected<SetterState, LocalizedReason> {
    return std::unexpected(LocalizedReason{"The fixed setter is not configured.",
                                           MessageKey{"backend.nmlPort.setterNotConfigured"}});
  }

protected:
  ControllerPort(const ControllerPort&) = default;
  ControllerPort(ControllerPort&&) = default;
  auto operator=(const ControllerPort&) -> ControllerPort& = default;
  auto operator=(ControllerPort&&) -> ControllerPort& = default;
};

} // namespace coffeecnc

#endif // COFFEECNC_CORE_CONTROLLER_PORT_HPP
