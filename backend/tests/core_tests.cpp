#include <coffeecnc/core/command_policy.hpp>
#include <coffeecnc/core/jog_lease.hpp>

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

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
} // namespace

auto main() -> int {
  try {
    execution_gates();
    stops_survive_lost_state();
    jog_constraints();
    hostile_values();
    independent_jog_leases();
    std::cout << checks << " core behavior checks passed\n";
  } catch (const std::exception& error) {
    std::cerr << "Check " << checks << " failed: " << error.what() << '\n';
    return 1;
  }
}
