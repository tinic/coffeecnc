#include <coffeecnc/core/command_policy.hpp>
#include <coffeecnc/core/controller_port.hpp>
#include <coffeecnc/core/jog_lease.hpp>
#include <coffeecnc/core/jog_stop_report.hpp>
#include <coffeecnc/core/task_heartbeat.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace coffeecnc;
using namespace std::chrono_literals;

namespace {
int checks = 0;
auto simulator_limits() -> Limits { return {3000, 1.2, 0.5, 1.2, 24000}; }
void require(bool condition, const std::string& message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}
auto ready() -> ControllerState {
  ControllerState s;
  s.link = Link::live;
  s.machine = MachineState::on;
  s.program_loaded = true;
  for (auto& a : s.axes) {
    a.enabled = true;
    a.homed = true;
    a.min_limit = -1000;
    a.max_limit = 1000;
  }
  return s;
}
void execution_gates() {
  CommandPolicy policy{simulator_limits()};
  auto s = ready();
  auto d = policy.evaluate(Run{}, s);
  require(d.allowed && d.required_mode == TaskMode::automatic,
          "run requests AUTO after validation");
  s.axes[2].homed = false;
  require(!policy.evaluate(Run{}, s).allowed, "unhomed run rejected");
  require(!policy.evaluate(Mdi{"G0 X10"}, s).allowed, "unhomed MDI rejected");
  require(policy.evaluate(Jog{Axis::z, -1, 100, true}, s).allowed,
          "manual pre-home jog can locate machine");
  s = ready();
  s.interp = InterpState::reading;
  s.mode = TaskMode::automatic;
  require(!policy.evaluate(TouchOff{Axis::x, 0}, s).allowed, "cannot change offsets while reading");
  require(!policy.evaluate(Mdi{"G0 X10"}, s).allowed, "cannot switch busy AUTO into MDI");
  require(!policy.evaluate(OpenProgram{"part.ngc"}, s).allowed, "cannot replace executing program");
  require(policy.evaluate(Pause{}, s).allowed, "can hold executing program");
  s.interp = InterpState::paused;
  s.in_position = false;
  require(policy.evaluate(Step{}, s).allowed, "mid-move feed hold can single-step remaining block");
  require(policy.evaluate(Resume{}, s).allowed, "paused program can resume");
  s.requested_tool = 2;
  require(!policy.evaluate(Resume{}, s).allowed, "tool handshake is not ordinary resume");
  require(!policy.evaluate(Step{}, s).allowed, "single block does not bypass tool handshake");
  require(policy.evaluate(ToolChanged{}, s).allowed, "requested tool change can be acknowledged");
  s.requested_tool = 0;
  require(policy.evaluate(ToolChanged{}, s).allowed, "tool zero is a valid unload request");
}
void work_origin_gates() {
  CommandPolicy policy{simulator_limits()};
  const SetWorkOffset command{2, {25.4, -50.8, 0}};
  auto s = ready();
  const auto allowed = policy.evaluate(command, s);
  require(allowed.allowed && allowed.required_mode == TaskMode::mdi, "Work origins use MDI");
  for (int scenario = 0; scenario < 8; ++scenario) {
    s = ready();
    switch (scenario) {
    case 0: s.link = Link::stale; break;
    case 1: s.machine = MachineState::off; break;
    case 2: s.interp = InterpState::reading; break;
    case 3: s.in_position = false; break;
    case 4: s.requested_tool = 2; break;
    case 5: s.axes[2].homing = true; break;
    case 6: s.axes[2].homed = false; break;
    case 7: s.axes[2].enabled = false; break;
    }
    require(!policy.evaluate(command, s).allowed, "Work origin write respects execution gates");
  }
  s = ready();
  for (const int wcs : {0, 10})
    require(!policy.evaluate(SetWorkOffset{wcs, {0, 0, 0}}, s).allowed, "No implicit/invalid WCS");
  for (const double value : {std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN()})
    require(!policy.evaluate(SetWorkOffset{1, {0, value, 0}}, s).allowed, "No nonfinite origins");
}
// A machine LinuxCNC runs unhomed ([TRAJ] NO_FORCE_HOMING): execution is allowed, jog rules stay.
void homing_capability() {
  const Limits unforced{3000, {3000, 3000, 3000}, 1.2, 0.5, 1.2, 24000, false};
  CommandPolicy forced{simulator_limits()};
  CommandPolicy policy{unforced};
  auto s = ready();
  for (auto& a : s.axes)
    a.homed = false;
  s.tool_in_spindle = 1;
  const auto mdi = policy.evaluate(Mdi{"G0 X10"}, s);
  require(mdi.allowed && mdi.required_mode == TaskMode::mdi, "unhomed MDI allowed");
  const auto run = policy.evaluate(Run{}, s);
  require(run.allowed && run.required_mode == TaskMode::automatic, "unhomed run allowed");
  require(policy.evaluate(Step{}, s).allowed, "unhomed single block allowed");
  // Permission to work unhomed is not permission to move while an axis is homing.
  auto paused_homing = s;
  paused_homing.mode = TaskMode::automatic;
  paused_homing.interp = InterpState::paused;
  require(policy.evaluate(Step{}, paused_homing).allowed, "unhomed step from a pause allowed");
  paused_homing.axes[1].homing = true;
  const auto stepping = policy.evaluate(Step{}, paused_homing);
  require(!stepping.allowed && stepping.reason == "Wait for homing to finish.",
          "no step from a pause while an axis is homing");
  auto idle_homing = s;
  idle_homing.axes[2].homing = true;
  require(!policy.evaluate(Step{}, idle_homing).allowed &&
              !policy.evaluate(Run{}, idle_homing).allowed &&
              !policy.evaluate(Mdi{"G0 X1"}, idle_homing).allowed,
          "no step, run or MDI while an axis is homing, homing required or not");
  require(policy.evaluate(SelectWcs{2}, s).allowed, "unhomed WCS selection allowed");
  require(policy.evaluate(TouchOff{Axis::x, 0}, s).allowed, "unhomed touch-off allowed");
  require(policy.evaluate(ToolTouchOff{0}, s).allowed, "unhomed tool touch-off allowed");
  for (const Command& command :
       {Command{Mdi{"G0 X10"}}, Command{Run{}}, Command{Step{}}, Command{SelectWcs{2}},
        Command{TouchOff{Axis::x, 0}}, Command{ToolTouchOff{0}}})
    require(forced.evaluate(command, s).reason == "Home all configured axes first.",
            "the same command needs homing where LinuxCNC forces it");

  require(policy.evaluate(Jog{Axis::x, 1, 100, true}, s).jog_space == JogSpace::joint,
          "jogs stay joint jogs until every axis is homed");
  s.axes[0].machine = 5000;
  require(policy.evaluate(Jog{Axis::x, 1, 100, true}, s).allowed,
          "an unhomed axis has no soft limit to check");
  s.axes[0].homed = true;
  require(!policy.evaluate(Jog{Axis::x, 1, 100, true}, s).allowed,
          "a homed axis keeps its soft limit");
  require(policy.evaluate(Jog{Axis::x, -1, 100, true}, s).jog_space == JogSpace::joint,
          "partial homing is still joint space");

  s = ready();
  for (auto& a : s.axes)
    a.homed = false;
  s.axes[1].homing = true;
  require(!policy.evaluate(Mdi{"G0 X10"}, s).allowed, "no MDI while an axis is homing");
  require(!policy.evaluate(Run{}, s).allowed, "no run while an axis is homing");
  s.axes[1].homing = false;
  s.machine = MachineState::off;
  require(!policy.evaluate(Mdi{"G0 X10"}, s).allowed, "unhomed MDI still needs machine power");
  s.machine = MachineState::on;
  s.program_loaded = false;
  require(!policy.evaluate(Run{}, s).allowed && !policy.evaluate(Step{}, s).allowed,
          "unhomed run and step still need a program");
}
// X and Y 1500 mm/min, Z 750 mm/min, 1500 mm/min overall: the G0759's values.
void axis_jog_ceilings() {
  CommandPolicy policy{Limits{1500, {1500, 1500, 750}, 2.0, 0.5, 1.0, 1500, false}};
  const auto s = ready();
  require(policy.evaluate(Jog{Axis::x, 1, 1500, true}, s).allowed, "X jogs at its ceiling");
  require(policy.evaluate(Jog{Axis::z, 1, 750, true}, s).allowed, "Z jogs at its ceiling");
  const auto z = policy.evaluate(Jog{Axis::z, 1, 750.5, true}, s);
  require(!z.allowed && z.reason == "Jog velocity is above the Z axis limit of 750 mm/min.",
          "a Z jog above its ceiling names the axis and the limit");
  const auto step = policy.evaluate(JogStep{Axis::z, -1, 1500}, s);
  require(!step.allowed && step.reason == "Jog velocity is above the Z axis limit of 750 mm/min.",
          "a Z step above its ceiling names the axis and the limit");
  require(policy.evaluate(JogStep{Axis::y, -1, 1500}, s).allowed, "Y steps at its ceiling");
  require(policy.evaluate(Jog{Axis::x, 1, 1501, true}, s).reason ==
              "Jog velocity is outside the configured range.",
          "the global maximum still applies");
  require(policy.evaluate(Jog{Axis::z, 1, 0, false}, s).allowed,
          "a release is never a velocity question");
  // An axis ceiling above the global maximum cannot raise it.
  CommandPolicy wide{Limits{1500, {3000, 1500, 750}, 2.0, 0.5, 1.0, 1500, true}};
  require(!wide.evaluate(Jog{Axis::x, 1, 2000, true}, s).allowed,
          "the lower of both ceilings decides");
  // The short form: every axis at the global maximum, homing required.
  const Limits plain{3000, 1.2, 0.5, 1.2, 24000};
  require(plain.max_axis_jog_velocity == std::array<double, 3>{3000, 3000, 3000} &&
              plain.homing_required,
          "limits without per-axis values use the global maximum and require homing");
  for (const double bad : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN()}) {
    bool invalid = false;
    try {
      CommandPolicy broken{Limits{1500, {1500, 1500, bad}, 2.0, 0.5, 1.0, 1500, true}};
    } catch (const std::invalid_argument&) {
      invalid = true;
    }
    require(invalid, "an invalid axis ceiling fails construction");
  }
}
void stops_survive_lost_state() {
  CommandPolicy policy{simulator_limits()};
  auto s = ready();
  for (const auto link : {Link::stale, Link::down}) {
    s.link = link;
    s.machine = MachineState::estop;
    require(!policy.evaluate(Jog{Axis::x, 1, 100, true}, s).allowed,
            "lost status blocks jog starts");
    require(!policy.evaluate(Run{}, s).allowed, "lost status blocks cycle start");
    require(policy.evaluate(Jog{Axis::x, 1, 0, false}, s).allowed,
            "lost status does not block release");
    require(policy.evaluate(JogStopAll{}, s).allowed, "lost status does not block stop-all");
    require(policy.evaluate(Abort{}, s).allowed, "lost status does not block abort attempt");
    require(policy.evaluate(SetMachine{MachineState::estop}, s).allowed,
            "lost status does not block E-stop attempt");
    require(policy.evaluate(SetSpindle{SpindleDirection::stopped, 0}, s).allowed,
            "lost status does not block spindle stop");
  }
}
void jog_constraints() {
  CommandPolicy policy{simulator_limits()};
  auto s = ready();
  s.axes[0].machine = 999;
  require(!policy.evaluate(JogStep{Axis::x, 2, 100}, s).allowed,
          "increment cannot cross travel limit");
  require(policy.evaluate(JogStep{Axis::x, -2, 100}, s).allowed,
          "increment can move away from soft limit");
  s.axes[0].machine = 1000;
  require(!policy.evaluate(Jog{Axis::x, 1, 100, true}, s).allowed, "outward jog at limit rejected");
  require(policy.evaluate(Jog{Axis::x, -1, 100, true}, s).allowed, "inward jog at limit allowed");
  s.axes[0].on_hard_limit = true;
  require(!policy.evaluate(Jog{Axis::x, -1, 100, true}, s).allowed,
          "hard limit needs explicit recovery workflow");
  s = ready();
  s.in_position = false;
  require(!policy.evaluate(JogStep{Axis::x, 2, 100}, s).allowed,
          "incremental moves cannot accumulate during motion");
  require(policy.evaluate(Jog{Axis::x, 1, 100, true}, s).allowed,
          "held jog refresh remains valid while moving");
  require(policy.evaluate(SetSpindle{SpindleDirection::forward, 5000}, s).allowed,
          "spindle may start during a manual jog");
  require(policy.evaluate(Jog{Axis::x, 1, 100, true}, s).jog_space == JogSpace::axis,
          "homed jog requires axis-space teleop");
  s.axes[2].homed = false;
  require(policy.evaluate(Jog{Axis::x, 1, 100, true}, s).jog_space == JogSpace::joint,
          "partial homing keeps all jogs in joint space");
  s.axes[1].homing = true;
  require(!policy.evaluate(Jog{Axis::x, 1, 100, true}, s).allowed,
          "other axis homing prevents manual jog");
}
void hostile_values() {
  CommandPolicy policy{simulator_limits()};
  const auto s = ready();
  for (const auto value : {std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity(), -1.0, 50000.0}) {
    require(!policy.evaluate(Jog{Axis::x, 1, value, true}, s).allowed,
            "invalid jog velocity rejected");
    require(!policy.evaluate(SetOverride{Override::rapid, value}, s).allowed,
            "invalid rapid factor rejected");
  }
  // Deliberately exercise rejection of an invalid decoded enum value.
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  require(!policy.evaluate(Jog{static_cast<Axis>(99), 1, 100, true}, s).allowed,
          "invalid axis is bounded");
  require(!policy.evaluate(Jog{Axis::x, 0, 100, true}, s).allowed, "zero direction rejected");
  require(!policy.evaluate(Mdi{"G0 X0\nM3"}, s).allowed, "MDI is a single block");
  require(!policy.evaluate(Mdi{std::string("G0 X0\0M3", 8)}, s).allowed,
          "MDI rejects embedded null");
  require(!policy.evaluate(SetTool{Tool{1, 1, 0, 6, "cutter\nT2 P2 Z0"}}, s).allowed,
          "tool comment cannot inject an extra row");
  require(!policy.evaluate(ToolTouchOff{0}, s).allowed, "tool length needs a loaded tool");
  require(!policy.evaluate(SetTool{Tool{}}, s).allowed,
          "unspecified tool dimensions remain invalid");
  bool invalid_limits = false;
  try {
    CommandPolicy invalid{Limits{0, 1.2, 0.5, 1.2, 24000}};
  } catch (const std::invalid_argument&) {
    invalid_limits = true;
  }
  require(invalid_limits, "invalid configuration fails construction");
  auto missing_limits = ready();
  missing_limits.axes[0].min_limit = std::numeric_limits<double>::quiet_NaN();
  require(!policy.evaluate(Jog{Axis::x, 1, 100, true}, missing_limits).allowed,
          "homed axis with missing travel limits cannot jog");
}
void independent_jog_leases() {
  JogLease leases;
  const JogLease::Time start{};
  leases.refresh(Axis::x, start);
  leases.refresh(Axis::y, start + 50ms);
  require(!leases.expired(start + 149ms)[0], "jog is held before timeout");
  const auto first = leases.expired(start + 150ms);
  require(first[0] && !first[1] && !first[2], "only expired axis stops at exactly 150 ms");
  require(!leases.expired(start + 151ms)[0], "expired lease produces one stop request");
  leases.refresh(Axis::y, start + 180ms);
  require(!leases.expired(start + 200ms)[1], "fresh hold extends only that axis lease");
  leases.refresh(Axis::z, start + 200ms);
  leases.release(Axis::z);
  require(!leases.active(Axis::z), "release clears its own lease");
  const auto cancelled = leases.cancel_all();
  require(!cancelled[0] && cancelled[1] && !cancelled[2],
          "disconnect cancels remaining active jogs");
  require(!leases.active(Axis::y), "new panel starts without old held jog");
  require(!leases.expired(start + 10s)[1], "cancel does not replay old jog intent");
}

void task_heartbeat() {
  TaskHeartbeat heartbeat;
  const TaskHeartbeat::Clock::time_point start{};
  require(heartbeat.observe(7, start) == Link::stale, "one reading proves nothing");
  require(heartbeat.observe(7, start + 10ms) == Link::stale,
          "an unchanged counter is not a running task");
  require(heartbeat.observe(8, start + 20ms) == Link::live, "an advancing counter is");
  require(heartbeat.observe(8, start + 1019ms) == Link::live,
          "live for a second after the last write");
  require(heartbeat.observe(8, start + 1020ms) == Link::stale, "stale from then on");
  require(heartbeat.observe(9, start + 1030ms) == Link::live, "task writes again");
  require(heartbeat.observe(10, start + 5s) == Link::stale,
          "an advance since a reading seconds ago may be seconds old");
  require(heartbeat.observe(11, start + 5s + 10ms) == Link::live, "the next reading settles it");
}
} // namespace

// An E-stop request is answered by the state reported after it, in these words.
void estop_request_results() {
  const std::string unreadable = "LinuxCNC's status cannot be read; E-stop was not confirmed.";
  // A state from a status that is not evidence confirms and names nothing: a stale or absent
  // link, or a status from before the command, whatever state is still readable in it.
  for (const auto state : {MachineState::estop, MachineState::off, MachineState::on}) {
    for (const auto link : {Link::stale, Link::down}) {
      const auto stale = estop_request_result(state, link, true);
      require(stale.status == ActionStatus::failed && stale.reason == unreadable,
              "a state of a link that is not live is no evidence");
    }
    const auto earlier = estop_request_result(state, Link::live, false);
    require(earlier.status == ActionStatus::failed && earlier.reason == unreadable,
            "a state published before the command is no evidence");
  }
  const auto confirmed = estop_request_result(MachineState::estop, Link::live, true);
  require(confirmed.status == ActionStatus::accepted && confirmed.reason.empty(),
          "E-stop reported: accepted");
  const auto off = estop_request_result(MachineState::off, Link::live, true);
  require(off.status == ActionStatus::failed &&
              off.reason == "LinuxCNC reports machine off; E-stop was not confirmed.",
          "off reported: failed, and says off");
  const auto on = estop_request_result(MachineState::on, Link::live, true);
  require(on.status == ActionStatus::failed &&
              on.reason == "LinuxCNC still reports machine on; E-stop was not confirmed.",
          "on reported: failed, and says still on");
  const auto unread = estop_request_result(std::nullopt, Link::live, true);
  require(unread.status == ActionStatus::failed && unread.reason == unreadable,
          "no status: failed, and says so");
}

// Motion's refusal of a start under motion.jog-inhibit, as the port's reason carries it.
void jog_inhibit_refusal() {
  require(is_jog_inhibit_refusal("Cannot jog while jog-inhibit is active."), "LinuxCNC's text");
  require(is_jog_inhibit_refusal("Cannot jog while jog-inhibit is active"), "without the period");
  require(is_jog_inhibit_refusal(
              "Cannot jog while jog-inhibit is active.; Cannot jog while jog-inhibit is active."),
          "published twice for one start");
  require(!is_jog_inhibit_refusal(""), "no reason");
  require(!is_jog_inhibit_refusal("Cannot jog while jog-inhibit is active.; Joint 0 following "
                                  "error."),
          "with another error: not only the inhibit");
  require(!is_jog_inhibit_refusal("Jog aborted by jog-stop"), "another text");
  require(!is_jog_inhibit_refusal("cannot jog while jog-inhibit is active."), "exact text only");
}

// Whose stop a "Jog aborted by jog-stop" text reports.
void jog_stop_attribution() {
  using Time = std::chrono::steady_clock::time_point;
  const Time read{std::chrono::hours{1}};
  const std::string stop = "Jog aborted by jog-stop";
  const std::string immediate = "Jog aborted by jog-stop-immediate";
  const std::optional<Time> never;
  require(classify_jog_stop(stop, read, read - 12ms, never) == JogStopCause::own,
          "own level high 12 ms before the read: own");
  require(classify_jog_stop(immediate, read, read - 12ms, never) == JogStopCause::not_jog_stop,
          "the immediate text is not ours: own level high in the window, passed on unchanged");
  require(classify_jog_stop(immediate, read, read - 12ms, read - 5ms) ==
              JogStopCause::not_jog_stop,
          "the immediate text is not the watchdog's either: passed on unchanged");
  require(classify_jog_stop(stop, read, read - 12ms, read - 5ms) == JogStopCause::watchdog,
          "a watchdog seen tripped in the window wins over the own level of the same stall");
  require(classify_jog_stop(stop, read, read - 12ms, read - jog_stop_window - 1ns) ==
              JogStopCause::own,
          "a trip before the window does not take the own level's text");
  require(classify_jog_stop(stop, read, read + 1ms, never) == JogStopCause::own,
          "a level still high right after the read: own");
  require(classify_jog_stop(stop, read, read - jog_stop_window, never) == JogStopCause::own,
          "window boundary: high exactly jog_stop_window before the read is own");
  require(classify_jog_stop(stop, read, read - jog_stop_window - 1ns, never) ==
              JogStopCause::unattributed,
          "window boundary: high just before the window is not own");
  require(classify_jog_stop(stop, read, read - 1s, read + 8ms) == JogStopCause::watchdog,
          "no own level in the window, watchdog seen tripped after the read: watchdog");
  require(classify_jog_stop(stop, read, never, read - jog_stop_window) == JogStopCause::watchdog,
          "watchdog seen tripped exactly jog_stop_window before: watchdog");
  require(classify_jog_stop(stop, read, never, read - jog_stop_window - 1ns) ==
              JogStopCause::unattributed,
          "watchdog seen tripped only before the window: unattributed");
  require(classify_jog_stop(stop, read, read - 1s, never) == JogStopCause::unattributed,
          "no own level in the window, watchdog fine: unattributed, the text unchanged");
  require(classify_jog_stop(stop, read, never, never) == JogStopCause::unattributed,
          "no history at all: unattributed");
  for (const std::string other :
       {"Cannot jog while jog-inhibit is active", "Jog aborted by jog-stop.",
        "jog aborted by jog-stop", "Joint 0 following error", ""})
    require(classify_jog_stop(other, read, read, read) == JogStopCause::not_jog_stop &&
                !is_jog_stop_text(other),
            "another text is not a jog-stop text: " + other);
  require(watchdog_stop_text.find("watchdog") != std::string_view::npos &&
              watchdog_stop_text.find("Jog aborted") == std::string_view::npos,
          "the watchdog text is CoffeeCNC's own");
}

auto main() -> int {
  try {
    execution_gates();
    work_origin_gates();
    homing_capability();
    axis_jog_ceilings();
    stops_survive_lost_state();
    jog_constraints();
    hostile_values();
    independent_jog_leases();
    task_heartbeat();
    jog_stop_attribution();
    jog_inhibit_refusal();
    estop_request_results();
    std::cout << checks << " core behavior checks passed\n";
  } catch (const std::exception& error) {
    std::cerr << "Check " << checks << " failed: " << error.what() << '\n';
    return 1;
  }
}
