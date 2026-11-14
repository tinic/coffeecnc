#ifndef COFFEECNC_CORE_CONTROLLER_PORT_HPP
#define COFFEECNC_CORE_CONTROLLER_PORT_HPP

#include <coffeecnc/core/command_policy.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace coffeecnc {

enum class ActionStatus : std::uint8_t {
  accepted,  // the controller took the command; motion it starts is read from the snapshot
  failed,    // the controller reported an error
  timed_out, // the controller did not answer
  cancelled, // the caller withdrew the action before its next command was written
};

struct ActionResult {
  ActionStatus status{ActionStatus::accepted};
  // In words for the operator. Empty when accepted.
  std::string reason;
};

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
// nothing further is written and the result is `cancelled`. The actions that only ever stop
// something take none and cannot be withdrawn: stop_jog and abort; and the stopping values of
// the others are carried out whatever the answer: E-stop and power off, spindle stop, both
// coolants off.
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

  [[nodiscard]] virtual auto set_machine_state(MachineState state, const Wanted& wanted)
      -> ActionResult = 0;
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
  [[nodiscard]] virtual auto pause(const Wanted& wanted) -> ActionResult = 0;
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
  // Replaces the row with this tool number, or adds it.
  [[nodiscard]] virtual auto set_tool(const Tool& tool, const Wanted& wanted) -> ActionResult = 0;
  [[nodiscard]] virtual auto remove_tool(int number, const Wanted& wanted) -> ActionResult = 0;

protected:
  ControllerPort(const ControllerPort&) = default;
  ControllerPort(ControllerPort&&) = default;
  auto operator=(const ControllerPort&) -> ControllerPort& = default;
  auto operator=(ControllerPort&&) -> ControllerPort& = default;
};

} // namespace coffeecnc

#endif // COFFEECNC_CORE_CONTROLLER_PORT_HPP
