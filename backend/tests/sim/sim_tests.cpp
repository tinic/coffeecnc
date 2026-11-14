// Drives the real Session on top of NmlPort against a running LinuxCNC. This program is the
// DISPLAY program of CoffeeCNC's simulated mill: start it with tests/sim/run.sh, never by hand.
// It sends protocol v1 JSON requests and checks each CommandResult and what LinuxCNC then reports.
//
// Output: one `check=<name> result=pass|fail detail=<text>` line per check, `fact=` lines with
// observations for the record, and `exit=<code>` last. Exit code 0 only when every check passed.

#include <coffeecnc/app/session.hpp>
#include <coffeecnc/linuxcnc/hal_component.hpp>
#include <coffeecnc/linuxcnc/machine_config.hpp>
#include <coffeecnc/linuxcnc/nml_port.hpp>
#include <coffeecnc/linuxcnc/tool_table.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <signal.h> // kill, SIGSTOP: POSIX, not in <csignal>
#include <sys/types.h>

using namespace coffeecnc;
using namespace std::chrono_literals;

namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

int failures = 0;

// Spaces would break the key=value line.
auto word(std::string text) -> std::string {
  std::ranges::replace(text, ' ', '_');
  std::ranges::replace(text, '\n', '_');
  return text.empty() ? std::string("-") : text;
}

void check(std::string_view name, bool passed, const std::string& detail = {}) {
  if (!passed)
    ++failures;
  std::cout << "check=" << name << " result=" << (passed ? "pass" : "fail")
            << " detail=" << word(detail) << std::endl;
}

void fact(std::string_view name, const std::string& detail) {
  std::cout << "fact=" << name << ' ' << detail << std::endl;
}

auto near(double value, double expected, double tolerance = 0.01) -> bool {
  return std::fabs(value - expected) <= tolerance;
}

// First line a shell command prints.
auto output_of(const std::string& command) -> std::string {
  std::string line;
  if (FILE* pipe = popen(command.c_str(), "r"); pipe != nullptr) {
    std::array<char, 256> buffer{};
    if (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
      line = buffer.data();
    static_cast<void>(pclose(pipe));
  }
  while (!line.empty() && (line.back() == '\n' || line.back() == ' '))
    line.pop_back();
  return line;
}

auto hal_pin(const std::string& pin) -> std::string { return output_of("halcmd getp " + pin); }
auto hal_number(const std::string& pin) -> double { return std::atof(hal_pin(pin).c_str()); }

auto name(Link link) -> std::string_view {
  return link == Link::live ? "live" : link == Link::stale ? "stale" : "down";
}

struct Result {
  std::string outcome;
  std::string reason;
  bool send_snapshots{};
  [[nodiscard]] auto accepted() const -> bool { return outcome == "accepted"; }
  [[nodiscard]] auto text() const -> std::string { return outcome + ":" + reason; }
};

// The executable as it will be: one HAL component, one NML port, one session, and a control loop
// that beats the heartbeat and ticks the session.
class Rig {
public:
  Rig(const MachineConfig& machine, HalComponent& component, NmlPort& controller)
      : config(machine), hal(component), port(controller), session(controller, machine.limits),
        page(session.begin_page()) {}

  MachineConfig config;
  HalComponent& hal;
  NmlPort& port;
  Session session;
  PageToken page;
  RequestId next_id{1};
  // Operator messages, from LinuxCNC and from the session, as they arrived.
  std::vector<std::string> messages;

  static auto now() -> Instant {
    return {Clock::now(), std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())};
  }

  // One request as the panel sends it; `body` is the JSON without braces and ID.
  auto send(std::string_view body) -> Result {
    // A request that blocked in the port starved the heartbeat (the session runs on the control
    // thread in this increment). The executable beats on; so does the rig before the next one.
    hal.beat();
    if (!hal.watchdog_ok())
      pump_until(200ms, [&]() -> bool { return hal.watchdog_ok(); });
    const auto id = next_id++;
    const auto outcome = session.handle(page, std::format("{{\"id\":{},{}}}", id, body), now());
    Result result;
    result.send_snapshots = outcome.send_snapshots;
    int results = 0;
    for (const auto& text : outcome.messages) {
      const auto message = Json::parse(text);
      if (message.at("type") == "result" && message.at("id") == id) {
        ++results;
        result.outcome = message.at("outcome").get<std::string>();
        result.reason = message.at("reason").get<std::string>();
      } else if (message.at("type") == "message") {
        messages.push_back(message.at("text").get<std::string>());
      }
    }
    if (results != 1)
      result.outcome = std::format("{}_results", results);
    return result;
  }

  // The control loop for a while: heartbeat, session tick, operator messages.
  void pump(std::chrono::milliseconds time, bool tick = true) {
    for (const auto end = Clock::now() + time; Clock::now() < end;) {
      cycle(tick);
      std::this_thread::sleep_for(5ms);
    }
  }

  // The control loop until `done` holds. False after the timeout.
  auto pump_until(std::chrono::milliseconds timeout, const std::function<bool()>& done) -> bool {
    for (const auto end = Clock::now() + timeout; Clock::now() < end;) {
      cycle(true);
      if (done())
        return true;
      std::this_thread::sleep_for(5ms);
    }
    return false;
  }

  // Keeps a held jog alive as the panel does: the same request again every 50 ms.
  void hold(std::string_view body, std::chrono::milliseconds time) {
    for (const auto end = Clock::now() + time; Clock::now() < end;) {
      static_cast<void>(send(body));
      pump(50ms);
    }
  }

  auto machine(std::size_t axis) -> double {
    return port.status().controller.axes.at(axis).machine;
  }

  // The machine is at rest with nothing left to execute.
  auto idle() -> bool {
    const auto state = port.snapshot();
    return state.interp == InterpState::idle && state.in_position;
  }

  auto has_message(std::string_view part) const -> bool {
    return std::ranges::any_of(
        messages, [&](const auto& text) -> bool { return text.find(part) != std::string::npos; });
  }

private:
  void cycle(bool tick) {
    hal.beat();
    if (tick)
      for (const auto& text : session.tick(now()).messages)
        if (const auto message = Json::parse(text); message.at("type") == "message")
          messages.push_back(message.at("text").get<std::string>());
    for (auto& message : port.take_messages())
      messages.push_back(std::move(message.text));
  }
};

// Stops LinuxCNC's task process for as long as it lives: status and command echo cease, motion
// and HAL (another process) carry on. What a hung or dead task looks like to the adapter.
class FrozenTask {
public:
  explicit FrozenTask(const std::string& ini_file)
      : pid_(std::atoi(output_of("pgrep -f 'milltask -ini " + ini_file + "'").c_str())) {
    if (pid_ <= 0)
      throw std::runtime_error("LinuxCNC's task process was not found");
    static_cast<void>(kill(pid_, SIGSTOP));
  }
  FrozenTask(const FrozenTask&) = delete;
  auto operator=(const FrozenTask&) -> FrozenTask& = delete;
  ~FrozenTask() { static_cast<void>(kill(pid_, SIGCONT)); }

private:
  pid_t pid_;
};

constexpr auto hold_x = R"("type":"jog","axis":"x","direction":1,"velocity":600,"held":true)";
constexpr auto release_x = R"("type":"jog","axis":"x","direction":1,"velocity":600,"held":false)";
constexpr auto hold_y = R"("type":"jog","axis":"y","direction":-1,"velocity":600,"held":true)";
constexpr auto release_y = R"("type":"jog","axis":"y","direction":-1,"velocity":600,"held":false)";

void tool_table_text() {
  const std::string table = "T1 P1 Z50 D6 ;6 mm end mill\nT2 P2 Z40 D3 I30 ;drill\n";
  Tool tool;
  tool.number = 2;
  tool.pocket = 5;
  tool.length = 25.4;
  tool.diameter = 12.7;
  tool.comment = "half inch";
  check("tool_table_replace_keeps_other_words",
        with_tool(table, tool, 25.4) ==
            "T1 P1 Z50 D6 ;6 mm end mill\nT2 P5 Z1 D0.5 I30 ;half inch\n",
        with_tool(table, tool, 25.4));
  tool.number = 7;
  check("tool_table_add", with_tool(table, tool, 1) == table + "T7 P5 Z25.4 D12.7 ;half inch\n");
  check("tool_table_remove", without_tool(table, 1) == "T2 P2 Z40 D3 I30 ;drill\n" &&
                                 !without_tool(table, 9).has_value());
}

void power(Rig& rig) {
  auto state = rig.port.snapshot();
  check("link_live", state.link == Link::live, std::string(name(state.link)));
  check("starts_in_estop", state.machine == MachineState::estop);
  check("watchdog_armed", rig.hal.watchdog_ok());

  const auto ready = rig.send(R"("type":"ready")");
  check("ready", ready.accepted() && ready.send_snapshots, ready.text());
  const auto early = rig.send(R"("type":"set_machine","state":"on")");
  check("on_rejected_in_estop", early.outcome == "rejected", early.text());
  // The same request without the policy: task reports it done and stays in E-stop.
  const auto forced = rig.port.set_machine_state(MachineState::on);
  check("port_on_in_estop_fails", forced.status == ActionStatus::failed, forced.reason);

  const auto reset = rig.send(R"("type":"set_machine","state":"off")");
  check("estop_reset", reset.accepted() && rig.port.snapshot().machine == MachineState::off,
        reset.text());
  const auto on = rig.send(R"("type":"set_machine","state":"on")");
  check("machine_on", on.accepted() && rig.port.snapshot().machine == MachineState::on, on.text());
}

void unhomed(Rig& rig) {
  const auto mdi = rig.send(R"("type":"mdi","text":"G0 X1")");
  check("mdi_rejected_unhomed", mdi.outcome == "rejected", mdi.text());
  // What LinuxCNC itself says when the policy is not in the way.
  auto mode = rig.port.set_task_mode(TaskMode::mdi);
  const auto forced = rig.port.mdi("G0 X1");
  check("port_mdi_unhomed_fails",
        mode.status == ActionStatus::accepted && forced.status == ActionStatus::failed &&
            forced.reason.find("not homed") != std::string::npos,
        forced.reason);
  mode = rig.port.set_task_mode(TaskMode::manual);
  check("mode_back_to_manual",
        mode.status == ActionStatus::accepted && rig.port.snapshot().mode == TaskMode::manual);

  // Joint jog, held by refreshes, ended by the release.
  const double start = rig.machine(0);
  const auto pressed = rig.send(hold_x);
  rig.hold(hold_x, 300ms);
  const double moving = rig.machine(0);
  const auto released = rig.send(release_x);
  rig.pump(150ms);
  const double stopped = rig.machine(0);
  rig.pump(150ms);
  check("jog_joint_hold_release",
        pressed.accepted() && released.accepted() && !rig.port.status().teleop &&
            moving - start > 1.5 && near(rig.machine(0), stopped, 0.001),
        std::format("{} moved={:.3f} after_release={:.3f}", pressed.text(), moving - start,
                    rig.machine(0) - stopped));

  // No refresh: the 150 ms lease expires in Session::tick, which stops the jog over NML.
  const double y0 = rig.machine(1);
  const auto unrefreshed = rig.send(hold_y);
  rig.pump(400ms);
  const double y1 = rig.machine(1);
  rig.pump(150ms);
  const double travelled = y0 - y1; // 10 mm/s: about 1.5 mm in 150 ms, plus ramps
  check("jog_lease_expiry_stops",
        unrefreshed.accepted() && travelled > 0.8 && travelled < 3.0 &&
            near(rig.machine(1), y1, 0.001),
        std::format("travelled={:.3f}", travelled));

  // The HAL pin stops a jog the session still holds.
  static_cast<void>(rig.send(hold_x));
  rig.hold(hold_x, 150ms);
  const double before = rig.machine(0);
  rig.hal.request_jog_stop();
  rig.hold(hold_x, 150ms);
  const double after = rig.machine(0);
  rig.hold(hold_x, 150ms);
  check("hal_jog_stop_joint", after - before < 1.0 && near(rig.machine(0), after, 0.001),
        std::format("moved_after_request={:.3f}", after - before));
  static_cast<void>(rig.send(release_x));
  rig.pump(50ms);
  check("jog_stop_reported", rig.has_message("Jog aborted by jog-stop"));
}

void homing(Rig& rig) {
  const auto start = Clock::now();
  const auto home = rig.send(R"("type":"home","axis":"all")");
  bool saw_homing = false;
  const bool homed = rig.pump_until(60s, [&]() -> bool {
    const auto state = rig.port.snapshot();
    bool all = true;
    for (const auto& axis : state.axes) {
      saw_homing = saw_homing || axis.homing;
      all = all && axis.homed && !axis.homing;
    }
    return all;
  });
  const auto seconds = std::chrono::duration<double>(Clock::now() - start).count();
  rig.pump(300ms);
  const auto status = rig.port.status();
  check("home_all", home.accepted() && homed && saw_homing,
        std::format("{} seconds={:.1f}", home.text(), seconds));
  check("teleop_after_homing", status.teleop && near(status.controller.axes[2].machine, 0, 0.01),
        std::format("z={:.3f}", status.controller.axes[2].machine));
  check("limits_in_mm",
        near(status.controller.axes[0].min_limit, -250) &&
            near(status.controller.axes[2].max_limit, 100),
        std::format("x_min={:.3f} z_max={:.3f}", status.controller.axes[0].min_limit,
                    status.controller.axes[2].max_limit));
}

void axis_jogs(Rig& rig) {
  const double start = rig.machine(0);
  const auto pressed = rig.send(hold_x);
  rig.hold(hold_x, 300ms);
  const double moving = rig.machine(0);
  const bool busy = !rig.port.snapshot().in_position;
  const auto released = rig.send(release_x);
  rig.pump(150ms);
  const double stopped = rig.machine(0);
  rig.pump(150ms);
  check("jog_axis_hold_release",
        pressed.accepted() && released.accepted() && rig.port.status().teleop && busy &&
            moving - start > 1.5 && near(rig.machine(0), stopped, 0.001),
        std::format("{} moved={:.3f}", pressed.text(), moving - start));

  const double z0 = rig.machine(2);
  const auto step = rig.send(R"("type":"jog_step","axis":"z","distance":-0.5,"velocity":600)");
  rig.pump(500ms);
  check("jog_step", step.accepted() && near(rig.machine(2) - z0, -0.5, 0.001),
        std::format("{} moved={:.4f}", step.text(), rig.machine(2) - z0));

  const auto beyond = rig.send(R"("type":"jog_step","axis":"z","distance":500,"velocity":600)");
  check("jog_step_beyond_travel_rejected", beyond.outcome == "rejected", beyond.text());
}

// The key property of the HAL path: it stops a jog while the NML command channel is useless.
void hal_stop_without_nml(Rig& rig) {
  const auto pressed = rig.send(hold_x);
  rig.hold(hold_x, 100ms);
  ActionResult nml;
  double a{};
  double b{};
  double c{};
  double d{};
  std::string active;
  {
    const FrozenTask frozen{rig.config.ini_file};
    // The NML stop cannot be echoed: this call blocks for the echo timeout.
    std::thread worker{[&]() -> void { nml = rig.port.stop_jog(Axis::x); }};
    const auto beat = [&](std::chrono::milliseconds time) -> void {
      for (const auto end = Clock::now() + time; Clock::now() < end;) {
        rig.hal.beat();
        std::this_thread::sleep_for(5ms);
      }
    };
    // Motion's own feedback pin, in machine units: status is not written while task is stopped.
    const auto feedback = [&]() -> double {
      return hal_number("joint.0.pos-fb") * rig.config.millimetres_per_unit;
    };
    a = feedback();
    beat(100ms);
    b = feedback();
    rig.hal.request_jog_stop();
    beat(150ms);
    c = feedback();
    beat(100ms);
    d = feedback();
    active = hal_pin("motion.jog-is-active");
    worker.join();
  }
  check("hal_jog_stop_during_nml_wait",
        pressed.accepted() && b - a > 0.5 && near(d, c, 0.0001) && active == "FALSE" &&
            nml.status == ActionStatus::timed_out,
        std::format("moving_before={:.3f} moved_after_stop={:.4f} jog_active={} nml_stop={}", b - a,
                    d - c, active, nml.status == ActionStatus::timed_out ? "timed_out" : "other"));
  // Task runs again: the session's own stop for the expired lease now gets through.
  rig.pump(300ms);
  const double rest = rig.machine(0);
  rig.pump(100ms);
  check("recovers_after_task_resumes",
        rig.port.snapshot().link == Link::live && near(rig.machine(0), rest, 0.001));
}

// The executable stalls: no heartbeat, no lease handling. The HAL watchdog stops the jog.
void watchdog(Rig& rig) {
  const auto pressed = rig.send(hold_y);
  rig.hold(hold_y, 100ms);
  const double before = rig.machine(1);
  std::this_thread::sleep_for(450ms); // the stall; the watchdog's timeout is 300 ms
  const double stalled = rig.machine(1);
  std::this_thread::sleep_for(100ms);
  const double later = rig.machine(1);
  const bool tripped = !rig.hal.watchdog_ok();
  const double travelled = before - stalled; // 10 mm/s: about 3 mm in 300 ms
  check("watchdog_stops_jog",
        pressed.accepted() && tripped && travelled > 1.5 && travelled < 4.5 &&
            near(later, stalled, 0.001),
        std::format("tripped={} travelled={:.3f} moved_after={:.4f}", tripped, travelled,
                    stalled - later));
  // Beating again arms it again, and jogging works as before.
  rig.pump(200ms);
  const bool armed = rig.hal.watchdog_ok();
  const double y0 = rig.machine(1);
  const auto again = rig.send(hold_y);
  rig.hold(hold_y, 200ms);
  const double y1 = rig.machine(1);
  static_cast<void>(rig.send(release_y));
  rig.pump(200ms);
  check("watchdog_rearms", armed && again.accepted() && y0 - y1 > 1.0,
        std::format("armed={} {} moved={:.3f}", armed, again.text(), y0 - y1));
}

void mdi(Rig& rig) {
  const auto units = rig.send(R"("type":"mdi","text":"G21 G90 G54")");
  rig.pump_until(5s, [&]() -> bool { return rig.idle(); });
  check("mdi_units",
        units.accepted() && rig.port.snapshot().mode == TaskMode::mdi &&
            rig.port.snapshot().program_units == LengthUnit::mm,
        units.text());
  const auto move = rig.send(R"("type":"mdi","text":"G0 X10 Y-5.5")");
  const bool busy = !rig.idle();
  const bool arrived = rig.pump_until(20s, [&]() -> bool { return rig.idle(); });
  auto status = rig.port.status();
  check("mdi_move",
        move.accepted() && busy && arrived && near(status.controller.axes[0].machine, 10) &&
            near(status.controller.axes[1].machine, -5.5),
        std::format("{} x={:.3f} y={:.3f}", move.text(), status.controller.axes[0].machine,
                    status.controller.axes[1].machine));

  const auto touch = rig.send(R"("type":"touch_off","axis":"x","value":2.5)");
  rig.pump_until(5s, [&]() -> bool { return rig.idle(); });
  status = rig.port.status();
  check("touch_off",
        touch.accepted() && near(status.work[0], 2.5) && near(status.g5x[0], 7.5) &&
            status.wcs == 1,
        std::format("{} work_x={:.3f} g5x_x={:.3f} wcs={}", touch.text(), status.work[0],
                    status.g5x[0], status.wcs));
  const auto g55 = rig.send(R"("type":"select_wcs","wcs":2)");
  rig.pump_until(5s, [&]() -> bool { return rig.idle(); });
  const int second = rig.port.status().wcs;
  const auto g54 = rig.send(R"("type":"select_wcs","wcs":1)");
  rig.pump_until(5s, [&]() -> bool { return rig.idle(); });
  check("select_wcs", g55.accepted() && g54.accepted() && second == 2 && rig.port.status().wcs == 1,
        std::format("second={} back={}", second, rig.port.status().wcs));

  // With the interpreter in inches the composed block must carry inches for the same millimetres.
  static_cast<void>(rig.send(R"("type":"mdi","text":"G20")"));
  rig.pump_until(5s, [&]() -> bool { return rig.idle(); });
  const bool inches = rig.port.snapshot().program_units == LengthUnit::inch;
  const auto inch_touch = rig.send(R"("type":"touch_off","axis":"y","value":25.4)");
  rig.pump_until(5s, [&]() -> bool { return rig.idle(); });
  status = rig.port.status();
  check("touch_off_in_inch_program_units",
        inches && inch_touch.accepted() && near(status.work[1], 25.4) && near(status.g5x[1], -30.9),
        std::format("units_inch={} work_y={:.3f} g5x_y={:.3f}", inches, status.work[1],
                    status.g5x[1]));
  static_cast<void>(rig.send(R"("type":"mdi","text":"G21")"));
  rig.pump_until(5s, [&]() -> bool { return rig.idle(); });

  // LinuxCNC's refusal reaches the result, and is not repeated as an operator message.
  rig.messages.clear();
  const auto beyond = rig.send(R"("type":"mdi","text":"G1 X1000 F600")");
  rig.pump(200ms);
  check("mdi_beyond_travel_fails_with_text",
        beyond.outcome == "failed" && beyond.reason.find("exceed") != std::string::npos &&
            !rig.has_message("exceed"),
        beyond.text());
  const auto syntax = rig.send(R"("type":"mdi","text":"G1 Q")");
  check("mdi_syntax_error_fails_with_text", syntax.outcome == "failed" && !syntax.reason.empty(),
        syntax.text());
  const auto message = rig.send(R"j("type":"mdi","text":"(MSG, coffeecnc says hello)")j");
  rig.pump(300ms);
  check("operator_message_arrives", message.accepted() && rig.has_message("coffeecnc says hello"));
}

void overrides(Rig& rig) {
  const auto feed = rig.send(R"("type":"set_override","which":"feed","value":0.85)");
  const auto rapid = rig.send(R"("type":"set_override","which":"rapid","value":0.5)");
  const auto spindle = rig.send(R"("type":"set_override","which":"spindle","value":1.1)");
  rig.pump(50ms);
  auto status = rig.port.status();
  check("overrides",
        feed.accepted() && rapid.accepted() && spindle.accepted() &&
            near(status.feed_override, 0.85, 1e-9) && near(status.rapid_override, 0.5, 1e-9) &&
            near(status.spindle_override, 1.1, 1e-9),
        std::format("feed={} rapid={} spindle={}", status.feed_override, status.rapid_override,
                    status.spindle_override));
  const auto over = rig.send(R"("type":"set_override","which":"feed","value":2.0)");
  check("override_above_ini_limit_rejected", over.outcome == "rejected", over.text());
  for (const auto which : {"feed", "rapid", "spindle"})
    static_cast<void>(
        rig.send(std::format(R"("type":"set_override","which":"{}","value":1)", which)));

  const auto forward = rig.send(R"("type":"set_spindle","direction":"forward","rpm":6000)");
  rig.pump(50ms);
  status = rig.port.status();
  const bool runs_forward =
      status.spindle_direction == SpindleDirection::forward && near(status.spindle_rpm, 6000);
  const auto reverse = rig.send(R"("type":"set_spindle","direction":"reverse","rpm":3000)");
  rig.pump(50ms);
  status = rig.port.status();
  const bool runs_reverse =
      status.spindle_direction == SpindleDirection::reverse && near(status.spindle_rpm, 3000);
  const auto stop = rig.send(R"("type":"set_spindle","direction":"stopped","rpm":0)");
  rig.pump(50ms);
  status = rig.port.status();
  check("spindle",
        forward.accepted() && reverse.accepted() && stop.accepted() && runs_forward &&
            runs_reverse && status.spindle_direction == SpindleDirection::stopped,
        std::format("forward={} reverse={} {}", runs_forward, runs_reverse, stop.text()));

  const auto flood = rig.send(R"("type":"set_coolant","flood":true,"mist":false)");
  rig.pump(300ms);
  status = rig.port.status();
  const bool flood_only = status.flood && !status.mist;
  const auto both = rig.send(R"("type":"set_coolant","flood":true,"mist":true)");
  rig.pump(300ms);
  status = rig.port.status();
  const bool flood_and_mist = status.flood && status.mist;
  const auto none = rig.send(R"("type":"set_coolant","flood":false,"mist":false)");
  rig.pump(300ms);
  status = rig.port.status();
  check("coolant",
        flood.accepted() && both.accepted() && none.accepted() && flood_only && flood_and_mist &&
            !status.flood && !status.mist,
        std::format("flood_only={} both={}", flood_only, flood_and_mist));
}

auto trace_line(const NmlStatus& status) -> std::string {
  const auto& c = status.controller;
  return std::format(
      "mode={} interp={} paused={} current_line={} motion_line={} in_position={} dwell={} "
      "requested_tool={} tool={} units={}",
      static_cast<int>(c.mode), static_cast<int>(c.interp), status.paused, status.current_line,
      status.motion_line, c.in_position, status.dwell_seconds_left > 0,
      c.requested_tool ? *c.requested_tool : -1, c.tool_in_spindle,
      c.program_units == LengthUnit::inch ? "inch" : "mm");
}

// The control loop until `done`, recording every change of what the panel's program view shows.
auto traced(Rig& rig, std::chrono::milliseconds timeout, const std::function<bool()>& done)
    -> bool {
  static const auto origin = Clock::now();
  static std::string last;
  return rig.pump_until(timeout, [&]() -> bool {
    if (auto line = trace_line(rig.port.status()); line != last) {
      fact("trace",
           std::format(
               "t_ms={} {}",
               std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - origin).count(),
               line));
      last = std::move(line);
    }
    return done();
  });
}

void program(Rig& rig) {
  const std::string file = rig.config.program_prefix + "/coffeecnc_sim.ngc";
  {
    std::ofstream out{file};
    out << "G21 G90 G54\n"  // 1
           "G0 X0 Y0 Z10\n" // 2
           "G1 X30 F1200\n" // 3
           "M1\n"           // 4
           "G1 Y20\n"       // 5
           "M0\n"           // 6
           "G4 P0.5\n"      // 7
           "T2 M6\n"        // 8
           "G20\n"          // 9
           "G1 X0 F50\n"    // 10
           "G21\n"          // 11
           "M2\n";          // 12
  }
  const auto paused = [&]() -> bool { return rig.port.status().paused; };
  const auto request = [&]() -> bool { return rig.port.snapshot().requested_tool.has_value(); };

  const auto missing = rig.send(R"("type":"open_program","path":"/nonexistent/none.ngc")");
  check("open_missing_program_fails", missing.outcome == "failed" && !missing.reason.empty(),
        missing.text());
  const auto open = rig.send(std::format(R"("type":"open_program","path":"{}")", file));
  check("open_program",
        open.accepted() && rig.port.snapshot().program_loaded &&
            rig.port.status().program_file == file,
        open.text());
  const auto optional =
      rig.send(R"("type":"set_program_option","option":"optional_stop","on":true)");
  check("optional_stop_on", optional.accepted() && rig.port.status().optional_stop,
        optional.text());

  const auto run = rig.send(R"("type":"run")");
  // Into the first feed move (line 3).
  const bool cutting = traced(rig, 10s, [&]() -> bool {
    const auto status = rig.port.status();
    return status.motion_line == 3 && status.work[0] > 3;
  });
  check("run", run.accepted() && cutting && rig.port.snapshot().mode == TaskMode::automatic,
        run.text());

  const auto pause = rig.send(R"("type":"pause")");
  rig.pump(200ms);
  const double held = rig.machine(0);
  traced(rig, 200ms, []() -> bool { return false; });
  auto status = rig.port.status();
  check("pause",
        pause.accepted() && status.paused && near(rig.machine(0), held, 0.001) &&
            status.work[0] < 29 && status.controller.interp == InterpState::paused,
        std::format("{} x={:.3f}", pause.text(), status.work[0]));

  const auto resume = rig.send(R"("type":"resume")");
  // Runs on to the optional stop on line 4.
  const bool at_m1 = traced(rig, 10s, [&]() -> bool {
    const auto now = rig.port.status();
    return now.paused && near(now.work[0], 30);
  });
  status = rig.port.status();
  check("resume_runs_to_m1", resume.accepted() && at_m1 && near(status.work[1], 0),
        std::format("{} x={:.3f} y={:.3f} current_line={}", resume.text(), status.work[0],
                    status.work[1], status.current_line));

  // Single block from the stop: line 5 only, then waiting again before M0 and the rest. The
  // first step after an M0/M1 stop only takes up the next block; the second one moves.
  int steps = 0;
  bool moved = false;
  while (steps < 4 && !moved) {
    const auto step = rig.send(R"("type":"step")");
    ++steps;
    if (!step.accepted()) {
      fact("step_refused", word(step.text()));
      break;
    }
    moved = traced(rig, 1500ms, [&]() -> bool { return rig.port.status().work[1] > 1; });
  }
  traced(rig, 5s, [&]() -> bool { return near(rig.port.status().work[1], 20); });
  traced(rig, 800ms, []() -> bool { return false; });
  status = rig.port.status();
  check("step_executes_one_block",
        moved && steps == 2 && near(status.work[1], 20) && near(status.work[0], 30) &&
            status.controller.interp != InterpState::idle && !status.controller.requested_tool &&
            status.dwell_seconds_left <= 0,
        std::format("steps_sent={} x={:.3f} y={:.3f} interp={} paused={}", steps, status.work[0],
                    status.work[1], static_cast<int>(status.controller.interp), status.paused));

  // Resume through M0 and the dwell to the tool change on line 8.
  int resumes = 0;
  bool dwell = false;
  const bool asked = traced(rig, 20s, [&]() -> bool {
    dwell = dwell || rig.port.status().dwell_seconds_left > 0;
    if (request())
      return true;
    if (paused() && resumes < 6) {
      static_cast<void>(rig.send(R"("type":"resume")"));
      ++resumes;
    }
    return false;
  });
  auto state = rig.port.snapshot();
  check("program_tool_change_request",
        asked && state.requested_tool == 2 && dwell && state.tool_in_spindle != 2,
        std::format("requested={} resumes={} saw_dwell={}",
                    state.requested_tool ? *state.requested_tool : -1, resumes, dwell));
  const auto confirmed = rig.send(R"("type":"tool_changed")");
  const bool finished = traced(rig, 30s, [&]() -> bool { return rig.idle() && !paused(); });
  status = rig.port.status();
  check("tool_changed_finishes_program",
        confirmed.accepted() && finished && status.controller.tool_in_spindle == 2 &&
            !status.controller.requested_tool && near(status.work[0], 0) &&
            status.controller.program_units == LengthUnit::mm,
        std::format("{} tool={} x={:.3f}", confirmed.text(), status.controller.tool_in_spindle,
                    status.work[0]));

  // Optional stop off: M1 is passed, M0 still stops. Then abort in the move after it.
  static_cast<void>(rig.send(R"("type":"set_program_option","option":"optional_stop","on":false)"));
  const auto rerun = rig.send(R"("type":"run")");
  const bool at_m0 = traced(rig, 20s, [&]() -> bool {
    const auto now = rig.port.status();
    return now.paused && near(now.work[1], 20);
  });
  check("m1_skipped_m0_stops", rerun.accepted() && at_m0, rerun.text());
  static_cast<void>(rig.send(R"("type":"resume")"));
  // The tool is in the spindle already, so line 8 asks for nothing; line 10 moves slowly.
  bool asked_again = false;
  const bool last_move = traced(rig, 20s, [&]() -> bool {
    if (request()) {
      asked_again = true;
      static_cast<void>(rig.send(R"("type":"tool_changed")"));
    }
    const auto now = rig.port.status();
    return now.motion_line == 10 && now.work[0] < 29.5;
  });
  fact("m6_for_the_loaded_tool", std::format("asks_again={}", asked_again));
  const bool inch_program = rig.port.snapshot().program_units == LengthUnit::inch;
  const auto abort = rig.send(R"("type":"abort")");
  rig.pump(300ms);
  const double stopped = rig.machine(0);
  rig.pump(200ms);
  status = rig.port.status();
  check("abort",
        last_move && abort.accepted() && status.controller.interp == InterpState::idle &&
            near(rig.machine(0), stopped, 0.001) && status.work[0] > 1,
        std::format("{} x={:.3f} program_units_inch_at_abort={}", abort.text(), status.work[0],
                    inch_program));
  traced(rig, 100ms, []() -> bool { return false; });
}

void tools(Rig& rig) {
  static_cast<void>(rig.send(R"("type":"mdi","text":"G21 G90 G49")"));
  rig.pump_until(5s, [&]() -> bool { return rig.idle(); });
  const auto change = rig.send(R"("type":"mdi","text":"T1 M6")");
  const bool asked =
      rig.pump_until(10s, [&]() -> bool { return rig.port.snapshot().requested_tool.has_value(); });
  const auto waiting = rig.port.status();
  check("mdi_tool_change_request",
        change.accepted() && asked && waiting.controller.requested_tool == 1 &&
            waiting.controller.tool_in_spindle == 2 && waiting.prepared_pocket.has_value(),
        std::format("{} requested={} in_spindle={} prepared_pocket={} hal_tool_change={}",
                    change.text(),
                    waiting.controller.requested_tool ? *waiting.controller.requested_tool : -1,
                    waiting.controller.tool_in_spindle,
                    waiting.prepared_pocket ? *waiting.prepared_pocket : -1,
                    hal_pin("iocontrol.0.tool-change")));
  const auto resume = rig.send(R"("type":"resume")");
  check("no_resume_during_tool_change", resume.outcome == "rejected", resume.text());
  const auto confirmed = rig.send(R"("type":"tool_changed")");
  const bool done = rig.pump_until(
      10s, [&]() -> bool { return rig.idle() && rig.port.snapshot().tool_in_spindle == 1; });
  const auto after = rig.port.status();
  check("tool_changed",
        confirmed.accepted() && done && !after.controller.requested_tool &&
            hal_pin("coffeecnc.tool-changed") == "FALSE",
        std::format("{} in_spindle={} prepared_pocket={}", confirmed.text(),
                    after.controller.tool_in_spindle,
                    after.prepared_pocket ? *after.prepared_pocket : -1));
  const auto again = rig.send(R"("type":"tool_changed")");
  check("tool_changed_without_request_rejected", again.outcome == "rejected", again.text());

  // Tool length from the current position: the work Z becomes the given value.
  const auto touch = rig.send(R"("type":"tool_touch_off","value":25.4)");
  rig.pump_until(5s, [&]() -> bool { return rig.idle(); });
  const auto status = rig.port.status();
  check("tool_touch_off", touch.accepted() && near(status.work[2], 25.4),
        std::format("{} work_z={:.3f} tool_offset_z={:.3f}", touch.text(), status.work[2],
                    status.tool_offset[2]));

  const auto table = [&]() -> std::string {
    std::ifstream in{rig.config.tool_table};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  };
  const auto set = rig.send(
      R"("type":"set_tool","tool":{"number":3,"pocket":3,"length":48.2,"diameter":6,"comment":"End mill 6 mm"})");
  const bool added = table().find("T3 P3") != std::string::npos;
  const auto remove = rig.send(R"("type":"remove_tool","number":3)");
  const bool removed = table().find("T3 P3") == std::string::npos;
  const auto absent = rig.send(R"("type":"remove_tool","number":99)");
  const auto loaded = rig.send(R"("type":"remove_tool","number":1)");
  check("tool_table",
        set.accepted() && added && remove.accepted() && removed && absent.outcome == "failed" &&
            loaded.outcome == "failed",
        std::format("set={} remove={} absent={} in_spindle={}", set.text(), remove.text(),
                    absent.text(), loaded.text()));
}

void stale_link(Rig& rig) {
  static_cast<void>(rig.send(R"("type":"jog_stop_all")"));
  const bool live = rig.port.snapshot().link == Link::live;
  Link frozen_link{};
  Result jog;
  Result abort;
  {
    const FrozenTask frozen{rig.config.ini_file};
    rig.pump(1200ms);
    frozen_link = rig.port.snapshot().link;
    jog = rig.send(hold_x);
    // Never gated by stale state: it is attempted, and its real outcome reported.
    abort = rig.send(R"("type":"abort")");
  }
  check("stale_link_detected", live && frozen_link == Link::stale, std::string(name(frozen_link)));
  check("no_jog_on_stale_link", jog.outcome == "rejected", jog.text());
  check("abort_on_dead_task_times_out", abort.outcome == "timed_out", abort.text());
  rig.pump(300ms);
  check("link_live_again", rig.port.snapshot().link == Link::live);
}

void power_off(Rig& rig) {
  const auto manual = rig.send(R"("type":"jog_stop_all")");
  const auto off = rig.send(R"("type":"set_machine","state":"off")");
  const bool is_off = rig.port.snapshot().machine == MachineState::off;
  const auto estop = rig.send(R"("type":"set_machine","state":"estop")");
  check("power_off_and_estop",
        manual.accepted() && off.accepted() && is_off && estop.accepted() &&
            rig.port.snapshot().machine == MachineState::estop,
        std::format("{} {} {}", manual.text(), off.text(), estop.text()));
  rig.session.end_page(rig.page);
}

auto run(const std::string& ini_file) -> int {
  tool_table_text();
  const auto config = load_machine_config(ini_file);
  check("machine_config", config.has_value(), config ? config->machine_name : config.error());
  if (!config)
    return 1;
  fact("config", std::format("units={} mm_per_unit={} max_jog_mm_min={} max_feed_override={} "
                             "spindle_override={}..{} max_rpm={}",
                             config->machine_units == LengthUnit::inch ? "inch" : "mm",
                             config->millimetres_per_unit, config->limits.max_jog_velocity,
                             config->limits.max_feed_override, config->limits.min_spindle_override,
                             config->limits.max_spindle_override, config->limits.max_spindle_rpm));
  check("config_limits", near(config->limits.max_jog_velocity, 1800, 0.01) &&
                             near(config->limits.max_feed_override, 1.5) &&
                             near(config->limits.max_spindle_rpm, 12000) &&
                             near(config->travel[2].min, -50));

  auto hal = HalComponent::create();
  check("hal_component", hal.has_value(), hal ? "coffeecnc" : hal.error());
  if (!hal)
    return 1;
  // As the executable does: wire the component once it exists.
  const int wired = std::system(
      std::format("halcmd -i '{}' -f '{}'", config->ini_file, config->postgui_halfile).c_str());
  check("postgui_halfile", wired == 0, config->postgui_halfile);
  auto port = NmlPort::connect(*config, &*hal);
  check("nml_connect", port.has_value(), port ? config->nml_file : port.error());
  if (wired != 0 || !port)
    return 1;

  Rig rig{*config, *hal, **port};
  rig.pump(300ms);
  power(rig);
  unhomed(rig);
  homing(rig);
  axis_jogs(rig);
  hal_stop_without_nml(rig);
  watchdog(rig);
  mdi(rig);
  overrides(rig);
  program(rig);
  tools(rig);
  stale_link(rig);
  power_off(rig);
  for (const auto& message : rig.messages)
    fact("operator_message", "text=" + word(message));
  return failures == 0 ? 0 : 1;
}

} // namespace

auto main(int argc, char** argv) -> int {
  std::string ini_file;
  for (int i = 1; i + 1 < argc; ++i)
    if (std::string_view(argv[i]) == "-ini")
      ini_file = argv[i + 1];
  int code = 1;
  try {
    if (ini_file.empty())
      throw std::runtime_error("usage: coffeecnc_sim_tests -ini <file> (see tests/sim/run.sh)");
    code = run(ini_file);
  } catch (const std::exception& error) {
    check("no_exception", false, error.what());
  }
  // LinuxCNC's start script ends with its own cleanup: this line carries the exit code.
  std::cout << "exit=" << code << std::endl;
  return code;
}
