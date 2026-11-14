#ifndef COFFEECNC_CORE_CONTROLLER_PORT_HPP
#define COFFEECNC_CORE_CONTROLLER_PORT_HPP

#include <coffeecnc/core/command_policy.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace coffeecnc {

enum class ActionStatus : std::uint8_t {
  accepted,  // the controller took the command; motion it starts is read from the snapshot
  failed,    // the controller reported an error
  timed_out, // the controller did not answer
};

struct ActionResult {
  ActionStatus status{ActionStatus::accepted};
  // In words for the operator. Empty when accepted.
  std::string reason;
};

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

  [[nodiscard]] virtual auto set_machine_state(MachineState state) -> ActionResult = 0;
  // Accepted only once the controller reports the mode; the next snapshot shows it.
  [[nodiscard]] virtual auto set_task_mode(TaskMode mode) -> ActionResult = 0;
  // No axis homes all configured axes.
  [[nodiscard]] virtual auto home(std::optional<Axis> axis) -> ActionResult = 0;

  // Runs until stop_jog. Axis space needs teleop; the port enables it.
  [[nodiscard]] virtual auto start_jog(Axis axis, int direction, double velocity, JogSpace space)
      -> ActionResult = 0;
  // Stops a jog in whichever space it was started. Valid with no jog running.
  [[nodiscard]] virtual auto stop_jog(Axis axis) -> ActionResult = 0;
  // Signed distance.
  [[nodiscard]] virtual auto jog_increment(Axis axis, double distance, double velocity,
                                           JogSpace space) -> ActionResult = 0;

  // One block. Accepted once a following block may be submitted.
  [[nodiscard]] virtual auto mdi(std::string_view text) -> ActionResult = 0;
  [[nodiscard]] virtual auto open_program(std::string_view path) -> ActionResult = 0;
  // From the first line of the loaded program.
  [[nodiscard]] virtual auto run() -> ActionResult = 0;
  [[nodiscard]] virtual auto pause() -> ActionResult = 0;
  [[nodiscard]] virtual auto resume() -> ActionResult = 0;
  [[nodiscard]] virtual auto step() -> ActionResult = 0;
  [[nodiscard]] virtual auto abort() -> ActionResult = 0;

  [[nodiscard]] virtual auto set_override(Override which, double value) -> ActionResult = 0;
  // The speed is ignored for SpindleDirection::stopped.
  [[nodiscard]] virtual auto set_spindle(SpindleDirection direction, double rpm)
      -> ActionResult = 0;
  [[nodiscard]] virtual auto set_coolant(bool flood, bool mist) -> ActionResult = 0;
  [[nodiscard]] virtual auto set_program_option(ProgramOption option, bool on) -> ActionResult = 0;

  // The operator has put this tool, the one the controller asked for, in the spindle. Fails
  // when the controller is not, or no longer, asking for it.
  [[nodiscard]] virtual auto tool_changed(int tool) -> ActionResult = 0;
  // Replaces the row with this tool number, or adds it.
  [[nodiscard]] virtual auto set_tool(const Tool& tool) -> ActionResult = 0;
  [[nodiscard]] virtual auto remove_tool(int number) -> ActionResult = 0;

protected:
  ControllerPort(const ControllerPort&) = default;
  ControllerPort(ControllerPort&&) = default;
  auto operator=(const ControllerPort&) -> ControllerPort& = default;
  auto operator=(ControllerPort&&) -> ControllerPort& = default;
};

} // namespace coffeecnc

#endif // COFFEECNC_CORE_CONTROLLER_PORT_HPP
