// Drives the real Runtime (session, worker, control and status threads) on top of NmlPort,
// NmlStatusReader and the HAL component against a running LinuxCNC. This program is the DISPLAY
// program of CoffeeCNC's simulated mill: start it with tests/sim/run.sh, never by hand. It sends
// protocol v1 JSON requests as the shell would and checks each CommandResult and what LinuxCNC
// then reports. Where a check is about the adapter alone (what the port or a pin does without
// the session), the runtime is taken down for it and the test's thread drives port and pins.
//
// Output: one `check=<name> result=pass|fail detail=<text>` line per check, `fact=` lines with
// observations for the record, and `exit=<code>` last. Exit code 0 only when every check passed.

#include <coffeecnc/app/file_system.hpp>
#include <coffeecnc/app/program_store.hpp>
#include <coffeecnc/app/runtime.hpp>
#include <coffeecnc/app/session.hpp>
#include <coffeecnc/linuxcnc/hal_component.hpp>
#include <coffeecnc/linuxcnc/hal_control.hpp>
#include <coffeecnc/linuxcnc/machine_config.hpp>
#include <coffeecnc/linuxcnc/nml_port.hpp>
#include <coffeecnc/linuxcnc/nml_status_reader.hpp>
#include <coffeecnc/linuxcnc/tool_table.hpp>

#include "hal_probe.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
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
auto word(std::string_view given) -> std::string {
  std::string text(given);
  std::ranges::replace(text, ' ', '_');
  std::ranges::replace(text, '\n', '_');
  return text.empty() ? std::string("-") : text;
}

void check(std::string_view name, bool passed, const std::string& detail = {}) {
  if (!passed)
    ++failures;
  std::cout << "check=" << name << " result=" << (passed ? "pass" : "fail")
            << " detail=" << word(detail) << '\n'
            << std::flush;
}

void fact(std::string_view name, const std::string& detail) {
  std::cout << "fact=" << name << ' ' << detail << '\n' << std::flush;
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
// One halcmd command, such as `sets <signal> <value>` or `setp <pin> <value>`.
auto halcmd(const std::string& command) -> bool {
  return std::system(("halcmd " + command).c_str()) == 0;
}
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

auto milliseconds(Clock::duration time) -> double {
  return std::chrono::duration<double, std::milli>(time).count();
}

// Everything the executable is made of, and the test as its shell: the HAL component with the
// control thread's adapter, the port for the worker, a status reader for the status thread, and
// the runtime that runs them. The test looks at LinuxCNC through a status reader and a HAL
// component of its own, never through those of the runtime's threads.
class Rig {
public:
  Rig(const MachineConfig& machine, HalComponent& component, HalShared& shared_pins,
      NmlPort& controller, NmlStatusReader& status_reader, NmlStatusReader& observer,
      const HalProbe& hal_probe)
      : config(machine), hal(component), shared(shared_pins), control(component, shared_pins),
        port(controller), reader(status_reader), view(observer), probe(hal_probe),
        programs(files, machine.program_prefix) {
    start();
  }

  MachineConfig config;
  HalComponent& hal;
  HalShared& shared;
  HalControl control;
  NmlPort& port;
  NmlStatusReader& reader;
  NmlStatusReader& view;
  const HalProbe& probe;
  LocalFileSystem files;
  ProgramStore programs; // [DISPLAY] PROGRAM_PREFIX of the machine
  std::unique_ptr<Runtime> runtime;
  PageToken page;
  RequestId next_id{1};

  // The runtime's threads, a page, and its `ready`.
  void start() {
    {
      // A new runtime counts its pages from one again.
      const std::scoped_lock lock{mutex_};
      results_.clear();
    }
    runtime = std::make_unique<Runtime>(port, reader, control, config.limits, programs,
                                        [this](const Outcome& outcome) -> void { absorb(outcome); });
    // The control thread arms the watchdog and lowers the level a stopped runtime left high.
    pump_until(2s, [&]() -> bool { return probe.watchdog_ok() && !probe.jog_stop(); });
    new_page();
  }
  void new_page() {
    page = runtime->begin_page();
    next_id = 1;
    ready = send(R"("type":"ready")");
  }
  // Ends the threads: the test's thread is then the only user of the port and the pins.
  void stop() { runtime.reset(); }

  // For checks of the adapter alone: `actions` run with the runtime down, on this thread, with
  // the jog-stop level lowered and the watchdog armed as direct_pump keeps them.
  void direct(const std::function<void()>& actions) {
    stop();
    hal.hold_jog_stop(false);
    direct_until(2s, [&]() -> bool { return probe.watchdog_ok() && !probe.jog_stop(); });
    // The servo thread takes two periods to pass the fallen level on to motion.jog-inhibit.
    direct_pump(20ms);
    actions();
    start();
  }
  // The control loop by hand, runtime down: heartbeat, pins, operator messages.
  void direct_pump(std::chrono::milliseconds time) {
    static_cast<void>(direct_until(time, []() -> bool { return false; }));
  }
  auto direct_until(std::chrono::milliseconds timeout, const std::function<bool()>& done) -> bool {
    for (const auto end = Clock::now() + timeout; Clock::now() < end;) {
      control.beat();
      auto published = port.take_messages();
      {
        const std::scoped_lock lock{mutex_};
        std::ranges::transform(published, std::back_inserter(messages_),
                               [](auto& message) -> std::string { return std::move(message.text); });
      }
      if (done())
        return true;
      std::this_thread::sleep_for(5ms);
    }
    return false;
  }

  // One request as the panel sends it, not waiting for its result; `body` is the JSON without
  // braces and ID.
  auto post(std::string_view body) -> RequestId {
    const auto id = next_id++;
    absorb(runtime->submit(page, std::format("{{\"id\":{},{}}}", id, body)));
    return id;
  }
  // The result of a request of the current page, if it has arrived.
  auto result(RequestId id) -> std::optional<Result> {
    const std::scoped_lock lock{mutex_};
    const auto found = results_.find({page.value, id});
    return found == results_.end() ? std::nullopt : std::optional<Result>{found->second};
  }
  auto await(RequestId id, std::chrono::milliseconds timeout = 30s) -> Result {
    std::unique_lock lock{mutex_};
    const std::pair key{page.value, id};
    if (!arrived_.wait_for(lock, timeout, [&]() -> bool { return results_.contains(key); }))
      return {"no_result", "", false};
    return results_.at(key);
  }
  // A request and its result.
  auto send(std::string_view body) -> Result { return await(post(body)); }

  // Lets the runtime run for a while, keeping the test's own view of the status current.
  void pump(std::chrono::milliseconds time) {
    static_cast<void>(pump_until(time, []() -> bool { return false; }));
  }
  // The same until `done` holds. False after the timeout.
  auto pump_until(std::chrono::milliseconds timeout, const std::function<bool()>& done) -> bool {
    for (const auto end = Clock::now() + timeout; Clock::now() < end;) {
      static_cast<void>(view.read());
      if (done())
        return true;
      std::this_thread::sleep_for(5ms);
    }
    return false;
  }

  // The axes at rest and the jog-stop level released for long enough that the session sends
  // jogs again: an increment asked for under the level is refused.
  void settle() {
    pump_until(3s, [&]() -> bool { return idle() && !probe.jog_stop(); });
    pump(5 * Runtime::control_period);
  }

  // Keeps a held jog alive as the panel does: the same request again every 50 ms.
  void hold(std::string_view body, std::chrono::milliseconds time) {
    for (const auto end = Clock::now() + time; Clock::now() < end;) {
      static_cast<void>(send(body));
      pump(50ms);
    }
  }

  auto status() -> NmlStatus { return view.status(); }
  auto snapshot() -> ControllerState { return view.read(); }
  auto machine(std::size_t axis) -> double { return status().controller.axes.at(axis).machine; }
  // Joint 0 as motion reports it to HAL, in millimetres: also while task writes no status.
  [[nodiscard]] auto feedback() const -> double {
    return probe.x() * config.millimetres_per_unit;
  }
  // `link` needs readings a short time apart.
  auto link() -> Link {
    pump(50ms);
    return snapshot().link;
  }

  // The machine is at rest with nothing left to execute.
  auto idle() -> bool {
    const auto state = snapshot();
    return state.interp == InterpState::idle && state.in_position;
  }

  auto has_message(std::string_view part) -> bool {
    const std::scoped_lock lock{mutex_};
    return std::ranges::any_of(
        messages_, [&](const auto& text) -> bool { return text.find(part) != std::string::npos; });
  }
  void clear_messages() {
    const std::scoped_lock lock{mutex_};
    messages_.clear();
  }
  auto all_messages() -> std::vector<std::string> {
    const std::scoped_lock lock{mutex_};
    return messages_;
  }

  // The result of the `ready` of the page begun last.
  Result ready;

private:
  // Called by the worker thread (the sink) and by this one (what submit answers at once).
  void absorb(const Outcome& outcome) {
    const std::scoped_lock lock{mutex_};
    for (const auto& text : outcome.messages) {
      const auto message = Json::parse(text);
      if (message.at("type") == "result")
        results_[{outcome.page.value, message.at("id").get<RequestId>()}] = {
            message.at("outcome").get<std::string>(), message.at("reason").get<std::string>(),
            outcome.send_snapshots};
      else if (message.at("type") == "message")
        messages_.push_back(message.at("text").get<std::string>());
    }
    arrived_.notify_all();
  }

  std::mutex mutex_;
  std::condition_variable arrived_;
  std::map<std::pair<std::uint64_t, RequestId>, Result> results_;
  // Operator messages, from LinuxCNC and from the session, as they arrived.
  std::vector<std::string> messages_;
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
  ~FrozenTask() { resume(); }
  // Lets task run again before the end of the scope.
  void resume() const { static_cast<void>(kill(pid_, SIGCONT)); }

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
  const auto link = rig.link();
  auto state = rig.snapshot();
  check("link_live", link == Link::live, std::string(name(link)));
  check("starts_in_estop", state.machine == MachineState::estop);
  check("watchdog_armed", rig.probe.watchdog_ok());

  check("ready", rig.ready.accepted() && rig.ready.send_snapshots, rig.ready.text());
  const auto early = rig.send(R"("type":"set_machine","state":"on")");
  check("on_rejected_in_estop", early.outcome == "rejected", early.text());
  // The same request without the policy: task reports it done and stays in E-stop.
  rig.direct([&]() -> void {
    const auto forced = rig.port.set_machine_state(MachineState::on);
    check("port_on_in_estop_fails", forced.status == ActionStatus::failed, forced.reason);
  });

  const auto reset = rig.send(R"("type":"set_machine","state":"off")");
  rig.pump(50ms);
  check("estop_reset", reset.accepted() && rig.snapshot().machine == MachineState::off,
        reset.text());
  const auto on = rig.send(R"("type":"set_machine","state":"on")");
  rig.pump(50ms);
  check("machine_on", on.accepted() && rig.snapshot().machine == MachineState::on, on.text());
}

void unhomed(Rig& rig) {
  const auto mdi = rig.send(R"("type":"mdi","text":"G0 X1")");
  check("mdi_rejected_unhomed", mdi.outcome == "rejected", mdi.text());
  // What LinuxCNC itself says when the policy is not in the way.
  rig.direct([&]() -> void {
    auto mode = rig.port.set_task_mode(TaskMode::mdi);
    const auto forced = rig.port.mdi("G0 X1");
    check("port_mdi_unhomed_fails",
          mode.status == ActionStatus::accepted && forced.status == ActionStatus::failed &&
              forced.reason.find("not homed") != std::string::npos,
          forced.reason);
    mode = rig.port.set_task_mode(TaskMode::manual);
    check("mode_back_to_manual",
          mode.status == ActionStatus::accepted && rig.port.snapshot().mode == TaskMode::manual);
  });

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
        pressed.accepted() && released.accepted() && !rig.status().teleop &&
            moving - start > 1.5 && near(rig.machine(0), stopped, 0.001),
        std::format("{} moved={:.3f} after_release={:.3f}", pressed.text(), moving - start,
                    rig.machine(0) - stopped));

  // No refresh: the 150 ms lease expires in the control thread, which raises the jog-stop level;
  // the worker stops the jog over NML.
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
  static_cast<void>(rig.send(release_y));

  // The pin alone stops a jog that nothing else ends: port and pin without the session.
  rig.direct([&]() -> void {
    const auto jog = rig.port.start_jog(Axis::x, 1, 600, JogSpace::joint);
    rig.direct_pump(150ms);
    const double before = rig.feedback();
    rig.hal.hold_jog_stop(true);
    rig.direct_pump(150ms);
    const double after = rig.feedback();
    rig.direct_pump(150ms);
    check("hal_jog_stop_joint",
          jog.status == ActionStatus::accepted && after - before < 1.0 &&
              near(rig.feedback(), after, 0.001),
          std::format("start={}:{} moved_after_request={:.3f} moved_at_rest={:.4f}",
                      static_cast<int>(jog.status), jog.reason, after - before,
                      rig.feedback() - after));
    // While the level is high a new jog is refused: motion.jog-inhibit follows the same level.
    const auto refused = rig.port.start_jog(Axis::x, 1, 600, JogSpace::joint);
    rig.direct_pump(150ms);
    check("no_jog_under_the_level", near(rig.feedback(), after, 0.001),
          std::format("start={} moved={:.4f}", static_cast<int>(refused.status),
                      rig.feedback() - after));
    rig.hal.hold_jog_stop(false);
    static_cast<void>(rig.port.stop_jog(Axis::x));
    rig.direct_pump(50ms);
    check("jog_stop_reported", rig.has_message("Jog aborted by jog-stop"));
  });
}

void homing(Rig& rig) {
  const auto start = Clock::now();
  const auto home = rig.send(R"("type":"home","axis":"all")");
  bool saw_homing = false;
  const bool homed = rig.pump_until(60s, [&]() -> bool {
    const auto state = rig.snapshot();
    bool all = true;
    for (const auto& axis : state.axes) {
      saw_homing = saw_homing || axis.homing;
      all = all && axis.homed && !axis.homing;
    }
    return all;
  });
  const auto seconds = std::chrono::duration<double>(Clock::now() - start).count();
  rig.pump(300ms);
  const auto status = rig.status();
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
  const bool busy = !rig.snapshot().in_position;
  const auto released = rig.send(release_x);
  rig.pump(150ms);
  const double stopped = rig.machine(0);
  rig.pump(150ms);
  check("jog_axis_hold_release",
        pressed.accepted() && released.accepted() && rig.status().teleop && busy &&
            moving - start > 1.5 && near(rig.machine(0), stopped, 0.001),
        std::format("{} moved={:.3f}", pressed.text(), moving - start));

  rig.settle();
  const double z0 = rig.machine(2);
  const auto step = rig.send(R"("type":"jog_step","axis":"z","distance":-0.5,"velocity":600)");
  rig.pump(500ms);
  check("jog_step", step.accepted() && near(rig.machine(2) - z0, -0.5, 0.001),
        std::format("{} moved={:.4f}", step.text(), rig.machine(2) - z0));

  const auto beyond = rig.send(R"("type":"jog_step","axis":"z","distance":500,"velocity":600)");
  check("jog_step_beyond_travel_rejected", beyond.outcome == "rejected", beyond.text());
}

// The key property of the HAL path: it stops a jog while the NML command channel is useless.
// Port and pin without the session.
void hal_stop_without_nml(Rig& rig) {
  rig.direct([&]() -> void {
    const auto pressed = rig.port.start_jog(Axis::x, 1, 600, JogSpace::axis);
    rig.direct_pump(100ms);
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
      // The port is the other thread's for now: only the pins here.
      const auto beat = [&](std::chrono::milliseconds time) -> void {
        for (const auto end = Clock::now() + time; Clock::now() < end;) {
          rig.control.beat();
          std::this_thread::sleep_for(5ms);
        }
      };
      a = rig.feedback();
      beat(100ms);
      b = rig.feedback();
      rig.hal.hold_jog_stop(true);
      beat(150ms);
      c = rig.feedback();
      beat(100ms);
      d = rig.feedback();
      active = hal_pin("motion.jog-is-active");
      worker.join();
      rig.hal.hold_jog_stop(false);
    }
    check("hal_jog_stop_during_nml_wait",
          pressed.status == ActionStatus::accepted && b - a > 0.5 && near(d, c, 0.0001) &&
              active == "FALSE" && nml.status == ActionStatus::timed_out,
          std::format("start={}:{} moving_before={:.3f} moved_after_stop={:.4f} jog_active={} "
                      "nml_stop={}",
                      static_cast<int>(pressed.status), pressed.reason, b - a, d - c, active,
                      nml.status == ActionStatus::timed_out ? "timed_out" : "other"));
    // Task runs again: the stop that was written is executed, and one more is accepted.
    rig.direct_pump(300ms);
    const auto again = rig.port.stop_jog(Axis::x);
    const double rest = rig.feedback();
    rig.direct_pump(100ms);
    check("recovers_after_task_resumes",
          again.status == ActionStatus::accepted && rig.port.snapshot().link == Link::live &&
              near(rig.feedback(), rest, 0.001));
  });
}

// The executable stalls: no heartbeat, no lease handling. The HAL watchdog stops the jog. Port
// and pins without the runtime, whose threads the test cannot stall.
void watchdog(Rig& rig) {
  rig.direct([&]() -> void {
    const auto pressed = rig.port.start_jog(Axis::y, -1, 600, JogSpace::axis);
    rig.direct_pump(100ms);
    const double before = rig.port.status().controller.axes[1].machine;
    std::this_thread::sleep_for(450ms); // the stall; the watchdog's timeout is 300 ms
    const double stalled = rig.port.status().controller.axes[1].machine;
    std::this_thread::sleep_for(100ms);
    const double later = rig.port.status().controller.axes[1].machine;
    const bool tripped = !rig.hal.watchdog_ok();
    const double travelled = before - stalled; // 10 mm/s: about 3 mm in 300 ms
    check("watchdog_stops_jog",
          pressed.status == ActionStatus::accepted && tripped && travelled > 1.5 &&
              travelled < 4.5 && near(later, stalled, 0.001),
          std::format("tripped={} travelled={:.3f} moved_after={:.4f}", tripped, travelled,
                      stalled - later));
    // Beating again arms it again, and jogging works as before.
    rig.direct_pump(200ms);
    const bool armed = rig.hal.watchdog_ok();
    static_cast<void>(rig.port.stop_jog(Axis::y));
    const double y0 = rig.port.status().controller.axes[1].machine;
    const auto again = rig.port.start_jog(Axis::y, -1, 600, JogSpace::axis);
    rig.direct_pump(200ms);
    const double y1 = rig.port.status().controller.axes[1].machine;
    static_cast<void>(rig.port.stop_jog(Axis::y));
    rig.direct_pump(200ms);
    check("watchdog_rearms", armed && again.status == ActionStatus::accepted && y0 - y1 > 1.0,
          std::format("armed={} moved={:.3f}", armed, y0 - y1));
  });
}

// A press that is released again, and the axis at rest: every jog test starts from here.
void rest(Rig& rig) {
  static_cast<void>(rig.send(release_x));
  rig.settle();
}

// What the runtime is for. Without it, a jog start written while task is stopped is executed
// when task runs again, however long ago the reason for it went: the axis moves with nobody
// holding the control. Port and pins alone, to show the hazard is real on this LinuxCNC.
void delayed_start_unfenced(Rig& rig) {
  rig.direct([&]() -> void {
    const double before = rig.feedback();
    ActionResult start;
    double during{};
    {
      const FrozenTask frozen{rig.config.ini_file};
      std::thread worker{[&]() -> void { start = rig.port.start_jog(Axis::x, 1, 600, JogSpace::axis); }};
      for (const auto end = Clock::now() + 400ms; Clock::now() < end;) {
        rig.control.beat();
        std::this_thread::sleep_for(5ms);
      }
      during = rig.feedback();
      frozen.resume();
      worker.join();
    }
    rig.direct_pump(300ms);
    const double after = rig.feedback();
    static_cast<void>(rig.port.stop_jog(Axis::x));
    rig.direct_pump(200ms);
    check("delayed_start_moves_without_the_fence",
          near(during, before, 0.0001) && after - before > 1.0,
          std::format("start={} moved_while_frozen={:.4f} moved_after_resume={:.3f}",
                      static_cast<int>(start.status), during - before, after - before));
  });
}

enum class Reason : std::uint8_t { page_end, release, lease_expiry };

// The delayed start with the runtime: the press is in flight in a stopped task when its reason
// goes. The level must be high from the next control period, stay high without a gap until task
// has run again and confirmed the stop, and the axis must not move at all.
void delayed_start(Rig& rig, Reason reason, std::string_view name) {
  rest(rig);
  rig.clear_messages();
  const double before = rig.feedback();
  double moved = 0;
  const auto sample = [&]() -> void { moved = std::max(moved, std::fabs(rig.feedback() - before)); };
  RequestId press{};
  bool in_flight = false;
  bool high_while_frozen = true;
  Clock::time_point decided;
  Clock::time_point raised;
  Clock::time_point resumed;
  {
    const FrozenTask frozen{rig.config.ini_file};
    press = rig.post(hold_x);
    const auto pressed = Clock::now();
    std::this_thread::sleep_for(60ms);
    // Not answered: the worker is inside the port, waiting for task's echo of the start.
    in_flight = !rig.result(press).has_value();
    decided = Clock::now();
    if (reason == Reason::page_end)
      rig.runtime->end_page(rig.page);
    else if (reason == Reason::release)
      static_cast<void>(rig.post(release_x));
    else
      decided = pressed + JogLease::timeout; // nothing: the lease runs out by itself
    while (!rig.probe.jog_stop() && Clock::now() < decided + 1s)
      std::this_thread::sleep_for(100us);
    raised = Clock::now();
    for (const auto end = pressed + 500ms; Clock::now() < end;) {
      high_while_frozen = high_while_frozen && rig.probe.jog_stop();
      sample();
      std::this_thread::sleep_for(200us);
    }
    resumed = Clock::now();
  }
  // Task runs: it takes the start up now. The level is watched until it falls.
  bool fell = false;
  Clock::time_point released;
  for (const auto end = resumed + 10s; Clock::now() < end && !fell;) {
    fell = !rig.probe.jog_stop();
    released = Clock::now();
    sample();
    std::this_thread::sleep_for(200us);
  }
  for (const auto end = Clock::now() + 300ms; Clock::now() < end;) {
    sample();
    std::this_thread::sleep_for(1ms);
  }
  const double to_level = milliseconds(raised - decided);
  const double held = milliseconds(released - raised);
  const double after_resume = milliseconds(released - resumed);
  const auto answer = reason == Reason::page_end ? std::optional<Result>{} : rig.result(press);
  check(std::format("delayed_start_{}", name),
        in_flight && high_while_frozen && fell && after_resume > 0 && moved < 0.001 &&
            to_level < 30,
        std::format("in_flight={} level_after_decision_ms={:.1f} high_while_frozen={} "
                    "hold_ms={:.1f} released_ms_after_resume={:.1f} moved_mm={:.5f} start_result={}",
                    in_flight, to_level, high_while_frozen, held, after_resume, moved,
                    answer ? answer->text() : std::string("none")));
  if (reason == Reason::page_end) {
    check(std::format("delayed_start_{}_not_answered", name), !rig.result(press).has_value());
    rig.new_page();
  }
  // The control is still held as far as the session knows: only a release frees the axis.
  rest(rig);
  const double x0 = rig.feedback();
  const auto again = rig.send(hold_x);
  rig.hold(hold_x, 150ms);
  const double x1 = rig.feedback();
  rest(rig);
  check(std::format("jog_works_after_delayed_start_{}", name),
        again.accepted() && x1 - x0 > 0.5,
        std::format("{} moved={:.3f}", again.text(), x1 - x0));
}

// A jog that is moving when task stops and its page ends: NML cannot stop it, the level does,
// within the distance one control period and the deceleration take; the level stays until task
// has confirmed the stop.
void moving_jog_page_end(Rig& rig) {
  rest(rig);
  static_cast<void>(rig.send(hold_x));
  rig.hold(hold_x, 200ms);
  double at_decision{};
  double at_rest{};
  double later{};
  bool high_while_frozen = true;
  Clock::time_point decided;
  Clock::time_point raised;
  Clock::time_point resumed;
  {
    const FrozenTask frozen{rig.config.ini_file};
    static_cast<void>(rig.send(hold_x));
    std::this_thread::sleep_for(20ms);
    at_decision = rig.feedback();
    decided = Clock::now();
    rig.runtime->end_page(rig.page);
    while (!rig.probe.jog_stop() && Clock::now() < decided + 1s)
      std::this_thread::sleep_for(100us);
    raised = Clock::now();
    std::this_thread::sleep_for(150ms);
    at_rest = rig.feedback();
    for (const auto end = Clock::now() + 300ms; Clock::now() < end;) {
      high_while_frozen = high_while_frozen && rig.probe.jog_stop();
      std::this_thread::sleep_for(200us);
    }
    later = rig.feedback();
    resumed = Clock::now();
  }
  const bool fell = rig.pump_until(10s, [&]() -> bool { return !rig.probe.jog_stop(); });
  const double held = milliseconds(Clock::now() - raised);
  // 10 mm/s: 0.1 mm in one control period, 0.1 mm to decelerate at 500 mm/s2.
  check("moving_jog_stops_at_page_end_with_task_frozen",
        at_rest - at_decision < 0.35 && near(later, at_rest, 0.0001) && high_while_frozen && fell,
        std::format("level_after_decision_ms={:.1f} moved_after_decision_mm={:.3f} "
                    "moved_at_rest_mm={:.5f} high_while_frozen={} hold_ms={:.1f}",
                    milliseconds(raised - decided), at_rest - at_decision, later - at_rest,
                    high_while_frozen, held));
  rig.new_page();
  rest(rig);
}

// The control thread while the worker is blocked in a command for its whole timeout.
void heartbeat_under_blocked_command(Rig& rig) {
  rest(rig);
  rig.pump(100ms);
  rig.runtime->reset_timing();
  bool watchdog = true;
  Result abort;
  double blocked_ms{};
  {
    const FrozenTask frozen{rig.config.ini_file};
    const auto started = Clock::now();
    const auto id = rig.post(R"("type":"abort")");
    while (!rig.result(id).has_value() && Clock::now() < started + 20s) {
      watchdog = watchdog && rig.probe.watchdog_ok();
      std::this_thread::sleep_for(500us);
    }
    blocked_ms = milliseconds(Clock::now() - started);
    abort = rig.await(id);
  }
  const auto timing = rig.runtime->timing();
  const double longest = milliseconds(timing.longest_control_period);
  const double period = milliseconds(Runtime::control_period);
  // Stated bound: no control period longer than three, a tenth of the watchdog's timeout.
  check("heartbeat_under_blocked_command",
        abort.outcome == "timed_out" && blocked_ms > 900 && watchdog && longest < 3 * period,
        std::format("command={} blocked_ms={:.0f} watchdog_ok={} control_periods={} "
                    "longest_period_ms={:.2f} max_jitter_ms={:.2f}",
                    abort.outcome, blocked_ms, watchdog, timing.control_periods, longest,
                    longest - period));
  rig.pump(300ms);
}

// From the last refresh to the axis at rest.
void dead_man_timing(Rig& rig) {
  rest(rig);
  rig.runtime->reset_timing();
  static_cast<void>(rig.send(hold_x));
  for (int i = 0; i < 6; ++i) {
    std::this_thread::sleep_for(50ms);
    static_cast<void>(rig.send(hold_x));
  }
  const auto refreshed = Clock::now();
  while (!rig.probe.jog_stop() && Clock::now() < refreshed + 2s)
    std::this_thread::sleep_for(100us);
  const auto raised = Clock::now();
  const double at_level = rig.feedback();
  // At rest: the position has not changed for 20 ms.
  auto still_since = Clock::now();
  double last = rig.feedback();
  while (Clock::now() - still_since < 20ms && Clock::now() < raised + 2s) {
    if (const double now = rig.feedback(); now != last) {
      last = now;
      still_since = Clock::now();
    }
    std::this_thread::sleep_for(100us);
  }
  const double to_level = milliseconds(raised - refreshed);
  const double stopping = milliseconds(still_since - raised);
  const double longest = milliseconds(rig.runtime->timing().longest_control_period);
  const double period = milliseconds(Runtime::control_period);
  // 150 ms lease, one control period, the stopping time; and what the measurement adds: the
  // level is polled every 0.1 ms and a period can start late by its measured jitter.
  const double bound = 150 + period + stopping + (longest - period) + 1;
  check("dead_man_timing", to_level + stopping <= bound && to_level >= 149,
        std::format("last_refresh_to_level_ms={:.1f} stopping_ms={:.1f} total_ms={:.1f} "
                    "bound_ms={:.1f} within_150_plus_period={} stop_distance_mm={:.3f} "
                    "longest_period_ms={:.2f}",
                    to_level, stopping, to_level + stopping, bound, to_level <= 150 + period,
                    last - at_level, longest));

  // The control is still held: the refreshes go on and must not start the axis again.
  rig.pump_until(2s, [&]() -> bool { return !rig.probe.jog_stop(); });
  const double stopped = rig.feedback();
  double moved = 0;
  bool all_rejected = true;
  std::string reason;
  for (int i = 0; i < 12; ++i) {
    const auto refresh = rig.send(hold_x);
    all_rejected = all_rejected && refresh.outcome == "rejected";
    reason = refresh.reason;
    for (const auto end = Clock::now() + 50ms; Clock::now() < end;) {
      moved = std::max(moved, std::fabs(rig.feedback() - stopped));
      std::this_thread::sleep_for(1ms);
    }
  }
  check("refreshes_after_a_stop_do_not_restart", all_rejected && moved < 0.001,
        std::format("moved_mm={:.5f} reason={}", moved, reason));
  const auto released = rig.send(release_x);
  rig.pump_until(2s, [&]() -> bool { return !rig.probe.jog_stop(); });
  const auto pressed = rig.send(hold_x);
  rig.hold(hold_x, 150ms);
  const double restarted = rig.feedback();
  rest(rig);
  check("release_then_press_restarts",
        released.accepted() && pressed.accepted() && restarted - stopped > 0.5,
        std::format("{} moved={:.3f}", pressed.text(), restarted - stopped));
}

// An increment is motion nobody holds: the end of its page stops it.
void increment_interrupted_by_page_end(Rig& rig) {
  rest(rig);
  const double before = rig.feedback();
  const auto step = rig.send(R"("type":"jog_step","axis":"x","distance":20,"velocity":600)");
  std::this_thread::sleep_for(300ms);
  const double at_decision = rig.feedback();
  const auto decided = Clock::now();
  rig.runtime->end_page(rig.page);
  while (!rig.probe.jog_stop() && Clock::now() < decided + 1s)
    std::this_thread::sleep_for(100us);
  const double to_level = milliseconds(Clock::now() - decided);
  std::this_thread::sleep_for(150ms);
  const double at_rest = rig.feedback();
  std::this_thread::sleep_for(300ms);
  const double later = rig.feedback();
  check("increment_stops_at_page_end",
        step.accepted() && at_decision - before > 1.0 && at_rest - at_decision < 0.35 &&
            near(later, at_rest, 0.0001) && later - before < 10,
        std::format("{} moved_before={:.3f} level_after_decision_ms={:.1f} "
                    "moved_after_decision_mm={:.3f} of_20_mm={:.3f}",
                    step.text(), at_decision - before, to_level, at_rest - at_decision,
                    later - before));
  rig.pump_until(5s, [&]() -> bool { return !rig.probe.jog_stop(); });
  rig.new_page();
}

// The status reader and the port inside LinuxCNC's NML library at the same time, with task
// stopped and with libnml's error paths taken on both threads. Every call goes through the NML
// gate. What a thread sanitizer run of this proves is that CoffeeCNC's own code around those
// calls is free of races; libnml itself is not instrumented.
void nml_gate(Rig& rig) {
  rig.direct([&]() -> void {
    auto missing = rig.config;
    missing.nml_file = rig.config.directory + "/no-such-file.nml";
    std::atomic<bool> stop{};
    std::atomic<int> readings{};
    std::atomic<int> refused{};
    std::thread status{[&]() -> void {
      while (!stop) {
        static_cast<void>(rig.reader.status());
        ++readings;
        if (readings % 50 == 0 && !NmlStatusReader::connect(missing, nullptr))
          ++refused;
      }
    }};
    int port_refused = 0;
    ActionResult abort;
    ActionResult stopped;
    int read_while_frozen = 0;
    {
      const FrozenTask frozen{rig.config.ini_file};
      const int before = readings;
      abort = rig.port.abort(); // polls the status for the whole echo timeout
      for (int i = 0; i < 5; ++i)
        port_refused += NmlPort::connect(missing, nullptr) ? 0 : 1;
      stopped = rig.port.stop_jog(Axis::x);
      read_while_frozen = readings - before;
    }
    for (const auto end = Clock::now() + 300ms; Clock::now() < end;) {
      rig.control.beat();
      static_cast<void>(rig.port.take_messages());
      std::this_thread::sleep_for(5ms);
    }
    const auto after = rig.port.set_override(Override::feed, 1.0);
    stop = true;
    status.join();
    check("nml_gate_concurrent_use",
          abort.status == ActionStatus::timed_out && stopped.status == ActionStatus::timed_out &&
              port_refused == 5 && refused > 0 && read_while_frozen > 100 &&
              after.status == ActionStatus::accepted && rig.port.snapshot().link == Link::live,
          std::format("abort={} readings_while_port_waited={} reader_error_paths={} "
                      "port_error_paths={} command_after={}",
                      static_cast<int>(abort.status), read_while_frozen, refused.load(),
                      port_refused, static_cast<int>(after.status)));
  });
}

void mdi(Rig& rig) {
  const auto units = rig.send(R"("type":"mdi","text":"G21 G90 G54")");
  rig.pump_until(5s, [&]() -> bool { return rig.idle(); });
  check("mdi_units",
        units.accepted() && rig.snapshot().mode == TaskMode::mdi &&
            rig.snapshot().program_units == LengthUnit::mm,
        units.text());
  const auto move = rig.send(R"("type":"mdi","text":"G0 X10 Y-5.5")");
  const bool busy = !rig.idle();
  const bool arrived = rig.pump_until(20s, [&]() -> bool { return rig.idle(); });
  auto status = rig.status();
  check("mdi_move",
        move.accepted() && busy && arrived && near(status.controller.axes[0].machine, 10) &&
            near(status.controller.axes[1].machine, -5.5),
        std::format("{} x={:.3f} y={:.3f}", move.text(), status.controller.axes[0].machine,
                    status.controller.axes[1].machine));

  const auto touch = rig.send(R"("type":"touch_off","axis":"x","value":2.5)");
  rig.pump_until(5s, [&]() -> bool { return rig.idle(); });
  status = rig.status();
  check("touch_off",
        touch.accepted() && near(status.work[0], 2.5) && near(status.g5x[0], 7.5) &&
            status.wcs == 1,
        std::format("{} work_x={:.3f} g5x_x={:.3f} wcs={}", touch.text(), status.work[0],
                    status.g5x[0], status.wcs));
  const auto g55 = rig.send(R"("type":"select_wcs","wcs":2)");
  rig.pump_until(5s, [&]() -> bool { return rig.idle(); });
  const int second = rig.status().wcs;
  const auto g54 = rig.send(R"("type":"select_wcs","wcs":1)");
  rig.pump_until(5s, [&]() -> bool { return rig.idle(); });
  check("select_wcs", g55.accepted() && g54.accepted() && second == 2 && rig.status().wcs == 1,
        std::format("second={} back={}", second, rig.status().wcs));

  // With the interpreter in inches the composed block must carry inches for the same millimetres.
  static_cast<void>(rig.send(R"("type":"mdi","text":"G20")"));
  rig.pump_until(5s, [&]() -> bool { return rig.idle(); });
  const bool inches = rig.snapshot().program_units == LengthUnit::inch;
  const auto inch_touch = rig.send(R"("type":"touch_off","axis":"y","value":25.4)");
  rig.pump_until(5s, [&]() -> bool { return rig.idle(); });
  status = rig.status();
  check("touch_off_in_inch_program_units",
        inches && inch_touch.accepted() && near(status.work[1], 25.4) && near(status.g5x[1], -30.9),
        std::format("units_inch={} work_y={:.3f} g5x_y={:.3f}", inches, status.work[1],
                    status.g5x[1]));
  static_cast<void>(rig.send(R"("type":"mdi","text":"G21")"));
  rig.pump_until(5s, [&]() -> bool { return rig.idle(); });

  // LinuxCNC's refusal reaches the result, and is not repeated as an operator message.
  rig.clear_messages();
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
  auto status = rig.status();
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
  status = rig.status();
  const bool runs_forward =
      status.spindle_direction == SpindleDirection::forward && near(status.spindle_rpm, 6000);
  const auto reverse = rig.send(R"("type":"set_spindle","direction":"reverse","rpm":3000)");
  rig.pump(50ms);
  status = rig.status();
  const bool runs_reverse =
      status.spindle_direction == SpindleDirection::reverse && near(status.spindle_rpm, 3000);
  const auto stop = rig.send(R"("type":"set_spindle","direction":"stopped","rpm":0)");
  rig.pump(50ms);
  status = rig.status();
  check("spindle",
        forward.accepted() && reverse.accepted() && stop.accepted() && runs_forward &&
            runs_reverse && status.spindle_direction == SpindleDirection::stopped,
        std::format("forward={} reverse={} {}", runs_forward, runs_reverse, stop.text()));

  const auto flood = rig.send(R"("type":"set_coolant","flood":true,"mist":false)");
  rig.pump(300ms);
  status = rig.status();
  const bool flood_only = status.flood && !status.mist;
  const auto both = rig.send(R"("type":"set_coolant","flood":true,"mist":true)");
  rig.pump(300ms);
  status = rig.status();
  const bool flood_and_mist = status.flood && status.mist;
  const auto none = rig.send(R"("type":"set_coolant","flood":false,"mist":false)");
  rig.pump(300ms);
  status = rig.status();
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
    if (auto line = trace_line(rig.status()); line != last) {
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
  const auto paused = [&]() -> bool { return rig.status().paused; };
  const auto request = [&]() -> bool { return rig.snapshot().requested_tool.has_value(); };

  // The program store decides before LinuxCNC is asked: nothing outside the program directory,
  // and nothing that is not a file in it.
  const auto outside = rig.send(R"("type":"open_program","path":"/etc/hostname")");
  check("open_outside_program_directory_rejected",
        outside.outcome == "rejected" && !rig.snapshot().program_loaded, outside.text());
  const auto missing = rig.send(R"("type":"open_program","path":"none.ngc")");
  check("open_missing_program_rejected", missing.outcome == "rejected" && !missing.reason.empty(),
        missing.text());
  const auto listed = rig.send(R"("type":"list_directory","path":null)");
  check("list_program_directory", listed.accepted(), listed.text());
  const auto open = rig.send(std::format(R"("type":"open_program","path":"{}")", file));
  check("open_program",
        open.accepted() && rig.snapshot().program_loaded &&
            rig.status().program_file == file,
        open.text());
  const auto optional =
      rig.send(R"("type":"set_program_option","option":"optional_stop","on":true)");
  check("optional_stop_on", optional.accepted() && rig.status().optional_stop,
        optional.text());

  const auto run = rig.send(R"("type":"run")");
  // Into the first feed move (line 3).
  const bool cutting = traced(rig, 10s, [&]() -> bool {
    const auto status = rig.status();
    return status.motion_line == 3 && status.work[0] > 3;
  });
  check("run", run.accepted() && cutting && rig.snapshot().mode == TaskMode::automatic,
        run.text());

  const auto pause = rig.send(R"("type":"pause")");
  rig.pump(200ms);
  const double held = rig.machine(0);
  traced(rig, 200ms, []() -> bool { return false; });
  auto status = rig.status();
  check("pause",
        pause.accepted() && status.paused && near(rig.machine(0), held, 0.001) &&
            status.work[0] < 29 && status.controller.interp == InterpState::paused,
        std::format("{} x={:.3f}", pause.text(), status.work[0]));

  const auto resume = rig.send(R"("type":"resume")");
  // Runs on to the optional stop on line 4.
  const bool at_m1 = traced(rig, 10s, [&]() -> bool {
    const auto now = rig.status();
    return now.paused && near(now.work[0], 30);
  });
  status = rig.status();
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
    moved = traced(rig, 1500ms, [&]() -> bool { return rig.status().work[1] > 1; });
  }
  traced(rig, 5s, [&]() -> bool { return near(rig.status().work[1], 20); });
  traced(rig, 800ms, []() -> bool { return false; });
  status = rig.status();
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
    dwell = dwell || rig.status().dwell_seconds_left > 0;
    if (request())
      return true;
    if (paused() && resumes < 6) {
      static_cast<void>(rig.send(R"("type":"resume")"));
      ++resumes;
    }
    return false;
  });
  auto state = rig.snapshot();
  check("program_tool_change_request",
        asked && state.requested_tool == 2 && dwell && state.tool_in_spindle != 2,
        std::format("requested={} resumes={} saw_dwell={}",
                    state.requested_tool ? *state.requested_tool : -1, resumes, dwell));
  const auto confirmed = rig.send(R"("type":"tool_changed")");
  const bool finished = traced(rig, 30s, [&]() -> bool { return rig.idle() && !paused(); });
  status = rig.status();
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
    const auto now = rig.status();
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
    const auto now = rig.status();
    return now.motion_line == 10 && now.work[0] < 29.5;
  });
  fact("m6_for_the_loaded_tool", std::format("asks_again={}", asked_again));
  const bool inch_program = rig.snapshot().program_units == LengthUnit::inch;
  const auto abort = rig.send(R"("type":"abort")");
  rig.pump(300ms);
  const double stopped = rig.machine(0);
  rig.pump(200ms);
  status = rig.status();
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
      rig.pump_until(10s, [&]() -> bool { return rig.snapshot().requested_tool.has_value(); });
  const auto waiting = rig.status();
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
  // The confirmation is raised by the control thread and must stay on its pin until LinuxCNC
  // has dropped tool-change: the test's own pins watch both signals.
  const auto confirmation = rig.post(R"("type":"tool_changed")");
  bool raised = false;
  bool dropped_early = false;
  bool finished_handshake = false;
  for (const auto end = Clock::now() + 5s; Clock::now() < end && !finished_handshake;) {
    const bool asking = rig.probe.tool_change();
    const bool changed = rig.probe.tool_changed();
    raised = raised || changed;
    dropped_early = dropped_early || (raised && asking && !changed);
    finished_handshake = raised && !asking && !changed;
    std::this_thread::sleep_for(200us);
  }
  const auto confirmed = rig.await(confirmation);
  check("tool_changed_pin_held_until_request_drops",
        raised && !dropped_early && finished_handshake,
        std::format("raised={} dropped_early={} finished={}", raised, dropped_early,
                    finished_handshake));
  const bool done = rig.pump_until(
      10s, [&]() -> bool { return rig.idle() && rig.snapshot().tool_in_spindle == 1; });
  const auto after = rig.status();
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
  const auto status = rig.status();
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

// The optional input pins exist on every machine; without [COFFEECNC] ..._SIGNAL in the INI file
// whatever they read is not reported.
void signals_not_configured(Rig& rig) {
  const bool driven = halcmd("setp coffeecnc.spindle-rpm 500") &&
                      halcmd("setp coffeecnc.spindle-at-speed 1") &&
                      halcmd("setp coffeecnc.probe-input 1");
  rig.pump(50ms);
  const auto status = rig.status();
  check("signals_not_configured",
        driven && hal_number("coffeecnc.spindle-rpm") == 500 && !status.spindle_actual_rpm &&
            !status.spindle_at_speed && !status.probe_input,
        std::format("pins_driven={} rpm_pin={}", driven, hal_pin("coffeecnc.spindle-rpm")));
}

// G0759 profile: the three signals are declared in the INI file and connected by its post-GUI
// file to the machine's signal names.
void g0759_signals(Rig& rig) {
  auto status = rig.status();
  check("signals_at_rest",
        status.spindle_actual_rpm == 0.0 && status.spindle_at_speed == true &&
            status.probe_input == false,
        std::format("rpm={} at_speed={} probe={}", status.spindle_actual_rpm.value_or(-1),
                    status.spindle_at_speed.value_or(false), status.probe_input.value_or(true)));
  const bool set = halcmd("sets spindle-vel-fb-rpm-abs 1234.5") &&
                   halcmd("sets spindle-at-speed 0") && halcmd("sets probe-in 1");
  rig.pump(50ms);
  status = rig.status();
  check("signals_report_values",
        set && status.spindle_actual_rpm == 1234.5 && status.spindle_at_speed == false &&
            status.probe_input == true,
        std::format("rpm={} at_speed={} probe={}", status.spindle_actual_rpm.value_or(-1),
                    status.spindle_at_speed.value_or(true), status.probe_input.value_or(false)));
  const bool negative = halcmd("sets spindle-vel-fb-rpm-abs -600");
  rig.pump(50ms);
  check("spindle_rpm_is_a_magnitude", negative && rig.status().spindle_actual_rpm == 600.0);
  const bool reset = halcmd("sets spindle-vel-fb-rpm-abs 0") && halcmd("sets spindle-at-speed 1") &&
                     halcmd("sets probe-in 0");
  rig.pump(50ms);
  check("signals_reset", reset && rig.status().probe_input == false);
}

auto none_homed(Rig& rig) -> bool {
  return std::ranges::none_of(rig.snapshot().axes,
                              [](const auto& axis) -> bool { return axis.homed; });
}

// [TRAJ] NO_FORCE_HOMING = 1: LinuxCNC executes MDI blocks and programs on an unhomed machine.
void g0759_unhomed(Rig& rig) {
  check("starts_unhomed", none_homed(rig) && !rig.config.limits.homing_required);
  fact("unhomed_start", std::format("teleop={}", rig.status().teleop));

  const auto move = rig.send(R"("type":"mdi","text":"G21 G90 G54 G0 X2 Y-1")");
  const bool arrived = rig.pump_until(20s, [&]() -> bool { return rig.idle(); });
  auto status = rig.status();
  check("mdi_move_unhomed",
        move.accepted() && arrived && near(status.controller.axes[0].machine, 2) &&
            near(status.controller.axes[1].machine, -1) && none_homed(rig) &&
            status.controller.mode == TaskMode::mdi,
        std::format("{} x={:.3f} y={:.3f} teleop={}", move.text(),
                    status.controller.axes[0].machine, status.controller.axes[1].machine,
                    status.teleop));

  const auto touch = rig.send(R"("type":"touch_off","axis":"x","value":10)");
  rig.pump_until(5s, [&]() -> bool { return rig.idle(); });
  status = rig.status();
  check("touch_off_unhomed",
        touch.accepted() && near(status.work[0], 10) && near(status.g5x[0], -8) && none_homed(rig),
        std::format("{} work_x={:.3f} g5x_x={:.3f}", touch.text(), status.work[0], status.g5x[0]));
  const auto g55 = rig.send(R"("type":"select_wcs","wcs":2)");
  rig.pump_until(5s, [&]() -> bool { return rig.idle(); });
  const int second = rig.status().wcs;
  const auto g54 = rig.send(R"("type":"select_wcs","wcs":1)");
  rig.pump_until(5s, [&]() -> bool { return rig.idle(); });
  check("select_wcs_unhomed",
        g55.accepted() && g54.accepted() && second == 2 && rig.status().wcs == 1,
        std::format("second={} back={}", second, rig.status().wcs));

  // LinuxCNC checks MDI targets against the travel limits on an unhomed machine too, in machine
  // coordinates that then start wherever the machine was switched on.
  const auto beyond = rig.send(R"("type":"mdi","text":"G1 X1500 F600")");
  rig.pump(300ms);
  check("mdi_beyond_travel_fails_unhomed",
        beyond.outcome == "failed" && beyond.reason.find("exceed") != std::string::npos &&
            rig.idle(),
        beyond.text());
}

// Per-axis jog ceilings: X and Y 1500 mm/min, Z 750 mm/min, and the machine is still unhomed, so
// every jog is a joint jog.
void g0759_jogs(Rig& rig) {
  const auto z_fast =
      rig.send(R"("type":"jog","axis":"z","direction":1,"velocity":900,"held":true)");
  check("z_jog_above_axis_ceiling_rejected",
        z_fast.outcome == "rejected" && z_fast.reason.find("Z axis") != std::string::npos &&
            z_fast.reason.find("750") != std::string::npos,
        z_fast.text());
  const auto z_step = rig.send(R"("type":"jog_step","axis":"z","distance":1,"velocity":751)");
  check("z_jog_step_above_axis_ceiling_rejected",
        z_step.outcome == "rejected" && z_step.reason.find("750") != std::string::npos,
        z_step.text());
  const auto x_over =
      rig.send(R"("type":"jog","axis":"x","direction":1,"velocity":1501,"held":true)");
  check("jog_above_global_ceiling_rejected", x_over.outcome == "rejected", x_over.text());

  constexpr auto fast_x = R"("type":"jog","axis":"x","direction":1,"velocity":1500,"held":true)";
  constexpr auto free_x = R"("type":"jog","axis":"x","direction":1,"velocity":1500,"held":false)";
  const double start = rig.machine(0);
  const auto pressed = rig.send(fast_x);
  rig.hold(fast_x, 400ms);
  const double moving = rig.machine(0);
  const bool joint_space = !rig.status().teleop;
  const auto released = rig.send(free_x);
  const bool stopped = rig.pump_until(3s, [&]() -> bool { return rig.idle(); });
  check("x_jog_at_1500_accepted",
        pressed.accepted() && released.accepted() && joint_space && stopped &&
            moving - start > 1.5 && rig.snapshot().mode == TaskMode::manual,
        std::format("{} moved={:.3f} joint_space={}", pressed.text(), moving - start, joint_space));

  rig.settle();
  const double z0 = rig.machine(2);
  const auto z_step_ok = rig.send(R"("type":"jog_step","axis":"z","distance":-0.5,"velocity":750)");
  rig.pump(100ms);
  const bool z_done = rig.pump_until(3s, [&]() -> bool { return rig.idle(); });
  check("z_jog_step_at_ceiling",
        z_step_ok.accepted() && z_done && near(rig.machine(2) - z0, -0.5, 0.001),
        std::format("{} moved={:.4f}", z_step_ok.text(), rig.machine(2) - z0));

  // What an axis (teleop) jog does on the unhomed machine, without the policy in the way.
  rig.direct([&]() -> void {
    const double y0 = rig.port.status().controller.axes[1].machine;
    const auto forced = rig.port.start_jog(Axis::y, 1, 600, JogSpace::axis);
    rig.direct_pump(300ms);
    const auto status = rig.port.status();
    static_cast<void>(rig.port.stop_jog(Axis::y));
    static_cast<void>(rig.direct_until(3s, [&]() -> bool {
      const auto state = rig.port.snapshot();
      return state.interp == InterpState::idle && state.in_position;
    }));
    fact("unhomed_axis_jog",
         std::format("status={} reason={} teleop={} moved={:.3f}",
                     static_cast<int>(forced.status), word(forced.reason), status.teleop,
                     status.controller.axes[1].machine - y0));
  });
}

void g0759_limits(Rig& rig) {
  const auto over = rig.send(R"("type":"set_spindle","direction":"forward","rpm":1501)");
  check("spindle_above_ceiling_rejected", over.outcome == "rejected", over.text());
  const auto top = rig.send(R"("type":"set_spindle","direction":"forward","rpm":1500)");
  rig.pump(100ms);
  auto status = rig.status();
  const bool runs =
      status.spindle_direction == SpindleDirection::forward && near(status.spindle_rpm, 1500);
  const auto stop = rig.send(R"("type":"set_spindle","direction":"stopped","rpm":0)");
  rig.pump(100ms);
  check("spindle_at_ceiling",
        top.accepted() && runs && stop.accepted() &&
            rig.status().spindle_direction == SpindleDirection::stopped,
        std::format("{} runs={}", top.text(), runs));

  const auto feed = rig.send(R"("type":"set_override","which":"feed","value":2.0)");
  rig.pump(100ms);
  const double feed_seen = rig.status().feed_override;
  const auto feed_over = rig.send(R"("type":"set_override","which":"feed","value":2.01)");
  const auto spindle_over = rig.send(R"("type":"set_override","which":"spindle","value":1.01)");
  const auto spindle_under = rig.send(R"("type":"set_override","which":"spindle","value":0.49)");
  const auto spindle_low = rig.send(R"("type":"set_override","which":"spindle","value":0.5)");
  rig.pump(100ms);
  status = rig.status();
  check("override_limits",
        feed.accepted() && near(feed_seen, 2.0, 1e-9) && feed_over.outcome == "rejected" &&
            spindle_over.outcome == "rejected" && spindle_under.outcome == "rejected" &&
            spindle_low.accepted() && near(status.spindle_override, 0.5, 1e-9),
        std::format("feed={} spindle={} {}", feed_seen, status.spindle_override, feed_over.text()));
  for (const auto which : {"feed", "spindle"})
    static_cast<void>(
        rig.send(std::format(R"("type":"set_override","which":"{}","value":1)", which)));
}

// A program on the unhomed machine, with a change to a tool the table gives no length.
void g0759_program(Rig& rig) {
  const std::string file = rig.config.program_prefix + "/coffeecnc_g0759.ngc";
  {
    std::ofstream out{file};
    out << "G21 G90 G54 G49\n" // 1
           "G0 X12 Y0\n"       // 2
           "G1 X20 F1200\n"    // 3
           "T2 M6\n"           // 4
           "G43\n"             // 5
           "G1 Y4\n"           // 6
           "M2\n";             // 7
  }
  const auto open = rig.send(std::format(R"("type":"open_program","path":"{}")", file));
  const auto run = rig.send(R"("type":"run")");
  const bool automatic = rig.snapshot().mode == TaskMode::automatic;
  const bool asked =
      traced(rig, 30s, [&]() -> bool { return rig.snapshot().requested_tool.has_value(); });
  auto status = rig.status();
  check("run_unhomed",
        open.accepted() && run.accepted() && automatic && asked && none_homed(rig) &&
            near(status.work[0], 20),
        std::format("{} {} x={:.3f}", open.text(), run.text(), status.work[0]));
  check("tool_change_request_t2",
        status.controller.requested_tool == 2 && status.controller.tool_in_spindle != 2,
        std::format("requested={} in_spindle={}",
                    status.controller.requested_tool ? *status.controller.requested_tool : -1,
                    status.controller.tool_in_spindle));
  const auto confirmed = rig.send(R"("type":"tool_changed")");
  const bool finished = traced(rig, 30s, [&]() -> bool {
    return rig.idle() && !rig.status().paused && near(rig.status().work[1], 4);
  });
  status = rig.status();
  check("tool_without_length_loaded",
        confirmed.accepted() && finished && status.controller.tool_in_spindle == 2 &&
            near(status.tool_offset[2], 0, 1e-9) && none_homed(rig),
        std::format("{} tool={} tool_offset_z={} y={:.3f}", confirmed.text(),
                    status.controller.tool_in_spindle, status.tool_offset[2], status.work[1]));

  // The length the table lacks is measured: the row gains a Z word.
  const auto touch = rig.send(R"("type":"tool_touch_off","value":3)");
  rig.pump_until(5s, [&]() -> bool { return rig.idle(); });
  status = rig.status();
  std::ifstream in{rig.config.tool_table};
  const std::string table{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  check("tool_touch_off_writes_length",
        touch.accepted() && near(status.work[2], 3) && near(status.tool_offset[2], -3.5) &&
            table.find("Z-3.5") != std::string::npos,
        std::format("{} work_z={:.3f} tool_offset_z={:.3f} table={}", touch.text(), status.work[2],
                    status.tool_offset[2], table));
}

// No home switches and no HOME_SEARCH_VEL: a joint is homed where it stands.
void g0759_homing(Rig& rig) {
  // No joint has a HOME_SEQUENCE. LinuxCNC itself accepts "home all" then, homes nothing and
  // publishes no message (seen with EMC_JOINT_HOME joint -1 on 2.9.4): the port refuses it.
  const auto all = rig.send(R"("type":"home","axis":"all")");
  check("home_all_without_sequence_fails",
        all.outcome == "failed" && all.reason.find("HOME_SEQUENCE") != std::string::npos &&
            none_homed(rig) && !rig.config.home_all_configured,
        all.text());
  bool each = true;
  for (const auto axis : {"x", "y", "z"}) {
    const auto home = rig.send(std::format(R"("type":"home","axis":"{}")", axis));
    each = each && home.accepted();
    rig.pump(300ms);
  }
  const bool homed = rig.pump_until(5s, [&]() -> bool {
    return std::ranges::all_of(rig.snapshot().axes,
                               [](const auto& axis) -> bool { return axis.homed && !axis.homing; });
  });
  rig.pump(300ms);
  const auto status = rig.status();
  // The present position becomes HOME (0): machine coordinates change without motion.
  check("home_each_axis",
        each && homed && status.teleop && near(status.controller.axes[0].machine, 0),
        std::format("homed={} teleop={} x={:.3f} y={:.3f} z={:.3f}", homed, status.teleop,
                    status.controller.axes[0].machine, status.controller.axes[1].machine,
                    status.controller.axes[2].machine));

  constexpr auto fast_y = R"("type":"jog","axis":"y","direction":1,"velocity":1500,"held":true)";
  constexpr auto free_y = R"("type":"jog","axis":"y","direction":1,"velocity":1500,"held":false)";
  const double start = rig.machine(1);
  const auto pressed = rig.send(fast_y);
  rig.hold(fast_y, 400ms);
  const double moving = rig.machine(1);
  const bool world = rig.status().teleop;
  const auto released = rig.send(free_y);
  const bool stopped = rig.pump_until(3s, [&]() -> bool { return rig.idle(); });
  check("axis_jog_after_homing",
        pressed.accepted() && released.accepted() && world && stopped && moving - start > 1.5,
        std::format("{} moved={:.3f} teleop={}", pressed.text(), moving - start, world));
}

void stale_link(Rig& rig) {
  static_cast<void>(rig.send(R"("type":"jog_stop_all")"));
  static_cast<void>(rig.send(release_x));
  const bool live = rig.link() == Link::live;
  Link frozen_link{};
  Result jog;
  Result abort;
  {
    const FrozenTask frozen{rig.config.ini_file};
    rig.pump(1200ms);
    frozen_link = rig.snapshot().link;
    jog = rig.send(hold_x);
    // Never gated by stale state: it is attempted, and its real outcome reported.
    abort = rig.send(R"("type":"abort")");
  }
  check("stale_link_detected", live && frozen_link == Link::stale, std::string(name(frozen_link)));
  check("no_jog_on_stale_link", jog.outcome == "rejected", jog.text());
  check("abort_on_dead_task_times_out", abort.outcome == "timed_out", abort.text());
  rig.pump(300ms);
  check("link_live_again", rig.link() == Link::live);
  static_cast<void>(rig.send(release_x));
}

void power_off(Rig& rig) {
  const auto manual = rig.send(R"("type":"jog_stop_all")");
  const auto off = rig.send(R"("type":"set_machine","state":"off")");
  rig.pump(50ms);
  const bool is_off = rig.snapshot().machine == MachineState::off;
  const auto estop = rig.send(R"("type":"set_machine","state":"estop")");
  rig.pump(50ms);
  check("power_off_and_estop",
        manual.accepted() && off.accepted() && is_off && estop.accepted() &&
            rig.snapshot().machine == MachineState::estop,
        std::format("{} {} {}", manual.text(), off.text(), estop.text()));
  rig.runtime->end_page(rig.page);
  // The end of the executable: every thread joined, the level left high.
  const auto started = Clock::now();
  rig.stop();
  const auto took = milliseconds(Clock::now() - started);
  std::this_thread::sleep_for(20ms);
  check("shutdown_leaves_the_level_high", rig.probe.jog_stop() && took < 2000,
        std::format("jog_stop={} join_ms={:.1f}", rig.probe.jog_stop(), took));
}

void sim_config(const MachineConfig& config) {
  check("config_limits", near(config.limits.max_jog_velocity, 1800, 0.01) &&
                             near(config.limits.max_axis_jog_velocity[2], 1800, 0.01) &&
                             near(config.limits.max_feed_override, 1.5) &&
                             near(config.limits.max_spindle_rpm, 12000) &&
                             near(config.travel[2].min, -50) && config.limits.homing_required &&
                             config.home_all_configured && config.jog_increments.empty() &&
                             !config.signals.spindle_rpm && !config.signals.spindle_at_speed &&
                             !config.signals.probe);
}

void g0759_config(const MachineConfig& config) {
  const std::vector<double> increments{5, 1, 0.5, 0.1, 0.05, 0.01, 0.005};
  check("config_limits",
        config.machine_units == LengthUnit::mm && near(config.limits.max_jog_velocity, 1500) &&
            near(config.limits.max_axis_jog_velocity[0], 1500) &&
            near(config.limits.max_axis_jog_velocity[1], 1500) &&
            near(config.limits.max_axis_jog_velocity[2], 750) &&
            near(config.limits.max_feed_override, 2.0) &&
            near(config.limits.min_spindle_override, 0.5) &&
            near(config.limits.max_spindle_override, 1.0) &&
            near(config.limits.max_spindle_rpm, 1500) && !config.limits.homing_required &&
            near(config.travel[0].min, -1000) && near(config.travel[2].max, 1000) &&
            config.jog_increments == increments && config.signals.spindle_rpm &&
            config.signals.spindle_at_speed && config.signals.probe);
}

auto run(const std::string& ini_file, bool g0759) -> int {
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
  if (g0759)
    g0759_config(*config);
  else
    sim_config(*config);

  auto hal = HalComponent::create("coffeecnc", config->signals);
  check("hal_component", hal.has_value(), hal ? "coffeecnc" : hal.error());
  auto probe = HalProbe::create();
  check("hal_probe", probe.has_value(), probe ? "coffeecnc-test" : probe.error());
  if (!hal || !probe)
    return 1;
  // As the executable does: wire the component once it exists.
  const int wired = std::system(
      std::format("halcmd -i '{}' -f '{}'", config->ini_file, config->postgui_halfile).c_str());
  check("postgui_halfile", wired == 0, config->postgui_halfile);
  // The test's own pins on the signals it watches. Xpos is joint 0 in LinuxCNC's core_sim.hal.
  const bool watching = halcmd("net coffeecnc-jog-stop-request coffeecnc-test.jog-stop") &&
                        halcmd("net coffeecnc-watchdog-ok coffeecnc-test.watchdog-ok") &&
                        halcmd("net coffeecnc-tool-change coffeecnc-test.tool-change") &&
                        halcmd("net coffeecnc-tool-changed coffeecnc-test.tool-changed") &&
                        (g0759 || halcmd("net Xpos coffeecnc-test.x-pos"));
  check("test_pins", watching);
  HalShared shared;
  auto port = NmlPort::connect(*config, &shared);
  check("nml_connect", port.has_value(), port ? config->nml_file : port.error());
  // One status channel for the runtime's status thread, one for this thread.
  auto reader = NmlStatusReader::connect(*config, &shared);
  auto view = NmlStatusReader::connect(*config, &shared);
  check("nml_status_readers", reader.has_value() && view.has_value(),
        reader ? (view ? std::string("two") : view.error()) : reader.error());
  if (wired != 0 || !watching || !port || !reader || !view)
    return 1;

  Rig rig{*config, *hal, shared, **port, **reader, **view, *probe};
  rig.pump(300ms);
  power(rig);
  if (g0759) {
    g0759_signals(rig);
    g0759_unhomed(rig);
    g0759_jogs(rig);
    g0759_limits(rig);
    g0759_program(rig);
    g0759_homing(rig);
    power_off(rig);
    for (const auto& message : rig.all_messages())
      fact("operator_message", "text=" + word(message));
    return failures == 0 ? 0 : 1;
  }
  signals_not_configured(rig);
  unhomed(rig);
  homing(rig);
  axis_jogs(rig);
  hal_stop_without_nml(rig);
  watchdog(rig);
  nml_gate(rig);
  delayed_start_unfenced(rig);
  delayed_start(rig, Reason::page_end, "page_end");
  delayed_start(rig, Reason::release, "release");
  delayed_start(rig, Reason::lease_expiry, "lease_expiry");
  moving_jog_page_end(rig);
  heartbeat_under_blocked_command(rig);
  dead_man_timing(rig);
  increment_interrupted_by_page_end(rig);
  mdi(rig);
  overrides(rig);
  program(rig);
  tools(rig);
  stale_link(rig);
  power_off(rig);
  for (const auto& message : rig.all_messages())
    fact("operator_message", "text=" + word(message));
  return failures == 0 ? 0 : 1;
}

} // namespace

auto main(int argc, char** argv) -> int {
  std::string ini_file;
  bool g0759 = false;
  for (int i = 1; i < argc; ++i) {
    if (std::string_view(argv[i]) == "-ini" && i + 1 < argc)
      ini_file = argv[i + 1];
    // The scenario for run.sh -p g0759.
    g0759 = g0759 || std::string_view(argv[i]) == "g0759";
  }
  int code = 1;
  try {
    if (ini_file.empty())
      throw std::runtime_error(
          "usage: coffeecnc_sim_tests -ini <file> [g0759] (see tests/sim/run.sh)");
    code = run(ini_file, g0759);
  } catch (const std::exception& error) {
    check("no_exception", false, error.what());
  }
  // LinuxCNC's start script ends with its own cleanup: this line carries the exit code.
  std::cout << "exit=" << code << '\n' << std::flush;
  return code;
}
