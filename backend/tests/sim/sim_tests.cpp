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
#include <coffeecnc/core/text_file.hpp>
#include <coffeecnc/app/panel_feed.hpp>
#include <coffeecnc/app/program_store.hpp>
#include <coffeecnc/app/runtime.hpp>
#include <coffeecnc/app/session.hpp>
#include <coffeecnc/app/tool_model_store.hpp>
#include <coffeecnc/app/tool_models.hpp>
#include <coffeecnc/linuxcnc/hal_component.hpp>
#include <coffeecnc/linuxcnc/hal_control.hpp>
#include <coffeecnc/linuxcnc/machine_config.hpp>
#include <coffeecnc/linuxcnc/nml_panel_source.hpp>
#include <coffeecnc/linuxcnc/nml_port.hpp>
#include <coffeecnc/linuxcnc/loaded_tools.hpp>
#include <coffeecnc/linuxcnc/nml_status_reader.hpp>
#include <coffeecnc/linuxcnc/preview_engine.hpp>
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
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <signal.h> // kill, SIGSTOP: POSIX, not in <csignal>
#include <sys/stat.h>
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
// A signal's value as halcmd prints it: TRUE, FALSE or a number.
auto hal_signal(const std::string& signal) -> std::string {
  return output_of("halcmd gets " + signal);
}
auto hal_number(const std::string& pin) -> double { return std::atof(hal_pin(pin).c_str()); }

auto name(Link link) -> std::string_view {
  return link == Link::live ? "live" : link == Link::stale ? "stale" : "down";
}

struct Result {
  std::string outcome;
  std::string reason;
  bool send_snapshots{};
  Json restart{};
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
  // Where the runtime's status thread leaves its readings, and what the port counts: what the
  // executable's panel source is made on.
  const NmlStatusMailbox* mailbox{};
  const NmlLoads* loads{};
  std::unique_ptr<Runtime> runtime;
  PreviewWorker* restart_preview{};
  PreviewEngine* restart_engine{};
  PageToken page;
  RequestId next_id{1};

  // The runtime's threads, a page, and its `ready`.
  void start() {
    {
      // A new runtime counts its pages from one again.
      const std::lock_guard lock{mutex_};
      results_.clear();
    }
    runtime =
        std::make_unique<Runtime>(port, reader, control, config.limits, programs,
                                  [this](const Outcome& outcome) -> void { absorb(outcome); },
                                  nullptr, Runtime::Quit{}, restart_preview, restart_engine);
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
        const std::lock_guard lock{mutex_};
        std::ranges::transform(
            published, std::back_inserter(messages_),
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
    const std::lock_guard lock{mutex_};
    const auto found = results_.find({page.value, id});
    return found == results_.end() ? std::nullopt : std::optional<Result>{found->second};
  }
  auto await(RequestId id, std::chrono::milliseconds timeout = 30s) -> Result {
    for (const auto end = Clock::now() + timeout; Clock::now() < end;) {
      if (const auto found = result(id))
        return *found;
      std::this_thread::sleep_for(500us);
    }
    return {"no_result", "", false};
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
  [[nodiscard]] auto feedback() const -> double { return probe.x() * config.millimetres_per_unit; }
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
    const std::lock_guard lock{mutex_};
    return std::ranges::any_of(
        messages_, [&](const auto& text) -> bool { return text.find(part) != std::string::npos; });
  }
  void clear_messages() {
    const std::lock_guard lock{mutex_};
    messages_.clear();
  }
  auto all_messages() -> std::vector<std::string> {
    const std::lock_guard lock{mutex_};
    return messages_;
  }

  // The result of the `ready` of the page begun last.
  Result ready;

private:
  // Called by the worker thread (the sink) and by this one (what submit answers at once).
  void absorb(const Outcome& outcome) {
    const std::lock_guard lock{mutex_};
    for (const auto& text : outcome.messages) {
      const auto message = Json::parse(text);
      if (message.at("type") == "result")
        results_[{outcome.page.value, message.at("id").get<RequestId>()}] = {
            message.at("outcome").get<std::string>(), message.at("reason").get<std::string>(),
            outcome.send_snapshots, message.value("restart", Json())};
      else if (message.at("type") == "message")
        messages_.push_back(message.at("text").get<std::string>());
    }
  }

  std::mutex mutex_;
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

// For the port used without the session: an action nobody withdraws.
auto always() -> const Wanted& {
  static const Wanted yes = []() -> bool { return true; };
  return yes;
}

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

// Whether the HAL files are the machine's own (run.sh -m), not the simulation profile's.
bool machine_hal = false;

void power(Rig& rig) {
  const auto link = rig.link();
  auto state = rig.snapshot();
  check("link_live", link == Link::live, std::string(name(link)));
  check("starts_in_estop", state.machine == MachineState::estop);
  check("watchdog_armed", rig.probe.watchdog_ok());
  // Quit can reach the adapter after an E-stop superseded the session's ON snapshot.
  // Its stop-only path must preserve that E-stop instead of sending ESTOP_RESET.
  rig.direct([&]() -> void {
    const auto off = rig.port.power_off();
    check("quit_power_off_preserves_estop", off.status == ActionStatus::accepted &&
          rig.port.snapshot().machine == MachineState::estop, off.reason);
  });

  check("ready", rig.ready.accepted() && rig.ready.send_snapshots, rig.ready.text());
  const auto early = rig.send(R"("type":"set_machine","state":"on")");
  check("on_rejected_in_estop", early.outcome == "rejected", early.text());
  // The same request without the policy: task reports it done and stays in E-stop.
  rig.direct([&]() -> void {
    const auto forced = rig.port.set_machine_state(MachineState::on, always());
    check("port_on_in_estop_fails", forced.status == ActionStatus::failed, forced.reason);
  });

  // The G0759's files: the E-stop state is the external chain's input (estop-ext) and nothing
  // else. The operator releases the button.
  if (machine_hal) {
    // Reset only once task has seen the input closed (io reports it once per 100 ms): a reset
    // sent before that is undone by task, which then drops user-enable-out again.
    const bool released = halcmd("sets estop-ext 1");
    rig.pump_until(2s, [&]() -> bool { return rig.snapshot().machine != MachineState::estop; });
    rig.pump(300ms);
    check("external_estop_released", released);
  }
  // A task that no longer answers, with E-stop still readable in the status buffer: that state
  // is no evidence, and the request is not confirmed by it.
  rig.direct([&]() -> void {
    ActionResult unanswered;
    {
      const FrozenTask frozen{rig.config.ini_file};
      unanswered = rig.port.set_machine_state(MachineState::estop, always());
    }
    check("estop_request_to_a_frozen_task_is_not_confirmed",
          unanswered.status == ActionStatus::failed &&
              unanswered.reason == "LinuxCNC's status cannot be read; E-stop was not confirmed.",
          std::format("status={} reason={}", static_cast<int>(unanswered.status),
                      unanswered.reason));
    // Task runs again and takes the queued request: wait for a live link before going on.
    rig.direct_until(5s, [&]() -> bool { return rig.port.status().controller.link == Link::live; });
  });
  check("link_live_again", rig.pump_until(2s, [&]() -> bool {
    return rig.snapshot().link == Link::live;
  }));
  const auto reset = rig.send(R"("type":"set_machine","state":"off")");
  rig.pump(50ms);
  check("estop_reset", reset.accepted() && rig.snapshot().machine == MachineState::off,
        reset.text());
  const auto on = rig.send(R"("type":"set_machine","state":"on")");
  rig.pump(50ms);
  check("machine_on", on.accepted() && rig.snapshot().machine == MachineState::on, on.text());
}

void jog_contact(Rig& rig) {
  rig.clear_messages();
  const double start = rig.feedback();
  const auto jog = rig.send(hold_x);
  rig.hold(hold_x, 200ms);
  const double at_contact = rig.feedback();

  // Model the machine's contact level and the separate pulse on the jog-stop input.
  const bool raised = halcmd("setp coffeecnc.jog-contact 1") &&
                      halcmd("setp coffeecnc-jog-stop.in-2 1");
  const bool reported = rig.pump_until(60ms, [&] {
    return rig.has_message("Jog stopped: probe contact");
  });
  const bool lowered = halcmd("setp coffeecnc-jog-stop.in-2 0");
  const bool stopped = rig.pump_until(300ms, [&] { return rig.idle(); });
  const double at_rest = rig.feedback();
  rig.hold(hold_x, 250ms);
  const double after_refresh = rig.feedback();
  static_cast<void>(rig.send(release_x));
  const bool cleared = halcmd("setp coffeecnc.jog-contact 0");
  const auto messages = rig.all_messages();
  check("probe_contact_stops_jog_without_estop",
        jog.accepted() && raised && lowered && cleared && stopped && at_contact - start > 0.5 &&
            at_rest - at_contact < 1.5 && near(after_refresh, at_rest, 0.001) &&
            rig.snapshot().machine == MachineState::on,
        std::format("start={} moved={:.3f} after_contact={:.3f} while_held={:.4f}", jog.text(),
                    at_contact - start, at_rest - at_contact, after_refresh - at_rest));
  check("probe_contact_reported_once",
        reported && std::ranges::count(messages, "Jog stopped: probe contact") == 1 &&
            !rig.has_message("Jog aborted by jog-stop"),
        std::format("messages={}", messages.size()));
}

void unhomed(Rig& rig) {
  const auto mdi = rig.send(R"("type":"mdi","text":"G0 X1")");
  check("mdi_rejected_unhomed", mdi.outcome == "rejected", mdi.text());
  // What LinuxCNC itself says when the policy is not in the way.
  rig.direct([&]() -> void {
    auto mode = rig.port.set_task_mode(TaskMode::mdi, always());
    const auto forced = rig.port.mdi("G0 X1", always());
    check("port_mdi_unhomed_fails",
          mode.status == ActionStatus::accepted && forced.status == ActionStatus::failed &&
              forced.reason.find("not homed") != std::string::npos,
          forced.reason);
    mode = rig.port.set_task_mode(TaskMode::manual, always());
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
        pressed.accepted() && released.accepted() && !rig.status().teleop && moving - start > 1.5 &&
            near(rig.machine(0), stopped, 0.001),
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
    const auto jog = rig.port.start_jog(Axis::x, 1, 600, JogSpace::joint, always());
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
    const auto refused = rig.port.start_jog(Axis::x, 1, 600, JogSpace::joint, always());
    rig.direct_pump(150ms);
    check("no_jog_under_the_level", near(rig.feedback(), after, 0.001),
          std::format("start={} moved={:.4f}", static_cast<int>(refused.status),
                      rig.feedback() - after));
    rig.hal.hold_jog_stop(false);
    static_cast<void>(rig.port.stop_jog(Axis::x));
    rig.direct_pump(50ms);
    // The level was CoffeeCNC's own: LinuxCNC's text about it goes to the log only.
    check("own_jog_stop_text_not_shown", !rig.has_message("Jog aborted by jog-stop"));
    // Another driver of motion's jog-stop pins than CoffeeCNC's level and the watchdog: halcmd
    // on motion.jog-stop-immediate, which the post-GUI file leaves unconnected. Its text
    // reaches the operator unchanged, also inside the attribution window of the level above.
    rig.clear_messages();
    const auto foreign = rig.port.start_jog(Axis::x, 1, 600, JogSpace::joint, always());
    rig.direct_pump(30ms);
    const bool raised = halcmd("setp motion.jog-stop-immediate 1");
    rig.direct_pump(50ms);
    const bool lowered = halcmd("setp motion.jog-stop-immediate 0");
    static_cast<void>(rig.port.stop_jog(Axis::x));
    rig.direct_pump(100ms);
    const auto shown = rig.all_messages();
    check("foreign_jog_stop_text_shown",
          foreign.status == ActionStatus::accepted && raised && lowered &&
              rig.has_message("Jog aborted by jog-stop-immediate"),
          std::format("start={}:{} messages={}", static_cast<int>(foreign.status), foreign.reason,
                      shown.size()));
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

void restore_spindle_tool(Rig& rig) {
  const auto file = std::filesystem::path(rig.config.tool_table).parent_path() /
                    "coffeecnc-spindle-tool.txt";
  const auto saved = [&] {
    std::ifstream input(file);
    int tool = -1;
    return input >> tool ? tool : -1;
  };
  if (rig.config.limits.homing_required) {
    check("startup_waits_for_homing",
          rig.snapshot().tool_in_spindle == 0 && saved() == 1 &&
              !rig.has_message("restored in the spindle"));
    homing(rig);
  }
  const bool restored = rig.pump_until(4s, [&] {
    return rig.has_message("T1 restored in the spindle");
  });
  const auto status = rig.status();
  const double target_length = rig.config.machine_units == LengthUnit::mm ? 50.0 : 50.8;
  check("startup_restores_spindle_tool_and_compensation",
        restored && status.controller.tool_in_spindle == 1 &&
            near(status.tool_offset[2], target_length, 0.001) &&
            std::ranges::contains(status.active_codes, "G43") &&
            status.controller.mode == TaskMode::manual && !rig.probe.tool_change(),
        std::format("restored={} tool={} length={}", restored, status.controller.tool_in_spindle,
                    status.tool_offset[2]));
  if (!rig.config.limits.homing_required)
    check("startup_restores_without_homing_when_allowed",
          restored && std::ranges::none_of(status.controller.axes,
                                           [](const auto& axis) { return axis.homed; }));

  const auto next = rig.send(R"("type":"mdi","text":"M61 Q2 G43 H2")");
  const bool saved_next = rig.pump_until(2s, [&] { return saved() == 2; });
  check("startup_record_tracks_tool_changes", next.accepted() && saved_next);
  const auto empty = rig.send(R"("type":"mdi","text":"M61 Q0 G49")");
  const bool saved_empty = rig.pump_until(2s, [&] { return saved() == 0; });
  check("startup_record_tracks_empty_spindle", empty.accepted() && saved_empty &&
            rig.snapshot().tool_in_spindle == 0);
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

// Waits out the rest of `time` since `since`.
void pump_rest(Rig& rig, Clock::time_point since, std::chrono::milliseconds time) {
  const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
      time - (Clock::now() - since));
  rig.pump(std::max(left, 0ms));
}

auto joined(const std::vector<std::string>& texts) -> std::string {
  std::string all;
  for (const auto& text : texts)
    all += (all.empty() ? "" : "|") + text;
  return all.empty() ? std::string("-") : all;
}

// A tap of a jog button as the panel sends it (ui/src/components/ManualTab.vue). Continuous:
// the press, and its release about 100 ms later. With an increment selected: one jog_step on the
// press and nothing on the release. A jog CoffeeCNC ends itself leaves no operator message, and
// an increment runs to its full distance.
void jog_taps(Rig& rig) {
  rig.settle();
  rig.clear_messages();
  const double x0 = rig.machine(0);
  const auto pressed_at = Clock::now();
  const auto pressed = rig.send(hold_x);
  pump_rest(rig, pressed_at, 100ms);
  const double at_release = rig.machine(0);
  const auto released = rig.send(release_x);
  rig.pump(300ms);
  const double stopped = rig.machine(0);
  rig.pump(100ms);
  const auto tap_messages = rig.all_messages();
  fact("jog_tap_continuous",
       std::format("held_ms=100 moved_at_release={:.4f} moved={:.4f} after_stop={:.4f}",
                   at_release - x0, stopped - x0, rig.machine(0) - stopped));
  check("jog_tap_continuous_stops",
        pressed.accepted() && released.accepted() && stopped - x0 > 0.1 &&
            near(rig.machine(0), stopped, 0.001),
        std::format("{} {} moved={:.4f}", pressed.text(), released.text(), stopped - x0));
  check("jog_tap_continuous_no_operator_message", tap_messages.empty(), joined(tap_messages));

  // Held with refreshes, then released: the same.
  rig.settle();
  rig.clear_messages();
  static_cast<void>(rig.send(hold_x));
  rig.hold(hold_x, 300ms);
  const auto held_release = rig.send(release_x);
  rig.pump(300ms);
  const auto held_messages = rig.all_messages();
  check("jog_held_release_no_operator_message",
        held_release.accepted() && held_messages.empty(),
        std::format("{} messages={}", held_release.text(), joined(held_messages)));

  // The increment and its name in the check.
  using Tap = std::pair<double, std::string_view>;
  for (const auto& [increment, label] : {Tap{1.0, "1_mm"}, Tap{0.05, "0_05_mm"}}) {
    rig.settle();
    rig.clear_messages();
    const double s0 = rig.machine(0);
    const auto tapped_at = Clock::now();
    const auto step = rig.send(
        std::format(R"("type":"jog_step","axis":"x","distance":{},"velocity":600)", increment));
    pump_rest(rig, tapped_at, 100ms);
    const double step_at_release = rig.machine(0) - s0;
    rig.pump_until(3s, [&]() -> bool { return rig.idle(); });
    rig.pump(100ms);
    const double moved = rig.machine(0) - s0;
    const auto step_messages = rig.all_messages();
    fact("jog_tap_step", std::format("increment={} moved_at_release={:.4f} moved={:.4f}",
                                     increment, step_at_release, moved));
    check(std::format("jog_step_tap_{}_full_increment", label),
          step.accepted() && near(moved, increment, 0.001) && step_messages.empty(),
          std::format("{} moved={:.4f} messages={}", step.text(), moved, joined(step_messages)));
  }
}

// Rapid continuous taps as on the machine: a release can raise the level before task has taken
// up the start, which motion then refuses under jog-inhibit. Such a tap is no error, and the axis
// does not move after its release.
void jog_rapid_taps(Rig& rig) {
  rig.settle();
  rig.clear_messages();
  std::vector<RequestId> sent;
  Clock::time_point released_at;
  for (int tap = 0; tap < 10; ++tap) {
    const auto pressed_at = Clock::now();
    sent.push_back(rig.post(hold_x));
    pump_rest(rig, pressed_at, 80ms);
    released_at = Clock::now();
    sent.push_back(rig.post(release_x));
    pump_rest(rig, released_at, 120ms);
  }
  pump_rest(rig, released_at, 300ms);
  const double settled = rig.machine(0);
  rig.pump(300ms);
  const double later = rig.machine(0);
  int accepted = 0;
  int rejected = 0;
  std::string refusals;
  for (const auto id : sent) {
    const auto result = rig.await(id, 5s);
    if (result.accepted())
      ++accepted;
    else if (result.outcome == "rejected")
      ++rejected;
    else
      refusals += (refusals.empty() ? "" : "|") + result.text();
  }
  const auto messages = rig.all_messages();
  fact("jog_rapid_taps",
       std::format("taps=10 accepted={} rejected={} failed={} moved_after_stop={:.4f}", accepted,
                   rejected, refusals.empty() ? "-" : refusals, later - settled));
  check("jog_rapid_taps_no_failed_result", refusals.empty(), refusals);
  check("jog_rapid_taps_no_operator_message", messages.empty(), joined(messages));
  check("jog_rapid_taps_stable_after_release", near(later, settled, 0.001),
        std::format("{:.4f} {:.4f}", settled, later));
}

// The key property of the HAL path: it stops a jog while the NML command channel is useless.
// Port and pin without the session.
void hal_stop_without_nml(Rig& rig) {
  rig.direct([&]() -> void {
    const auto pressed = rig.port.start_jog(Axis::x, 1, 600, JogSpace::axis, always());
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
    check("recovers_after_task_resumes", again.status == ActionStatus::accepted &&
                                             rig.port.snapshot().link == Link::live &&
                                             near(rig.feedback(), rest, 0.001));
  });
}

// The executable stalls: no heartbeat, no lease handling. The HAL watchdog stops the jog. Port
// and pins without the runtime, whose threads the test cannot stall.
void watchdog(Rig& rig) {
  rig.direct([&]() -> void {
    rig.clear_messages();
    const auto pressed = rig.port.start_jog(Axis::y, -1, 600, JogSpace::axis, always());
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
    // The operator is told what stopped the jog, in CoffeeCNC's words, once.
    const auto told = rig.all_messages();
    const auto watchdog_messages = std::ranges::count_if(told, [](const std::string& text) -> bool {
      return text.find("stopped by the HAL watchdog") != std::string::npos;
    });
    check("watchdog_stop_told_as_watchdog",
          watchdog_messages == 1 && !rig.has_message("Jog aborted by jog-stop"),
          std::format("watchdog_messages={} messages={} first={}", watchdog_messages, told.size(),
                      told.empty() ? std::string("-") : told.front()));
    static_cast<void>(rig.port.stop_jog(Axis::y));
    const double y0 = rig.port.status().controller.axes[1].machine;
    const auto again = rig.port.start_jog(Axis::y, -1, 600, JogSpace::axis, always());
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
      std::thread worker{
          [&]() -> void { start = rig.port.start_jog(Axis::x, 1, 600, JogSpace::axis, always()); }};
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

// What the port wrote after the first `from` commands of its trace, in order.
auto written_since(const Rig& rig, std::size_t from) -> std::vector<NmlPort::Written> {
  auto all = rig.port.writes();
  all.erase(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(std::min(from, all.size())));
  return all;
}
auto trace_text(const std::vector<NmlPort::Written>& writes) -> std::string {
  std::string text;
  for (const auto& written : writes)
    text += std::format("{}{}#{}", text.empty() ? "" : ",", written.type, written.serial);
  return text.empty() ? std::string("none") : text;
}
auto wrote(const std::vector<NmlPort::Written>& writes, long type) -> bool {
  return std::ranges::any_of(writes,
                             [&](const auto& written) -> bool { return written.type == type; });
}

// The delayed start with the runtime. The order of events is what is checked, each seen on its
// own evidence: the jog start is written into LinuxCNC's command queue while task is stopped
// (the port's write trace); its reason goes (page end, release, or the lease running out); the
// level rises; task runs again and takes the start up (it echoes the start's serial number);
// the port writes a jog stop after it (the trace); task takes that up and reports it done; and
// only then does the level fall. The level is sampled all the time and must never be low
// between its rise and the stop's acceptance; the axis must not move at all.
void delayed_start(Rig& rig, Reason reason, std::string_view name) {
  rest(rig);
  rig.clear_messages();
  const auto types = NmlPort::types();
  const double before = rig.feedback();
  double moved = 0;
  const auto sample = [&]() -> void {
    moved = std::max(moved, std::fabs(rig.feedback() - before));
  };
  const auto trace_from = rig.port.writes().size();
  RequestId press{};
  NmlPort::Written start;
  bool written_frozen = false;
  bool low_before_accepted = false;
  Clock::time_point pressed;
  Clock::time_point decided;
  Clock::time_point raised;
  Clock::time_point resumed;
  {
    const FrozenTask frozen{rig.config.ini_file};
    press = rig.post(hold_x);
    pressed = Clock::now();
    // Not "no answer yet", which a worker that waits before writing would also show: the write
    // itself, a jog start, while task cannot take it up.
    while (Clock::now() < pressed + 100ms && start.type != types.jog_start) {
      if (const auto writes = written_since(rig, trace_from); !writes.empty())
        start = writes.back();
      std::this_thread::sleep_for(200us);
    }
    written_frozen = start.type == types.jog_start && !rig.result(press).has_value() &&
                     rig.status().echo_serial - start.serial < 0;
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
      low_before_accepted = low_before_accepted || !rig.probe.jog_stop();
      sample();
      std::this_thread::sleep_for(200us);
    }
    resumed = Clock::now();
  }
  // Task runs. In every pass the status is read first and the level after it: a level seen low
  // in a pass that has not seen the stop accepted fell before the stop was accepted.
  std::optional<Clock::time_point> start_echoed;
  std::optional<NmlPort::Written> stop;
  std::optional<Clock::time_point> stop_accepted;
  std::optional<Clock::time_point> fell;
  for (const auto end = resumed + 10s; Clock::now() < end && !fell;) {
    const auto status = rig.status();
    const auto now = Clock::now();
    if (!start_echoed && status.echo_serial - start.serial >= 0)
      start_echoed = now;
    if (!stop)
      for (const auto& written : written_since(rig, trace_from))
        if (!stop && written.type == types.jog_stop && written.serial - start.serial > 0)
          stop = written;
    // Done as task reports it, or left behind by a later command of the port.
    if (stop && !stop_accepted &&
        (status.echo_serial - stop->serial > 0 ||
         (status.echo_serial == stop->serial && status.command_done)))
      stop_accepted = now;
    if (!rig.probe.jog_stop()) {
      fell = Clock::now();
      low_before_accepted = low_before_accepted || !stop_accepted;
    }
    sample();
    std::this_thread::sleep_for(200us);
  }
  for (const auto end = Clock::now() + 300ms; Clock::now() < end;) {
    sample();
    std::this_thread::sleep_for(1ms);
  }
  const auto ms = [&](const std::optional<Clock::time_point>& at) -> double {
    return at ? milliseconds(*at - pressed) : -1;
  };
  // Task resumed while the port still waited for the echo of the start: the start was neither
  // answered nor given up when the reason for it went.
  const bool waiting = resumed - start.at < NmlPort::echo_timeout;
  const bool ordered = written_frozen && waiting && start.at <= decided + 1ms &&
                       decided <= raised && start_echoed && *start_echoed >= resumed && stop &&
                       stop->at >= resumed && stop_accepted && *stop_accepted >= stop->at && fell &&
                       *fell >= *stop_accepted;
  const auto answer = reason == Reason::page_end ? std::optional<Result>{} : rig.result(press);
  check(std::format("delayed_start_{}", name),
        ordered && !low_before_accepted && moved < 0.001 && milliseconds(raised - decided) < 30,
        std::format("jog_start_type={} start={}#{} stop={}#{} trace={} ms_after_press: "
                    "start_written={:.1f} decided={:.1f} level_high={:.1f} task_resumed={:.1f} "
                    "start_echoed={:.1f} stop_written={:.1f} stop_accepted={:.1f} "
                    "level_low={:.1f} level_low_before_stop_accepted={} hold_ms={:.1f} "
                    "moved_mm={:.5f} start_result={}",
                    types.jog_start, start.type, start.serial, stop ? stop->type : 0,
                    stop ? stop->serial : 0, trace_text(written_since(rig, trace_from)),
                    milliseconds(start.at - pressed), milliseconds(decided - pressed),
                    milliseconds(raised - pressed), milliseconds(resumed - pressed),
                    ms(start_echoed), stop ? milliseconds(stop->at - pressed) : -1,
                    ms(stop_accepted), ms(fell), low_before_accepted,
                    fell ? milliseconds(*fell - raised) : -1, moved,
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
  check(std::format("jog_works_after_delayed_start_{}", name), again.accepted() && x1 - x0 > 0.5,
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
  bool watchdog_ok = true;
  Result abort;
  double blocked_ms{};
  {
    const FrozenTask frozen{rig.config.ini_file};
    const auto started = Clock::now();
    const auto id = rig.post(R"("type":"abort")");
    while (!rig.result(id).has_value() && Clock::now() < started + 20s) {
      watchdog_ok = watchdog_ok && rig.probe.watchdog_ok();
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
        abort.outcome == "timed_out" && blocked_ms > 900 && watchdog_ok && longest < 3 * period,
        std::format("command={} blocked_ms={:.0f} watchdog_ok={} control_periods={} "
                    "longest_period_ms={:.2f} max_jitter_ms={:.2f}",
                    abort.outcome, blocked_ms, watchdog_ok, timing.control_periods, longest,
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
    // Those made while task was stopped and the port waited for it.
    std::atomic<bool> frozen_now{};
    std::atomic<int> frozen_readings{};
    std::atomic<int> refused{};
    std::thread status{[&]() -> void {
      while (!stop) {
        static_cast<void>(rig.reader.status());
        ++readings;
        if (frozen_now)
          ++frozen_readings;
        if (readings % 50 == 0 && !NmlStatusReader::connect(missing, nullptr))
          ++refused;
      }
    }};
    int port_refused = 0;
    ActionResult abort;
    ActionResult stopped;

    {
      const FrozenTask frozen{rig.config.ini_file};
      frozen_now = true;
      abort = rig.port.abort(); // polls the status for the whole echo timeout
      for (int i = 0; i < 5; ++i)
        port_refused += NmlPort::connect(missing, nullptr) ? 0 : 1;
      stopped = rig.port.stop_jog(Axis::x);
      frozen_now = false;
    }
    for (const auto end = Clock::now() + 300ms; Clock::now() < end;) {
      rig.control.beat();
      static_cast<void>(rig.port.take_messages());
      std::this_thread::sleep_for(5ms);
    }
    const auto after = rig.port.set_override(Override::feed, 1.0, always());
    stop = true;
    status.join();
    check("nml_gate_concurrent_use",
          abort.status == ActionStatus::timed_out && stopped.status == ActionStatus::timed_out &&
              port_refused == 5 && refused > 0 && frozen_readings.load() > 100 &&
              after.status == ActionStatus::accepted && rig.port.snapshot().link == Link::live,
          std::format("abort={} readings_while_port_waited={} reader_error_paths={} "
                      "port_error_paths={} command_after={}",
                      static_cast<int>(abort.status), frozen_readings.load(), refused.load(),
                      port_refused, static_cast<int>(after.status)));
  });
}

// The port itself withdraws an action whose answer turns to no while it works: asked after its
// own waits and, last, inside the NML gate before each write. Port alone, real NML: the answer
// is made to change at the first, second, third ... time the port asks, and each time nothing
// of what the action exists for may have been written.
void adapter_cancellation(Rig& rig) {
  rig.direct([&]() -> void {
    const auto types = NmlPort::types();
    struct Action {
      std::string name;
      long forbidden; // the command that must not be written once the answer is no
      // Prepares, notes in `from` where the port's trace stands, then acts: what the trace
      // holds after `from` is the action's own.
      std::function<ActionResult(const Wanted&, std::size_t&)> act;
      std::function<void()> undo; // after the run that was not cancelled
    };
    const auto mode = [&](TaskMode wanted) -> void {
      static_cast<void>(rig.port.set_task_mode(wanted, always()));
    };
    const auto rest = [&]() -> void {
      static_cast<void>(rig.direct_until(20s, [&]() -> bool {
        const auto state = rig.port.snapshot();
        return state.interp == InterpState::idle && state.in_position &&
               std::ranges::none_of(state.axes,
                                    [](const auto& axis) -> bool { return axis.homing; });
      }));
    };
    const auto table = [&]() -> std::string {
      return read_text_file(rig.config.tool_table, std::size_t{64} << 20).value_or(std::string());
    };
    Tool tool;
    tool.number = 9;
    tool.pocket = 9;
    tool.length = 10;
    tool.diameter = 1;
    const std::vector<Action> actions{
        {"mdi", types.mdi,
         [&](const Wanted& wanted, std::size_t& from) -> ActionResult {
           mode(TaskMode::mdi);
           from = rig.port.writes().size();
           return rig.port.mdi("G4 P0.01", wanted);
         },
         rest},
        {"run", types.run,
         [&](const Wanted& wanted, std::size_t& from) -> ActionResult {
           mode(TaskMode::automatic);
           from = rig.port.writes().size();
           return rig.port.run(wanted);
         },
         [&]() -> void {
           static_cast<void>(rig.port.abort());
           rest();
         }},
        {"spindle_on", types.spindle_on,
         [&](const Wanted& wanted, std::size_t& from) -> ActionResult {
           from = rig.port.writes().size();
           return rig.port.set_spindle(SpindleDirection::forward, 500, wanted);
         },
         [&]() -> void {
           static_cast<void>(rig.port.set_spindle(SpindleDirection::stopped, 0, always()));
         }},
        // Two writes: the mist must not follow a flood that was written before the answer changed.
        {"coolant", types.coolant_mist_on,
         [&](const Wanted& wanted, std::size_t& from) -> ActionResult {
           static_cast<void>(rig.port.set_coolant(false, false, always()));
           rig.direct_pump(250ms);
           from = rig.port.writes().size();
           return rig.port.set_coolant(true, true, wanted);
         },
         [&]() -> void {
           rig.direct_pump(250ms);
           static_cast<void>(rig.port.set_coolant(false, false, always()));
         }},
        // Two writes: leave world mode, then HOME.
        {"home", types.home,
         [&](const Wanted& wanted, std::size_t& from) -> ActionResult {
           mode(TaskMode::manual);
           static_cast<void>(rig.port.start_jog(Axis::y, 1, 60, JogSpace::axis, always()));
           static_cast<void>(rig.port.stop_jog(Axis::y));
           rest();
           from = rig.port.writes().size();
           return rig.port.home(Axis::x, wanted);
         },
         [&]() -> void {
           rig.direct_pump(300ms);
           rest();
         }},
        {"jog_start", types.jog_start,
         [&](const Wanted& wanted, std::size_t& from) -> ActionResult {
           mode(TaskMode::manual);
           from = rig.port.writes().size();
           return rig.port.start_jog(Axis::y, 1, 60, JogSpace::axis, wanted);
         },
         [&]() -> void {
           static_cast<void>(rig.port.stop_jog(Axis::y));
           rest();
         }},
        {"tool_table", types.load_tool_table,
         [&](const Wanted& wanted, std::size_t& from) -> ActionResult {
           from = rig.port.writes().size();
           return rig.port.set_tool(tool, wanted);
         },
         [&]() -> void { static_cast<void>(rig.port.remove_tool(9, always())); }},
    };
    for (const auto& action : actions) {
      const std::string table_before = table();
      bool clean = true;
      bool partial_commit = false;
      std::string half_sent;
      int cancelled = 0;
      std::string detail;
      for (int deny_at = 1; deny_at < 20; ++deny_at) {
        int asked = 0;
        const Wanted wanted = [&]() -> bool { return ++asked < deny_at; };
        std::size_t from = 0;
        const auto result = action.act(wanted, from);
        // Everything the action itself wrote in this attempt.
        const auto writes = written_since(rig, from);
        if (result.status == ActionStatus::accepted) {
          // The port asked fewer times than that: the action went through.
          clean = clean && wrote(writes, action.forbidden);
          detail += std::format(" completed_after_{}_questions={}:{}", asked,
                                static_cast<int>(result.status), word(result.reason));
          action.undo();
          break;
        }
        ++cancelled;
        // Nothing of the forbidden kind left the port in this attempt, whatever else did.
        const bool temporary_left = std::ifstream{rig.config.tool_table + ".coffeecnc"}.good();
        if (action.name == "tool_table" && result.status == ActionStatus::failed) {
          // Withdrawn after the file was replaced: said so, not rolled back, not loaded.
          clean = clean && table() != table_before && !temporary_left &&
                  result.reason.find("was changed") != std::string::npos;
          partial_commit = true;
          static_cast<void>(rig.port.remove_tool(9, always()));
        } else if (!writes.empty()) {
          // Half sent: not `cancelled`, which would say nothing was written, but `failed` with
          // what was.
          clean = clean && result.status == ActionStatus::failed &&
                  result.reason.find("Cancelled after this was sent") != std::string::npos;
          half_sent = result.reason;
        } else {
          clean = clean && result.status == ActionStatus::cancelled &&
                  (action.name != "tool_table" || (table() == table_before && !temporary_left));
        }
        clean = clean && !wrote(writes, action.forbidden);
        detail += std::format(" no_at_{}:{}:wrote={}", deny_at, static_cast<int>(result.status),
                              trace_text(writes));
      }
      // The tool table has both sides of its rename among the attempts; coolant and home an
      // attempt that stopped between their two commands.
      const bool two_commands = action.name == "coolant" || action.name == "home";
      detail += " half_sent_reason=" + word(half_sent);
      check(std::format("adapter_cancels_{}", action.name),
            clean && cancelled >= 2 && (action.name != "tool_table" || partial_commit) &&
                (!two_commands || !half_sent.empty()),
            std::format("forbidden_type={} cancelled_attempts={}{}", action.forbidden, cancelled,
                        detail));
    }
    mode(TaskMode::manual);

    // The same while the action waits for the NML gate: on its way into libnml when the
    // answer turns to no.
    std::string detail;
    bool clean = true;
    for (const auto& action : actions) {
      if (action.name == "coolant" || action.name == "home" || action.name == "tool_table")
        continue; // their first steps are other writes of this test's own
      if (action.name == "mdi")
        mode(TaskMode::mdi);
      if (action.name == "run")
        mode(TaskMode::automatic);
      if (action.name == "jog_start")
        mode(TaskMode::manual);
      std::atomic<bool> still{true};
      const Wanted wanted = [&]() -> bool { return still; };
      const auto from = rig.port.writes().size();
      ActionResult result;
      std::thread worker;
      {
        const NmlGateHold hold;
        worker = std::thread{[&]() -> void {
          result = action.name == "mdi"   ? rig.port.mdi("G4 P0.01", wanted)
                   : action.name == "run" ? rig.port.run(wanted)
                   : action.name == "spindle_on"
                       ? rig.port.set_spindle(SpindleDirection::forward, 500, wanted)
                       : rig.port.start_jog(Axis::y, 1, 60, JogSpace::axis, wanted);
        }};
        std::this_thread::sleep_for(50ms);
        still = false;
      }
      worker.join();
      const auto writes = written_since(rig, from);
      clean = clean && result.status == ActionStatus::cancelled && writes.empty();
      detail += std::format(" {}={}:wrote={}", action.name, static_cast<int>(result.status),
                            trace_text(writes));
    }
    mode(TaskMode::manual);
    check("adapter_cancels_while_waiting_for_the_gate", clean, detail);
  });
}

// Through the runtime, with task frozen inside the wait that precedes HOME: the page ends while
// the port waits for task to leave world mode; when task runs again that wait ends well, and
// HOME must not follow.
void home_withdrawn_in_its_wait(Rig& rig) {
  rest(rig);
  const auto types = NmlPort::types();
  // World mode, so that homing has to leave it first.
  static_cast<void>(rig.send(hold_x));
  rig.hold(hold_x, 100ms);
  rest(rig);
  const bool world = rig.status().teleop;
  NmlPort::Written waiting;
  const auto from = rig.port.writes().size();
  {
    const FrozenTask frozen{rig.config.ini_file};
    static_cast<void>(rig.post(R"("type":"home","axis":"x")"));
    for (const auto end = Clock::now() + 300ms; Clock::now() < end && waiting.type == 0;) {
      if (const auto writes = written_since(rig, from); !writes.empty())
        waiting = writes.back();
      std::this_thread::sleep_for(200us);
    }
    rig.runtime->end_page(rig.page);
    std::this_thread::sleep_for(200ms);
  }
  // Task takes the mode change up; the port's wait ends.
  const bool taken = rig.pump_until(3s, [&]() -> bool {
    return rig.status().echo_serial - waiting.serial >= 0 && !rig.status().teleop;
  });
  rig.pump(500ms);
  const auto writes = written_since(rig, from);
  const auto state = rig.snapshot();
  check("home_not_sent_after_page_end_in_its_wait",
        world && waiting.type == types.teleop && taken && !wrote(writes, types.home) &&
            state.axes[0].homed && !state.axes[0].homing,
        std::format("in_world_mode={} waited_in_type={} teleop_type={} mode_change_taken={} "
                    "home_type={} wrote={} homing={}",
                    world, waiting.type, types.teleop, taken, types.home, trace_text(writes),
                    state.axes[0].homing));
  rig.new_page();
}

// The panel's result for an action that was cancelled between its two commands, through the
// runtime: flood is on its way when an abort is asked; mist is not sent, and the result says
// that flood was.
void half_sent_action_is_reported(Rig& rig) {
  rest(rig);
  const auto types = NmlPort::types();
  static_cast<void>(rig.send(R"("type":"set_coolant","flood":false,"mist":false)"));
  rig.pump(300ms);
  const auto from = rig.port.writes().size();
  RequestId coolant{};
  RequestId abort{};
  bool flood_written = false;
  {
    // Task is stopped: the port writes flood on and then waits for an echo that cannot come.
    // The abort is asked while it waits there, on the evidence of the write trace and not of
    // any cycle time; then task runs, the wait ends, and mist on is the next thing to write.
    const FrozenTask frozen{rig.config.ini_file};
    coolant = rig.post(R"("type":"set_coolant","flood":true,"mist":true)");
    for (const auto end = Clock::now() + 500ms; Clock::now() < end && !flood_written;) {
      flood_written = wrote(written_since(rig, from), types.coolant_flood_on);
      std::this_thread::sleep_for(200us);
    }
    abort = rig.post(R"("type":"abort")");
  }
  const auto result = rig.await(coolant);
  const auto aborted = rig.await(abort);
  rig.pump(300ms);
  const auto writes = written_since(rig, from);
  const auto status = rig.status();
  check("half_sent_coolant_is_failed_with_what_was_sent",
        flood_written && result.outcome == "failed" &&
            result.reason == "Cancelled after this was sent: flood on. Not sent: mist on." &&
            aborted.accepted() && writes.size() == 2 &&
            writes.front().type == types.coolant_flood_on && writes.back().type == types.abort &&
            !status.mist,
        std::format("{} abort={} wrote={} mist={}", result.text(), aborted.text(),
                    trace_text(writes), status.mist));
  static_cast<void>(rig.send(R"("type":"set_coolant","flood":false,"mist":false)"));
  rig.pump(300ms);

  // Homing: the port leaves world mode, task is stopped in that, an abort is asked. HOME is
  // not sent, and the result says that the mode change was.
  static_cast<void>(rig.send(hold_x));
  rig.hold(hold_x, 100ms);
  rest(rig);
  const bool world = rig.status().teleop;
  const auto home_from = rig.port.writes().size();
  RequestId home{};
  RequestId stop{};
  {
    const FrozenTask frozen{rig.config.ini_file};
    home = rig.post(R"("type":"home","axis":"x")");
    for (const auto end = Clock::now() + 300ms;
         Clock::now() < end && written_since(rig, home_from).empty();)
      std::this_thread::sleep_for(200us);
    stop = rig.post(R"("type":"abort")");
    std::this_thread::sleep_for(200ms);
  }
  const auto homed = rig.await(home);
  const auto stopped = rig.await(stop);
  rig.pump(300ms);
  const auto home_writes = written_since(rig, home_from);
  check("half_sent_home_is_failed_with_what_was_sent",
        world && homed.outcome == "failed" &&
            homed.reason == "Cancelled after this was sent: the change between world and joint "
                            "mode. The rest of the action was not sent." &&
            stopped.accepted() && !wrote(home_writes, types.home) && home_writes.size() == 2 &&
            home_writes.front().type == types.teleop && home_writes.back().type == types.abort,
        std::format("{} abort={} wrote={} home_type={}", homed.text(), stopped.text(),
                    trace_text(home_writes), types.home));
}

// Through the runtime, with the worker held up on its way into libnml: an abort is asked while
// an MDI block and a spindle start wait there. Only the abort is written.
void abort_overtakes_what_waits_for_the_gate(Rig& rig) {
  rest(rig);
  const auto types = NmlPort::types();
  const auto from = rig.port.writes().size();
  RequestId block{};
  RequestId spindle{};
  RequestId abort{};
  {
    const NmlGateHold hold;
    block = rig.post(R"("type":"mdi","text":"G0 X1")");
    spindle = rig.post(R"("type":"set_spindle","direction":"forward","rpm":500)");
    std::this_thread::sleep_for(100ms);
    abort = rig.post(R"("type":"abort")");
    std::this_thread::sleep_for(50ms);
  }
  const auto aborted = rig.await(abort);
  const auto moved = rig.await(block);
  const auto spun = rig.await(spindle);
  rig.pump(200ms);
  const auto writes = written_since(rig, from);
  const auto status = rig.status();
  check("abort_overtakes_what_waits_for_the_gate",
        aborted.accepted() && moved.outcome != "accepted" && spun.outcome != "accepted" &&
            writes.size() == 1 && writes.front().type == types.abort &&
            status.spindle_direction == SpindleDirection::stopped,
        std::format("mdi={} spindle={} abort={} abort_type={} wrote={}", moved.text(), spun.text(),
                    aborted.text(), types.abort, trace_text(writes)));
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

  // The typed Tools command targets an inactive fixture even in G20/G91. Reading
  // it afterward must preserve rotation, G92 and the stationary machine position.
  const auto block = [&](std::string_view wire) -> bool {
    const auto result = rig.send(wire);
    return result.accepted() && rig.pump_until(5s, [&]() -> bool { return rig.idle(); });
  };
  const bool prepared = block(R"("type":"mdi","text":"G91 G10 L2 P3 R15")") &&
                        block(R"("type":"mdi","text":"G92 Z1")");
  const auto before_origin = rig.status();
  const bool written = block(R"("type":"set_work_offset","wcs":3,"offset":[25.4,-50.8,3.5])");
  const bool selection_preserved = rig.status().wcs == 1;
  const bool selected = block(R"("type":"select_wcs","wcs":3)");
  const auto origin = rig.status();
  bool stationary = true;
  for (std::size_t i = 0; i < 3; ++i)
    stationary = stationary && near(origin.controller.axes[i].machine,
                                    before_origin.controller.axes[i].machine, 0.0001);
  check("set_work_origin_in_inch_incremental_mode",
        prepared && written && selection_preserved && selected && stationary &&
            near(origin.g5x[0], 25.4, 0.000001) && near(origin.g5x[1], -50.8, 0.000001) &&
            near(origin.g5x[2], 3.5, 0.000001) && near(origin.rotation_xy, 15) &&
            origin.g92 == before_origin.g92 && rig.snapshot().program_units == LengthUnit::inch,
        std::format("selected={} XYZ={},{},{} rotation={} stationary={}", origin.wcs,
                    origin.g5x[0], origin.g5x[1], origin.g5x[2], origin.rotation_xy, stationary));
  const bool cleared = block(R"("type":"set_work_offset","wcs":3,"offset":[0,0,0])");
  const auto clear = rig.status();
  check("clear_active_work_origin_preserves_rotation_and_shift",
        cleared && near(clear.g5x[0], 0) && near(clear.g5x[1], 0) && near(clear.g5x[2], 0) &&
            near(clear.rotation_xy, 15) && clear.g92 == before_origin.g92,
        std::format("XYZ={},{},{} rotation={}", clear.g5x[0], clear.g5x[1], clear.g5x[2],
                    clear.rotation_xy));
  check("work_origin_test_restores_context",
        block(R"("type":"mdi","text":"G90 G54 G92.1")") &&
            block(R"("type":"mdi","text":"G10 L2 P3 R0")"));
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
  const auto loaded_before = rig.status().program_file;
  const auto outside = rig.send(R"("type":"open_program","path":"/etc/hostname")");
  rig.pump(100ms);
  check("open_outside_program_directory_rejected",
        outside.outcome == "rejected" && rig.status().program_file == loaded_before,
        outside.text());
  const auto missing = rig.send(R"("type":"open_program","path":"none.ngc")");
  check("open_missing_program_rejected", missing.outcome == "rejected" && !missing.reason.empty(),
        missing.text());
  const auto listed = rig.send(R"("type":"list_directory","path":null)");
  check("list_program_directory", listed.accepted(), listed.text());
  const auto open = rig.send(std::format(R"("type":"open_program","path":"{}")", file));
  check("open_program",
        open.accepted() && rig.snapshot().program_loaded && rig.status().program_file == file,
        open.text());
  const auto optional =
      rig.send(R"("type":"set_program_option","option":"optional_stop","on":true)");
  check("optional_stop_on", optional.accepted() && rig.status().optional_stop, optional.text());

  const auto run = rig.send(R"("type":"run")");
  // Into the first feed move (line 3).
  const bool cutting = traced(rig, 10s, [&]() -> bool {
    const auto status = rig.status();
    return status.motion_line == 3 && status.work[0] > 3;
  });
  check("run", run.accepted() && cutting && rig.snapshot().mode == TaskMode::automatic, run.text());

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

// Task's line counters across loads, MDI blocks and an E-stop (fact=lines, `task`: the facts
// of docs/linuxcnc-interface.md, "Observed on 2.9.4"), and beside them what the executable's
// panel source reports of them for the loaded revision (`shown`), read every 50 ms as the
// publisher does.
void lines_across_open(Rig& rig) {
  const std::string first = rig.config.program_prefix + "/coffeecnc_sim.ngc";
  const std::string second = rig.config.program_prefix + "/coffeecnc_sim_second.ngc";
  {
    std::ofstream out{second};
    out << "G21 G90 G54\n"  // 1
           "G0 X5 Y5\n"     // 2
           "G1 X8 F600\n"   // 3
           "M2\n";          // 4
  }
  NmlPanelSource source{rig.config, *rig.mailbox, {},
                        [&](std::string_view path, std::uint64_t revision)
                            -> std::expected<ProgramText, std::string> {
                          return rig.programs.read_program(path, revision);
                        },
                        rig.loads};
  using Pair = std::pair<int, int>;
  Pair shown;
  auto next = Clock::now();
  const auto watch = [&](std::chrono::milliseconds time, const std::function<bool()>& done) -> bool {
    return traced(rig, time, [&]() -> bool {
      if (Clock::now() >= next) {
        const auto program = source.read().status.program;
        shown = {program.current_line, program.motion_line};
        next = Clock::now() + 50ms;
      }
      return done();
    });
  };
  const auto never = []() -> bool { return false; };
  // What the source reported last, as a value of now.
  const auto seen = [&shown]() -> Pair { return shown; };
  // Task's counters now, and one line of both views.
  const auto lines = [&](std::string_view at) -> Pair {
    watch(120ms, never);
    const auto status = rig.status();
    fact("lines",
         std::format("at={} file={} interp={} mode={} task_current_line={} task_motion_line={} "
                     "shown_current_line={} shown_motion_line={}",
                     at, status.program_file.substr(status.program_file.rfind('/') + 1),
                     static_cast<int>(status.controller.interp),
                     static_cast<int>(status.controller.mode), status.current_line,
                     status.motion_line, shown.first, shown.second));
    return {status.current_line, status.motion_line};
  };
  const auto send = [&](std::string_view body) -> bool { return rig.send(body).accepted(); };
  const Pair none{0, 0};
  bool sent = true;

  // How long one status reading takes: a reading is dated when it begins, so this is by how
  // much the link age of a reading errs on the old side.
  {
    std::vector<double> took;
    for (int i = 0; i < 400; ++i) {
      const auto begun = Clock::now();
      static_cast<void>(rig.view.status());
      took.push_back(std::chrono::duration<double, std::micro>(Clock::now() - begun).count());
    }
    std::ranges::sort(took);
    fact("status_read_us", std::format("median={:.0f} p99={:.0f} max={:.0f}", took[200],
                                       took[396], took.back()));
  }

  // The first program was run and aborted before this source existed.
  auto task = lines("after_abort_of_first");
  check("lines_of_a_run_not_seen_are_withheld", shown == none && task.first > 0,
        std::format("task={},{} shown={},{}", task.first, task.second, shown.first, shown.second));

  sent = send(std::format(R"("type":"open_program","path":"{}")", second)) && sent;
  lines("open_second");
  sent = send(R"("type":"mdi","text":"G21 G0 X1")") && sent;
  watch(5s, [&]() -> bool { return rig.idle(); });
  lines("mdi_after_open");
  const bool unrun = shown == none;
  sent = send(R"("type":"step")") && sent;
  watch(1s, never);
  lines("first_step");
  sent = send(R"("type":"step")") && sent;
  watch(2s, never);
  task = lines("second_step");
  check("lines_paused_in_auto_are_reported",
        unrun && rig.status().paused && task.first == 2 && shown.first == 2,
        std::format("task={},{} shown={},{}", task.first, task.second, shown.first, shown.second));
  sent = send(R"("type":"resume")") && sent;
  const bool ended = watch(10s, [&]() -> bool { return rig.idle() && !rig.status().paused; });
  task = lines("end_of_second");
  check("lines_of_a_finished_program_are_kept", ended && task.first == 3 && shown == task,
        std::format("task={},{} shown={},{}", task.first, task.second, shown.first, shown.second));

  // An E-stop after a run of this revision: the lines it stopped at stay.
  const auto ended_at = shown;
  sent = send(R"("type":"set_machine","state":"estop")") && sent;
  lines("estop_after_own_run");
  sent = send(R"("type":"set_machine","state":"off")") && sent;
  sent = send(R"("type":"set_machine","state":"on")") && sent;
  lines("on_after_own_run");
  check("lines_stay_through_estop_after_own_run", shown == ended_at,
        std::format("shown={},{}", shown.first, shown.second));

  // An MDI block and a jog after the run: the counters are no longer the program's.
  sent = send(R"("type":"mdi","text":"G0 X2")") && sent;
  watch(100ms, never);
  const auto during = seen();
  watch(5s, [&]() -> bool { return rig.idle(); });
  task = lines("mdi_after_own_run");
  const auto after_mdi = seen();
  sent = send(R"("type":"jog_step","axis":"x","distance":1,"velocity":300)") && sent;
  watch(3s, [&]() -> bool { return rig.idle(); });
  lines("jog_after_own_run");
  check("lines_are_withheld_after_an_mdi_block",
        during == none && after_mdi == none && seen() == none,
        std::format("during={},{} after={},{} after_jog={},{}", during.first, during.second,
                    after_mdi.first, after_mdi.second, shown.first, shown.second));
  // And back with the next run.
  rig.settle();
  sent = send(R"("type":"run")") && sent;
  const bool again = watch(10s, [&]() -> bool { return shown.first > 0; }) &&
                     watch(10s, [&]() -> bool { return rig.idle(); });
  task = lines("second_run_again");
  check("lines_return_with_the_next_run", again && task.first == 3 && shown == task,
        std::format("task={},{} shown={},{}", task.first, task.second, shown.first, shown.second));

  // Loaded again and not run: the E-stop brings the current line of the run before back.
  sent = send(std::format(R"("type":"open_program","path":"{}")", second)) && sent;
  lines("reopen_second");
  sent = send(R"("type":"set_machine","state":"estop")") && sent;
  task = lines("estop_after_reopen");
  const auto at_estop = seen();
  sent = send(R"("type":"set_machine","state":"off")") && sent;
  lines("off_after_reopen");
  sent = send(R"("type":"set_machine","state":"on")") && sent;
  lines("on_after_reopen");
  fact("homed_after_power_cycle", std::format("x={}", rig.snapshot().axes.at(0).homed));
  check("lines_of_an_earlier_load_are_withheld", at_estop == none && seen() == none,
        std::format("task_at_estop={},{} shown_at_estop={},{} shown={},{}", task.first,
                    task.second, at_estop.first, at_estop.second, shown.first, shown.second));

  sent = send(std::format(R"("type":"open_program","path":"{}")", first)) && sent;
  lines("open_first_again");
  sent = send(R"("type":"run")") && sent;
  watch(3s, [&]() -> bool { return rig.status().motion_line >= 3; });
  task = lines("run_first_again");
  const bool running = shown == task && task.second == 3;
  sent = send(R"("type":"abort")") && sent;
  watch(500ms, never);
  task = lines("abort_first_again");
  check("lines_of_an_aborted_program_are_kept", running && shown == task && task.first > 0,
        std::format("running={} task={},{} shown={},{}", running, task.first, task.second,
                    shown.first, shown.second));
  check("lines_across_open_sequence", sent);
}

// The preview against the machine: what the helper's interpreter computes from the inputs the
// executable would give it, beside what LinuxCNC then does with the same program.
struct PreviewOracle {
  Rig& rig;
  LoadedToolTable table;
  std::vector<RawTool> pass;
  NmlPanelSource source;
  HelperPreviewEngine engine;
  std::string program;

  PreviewOracle(Rig& given, LoadedToolTable attached)
      : rig(given), table(attached),
        source(rig.config, *rig.mailbox,
               [this]() -> std::optional<std::vector<LoadedTool>> {
                 pass = table.read_raw();
                 return loaded_tools(pass, rig.config.millimetres_per_unit);
               },
               [this](std::string_view path, std::uint64_t revision)
                   -> std::expected<ProgramText, std::string> {
                 return rig.programs.read_program(path, revision);
               },
               rig.loads),
        engine(load_preview_settings(rig.config, COFFEECNC_PREVIEW_HELPER_BUILT,
                                     rig.config.directory + "/preview-work")),
        program(rig.config.program_prefix + "/coffeecnc_oracle.ngc") {
    source.raw_tools([this]() -> std::vector<PreviewTool> { return pass; });
  }

  // Loads `text` as the program and computes its preview from the machine as it is now, at
  // rest: the inputs the publisher would hand over.
  auto preview(std::string_view text) -> std::pair<PreviewInputs, PreviewOutcome> {
    {
      std::ofstream out{program};
      out << text;
    }
    const auto open = rig.send(std::format(R"("type":"open_program","path":"{}")", program));
    rig.settle();
    PanelReading reading;
    for (int i = 0; i < 60; ++i) {
      rig.pump(50ms);
      reading = source.read();
      if (reading.status.link == Link::live && reading.tool_table.revision > 0 &&
          !reading.preview_tools.empty() && reading.status.program.file == program)
        break;
    }
    const auto inputs = preview_inputs(reading, reading.commanded_position.value_or(position()));
    auto computed = engine.compute(inputs, {});
    fact("oracle_inputs",
         std::format("open={} codes={} wcs={} feed={} tool={} tool_offset_z={:.3f} g5x={:.3f},"
                     "{:.3f},{:.3f} rotation={:.2f} raw_tools={}",
                     open.outcome, word(join(inputs.start.active_codes)), inputs.start.wcs,
                     inputs.start.feed, inputs.start.tool_in_spindle, inputs.start.tool_offset[2],
                     inputs.systems.at(static_cast<std::size_t>(inputs.start.wcs - 1)).offset[0],
                     inputs.systems.at(static_cast<std::size_t>(inputs.start.wcs - 1)).offset[1],
                     inputs.systems.at(static_cast<std::size_t>(inputs.start.wcs - 1)).offset[2],
                     inputs.systems.at(static_cast<std::size_t>(inputs.start.wcs - 1)).rotation_xy,
                     inputs.raw_tools.size()));
    return {inputs, std::move(computed.outcome)};
  }
  // The controlled point in machine coordinates, mm.
  auto position() -> Vec3 {
    const auto status = rig.status();
    return {status.controller.axes[0].machine, status.controller.axes[1].machine,
            status.controller.axes[2].machine};
  }
  static auto join(const std::vector<std::string>& words) -> std::string {
    std::string text;
    for (const auto& w : words)
      text += (text.empty() ? "" : ",") + w;
    return text;
  }
};

auto distance(const Vec3& a, const Vec3& b) -> double {
  return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) +
                   (a[2] - b[2]) * (a[2] - b[2]));
}

// From a point to a straight piece.
auto distance(const Vec3& p, const Vec3& a, const Vec3& b) -> double {
  const Vec3 ab{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
  const double length = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
  double t = length == 0 ? 0
                         : ((p[0] - a[0]) * ab[0] + (p[1] - a[1]) * ab[1] + (p[2] - a[2]) * ab[2]) /
                               length;
  t = std::clamp(t, 0.0, 1.0);
  return distance(p, {a[0] + t * ab[0], a[1] + t * ab[1], a[2] + t * ab[2]});
}

// A program whose first block is axis words only, in the modal state the machine is in: the
// preview must say what the machine then does: a rapid, a feed move, or an error and no move.
void oracle_first_block(PreviewOracle& oracle, std::string_view state) {
  Rig& rig = oracle.rig;
  auto [inputs, preview] = oracle.preview("X4 Y2\nM2\n");
  const auto before = oracle.position();
  rig.clear_messages();
  const auto run = rig.send(R"("type":"run")");
  double peak = 0;
  rig.pump_until(1s, [&]() -> bool { return !rig.idle(); });
  rig.pump_until(30s, [&]() -> bool {
    peak = std::max(peak, rig.status().velocity);
    return rig.idle() && !rig.status().paused;
  });
  rig.pump(200ms);
  const auto after = oracle.position();
  const bool moved = distance(before, after) > 0.001;
  // The fastest the machine goes is [TRAJ] MAX_LINEAR_VELOCITY, 30 mm/s: a rapid reaches it.
  const bool rapid = peak > 0.9 * 1800;
  const auto messages = rig.all_messages();
  const std::string machine =
      !moved ? "error" : rapid ? "rapid" : "feed";
  std::string said = "error";
  Vec3 end{};
  if (preview.state == PreviewState::ready && !preview.segments.empty()) {
    said = preview.segments.front().kind == MoveKind::rapid ? "rapid" : "feed";
    end = preview.segments.back().points.back();
    for (std::size_t axis = 0; axis < 3; ++axis)
      end.at(axis) += inputs.start.tool_offset.at(axis);
  }
  const bool agree = said == machine && (machine == "error"
                                             ? preview.state == PreviewState::failed &&
                                                   preview.error && preview.error->line == 1 &&
                                                   (!run.accepted() || !messages.empty())
                                             : distance(end, after) < 0.01);
  check(std::format("preview_oracle_first_block_{}", state), agree,
        std::format("codes={} feed={} machine={} peak_mm_min={:.0f} run={} machine_message={} "
                    "moved_to={:.3f},{:.3f},{:.3f} preview={} preview_end={:.3f},{:.3f},{:.3f} "
                    "preview_error={}",
                    PreviewOracle::join(inputs.start.active_codes), inputs.start.feed, machine,
                    peak, run.text(), messages.empty() ? "-" : messages.front(), after[0],
                    after[1], after[2], said, end[0], end[1], end[2],
                    preview.error ? preview.error->text : "-"));
}

// Coordinates: a program with lines, arcs in the three planes, a helix and both units, run
// with a work offset, a rotation and a tool length in force. Every position the machine
// passes through is on the previewed path, and the path has no part the machine skips.
void oracle_path(PreviewOracle& oracle) {
  Rig& rig = oracle.rig;
  const auto block = [&](std::string_view text) -> bool {
    const auto sent = rig.send(std::format(R"("type":"mdi","text":"{}")", text));
    rig.pump_until(20s, [&]() -> bool { return rig.idle(); });
    return sent.accepted();
  };
  const auto was = rig.status();
  bool set = block("G21 G90 G54") && block("G10 L2 P1 X20 Y10 Z5 R30");
  if (rig.snapshot().tool_in_spindle != 1) {
    static_cast<void>(rig.send(R"("type":"mdi","text":"T1 M6")"));
    rig.pump_until(10s, [&]() -> bool { return rig.snapshot().requested_tool.has_value(); });
    static_cast<void>(rig.send(R"("type":"tool_changed")"));
    rig.pump_until(10s, [&]() -> bool { return rig.idle() && rig.snapshot().tool_in_spindle == 1; });
  }
  set = block("G43") && set;
  auto [inputs, preview] = oracle.preview("G21 G90 G61\n"                    // 1
                                          "G0 X0 Y0 Z10\n"                   // 2
                                          "G1 X20 F1500\n"                   // 3
                                          "G2 X30 Y0 I5 J0\n"                // 4
                                          "G18 G3 X40 Z10 I5 K0\n"           // 5
                                          "G19 G2 Y10 Z10 J5 K0\n"           // 6
                                          "G17 G3 X30 Y20 I-10 J0 Z5\n"      // 7
                                          "G20\n"                            // 8
                                          "G1 X1 Y0.5 Z0.3 F60\n"            // 9
                                          "G21\n"                            // 10
                                          "G0 X2 Y2\n"                       // 11
                                          "M2\n");                           // 12
  const Vec3 tool = inputs.start.tool_offset;
  // The path of the controlled point, as the machine reports its position.
  std::vector<std::pair<Vec3, Vec3>> pieces;
  std::vector<Vec3> corners;
  for (const auto& segment : preview.segments)
    for (std::size_t i = 0; i < segment.points.size(); ++i) {
      Vec3 point = segment.points[i];
      for (std::size_t axis = 0; axis < 3; ++axis)
        point.at(axis) += tool.at(axis);
      if (i > 0)
        pieces.emplace_back(corners.back(), point);
      corners.push_back(point);
    }
  std::vector<Vec3> samples;
  const auto run = rig.send(R"("type":"run")");
  rig.pump_until(1s, [&]() -> bool { return !rig.idle(); });
  const bool finished = rig.pump_until(120s, [&]() -> bool {
    samples.push_back(oracle.position());
    return rig.idle() && !rig.status().paused;
  });
  rig.pump(200ms);
  samples.push_back(oracle.position());
  // The farthest any sampled position is from the path, and any point of the path from a
  // sampled position.
  const auto farthest = [](const auto& from, const auto& nearest_to) -> double {
    return std::transform_reduce(
        from.begin(), from.end(), 0.0,
        [](double a, double b) -> double { return std::max(a, b); }, nearest_to);
  };
  const double off_path = farthest(samples, [&](const Vec3& sample) -> double {
    return std::transform_reduce(
        pieces.begin(), pieces.end(), 1e9,
        [](double a, double b) -> double { return std::min(a, b); },
        [&](const auto& piece) -> double { return distance(sample, piece.first, piece.second); });
  });
  const double skipped = farthest(corners, [&](const Vec3& corner) -> double {
    return std::transform_reduce(
        samples.begin(), samples.end(), 1e9,
        [](double a, double b) -> double { return std::min(a, b); },
        [&](const Vec3& sample) -> double { return distance(corner, sample); });
  });
  std::vector<int> lines;
  std::ranges::transform(preview.segments, std::back_inserter(lines), &PreviewSegment::line);
  const Vec3 last = corners.empty() ? Vec3{} : corners.back();
  check("preview_oracle_path",
        set && run.accepted() && finished && preview.state == PreviewState::ready &&
            lines == std::vector<int>{2, 3, 4, 5, 6, 7, 9, 11} && std::fabs(tool[2]) > 1 &&
            off_path < 0.03 && skipped < 1.0 && distance(last, samples.back()) < 0.01,
        std::format("set={} run={} finished={} preview_state={} error={} segments={} points={} "
                    "samples={} tool_offset_z={:.3f} machine_off_preview_mm={:.4f} "
                    "preview_not_visited_mm={:.3f} end_preview={:.3f},{:.3f},{:.3f} "
                    "end_machine={:.3f},{:.3f},{:.3f}",
                    set, run.text(), finished, static_cast<int>(preview.state),
                    preview.error ? preview.error->text : "-", preview.segments.size(),
                    corners.size(), samples.size(), tool[2], off_path, skipped, last[0], last[1],
                    last[2], samples.back()[0], samples.back()[1], samples.back()[2]));
  // As it was before.
  static_cast<void>(block("G21 G90 G64 G49"));
  static_cast<void>(block(std::format("G10 L2 P1 X{:.6f} Y{:.6f} Z{:.6f} R{:.6f}", was.g5x[0],
                                    was.g5x[1], was.g5x[2], was.rotation_xy)));
}

// A service that only counts what it is asked to compute.
class CountingService final : public PreviewService {
public:
  void submit(std::uint64_t context, const PreviewInputs&) override { contexts.push_back(context); }
  [[nodiscard]] auto latest() const -> std::shared_ptr<const Preview> override { return {}; }
  void revalidate(std::uint64_t) override {}
  [[nodiscard]] auto files_generation() const -> std::uint64_t override { return 0; }
  std::vector<std::uint64_t> contexts;
};

// A program that changes G92, a work offset and the tool while it runs: for the whole run,
// paused included, the preview context stays the one of its start and nothing is computed;
// when it has ended, what it left is one new context.
void preview_context_during_a_run(PreviewOracle& oracle) {
  Rig& rig = oracle.rig;
  const auto block = [&](std::string_view text) -> void {
    static_cast<void>(rig.send(std::format(R"("type":"mdi","text":"{}")", text)));
    rig.pump_until(20s, [&]() -> bool { return rig.idle(); });
  };
  block("G21 G90 G54 G49");
  block("G0 X0 Y0 Z20");
  if (rig.snapshot().tool_in_spindle != 1) {
    static_cast<void>(rig.send(R"("type":"mdi","text":"T1 M6")"));
    rig.pump_until(10s, [&]() -> bool { return rig.snapshot().requested_tool.has_value(); });
    static_cast<void>(rig.send(R"("type":"tool_changed")"));
    rig.pump_until(10s, [&]() -> bool { return rig.idle() && rig.snapshot().tool_in_spindle == 1; });
  }
  const auto was = rig.status();
  {
    std::ofstream out{oracle.program};
    out << "G21 G90\n"             // 1
           "G1 X2 F600\n"          // 2
           "G92 X10\n"             // 3
           "G1 X12\n"              // 4
           "M0\n"                  // 5
           "G10 L2 P1 Y3\n"        // 6
           "G1 Y2\n"               // 7
           "T2 M6\n"               // 8
           "G43\n"                 // 9
           "G1 Z25\n"              // 10
           "G4 P0.3\n"             // 11
           "G92.1\n"               // 12
           "M2\n";                 // 13
  }
  const auto open = rig.send(std::format(R"("type":"open_program","path":"{}")", oracle.program));
  rig.settle();
  CountingService service;
  PanelComposer composer{oracle.source.hello(), &service};
  std::uint64_t context = 0;
  // One reading through the composer. Whether THAT reading showed the program running: what
  // the composer did with it is judged by the reading it was given, not by what the machine
  // does a moment later.
  const auto step = [&]() -> bool {
    const auto reading = oracle.source.read();
    const auto messages = composer.compose(reading, PageToken{1}, false, 0ms);
    if (messages)
      for (const auto& text : *messages)
        if (const auto message = Json::parse(text); message.at("type") == "status")
          context = message.at("preview_context").get<std::uint64_t>();
    return reading.status.link != Link::down && reading.status.interp != InterpState::idle;
  };
  // At rest with the program loaded: the context of the run's start.
  for (int i = 0; i < 20; ++i) {
    rig.pump(50ms);
    step();
  }
  const auto at_start = context;
  const auto computed_at_start = service.contexts.size();
  const auto run = rig.send(R"("type":"run")");
  std::set<std::uint64_t> during;
  auto computed_during = computed_at_start;
  bool paused_seen = false;
  bool g92_seen = false;
  bool offset_seen = false;
  bool tool_seen = false;
  const bool ended = rig.pump_until(60s, [&]() -> bool {
    static auto next = Clock::now();
    const auto status = rig.status();
    const bool running = status.controller.interp != InterpState::idle;
    if (Clock::now() >= next) {
      next = Clock::now() + 50ms;
      if (step()) {
        during.insert(context);
        computed_during = service.contexts.size();
      }
    }
    g92_seen = g92_seen || std::fabs(status.g92[0]) > 0.001;
    offset_seen = offset_seen || !near(status.g5x[1], was.g5x[1], 0.001);
    tool_seen = tool_seen || status.controller.tool_in_spindle == 2;
    if (status.paused && running) {
      paused_seen = true;
      // Paused at M0 with G92 in force: still the same context after a while.
      for (int i = 0; i < 6; ++i) {
        rig.pump(50ms);
        if (step()) {
          during.insert(context);
          computed_during = service.contexts.size();
        }
      }
      static_cast<void>(rig.send(R"("type":"resume")"));
    }
    if (rig.snapshot().requested_tool)
      static_cast<void>(rig.send(R"("type":"tool_changed")"));
    return during.size() > 0 && rig.idle() && !status.paused && tool_seen;
  });
  // Ended: what the program left is a new context, one.
  for (int i = 0; i < 30; ++i) {
    rig.pump(50ms);
    step();
  }
  check("preview_context_frozen_during_a_run",
        open.accepted() && run.accepted() && ended && paused_seen && g92_seen && offset_seen &&
            tool_seen && during == std::set<std::uint64_t>{at_start} &&
            computed_during == computed_at_start && context == at_start + 1 &&
            service.contexts.size() == computed_at_start + 1,
        std::format("open={} run={} ended={} paused={} g92_seen={} offset_seen={} tool_seen={} "
                    "context_at_start={} contexts_during={} computations_during={} "
                    "context_after={} computations_after={}",
                    open.text(), run.text(), ended, paused_seen, g92_seen, offset_seen, tool_seen,
                    at_start, during.size(), computed_during - computed_at_start, context,
                    service.contexts.size() - computed_at_start));
  block("G21 G90 G49");
  block(std::format("G10 L2 P1 X{:.6f} Y{:.6f} Z{:.6f} R{:.6f}", was.g5x[0], was.g5x[1], was.g5x[2],
                  was.rotation_xy));
}

// The executable's source and composer attached to a task that is in the middle of a
// program: there is no start state, nothing is computed until the program was seen at rest.
void preview_attached_mid_program(PreviewOracle& oracle) {
  Rig& rig = oracle.rig;
  static_cast<void>(rig.send(R"("type":"mdi","text":"G21 G90 G54 G0 X0 Y0")"));
  rig.pump_until(20s, [&]() -> bool { return rig.idle(); });
  {
    std::ofstream out{oracle.program};
    out << "G21 G90\nG1 X20 F600\nG1 X0\nM2\n";
  }
  const auto open = rig.send(std::format(R"("type":"open_program","path":"{}")", oracle.program));
  rig.settle();
  const auto run = rig.send(R"("type":"run")");
  const bool moving = rig.pump_until(5s, [&]() -> bool { return rig.status().motion_line >= 2; });
  // Only now does a source of its own begin to read.
  NmlPanelSource source{rig.config, *rig.mailbox,
                        [&]() -> std::optional<std::vector<LoadedTool>> {
                          return loaded_tools(oracle.table.read_raw(),
                                              rig.config.millimetres_per_unit);
                        },
                        [&](std::string_view path, std::uint64_t revision)
                            -> std::expected<ProgramText, std::string> {
                          return rig.programs.read_program(path, revision);
                        },
                        rig.loads};
  CountingService service;
  PanelComposer composer{source.hello(), &service};
  Json preview;
  // One reading through the composer, and whether that reading showed the program running.
  const auto step = [&]() -> bool {
    const auto reading = source.read();
    const auto messages = composer.compose(reading, PageToken{1}, false, 0ms);
    if (messages)
      for (const auto& text : *messages)
        if (auto message = Json::parse(text); message.at("type") == "preview")
          preview = std::move(message);
    return reading.status.link != Link::down && reading.status.interp != InterpState::idle;
  };
  std::size_t computed_during = 0;
  Json during;
  bool seen_running = false;
  const bool ended = rig.pump_until(30s, [&]() -> bool {
    static auto next = Clock::now();
    if (Clock::now() < next)
      return false;
    next = Clock::now() + 50ms;
    if (step()) {
      seen_running = true;
      computed_during = service.contexts.size();
      during = preview;
    }
    return seen_running && rig.idle() && !rig.status().paused;
  });
  for (int i = 0; i < 10; ++i) {
    rig.pump(50ms);
    step();
  }
  check("preview_attached_mid_program_has_no_start_state",
        open.accepted() && run.accepted() && moving && ended && seen_running &&
            computed_during == 0 && during.is_object() && during.at("state") == "unavailable" &&
            during.at("error").at("line") == 0 &&
            during.at("error").at("text") == PanelComposer::no_start_state_reason &&
            service.contexts.size() == 1 && preview.at("state") == "pending",
        std::format("run={} moving={} ended={} computations_during={} preview_during={} "
                    "computations_after={} preview_after={}",
                    run.text(), moving, ended, computed_during, during.dump().substr(0, 200),
                    service.contexts.size(), preview.value("state", "-")));
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
  // A confirmation that is withdrawn when the port is about to hand it to the control thread
  // is not handed over: tool-changed stays low. Port and pins alone.
  rig.direct([&]() -> void {
    rig.direct_pump(50ms);
    const auto pending = rig.shared.view(HalShared::Clock::now());
    const bool requested = pending && pending->tool_change_request;
    const auto withdrawn =
        rig.port.tool_changed(1, requested ? pending->tool_change_request->generation : 0,
                              []() -> bool { return false; });
    bool raised_pin = false;
    for (const auto end = Clock::now() + 200ms; Clock::now() < end;) {
      rig.control.beat();
      raised_pin = raised_pin || rig.probe.tool_changed();
      std::this_thread::sleep_for(1ms);
    }
    check("withdrawn_tool_change_confirmation_is_not_raised",
          requested && withdrawn.status == ActionStatus::cancelled && !raised_pin &&
              rig.probe.tool_change(),
          std::format("request_pending={} result={} tool_changed_pin={}", requested,
                      static_cast<int>(withdrawn.status), raised_pin));
  });
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
  check("tool_changed_pin_held_until_request_drops", raised && !dropped_early && finished_handshake,
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
    return read_text_file(rig.config.tool_table, std::size_t{64} << 20).value_or(std::string());
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

// The measured spindle speed and at-speed as a test sets them. On the simulation profile they
// are bare signals. With the machine's own HAL files they come out of its scale, abs and near
// components: the test moves the simulated spindle away from its commanded speed (0 here), and
// at-speed follows by itself.
auto spindle_feedback(double rpm, bool at_speed) -> bool {
  if (machine_hal)
    return halcmd(std::format("sets coffeecnc-sim-spindle-offset {}", rpm / 60));
  return halcmd(std::format("sets spindle-vel-fb-rpm-abs {}", rpm)) &&
         halcmd(std::format("sets spindle-at-speed {}", at_speed ? 1 : 0));
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
  const bool set = spindle_feedback(1234.5, false) && halcmd("sets probe-in 1");
  rig.pump(50ms);
  status = rig.status();
  check("signals_report_values",
        set && status.spindle_actual_rpm && near(*status.spindle_actual_rpm, 1234.5, 1e-6) &&
            status.spindle_at_speed == false && status.probe_input == true,
        std::format("rpm={} at_speed={} probe={}", status.spindle_actual_rpm.value_or(-1),
                    status.spindle_at_speed.value_or(true), status.probe_input.value_or(false)));
  const bool negative = spindle_feedback(-600, false);
  rig.pump(50ms);
  status = rig.status();
  check("spindle_rpm_is_a_magnitude",
        negative && status.spindle_actual_rpm && near(*status.spindle_actual_rpm, 600, 1e-6),
        std::format("rpm={}", status.spindle_actual_rpm.value_or(-1)));
  const bool reset = spindle_feedback(0, true) && halcmd("sets probe-in 0");
  rig.pump(50ms);
  status = rig.status();
  check("signals_reset",
        reset && status.probe_input == false && status.spindle_at_speed == true &&
            status.spindle_actual_rpm == 0.0,
        std::format("rpm={} at_speed={}", status.spindle_actual_rpm.value_or(-1),
                    status.spindle_at_speed.value_or(false)));
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
    const auto forced = rig.port.start_jog(Axis::y, 1, 600, JogSpace::axis, always());
    rig.direct_pump(300ms);
    const auto status = rig.port.status();
    static_cast<void>(rig.port.stop_jog(Axis::y));
    static_cast<void>(rig.direct_until(3s, [&]() -> bool {
      const auto state = rig.port.snapshot();
      return state.interp == InterpState::idle && state.in_position;
    }));
    fact("unhomed_axis_jog",
         std::format("status={} reason={} teleop={} moved={:.3f}", static_cast<int>(forced.status),
                     word(forced.reason), status.teleop, status.controller.axes[1].machine - y0));
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
  const auto table = read_text_file(rig.config.tool_table, std::size_t{64} << 20).value_or(std::string());
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

// Everything a shell command prints.
auto all_output_of(const std::string& command) -> std::string {
  std::string text;
  if (FILE* pipe = popen(command.c_str(), "r"); pipe != nullptr) {
    std::array<char, 4096> buffer{};
    for (std::size_t got = 0; (got = std::fread(buffer.data(), 1, buffer.size(), pipe)) > 0;)
      text.append(buffer.data(), got);
    static_cast<void>(pclose(pipe));
  }
  return text;
}

auto file_text(const std::string& path) -> std::string {
  return read_text_file(path, std::size_t{8} << 20).value_or(std::string());
}

// Where this machine's tool models are kept, as the executable works it out: from the INI
// file, else from the environment tests/sim/run.sh starts its programs in.
auto tool_model_location(const MachineConfig& config) -> ToolModelLocation {
  const auto environment = [](const char* name) -> std::optional<std::string> {
    const char* value = std::getenv(name);
    return value != nullptr ? std::optional<std::string>{value} : std::nullopt;
  };
  return resolve_tool_model_location({config.machine_id, config.data_directory,
                                      environment("XDG_DATA_HOME"), environment("HOME")});
}

// `coffeecnc_sim_tests -ini <file> --tool-models`: another process of this machine opens its
// tool models and prints the snapshot it would send. Needs no LinuxCNC.
auto print_tool_models(const std::string& ini_file) -> int {
  const auto config = load_machine_config(ini_file);
  if (!config) {
    std::cout << config.error() << '\n';
    return 1;
  }
  LocalToolModelFiles files;
  ToolModelArchive archive{files, tool_model_location(*config)};
  archive.open();
  std::cout << archive.snapshot() << '\n' << std::flush;
  return 0;
}

// One session of the executable as far as the tool models go, wired as application.cpp wires
// it: the store on the real files with its writer thread, a runtime that hands it the panel's
// commands, and the publisher, whose `hello` the snapshot follows. Everything the three would
// post to the window is kept in the order it was posted.
class ModelSession {
public:
  ModelSession(Rig& rig, const LoadedToolTable& table)
      : source_(rig.config, *rig.mailbox,
                [&rig, &table]() -> std::optional<std::vector<LoadedTool>> {
                  return loaded_tools(table.read_raw(), rig.config.millimetres_per_unit);
                },
                [&rig](std::string_view path,
                       std::uint64_t revision) -> std::expected<ProgramText, std::string> {
                  return rig.programs.read_program(path, revision);
                },
                rig.loads),
        store_(files_, tool_model_location(rig.config),
               [this](PageToken at, const std::vector<std::string>& messages) -> void {
                 post(at, messages);
               }),
        runtime_(rig.port, rig.reader, rig.control, rig.config.limits, rig.programs,
                 [this](const Outcome& outcome) -> void { post(outcome.page, outcome.messages); },
                 &store_),
        feed_(source_,
              [this](PageToken at, const std::vector<std::string>& messages) -> void {
                post(at, messages);
              },
              nullptr, [this](PageToken at) -> void { store_.send_snapshot(at); }),
        page_(runtime_.begin_page()) {
    static_cast<void>(send(R"("type":"ready")"));
    feed_.page_ready(page_);
  }

  // A request and the position of its result among the messages; nothing when none came.
  auto send(std::string_view body) -> std::optional<std::size_t> {
    const auto id = next_id_++;
    post(page_, runtime_.submit(page_, std::format("{{\"id\":{},{}}}", id, body)).messages);
    return wait([&](const Json& message) -> bool {
      return message.at("type") == "result" && message.at("id") == id;
    });
  }
  // The position of the first message, from `from` on, that `wanted` holds for.
  auto wait(const std::function<bool(const Json&)>& wanted, std::size_t from = 0,
            std::chrono::milliseconds timeout = 10s) -> std::optional<std::size_t> {
    for (const auto end = Clock::now() + timeout; Clock::now() < end;) {
      {
        const std::lock_guard lock{mutex_};
        for (std::size_t i = from; i < messages_.size(); ++i)
          if (wanted(messages_[i]))
            return i;
      }
      std::this_thread::sleep_for(2ms);
    }
    return {};
  }
  auto first(std::string_view type, std::size_t from = 0) -> std::optional<std::size_t> {
    return wait([&](const Json& message) -> bool { return message.at("type") == type; }, from);
  }
  auto at(std::size_t position) -> Json {
    const std::lock_guard lock{mutex_};
    return messages_.at(position);
  }
  // The last message of this type so far.
  auto last(std::string_view type) -> Json {
    const std::lock_guard lock{mutex_};
    for (std::size_t i = messages_.size(); i > 0; --i)
      if (messages_[i - 1].at("type") == type)
        return messages_[i - 1];
    return nullptr;
  }
  auto count() -> std::size_t {
    const std::lock_guard lock{mutex_};
    return messages_.size();
  }

private:
  void post(PageToken at, const std::vector<std::string>& messages) {
    const std::lock_guard lock{mutex_};
    // As the window does: only for the page it shows.
    if (at != page_)
      return;
    std::ranges::transform(messages, std::back_inserter(messages_),
                           [](const std::string& text) -> Json { return Json::parse(text); });
  }

  std::mutex mutex_;
  std::vector<Json> messages_;
  LocalToolModelFiles files_;
  NmlPanelSource source_;
  ToolModelStore store_;
  Runtime runtime_;
  PanelFeed feed_;
  PageToken page_;
  RequestId next_id_{1};
};

// What of LinuxCNC a tool model must never change: the tool table file, the parameter file with
// the work systems, the tool table and the offsets as the panel is told them.
struct MachineFacts {
  std::string tool_file;
  std::string parameter_file;
  Json tools;
  Json offsets;
  friend auto operator==(const MachineFacts&, const MachineFacts&) -> bool = default;
};
auto machine_facts(const Rig& rig, ModelSession& session) -> MachineFacts {
  const auto status = session.first("status", session.count());
  return {file_text(rig.config.tool_table), file_text(rig.config.parameter_file),
          session.last("tool_table").value("tools", Json()),
          status ? session.at(*status).value("offsets", Json()) : Json()};
}

// The tool models end to end on the G0759 profile, whose INI file names its machine and keeps
// its data in the run directory: hello, the snapshot after ready, a defaults save and an
// assignment save in their stated order, a second process locked out while this one runs and
// shown the saved state after it has ended, and a second session that shows it too. LinuxCNC's
// tool table and offsets are compared before and after.
void tool_models(Rig& rig, const std::string& ini_file) {
  const auto where = tool_model_location(rig.config);
  const auto saved_file = where.directory + "/tool-models.json";
  const auto run_directory = std::filesystem::path(ini_file).parent_path().string();
  check("tool_models_location",
        where.availability == ToolModelAvailability::available &&
            where.directory == run_directory + "/coffeecnc-data/G0759-sim" &&
            !std::filesystem::exists(where.directory),
        "directory=" + where.directory + " reason=" + where.reason);
  const auto self = std::filesystem::read_symlink("/proc/self/exe").string();
  const auto other_process = [&]() -> Json {
    const auto printed = all_output_of(std::format("'{}' -ini '{}' --tool-models", self, ini_file));
    return Json::parse(printed, nullptr, false);
  };
  const auto mode = [](const std::string& path) -> unsigned {
    struct stat info {};
    return ::stat(path.c_str(), &info) == 0 ? static_cast<unsigned>(info.st_mode) & 07777U : 0U;
  };
  auto table = LoadedToolTable::attach(rig.config.millimetres_per_unit);
  check("tool_models_tool_data", table.has_value(), table ? "tool.mmap" : table.error());
  if (!table)
    return;

  rig.stop();
  const auto writes_before = rig.port.writes().size();
  Json saved;
  MachineFacts before;
  {
    ModelSession session{rig, *table};
    const auto hello = session.first("hello");
    const auto snapshot = session.first("tool_models");
    const auto table_at = session.wait([](const Json& message) -> bool {
      if (message.at("type") != "tool_table")
        return false;
      // For a nonrandom tool changer, T0 represents the empty spindle.
      std::vector<int> numbers;
      for (const auto& row : message.at("tools"))
        if (const int number = row.at("number").get<int>(); number > 0)
          numbers.push_back(number);
      std::ranges::sort(numbers);
      return numbers == std::vector{1, 2, 3};
    });
    check("tool_models_hello",
          hello && session.at(*hello).value("machine_id", Json()) == "G0759-sim" &&
              session.at(*hello).value("tool_model_storage", Json()) == 1,
          hello ? session.at(*hello).dump() : "no hello");
    if (!hello || !snapshot || !table_at) {
      check("tool_models_snapshot_after_ready", false,
            std::format("snapshot={} table={}", snapshot.has_value(),
                        session.last("tool_table").dump()));
      rig.start();
      return;
    }
    const auto first = session.at(*snapshot);
    check("tool_models_snapshot_after_ready",
          *snapshot > *hello && first.at("machine_id") == "G0759-sim" &&
              first.at("availability") == "available" && first.at("revision") == 0 &&
              first.at("reason") == "" && first.at("document").at("version") == 1 &&
              first.at("document").at("assignments").empty() &&
              first.at("document").at("defaults").at("drill").at("tipAngle") == 118 &&
              first.at("document").at("defaults").size() == tool_family_count,
          std::format("hello_at={} snapshot_at={} {}", *hello, *snapshot,
                      first.dump().substr(0, 160)));
    check("tool_models_no_file_by_reading",
          !std::filesystem::exists(saved_file) && mode(where.directory) == 0700 &&
              mode(where.directory + "/lock") == 0600,
          std::format("directory_mode={:o} lock_mode={:o}", mode(where.directory),
                      mode(where.directory + "/lock")));
    before = machine_facts(rig, session);
    fact("tool_models_machine",
         std::format("tools={} offsets={} tool_file_bytes={} parameter_file_bytes={}",
                     word(before.tools.dump()), word(before.offsets.dump()),
                     before.tool_file.size(), before.parameter_file.size()));

    // A defaults save: the drill's tip angle and a stick-out that is no short decimal.
    auto defaults = first.at("document").at("defaults");
    defaults["drill"]["tipAngle"] = 135;
    defaults["end-mill"]["stickout"] = 35 + 0.1 + 0.2;
    auto mark = session.count();
    auto result = session.send(std::format(
        R"("type":"set_tool_model_defaults","machine_id":"G0759-sim","base_revision":0,"defaults":{})",
        defaults.dump()));
    auto shown = session.first("tool_models", mark);
    check("tool_models_defaults_saved",
          result && shown && *shown < *result && session.at(*result).at("outcome") == "accepted" &&
              session.at(*shown).at("revision") == 1 &&
              session.at(*shown).at("document").at("defaults") == defaults &&
              session.at(*shown).at("document").at("assignments").empty() &&
              mode(saved_file) == 0600,
          std::format("snapshot_at={} result_at={} {} file_mode={:o}", shown.value_or(0),
                      result.value_or(0), result ? session.at(*result).dump() : "no result",
                      mode(saved_file)));

    // An assignment save for T1, with the row exactly as the tool table message gave it.
    const auto rows = session.at(*table_at).at("tools");
    const auto row = *std::ranges::find_if(
        rows, [](const Json& tool) -> bool { return tool.at("number") == 1; });
    const Json binding{{"number", row.at("number")},
                       {"pocket", row.at("pocket")},
                       {"diameter", row.at("diameter")},
                       {"comment", row.at("comment")}};
    const auto assign = [&](int base) -> std::string {
      return std::format(R"("type":"set_tool_model_assignment","machine_id":"G0759-sim",)"
                         R"("base_revision":{},"tool":{},"family":"drill",)"
                         R"("overrides":{{"stickout":30.5,"tipAngle":90}})",
                         base, binding.dump());
    };
    mark = session.count();
    result = session.send(assign(1));
    shown = session.first("tool_models", mark);
    const Json expected{{{"tool", binding},
                         {"family", "drill"},
                         {"overrides", {{"stickout", 30.5}, {"tipAngle", 90}}}}};
    check("tool_models_assignment_saved",
          result && shown && *shown < *result && session.at(*result).at("outcome") == "accepted" &&
              session.at(*shown).at("revision") == 2 &&
              session.at(*shown).at("document").at("assignments") == expected &&
              session.at(*shown).at("document").at("defaults") == defaults,
          std::format("snapshot_at={} result_at={} {} binding={}", shown.value_or(0),
                      result.value_or(0), result ? session.at(*result).dump() : "no result",
                      word(binding.dump())));

    // The same command again: its base revision is stale.
    mark = session.count();
    result = session.send(assign(1));
    shown = session.first("tool_models", mark);
    check("tool_models_stale_revision",
          result && shown && *shown < *result && session.at(*result).at("outcome") == "rejected" &&
              session.at(*shown).at("revision") == 2,
          result ? session.at(*result).dump() : "no result");
    // For another machine, and while the machine is off: the state of LinuxCNC plays no part.
    result = session.send(R"("type":"set_tool_model_assignment","machine_id":"G0759",)"
                          R"("base_revision":2,"tool":{"number":1,"pocket":1,"diameter":0,)"
                          R"("comment":""},"family":null,"overrides":{})");
    check("tool_models_other_machine_rejected",
          result && session.at(*result).at("outcome") == "rejected" &&
              session.last("tool_models").at("revision") == 2,
          result ? session.at(*result).dump() : "no result");
    saved = session.last("tool_models");

    // A second process while this one holds the lock.
    const auto locked_out = other_process();
    check("tool_models_second_process_locked_out",
          locked_out.is_object() && locked_out.value("availability", "") == "unavailable" &&
              locked_out.value("reason", "").find("already in use") != std::string::npos &&
              locked_out.value("machine_id", "") == "G0759-sim" &&
              locked_out.at("document").is_null(),
          word(locked_out.dump().substr(0, 240)));
    const auto after = machine_facts(rig, session);
    check("tool_models_linuxcnc_unchanged",
          after == before && !before.tools.empty() && before.offsets.is_object() &&
              !before.tool_file.empty(),
          "tools=" + word(after.tools.dump()) + " offsets=" + word(after.offsets.dump()));
  }
  check("tool_models_no_nml_command", rig.port.writes().size() == writes_before,
        "written=" + trace_text(written_since(rig, writes_before)));
  const auto on_disk = decode_tool_model_file(file_text(saved_file));
  check("tool_models_file",
        on_disk && on_disk->machine_id == "G0759-sim" && on_disk->revision == 2 &&
            on_disk->document.assignments.size() == 1 &&
            !std::filesystem::exists(saved_file + ".tmp"),
        on_disk ? std::format("bytes={}", file_text(saved_file).size()) : on_disk.error());

  // The process that saved has let go: another process is shown what it saved.
  const auto restarted = other_process();
  check("tool_models_second_process_restart", restarted == saved && saved.is_object(),
        word(restarted.dump().substr(0, 240)));
  // And so is a second session here: a new store, runtime and page.
  {
    ModelSession session{rig, *table};
    const auto hello = session.first("hello");
    const auto snapshot = session.first("tool_models");
    check("tool_models_second_session",
          hello && snapshot && *snapshot > *hello && session.at(*snapshot) == saved &&
              session.at(*snapshot).at("document").at("defaults").at("end-mill").at("stickout") ==
                  35 + 0.1 + 0.2,
          snapshot ? word(session.at(*snapshot).dump().substr(0, 240)) : "no snapshot");
    const auto after = machine_facts(rig, session);
    check("tool_models_linuxcnc_unchanged_after_restart", after == before,
          "tools=" + word(after.tools.dump()) + " offsets=" + word(after.offsets.dump()));
  }
  rig.start();
}

// The G0759's own INI file sets MACHINE_ID and no DATA_DIRECTORY, and nothing is added to it
// for the test: run.sh starts this program with XDG_DATA_HOME in the run directory, so the
// store opens there and not under the user's home. hello names the machine, and the snapshot
// that follows it is available. Nothing is saved here.
void machine_tool_models(Rig& rig, const std::string& ini_file) {
  const auto where = tool_model_location(rig.config);
  const auto run_directory = std::filesystem::path(ini_file).parent_path().string();
  const char* home = std::getenv("HOME");
  const auto in_home =
      std::filesystem::path(home != nullptr ? home : "/nonexistent") / ".local/share/coffeecnc/g0759";
  const bool in_home_before = std::filesystem::exists(in_home);
  check("machine_tool_models_location",
        where.availability == ToolModelAvailability::available &&
            where.directory == run_directory + "/xdg-data/coffeecnc/g0759" &&
            !rig.config.data_directory,
        "directory=" + where.directory + " reason=" + where.reason);
  auto table = LoadedToolTable::attach(rig.config.millimetres_per_unit);
  check("machine_tool_models_tool_data", table.has_value(), table ? "tool.mmap" : table.error());
  if (!table)
    return;
  rig.stop();
  {
    ModelSession session{rig, *table};
    const auto hello = session.first("hello");
    const auto snapshot = session.first("tool_models");
    check("machine_tool_models_hello",
          hello && session.at(*hello).value("machine_id", Json()) == "g0759" &&
              session.at(*hello).value("tool_model_storage", Json()) == 1,
          hello ? word(session.at(*hello).dump().substr(0, 300)) : "no hello");
    check("machine_tool_models_available_in_run_directory",
          hello && snapshot && *snapshot > *hello &&
              session.at(*snapshot).value("availability", "") == "available" &&
              session.at(*snapshot).value("machine_id", "") == "g0759" &&
              session.at(*snapshot).value("revision", -1) == 0 &&
              std::filesystem::exists(where.directory + "/lock") &&
              !std::filesystem::exists(where.directory + "/tool-models.json"),
          snapshot ? word(session.at(*snapshot).dump().substr(0, 200)) : "no snapshot");
  }
  check("machine_tool_models_nothing_in_home",
        std::filesystem::exists(in_home) == in_home_before,
        std::format("{} existed_before={}", in_home.string(), in_home_before));
  rig.start();
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
  if (machine_hal) {
    // G0759.hal connects iocontrol.0.emc-enable-in to the external input only. Observed here,
    // and only this: an E-stop request made while the machine is OFF with the external chain
    // closed leaves the reported state at off, and the port answers `failed`; the input
    // opening is reported as E-stop. What the request does to a machine that is on is
    // g0759_estop's.
    const auto after_request = rig.snapshot().machine;
    const bool pressed = halcmd("sets estop-ext 0");
    // LinuxCNC's io task reads the input once per [EMCIO] CYCLE_TIME, 100 ms on this machine.
    rig.pump_until(2s, [&]() -> bool { return rig.snapshot().machine == MachineState::estop; });
    const auto after_button = rig.snapshot().machine;
    check("estop_request_from_off_reports_no_estop_state",
          manual.accepted() && off.accepted() && is_off &&
              estop.text() == "failed:LinuxCNC reports machine off; E-stop was not confirmed." &&
              after_request == MachineState::off && pressed && after_button == MachineState::estop,
          std::format("{} {} {} off={} after_request={} after_button={}", manual.text(), off.text(),
                      estop.text(), is_off, static_cast<int>(after_request),
                      static_cast<int>(after_button)));
  } else {
    check("power_off_and_estop",
          manual.accepted() && off.accepted() && is_off && estop.accepted() &&
              rig.snapshot().machine == MachineState::estop,
          std::format("{} {} {}", manual.text(), off.text(), estop.text()));
  }
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
  // No machine identity: tool models are session-only, and nothing is opened for them.
  const auto models = tool_model_location(config);
  check("config_no_machine_id",
        !config.machine_id && models.availability == ToolModelAvailability::session_only &&
            models.directory.empty(),
        models.reason);
  check("config_limits", near(config.limits.max_jog_velocity, 1800, 0.01) &&
                             near(config.limits.max_axis_jog_velocity[2], 1800, 0.01) &&
                             near(config.limits.max_feed_override, 1.5) &&
                             near(config.limits.max_spindle_rpm, 12000) &&
                             near(config.travel[2].min, -50) && config.limits.homing_required &&
                             config.home_all_configured && config.jog_increments.empty() &&
                             !config.signals.spindle_rpm && !config.signals.spindle_at_speed &&
                             !config.signals.probe);
}

auto state_name(MachineState state) -> std::string_view {
  return state == MachineState::estop ? "estop" : state == MachineState::off ? "off" : "on";
}

// What was seen for 1.5 s after a cause to stop, on the machine's own HAL files. SAMPLED: the
// machine state at every status reading of the test (5 ms apart, more while halcmd runs), the
// two HAL signals and the position once per round of about 20 ms. A time is the first sample
// that showed the change, counted from the return of the call that caused it; a state shorter
// than the gap between two samples is not seen. None of this is stop-latency evidence for the
// physical machine: the hardware here is simulated and the host keeps no time.
struct StopSeen {
  std::string states;       // the distinct machine states in order, each with its first time
  int samples{};            // state samples taken
  int estop_samples{};      // of them, E-stop
  MachineState last{MachineState::on};
  double not_on_ms{-1};     // first state sample that was not "on"
  double spindle_ms{-1};    // spindle-enable (spindle.0.on) first sampled FALSE
  double enable_out_ms{-1}; // estop-out (iocontrol.0.user-enable-out) first sampled FALSE
  double still_ms{};        // last sampled change of the X position; 0 when it never moved
  double travel{};          // X travelled since the cause, mm
};

// `refresh` is a held jog that the panel would keep sending.
auto watch_stop(Rig& rig, Clock::time_point from, const char* refresh) -> StopSeen {
  StopSeen seen;
  const double start = rig.machine(0);
  double last = start;
  auto refreshed = from;
  std::optional<MachineState> previous;
  const auto sample = [&]() -> bool {
    const auto state = rig.snapshot().machine;
    const double at = milliseconds(Clock::now() - from);
    ++seen.samples;
    seen.estop_samples += state == MachineState::estop ? 1 : 0;
    if (state != previous) {
      seen.states += std::format("{}{}@{:.0f}", seen.states.empty() ? "" : ",", state_name(state), at);
      previous = state;
    }
    if (seen.not_on_ms < 0 && state != MachineState::on)
      seen.not_on_ms = at;
    seen.last = state;
    return false;
  };
  while (milliseconds(Clock::now() - from) < 1500) {
    if (refresh != nullptr && Clock::now() - refreshed >= 50ms) {
      static_cast<void>(rig.post(refresh));
      refreshed = Clock::now();
    }
    static_cast<void>(rig.pump_until(10ms, sample));
    const double at = milliseconds(Clock::now() - from);
    if (seen.spindle_ms < 0 && hal_signal("spindle-enable") == "FALSE")
      seen.spindle_ms = at;
    static_cast<void>(sample());
    if (seen.enable_out_ms < 0 && hal_signal("estop-out") == "FALSE")
      seen.enable_out_ms = milliseconds(Clock::now() - from);
    if (const double x = rig.machine(0); std::fabs(x - last) > 0.001) {
      last = x;
      seen.still_ms = milliseconds(Clock::now() - from);
    }
  }
  seen.travel = last - start;
  return seen;
}

constexpr std::string_view estop_not_confirmed_off =
    "failed:LinuxCNC reports machine off; E-stop was not confirmed.";

// The G0759's E-stop wiring, observed in simulation: G0759.hal connects
// iocontrol.0.emc-enable-in to the external input (estop-ext) only, and
// iocontrol.0.user-enable-out to the signal estop-out, which nothing reads. Two causes, a
// display program's E-stop request and the external input opening, each with the machine on
// and idle, during a held jog, and during a program with the spindle on.
void g0759_estop(Rig& rig) {
  // The way an operator gets there: the chain closed, E-stop reset, machine on. A protocol
  // `off` sent while the machine is on is a power-off (EMC_TASK_STATE_OFF); sent while it is
  // off it is the E-stop reset (EMC_TASK_STATE_ESTOP_RESET), and only that sets
  // user-enable-out. Both are sent. Returns what did not work, or nothing.
  const auto machine_on = [&]() -> std::string {
    const bool closed = halcmd("sets estop-ext 1");
    rig.pump_until(2s, [&]() -> bool { return rig.snapshot().machine != MachineState::estop; });
    // LinuxCNC's io task reports the input once per [EMCIO] CYCLE_TIME (100 ms): a reset sent
    // before task has seen the input closed is undone by task itself.
    rig.pump(300ms);
    static_cast<void>(rig.send(R"("type":"abort")"));
    static_cast<void>(rig.send(release_x));
    rig.settle();
    const auto off = rig.send(R"("type":"set_machine","state":"off")");
    rig.pump_until(2s, [&]() -> bool { return rig.snapshot().machine == MachineState::off; });
    const auto reset = rig.send(R"("type":"set_machine","state":"off")");
    const auto on = rig.send(R"("type":"set_machine","state":"on")");
    const bool is_on = rig.pump_until(
        2s, [&]() -> bool { return rig.snapshot().machine == MachineState::on; });
    const bool enabled =
        rig.pump_until(2s, [&]() -> bool { return hal_signal("estop-out") == "TRUE"; });
    return closed && off.accepted() && reset.accepted() && on.accepted() && is_on && enabled
               ? std::string()
               : std::format("closed={} off={} reset={} on={} is_on={} estop_out={}", closed,
                             off.text(), reset.text(), on.text(), is_on, hal_signal("estop-out"));
  };
  const std::string file = rig.config.program_prefix + "/coffeecnc_g0759_estop.ngc";
  {
    std::ofstream out{file};
    out << "G21 G91\nM3 S1000\nG1 X30 F300\nG90\nM5\nM2\n";
  }
  for (const std::string cause : {"request", "external_input"}) {
    for (const std::string situation : {"idle", "jog", "program_spindle_on"}) {
      const auto not_on = machine_on();
      bool ready = not_on.empty();
      const double before = rig.machine(0);
      if (situation == "jog") {
        ready = ready && rig.send(hold_x).accepted();
        rig.hold(hold_x, 200ms);
      } else if (situation == "program_spindle_on") {
        ready = ready &&
                rig.send(std::format(R"("type":"open_program","path":"{}")", file)).accepted() &&
                rig.send(R"("type":"run")").accepted() &&
                rig.pump_until(5s, [&]() -> bool {
                  return rig.machine(0) - before > 0.5 && hal_signal("spindle-enable") == "TRUE";
                });
      }
      const bool moving = std::fabs(rig.machine(0) - before) > 0.1;
      const auto spindle_before = hal_signal("spindle-enable");
      const auto enable_out_before = hal_signal("estop-out");
      // The precondition of every case: on, user-enable-out true.
      check(std::format("estop_{}_{}_prepared", cause, situation),
            ready && rig.snapshot().machine == MachineState::on && enable_out_before == "TRUE" &&
                moving == (situation != "idle") &&
                (spindle_before == "TRUE") == (situation == "program_spindle_on"),
            std::format("{} estop_out={}", not_on.empty() ? "-" : not_on, enable_out_before));
      auto from = Clock::now();
      std::optional<RequestId> request;
      if (cause == "request") {
        request = rig.post(R"("type":"set_machine","state":"estop")");
        from = Clock::now();
      } else {
        ready = ready && halcmd("sets estop-ext 0");
        from = Clock::now();
      }
      const auto seen = watch_stop(rig, from, situation == "jog" ? hold_x : nullptr);
      const auto answer = request ? rig.await(*request, 2s).text() : std::string("-");
      const auto interp = rig.snapshot().interp;
      const auto spindle_after = hal_signal("spindle-enable");
      const auto enable_out_after = hal_signal("estop-out");
      fact("g0759_estop",
           std::format("cause={} situation={} timing=sampled_about_20_ms_from_the_cause's_return "
                       "moving_before={} spindle_enable_before={} answer={} sampled_states={} "
                       "state_samples={} estop_samples={} sampled_not_on_ms={:.0f} "
                       "sampled_still_ms={:.0f} travel_mm={:.3f} spindle_enable_after={} "
                       "sampled_spindle_ms={:.0f} estop_out_after={} sampled_estop_out_ms={:.0f} "
                       "interp_idle={}",
                       cause, situation, moving, spindle_before, word(answer), seen.states,
                       seen.samples, seen.estop_samples, seen.not_on_ms, seen.still_ms, seen.travel,
                       spindle_after, seen.spindle_ms, enable_out_after, seen.enable_out_ms,
                       interp == InterpState::idle));
      // What every stop has in common: the machine is not on, nothing moves any more, the
      // spindle is not commanded, the interpreter is idle. The one-second bounds only say that
      // it happened; they are not measurements.
      const bool stopped = ready && seen.last != MachineState::on && seen.not_on_ms >= 0 &&
                           seen.not_on_ms < 1000 && seen.still_ms < 1000 &&
                           std::fabs(seen.travel) < 5 && spindle_after == "FALSE" &&
                           interp == InterpState::idle;
      if (cause == "request") {
        // Observed on LinuxCNC 2.9.4: task aborts and switches the machine off, io drops
        // user-enable-out. No sample showed E-stop: the watch ends with off reported. The port
        // answers by the reported state, whichever step failed first (in the jog case LinuxCNC
        // publishes "Jog aborted by jog-stop" for CoffeeCNC's own level: logged, not shown).
        check(std::format("estop_request_{}_ends_off_with_no_estop_sample", situation),
              stopped && seen.last == MachineState::off && seen.estop_samples == 0 &&
                  seen.enable_out_ms >= 0 && enable_out_after == "FALSE" &&
                  answer == estop_not_confirmed_off,
              std::format("states={} answer={}", seen.states, word(answer)));
      } else {
        // The input is what LinuxCNC reports as E-stop. user-enable-out stays as it was.
        check(std::format("estop_external_input_{}_is_estop", situation),
              stopped && seen.last == MachineState::estop && seen.enable_out_ms < 0 &&
                  enable_out_after == "TRUE",
              std::format("states={}", seen.states));
      }
    }
  }

  // After an E-stop request the panel shows the machine off, with user-enable-out false. What
  // brings user-enable-out back: `on` alone, or the reset (`off` sent while off) before it.
  auto left = machine_on();
  const auto request = rig.send(R"("type":"set_machine","state":"estop")");
  rig.pump(300ms);
  const auto after_request = hal_signal("estop-out");
  const auto on_only = rig.send(R"("type":"set_machine","state":"on")");
  rig.pump(300ms);
  const bool on_again = rig.snapshot().machine == MachineState::on;
  const auto after_on_only = hal_signal("estop-out");
  const auto off = rig.send(R"("type":"set_machine","state":"off")");
  rig.pump(300ms);
  const auto after_power_off = hal_signal("estop-out");
  const auto reset = rig.send(R"("type":"set_machine","state":"off")");
  rig.pump(300ms);
  const auto after_reset = hal_signal("estop-out");
  fact("g0759_estop_recovery",
       std::format("prepared={} request={} estop_out_after_request={} on_only={} machine_on={} "
                   "estop_out_after_on_only={} power_off={} estop_out_after_power_off={} "
                   "reset_from_off={} estop_out_after_reset_from_off={}",
                   left.empty(), word(request.text()), after_request, word(on_only.text()),
                   on_again, after_on_only, word(off.text()), after_power_off, word(reset.text()),
                   after_reset));
  // Observed on LinuxCNC 2.9.4: `on` after the request is accepted and the machine is on with
  // user-enable-out still false; a power-off does not change it; the reset does.
  check("estop_request_leaves_enable_out_false_until_a_reset",
        left.empty() && after_request == "FALSE" && on_only.accepted() && on_again &&
            after_on_only == "FALSE" && off.accepted() && after_power_off == "FALSE" &&
            reset.accepted() && after_reset == "TRUE");
  left = machine_on();
  check("estop_scenarios_leave_the_machine_on", left.empty(), left.empty() ? "-" : left);
}

void g0759_config(const MachineConfig& config) {
  const std::vector<double> increments{5, 1, 0.5, 0.1, 0.05, 0.01, 0.005};
  // The machine's own INI file names the machine and no data directory; the profile has both.
  check("config_machine_id",
        machine_hal ? config.machine_id == "g0759" && !config.data_directory
                    : config.machine_id == "G0759-sim" && config.data_directory.has_value(),
        config.machine_id.value_or("(none)"));
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

// The programs are tested with nonzero G54 and unequal tool lengths. The native preview
// reports the tip in machine coordinates; the simulated HAL signals carry commanded joint
// positions, so use the offsets in effect in each sample to recover commanded work XYZ.
auto air_envelope(const Vec3& work) -> bool {
  constexpr double tolerance = 0.001;
  return std::abs(work[0]) <= 30 + tolerance && std::abs(work[1]) <= 30 + tolerance &&
         work[2] >= 5 - tolerance;
}

void air_cuts(Rig& rig) {
  constexpr Vec3 origin{10, -8, -50};
  const auto t1 = rig.send(
      R"("type":"set_tool","tool":{"number":1,"pocket":1,"length":18.975,"diameter":6.35,"comment":"End mill"})");
  const auto t98 = rig.send(
      R"("type":"set_tool","tool":{"number":98,"pocket":98,"length":25.4,"diameter":6.35,"comment":"Reference"})");
  const auto optional = rig.send(
      R"("type":"set_program_option","option":"optional_stop","on":true)");
  auto table = LoadedToolTable::attach(rig.config.millimetres_per_unit);
  check("air_cut_setup", t1.accepted() && t98.accepted() && optional.accepted() && table.has_value(),
        std::format("T1={} T98={} optional={} tool_data={}", t1.text(), t98.text(), optional.text(),
                    table.has_value()));
  if (failures != 0)
    return;
  PreviewOracle oracle{rig, *table};

  struct Sample {
    const char* file;
    Vec3 end;
    double low_z;
    int pauses;
    int exchanges;
  };
  constexpr Sample samples[] = {
      {"01-square", {-20, -20, 30}, 10, 0, 0},
      {"02-arcs", {-15, 0, 30}, 5, 0, 0},
      {"03-pocket", {24, 20, 30}, 5, 0, 0},
      {"04-drill", {18, 12, 30}, 5, 0, 0},
      {"05-pauses", {-15, 15, 30}, 10, 2, 0},
      {"06-toolchange", {16, 12, 30}, 10, 0, 2},
  };

  for (const auto& sample : samples) {
    const auto sample_name = std::string{sample.file};
    const auto filename = std::filesystem::path{COFFEECNC_SAMPLES_DIRECTORY} / "air" /
                          (sample_name + ".ngc");
    const auto text = read_text_file(filename.string(), 1 << 16);
    if (!text) {
      check(sample_name + "_source", false, "could not read " + filename.string());
      return;
    }
    // Start the tool-change program with T98 so it actually requests both exchanges.
    const auto start_tool = sample.exchanges > 0 ? 98 : 1;
    const auto mdi = [&](std::string_view block) -> bool {
      const auto sent = rig.send(std::format(R"("type":"mdi","text":"{}")", block));
      const bool done = sent.accepted() && rig.pump_until(10s, [&]() -> bool {
        const auto s = rig.status();
        return s.command_done && !s.task_executing && s.queued_mdi == 0 &&
               s.controller.interp == InterpState::idle && s.controller.in_position;
      });
      if (!done)
        check(sample_name + "_setup_mdi", false, std::format("{}: {}", block, sent.text()));
      return done;
    };
    if (!mdi(std::format("G21 G90 G54 G92.1 G10 L2 P1 X{} Y{} Z{} R0", origin[0], origin[1],
                         origin[2])) ||
        !mdi(std::format("M61 Q{} G43 H{}", start_tool, start_tool)) || !mdi("G0 Z30") ||
        !mdi("G0 X0 Y0"))
      return;
    const auto start = rig.status();
    const bool at_start = start.controller.tool_in_spindle == start_tool &&
                          distance(start.work, {0, 0, 30}) < 0.01;
    check(sample_name + "_start", at_start,
          std::format("T{} XYZ={:.3f},{:.3f},{:.3f}", start.controller.tool_in_spindle,
                      start.work[0], start.work[1], start.work[2]));
    if (!at_start)
      return;

    const auto preview = oracle.preview(*text).second;
    const bool preview_inside = std::ranges::all_of(preview.segments, [&](const auto& segment) {
      return std::ranges::all_of(segment.points, [&](const auto& tip) {
        return air_envelope({tip[0] - origin[0], tip[1] - origin[1], tip[2] - origin[2]});
      });
    });
    check(sample_name + "_preview",
          preview.state == PreviewState::ready && !preview.error && !preview.segments.empty() &&
              preview_inside,
          std::format("state={} segments={} inside={} error={}", static_cast<int>(preview.state),
                      preview.segments.size(), preview_inside,
                      preview.error ? preview.error->text : "-"));
    if (failures != 0)
      return;

    rig.clear_messages();
    const auto run = rig.send(R"("type":"run")");
    bool ran = false;
    bool inside = true;
    bool was_paused = false;
    std::optional<std::uint64_t> last_exchange;
    int pauses = 0;
    int exchanges = 0;
    double low_z = 1000;
    const bool finished = run.accepted() && rig.pump_until(160s, [&]() -> bool {
      const auto s = rig.status();
      const auto& state = s.controller;
      const auto joint = rig.probe.commanded();
      const Vec3 work{joint[0] - origin[0] - s.tool_offset[0],
                      joint[1] - origin[1] - s.tool_offset[1],
                      joint[2] - origin[2] - s.tool_offset[2]};
      low_z = std::min(low_z, work[2]);
      inside = inside && air_envelope(work);
      ran = ran || state.interp != InterpState::idle;
      if (!inside || state.machine != MachineState::on)
        return true;

      if (state.requested_tool && last_exchange != state.tool_request) {
        last_exchange = state.tool_request;
        ++exchanges;
        if (!rig.send(R"("type":"tool_changed")").accepted())
          return true;
      }
      const bool paused = state.interp == InterpState::paused && !state.requested_tool;
      if (paused && !was_paused) {
        ++pauses;
        if (!rig.send(R"("type":"resume")").accepted())
          return true;
      }
      was_paused = paused;
      return ran && state.interp == InterpState::idle && state.in_position;
    });
    const auto end = rig.status();
    check(sample_name + "_execution",
          finished && ran && inside && end.controller.interp == InterpState::idle &&
              end.controller.in_position && distance(end.work, sample.end) < 0.01 &&
              end.spindle_direction == SpindleDirection::stopped && !end.flood && !end.mist &&
              near(low_z, sample.low_z) && pauses == sample.pauses && exchanges == sample.exchanges,
          std::format("run={} finished={} inside={} min_z={:.3f} end={:.3f},{:.3f},{:.3f} "
                      "pauses={} tool_changes={} messages={}",
                      run.text(), finished, inside, low_z, end.work[0], end.work[1], end.work[2],
                      pauses, exchanges, PreviewOracle::join(rig.all_messages())));
    if (failures != 0) {
      static_cast<void>(rig.send(R"("type":"abort")"));
      return;
    }
  }
}

void restart_cuts(Rig& rig) {
  constexpr double origin_z = -55;
  auto table = LoadedToolTable::attach(rig.config.millimetres_per_unit);
  check("restart_tool_table", table.has_value());
  if (!table)
    return;
  PreviewOracle oracle{rig, *table};
  PreviewWorker preview{oracle.engine};
  HelperPreviewEngine engine{load_preview_settings(rig.config, COFFEECNC_PREVIEW_HELPER_BUILT,
                                                  rig.config.directory + "/preview-work")};
  rig.stop();
  rig.restart_preview = &preview;
  rig.restart_engine = &engine;
  rig.start();
  const auto mdi = [&](std::string_view text) {
    const auto result = rig.send(std::format(R"("type":"mdi","text":"{}")", text));
    return result.accepted() && rig.pump_until(10s, [&] {
      const auto s = rig.status();
      return s.command_done && !s.task_executing && s.controller.in_position &&
             s.controller.interp == InterpState::idle;
    });
  };
  const bool setup =
      rig.send(R"("type":"set_tool","tool":{"number":1,"pocket":1,"length":18.975,"diameter":6.35,"comment":"End mill"})").accepted() &&
      rig.send(R"("type":"set_tool","tool":{"number":98,"pocket":98,"length":25.4,"diameter":6.35,"comment":"Reference"})").accepted() &&
      mdi(std::format("G21 G90 G54 G92.1 G10 L2 P1 X10 Y-8 Z{} R0", origin_z)) &&
      mdi("M61 Q1 G43 H1") && mdi("G0 Z20") && mdi("G0 X0 Y0");
  check("restart_setup", setup);

  std::uint64_t context = 0;
  const auto plan = [&](std::string_view text, int line) {
    auto [inputs, path] = oracle.preview(text);
    preview.submit(++context, inputs);
    const bool ready = rig.pump_until(4s, [&] { return preview.restart_source(context).inputs != nullptr; });
    if (!ready)
      return std::pair{Result{"no_preview", path.error ? path.error->text : ""}, std::move(path)};
    const auto trace = rig.port.writes().size();
    auto answer = rig.send(std::format(R"("type":"plan_restart","line":{},"preview_context":{})",
                                       line, context));
    check("restart_plan_is_read_only", rig.port.writes().size() == trace);
    return std::pair{std::move(answer), std::move(path)};
  };
  const auto start_command = [](const Result& result) {
    return std::format(R"("type":"start_restart","plan_id":{})",
                       result.restart.at("plan_id").get<std::uint64_t>());
  };
  const auto file = [](std::string_view name) {
    return read_text_file((std::filesystem::path{COFFEECNC_SAMPLES_DIRECTORY} / "air" / name).string(),
                          65536).value();
  };
  const auto approach = [&](std::string_view name, int line, int tool, bool exchange) {
    const auto [result, path] = plan(file(name), line);
    const bool planned = result.accepted() && result.restart.value("tool", 0) == tool &&
                         result.restart.value("h", 0) == tool &&
                         result.restart.value("needs_tool_change", !exchange) == exchange;
    check("restart_preview_modal_state", planned, result.text() + " " + result.restart.dump());
    if (!planned)
      return;
    auto id = rig.post(start_command(result));
    if (exchange) {
      const auto pending = rig.await(id);
      const auto s = rig.status();
      check("restart_normal_tool_change",
            pending.accepted() && s.controller.requested_tool == tool &&
            s.spindle_direction == SpindleDirection::stopped && s.motion_line < line,
            pending.text() + " " + PreviewOracle::join(rig.all_messages()));
      if (!pending.accepted())
        return;
      id = rig.post(R"("type":"tool_changed")");
    }
    const auto start = result.restart.at("start").get<Vec3>();
    const auto clearance = result.restart.at("clearance_z").get<double>();
    const auto before = rig.status().work;
    bool lifted = exchange;
    bool xy_at_clearance = true;
    bool traversed = false;
    bool spindle_on = false;
    bool suffix = true;
    int first_motion = 0;
    const bool finished = rig.pump_until(145s, [&] {
      const auto s = rig.status();
      if (auto done = rig.result(id); done && !done->accepted())
        return true;
      if (s.controller.mode != TaskMode::automatic) {
        lifted = lifted || s.work[2] >= clearance - 0.05;
        const bool moving_xy = std::abs(s.work[0] - before[0]) > 0.05 ||
                               std::abs(s.work[1] - before[1]) > 0.05;
        if (moving_xy && !traversed)
          xy_at_clearance = xy_at_clearance && lifted && s.work[2] >= clearance - 0.05;
        traversed = traversed || (moving_xy && std::abs(s.work[0] - start[0]) < 0.05 &&
                                 std::abs(s.work[1] - start[1]) < 0.05);
        return false;
      }
      if (s.motion_line > 0 && s.velocity > 0.01) {
        if (first_motion == 0)
          first_motion = s.motion_line;
        spindle_on = spindle_on || (s.spindle_rpm > 1000 &&
                                    s.spindle_direction == SpindleDirection::forward);
        const auto p = oracle.position();
        const Vec3 tool_tip{p[0] - s.tool_offset[0], p[1] - s.tool_offset[1],
                           p[2] - s.tool_offset[2]};
        bool near_path = false;
        for (const auto& segment : path.segments) {
          if (segment.line < line)
            continue;
          for (std::size_t i = 1; i < segment.points.size(); ++i)
            near_path = near_path || distance(tool_tip, segment.points[i - 1],
                                              segment.points[i]) < 0.25;
        }
        if (suffix && (!near_path || s.controller.tool_in_spindle != tool))
          fact("restart_out_of_path", std::format("line={} tool={} length={} tip={},{},{}",
               s.motion_line, s.controller.tool_in_spindle, s.tool_offset[2], tool_tip[0],
               tool_tip[1], tool_tip[2]));
        suffix = suffix && near_path && s.controller.tool_in_spindle == tool;
      }
      return first_motion != 0 && !s.task_executing && rig.idle();
    });
    const auto end = rig.await(id);
    check("restart_followed_preview_suffix",
          end.accepted() && finished && first_motion == line && suffix && lifted && xy_at_clearance &&
          spindle_on == !exchange,
          std::format("{} first={} exact_suffix={} z_first={} spindle={} status={} messages={}",
                      name, first_motion, suffix, xy_at_clearance, spindle_on, end.text(),
                      PreviewOracle::join(rig.all_messages())));
  };

  if (setup) {
    const auto [canned, unused] = plan("G21 G90 G17\nG0 Z20\nG81 X1 Y1 Z5 R10 F100\nG80\nM2\n", 3);
    check("restart_canned_block", canned.reason.find("canned cycle") != std::string::npos,
          canned.text());
    const auto missing = rig.send(R"("type":"plan_restart","line":23,"preview_context":7)");
    check("restart_preview_pending",
          missing.outcome == "rejected" && missing.reason.find("preview") != std::string::npos,
          missing.text());
    approach("06-toolchange.ngc", 22, 98, true);
    if (failures == 0) {
      const bool repositioned = mdi("M61 Q1 G43 H1") && mdi("G0 Z20") && mdi("G0 X0 Y0");
      check("restart_pocket_setup", repositioned);
      if (repositioned)
        approach("03-pocket.ngc", 24, 1, false);
    }
    if (failures == 0) {
      const bool repositioned = mdi("G43 H1") && mdi("G0 Z30") && mdi("G0 X0 Y0");
      const auto [approved, path] = plan("G21 G90 G17 G54 G94 G97 G40 G61\nG43 H1\n"
                                        "G0 Z30\nG1 X12 F600\nM30\n", 4);
      // The encoder changes after the operator has reviewed the plan. The trajectory's
      // commanded Z is already at the clearance and must not be moved down before XY.
      const bool encoder = halcmd("loadrt sum2 names=restart-z-encoder") &&
          halcmd("addf restart-z-encoder servo-thread") &&
          halcmd("net z-pos-cmd restart-z-encoder.in0") &&
          halcmd("net restart-z-feedback restart-z-encoder.out") &&
          halcmd("unlinkp joint.2.motor-pos-fb") &&
          halcmd("net restart-z-feedback joint.2.motor-pos-fb") &&
          halcmd("setp restart-z-encoder.in1 0.125") &&
          rig.pump_until(2s, [&] {
            const auto s = rig.status();
            return near(s.controller.axes[2].machine - s.commanded_position[2], 0.125, 0.01);
          });
      check("restart_encoder_backlash_setup", repositioned && approved.accepted() && encoder,
            approved.text());
      if (approved.accepted() && encoder) {
        const auto restarted = rig.send(start_command(approved));
        const bool finished = rig.pump_until(8s, [&] {
          return rig.idle() && near(rig.status().work[0], 12, 0.05);
        });
        check("restart_with_encoder_backlash", restarted.accepted() && finished, restarted.text());
      }
      if (encoder)
        static_cast<void>(halcmd("setp restart-z-encoder.in1 0"));
    }
  }

  static_cast<void>(rig.send(R"("type":"abort")"));
  rig.stop();
  rig.restart_preview = nullptr;
  rig.restart_engine = nullptr;
  rig.start();
}

void program_preflight(Rig& rig) {
  const auto table = LoadedToolTable::attach(rig.config.millimetres_per_unit);
  check("preflight_loaded_tool_table", table.has_value());
  if (!table)
    return;
  PreviewOracle oracle{rig, *table};
  PreviewWorker preview{oracle.engine};
  rig.stop();
  rig.restart_preview = &preview;
  rig.start();
  const auto setup = rig.send(
      R"("type":"set_tool","tool":{"number":2,"pocket":2,"length":0,"diameter":6.35,"comment":"Untouched"})");
  check("preflight_setup", setup.accepted(), setup.text());
  std::uint64_t context = 0;
  const auto load = [&](std::string_view blocks) {
    const std::string prefix = "G21 G90 G17 G54 G40 G49 G80 G94 G97 G61\n";
    const auto [inputs, path] = oracle.preview(prefix + std::string(blocks) + "M30\n");
    preview.submit(++context, inputs);
    const bool ready = rig.pump_until(3s, [&] {
      const auto shown = preview.checked_preview().preview;
      return shown && shown->context == context;
    });
    check("preflight_context_ready", ready && path.check.has_value(),
          path.error ? path.error->text : "");
    return path.check.value_or(ProgramCheck{});
  };
  const auto start = [&](bool confirmed, std::uint64_t reviewed) {
    return rig.send(
        std::format(R"("type":"run","preview_context":{},"display_units":"mm","confirmed":{})",
                    reviewed, confirmed));
  };
  const auto refused = [&](const char* scenario, std::string_view blocks, std::string_view reason) {
    const auto report = load(blocks);
    const auto trace = rig.port.writes();
    const auto outcome = start(true, context);
    check(scenario,
          !report.errors.empty() && outcome.outcome == "rejected" &&
              outcome.reason.find(reason) != std::string::npos && rig.port.writes() == trace,
          outcome.text());
  };
  refused("preflight_blocks_spindle_above_cap", "S1600 M3\nG0 X10\nM5\n", "spindle limit");
  refused("preflight_blocks_missing_tool_length", "T2 M6\nG43 H2\nG0 X10\n", "T2");
  refused("preflight_blocks_unverified_work_origin", "G55\nG0 X10\n", "G55");
  refused("preflight_blocks_configured_travel", "G53 G0 X1010\n", "machine travel");

  const auto report = load("G0 X8\nG1 X10 F300\n");
  check("preflight_reports_work_and_feed",
        report.errors.empty() && report.travel == TravelCheck::unknown && report.warnings.empty() &&
            report.frames.size() == 1 && report.frames[0].wcs == 1 && report.frames[0].bounds &&
            near(report.frames[0].bounds->max[0], 10) && report.feed == ProgramRange{300, 300});
  const auto direct = start(false, context);
  const bool reached_direct =
      rig.pump_until(5s, [&] { return rig.idle() && near(rig.status().work[0], 10, 0.02); });
  check("preflight_allows_unverified_physical_travel_without_confirmation",
        direct.accepted() && reached_direct, direct.text());

  const auto inch = load("G20\nG0 X0.25\n");
  const auto review = start(false, context);
  const auto stale = start(true, context - 1);
  check("preflight_requires_current_confirmation",
        std::ranges::contains(inch.units, LengthUnit::inch) && review.outcome == "rejected" &&
            review.reason.find("warnings") != std::string::npos && stale.outcome == "rejected" &&
            stale.reason.find("changed") != std::string::npos,
        review.text() + " " + stale.text());
  const auto accepted = start(true, context);
  const bool reached =
      rig.pump_until(5s, [&] { return rig.idle() && near(rig.status().work[0], 6.35, 0.02); });
  check("preflight_allows_confirmed_warning", accepted.accepted() && reached, accepted.text());

  // A file save can replace the inode while LinuxCNC still has the earlier program open.
  const auto [inputs, old] = oracle.preview("G21 G90 G54\nG0 X8\nM30\n");
  const auto replacement = oracle.program + ".replacement";
  {
    std::ofstream updated{replacement};
    updated << "G21 G90 G54\nG0 X14\nM30\n";
  }
  std::filesystem::rename(replacement, oracle.program);
  preview.submit(++context, inputs);
  const bool ready = rig.pump_until(3s, [&] {
    const auto shown = preview.checked_preview().preview;
    return shown && shown->context == context && shown->check && shown->check->errors.empty();
  });
  const auto replaced = start(true, context);
  const bool reached_new =
      rig.pump_until(5s, [&] { return rig.idle() && near(rig.status().work[0], 14, 0.02); });
  check("preflight_runs_the_checked_replacement_file",
        old.check.has_value() && ready && replaced.accepted() && reached_new, replaced.text());

  rig.stop();
  rig.restart_preview = nullptr;
  rig.start();
}

void raised_run(Rig& rig, NmlLoads& loads) {
  const auto table = LoadedToolTable::attach(rig.config.millimetres_per_unit);
  check("air_cut_loaded_tool_table", table.has_value());
  if (!table)
    return;
  PreviewOracle oracle{rig, *table};
  PreviewWorker preview{oracle.engine};
  rig.stop();
  rig.restart_preview = &preview;
  rig.start();

  const auto marker = std::filesystem::path(rig.config.directory) / "coffeecnc-air-cut.txt";
  std::uint64_t context = 0;
  constexpr double lift = 25;
  const auto load = [&](std::string_view blocks) {
    const std::string prefix = "G21 G90 G17 G54 G40 G49 G80 G94 G97 G61\n";
    auto [inputs, baseline] = oracle.preview(prefix + std::string(blocks) + "M30\n");
    const auto selected = rig.send(R"("type":"set_air_cut","air_z":25)");
    check("air_cut_selected_without_offset", selected.accepted() && near(rig.status().g92[2], 0),
          selected.text());
    inputs.air_z = lift;
    preview.submit(++context, inputs);
    check("air_cut_preview_computed", rig.pump_until(5s, [&] {
      const auto shown = preview.checked_preview().preview;
      return shown && shown->context == context;
    }));
    return preview.checked_preview().preview;
  };
  const auto start = [&](bool confirmed) {
    return rig.send(std::format(
        R"("type":"run","preview_context":{},"display_units":"mm","confirmed":{},"air_z":25)",
        context, confirmed));
  };
  const auto cleared = [&] {
    return rig.idle() && !loads.air_offset_owned && !std::filesystem::exists(marker) &&
           near(rig.status().g92[2], 0);
  };
  const auto stop = load("G0 Z15\nG1 Z5 F2400\nM0\n");
  check("air_cut_shows_raised_path",
        stop && stop->state == PreviewState::ready && stop->check && stop->check->errors.empty() &&
            stop->bounds && near(stop->bounds->min[2], 30) && stop->check->frames.size() == 1 &&
            stop->check->frames[0].bounds && near(stop->check->frames[0].bounds->min[2], 0),
        stop && stop->error ? stop->error->text : "");
  const auto no_confirmation = start(false);
  check("air_cut_requires_explicit_review",
        no_confirmation.outcome == "rejected" && !std::filesystem::exists(marker),
        no_confirmation.text());
  const auto started = start(true);
  const bool paused = rig.pump_until(5s, [&] {
    const auto s = rig.status();
    return s.controller.interp == InterpState::paused && near(s.g92[2], lift) &&
           near(s.commanded_position[2], 30, 0.02);
  });
  check("air_cut_reaches_raised_z",
        started.accepted() && paused && loads.air_offset_owned && std::filesystem::exists(marker),
        started.text());
  const auto aborted = rig.send(R"("type":"abort")");
  check("air_cut_abort_clears_offset", aborted.accepted() && rig.pump_until(5s, cleared),
        aborted.text());

  const auto end = load("G0 Z16\nG1 Z8 F2400\n");
  const auto completed = start(true);
  check(
      "air_cut_m30_clears_offset_without_motion",
      end && completed.accepted() &&
          rig.pump_until(
              6s, [&] { return cleared() && near(rig.status().commanded_position[2], 33, 0.02); }),
      completed.text());

  const auto unsafe = load("G0 Z15\nG53 G0 Z5\n");
  const auto denied = start(true);
  check("air_cut_blocks_machine_coordinates",
        unsafe && unsafe->check && !unsafe->check->errors.empty() && denied.outcome == "rejected" &&
            denied.reason.find("G53") != std::string::npos && !std::filesystem::exists(marker),
        denied.text());
  const auto initial_xy = load("G0 X16\nG0 Z15\nG1 X18 F2400\n");
  check("air_cut_blocks_unraised_initial_traverse", initial_xy && initial_xy->check &&
                                                        !initial_xy->check->errors.empty() &&
                                                        initial_xy->state == PreviewState::failed);

  const auto beyond_limit = load("G0 Z990\nG1 Z980 F2400\n");
  const auto travel_denied = start(true);
  check("air_cut_checks_travel_of_raised_path",
        beyond_limit && beyond_limit->check &&
            beyond_limit->check->travel == TravelCheck::outside &&
            travel_denied.outcome == "rejected" && !std::filesystem::exists(marker),
        travel_denied.text());

  const auto offset = rig.send(R"("type":"mdi","text":"G52 Z7")");
  const bool offset_set =
      rig.pump_until(3s, [&] { return rig.idle() && near(rig.status().g92[2], 7); });
  const auto suspended = rig.send(R"("type":"mdi","text":"G92.2")");
  const bool suspended_ok =
      rig.pump_until(3s, [&] { return rig.idle() && near(rig.status().g92[2], 0); });
  const auto with_stored = load("G0 Z15\nM0\n");
  const auto stored_denied = start(true);
  check("air_cut_preserves_suspended_g92",
        offset.accepted() && offset_set && suspended.accepted() && suspended_ok && with_stored &&
            stored_denied.reason.find("stored G52 or G92") != std::string::npos &&
            !std::filesystem::exists(marker),
        stored_denied.text());
  const auto restore = rig.send(R"("type":"mdi","text":"G92.3")");
  check("air_cut_stored_offset_unchanged",
        restore.accepted() &&
            rig.pump_until(3s, [&] { return rig.idle() && near(rig.status().g92[2], 7); }),
        restore.text());
  const auto reset = rig.send(R"("type":"mdi","text":"G92.1")");
  check("air_cut_reset_test_offset",
        reset.accepted() &&
            rig.pump_until(3s, [&] { return rig.idle() && near(rig.status().g92[2], 0); }),
        reset.text());

  const auto power_after = load("G0 Z15\nM0\n");
  const auto during_power = start(true);
  const bool ready_to_cut_power =
      rig.pump_until(5s, [&] { return rig.status().controller.interp == InterpState::paused; });
  const auto e_stop = rig.send(R"("type":"set_machine","state":"estop")");
  const bool record_preserved = std::filesystem::exists(marker);
  power(rig);
  check("air_cut_estop_recovers_on_power_enable",
        power_after && during_power.accepted() && ready_to_cut_power && e_stop.accepted() &&
            record_preserved && rig.pump_until(5s, cleared),
        during_power.text() + " " + e_stop.text());

  const auto seed = rig.send(R"("type":"mdi","text":"G52 Z18")");
  check("air_cut_seed_crash_offset",
        seed.accepted() &&
            rig.pump_until(3s, [&] { return rig.idle() && near(rig.status().g92[2], 18); }),
        seed.text());
  const auto suspend_owned = rig.send(R"("type":"mdi","text":"G92.2")");
  check("air_cut_seed_suspended_crash_offset",
        suspend_owned.accepted() &&
            rig.pump_until(3s, [&] { return rig.idle() && near(rig.status().g92[2], 0); }),
        suspend_owned.text());
  {
    std::ofstream out{marker};
    out << "18\n";
  }
  rig.stop();
  auto restarted = NmlPort::connect(rig.config, &rig.shared, &loads);
  check("air_cut_restart_connect", restarted.has_value());
  if (restarted) {
    Rig recovered{rig.config, rig.hal, rig.shared, **restarted, rig.reader, rig.view, rig.probe};
    check("air_cut_restart_recovers_owned_offset", recovered.pump_until(5s, cleared));
    const auto reactivate = recovered.send(R"("type":"mdi","text":"G92.3")");
    check("air_cut_restart_removes_saved_register",
          reactivate.accepted() && recovered.pump_until(3s, cleared), reactivate.text());
    recovered.stop();
  }
  rig.restart_preview = nullptr;
  rig.start();
}

void program_breakpoint(Rig& rig, NmlLoads& loads) {
  const auto table = LoadedToolTable::attach(rig.config.millimetres_per_unit);
  check("breakpoint_loaded_tool_table", table.has_value());
  if (!table)
    return;
  PreviewOracle oracle{rig, *table};
  PreviewWorker preview{oracle.engine};
  rig.stop();
  rig.restart_preview = &preview;
  rig.start();
  const auto copy = std::filesystem::path(rig.config.directory) / ".coffeecnc-breakpoint.ngc";
  const auto record = std::filesystem::path(rig.config.directory) / ".coffeecnc-breakpoint.txt";
  const auto restored = [&] {
    return rig.idle() && rig.status().program_file == oracle.program &&
           !std::filesystem::exists(copy) && !std::filesystem::exists(record);
  };
  std::uint64_t context{};
  const auto load = [&](std::string_view blocks) {
    const std::string text =
        "G21 G90 G17 G54 G40 G49 G80 G94 G97 G64\n" + std::string(blocks) + "M30\n";
    const auto [inputs, outcome] = oracle.preview(text);
    preview.submit(++context, inputs);
    check("breakpoint_preview_ready",
          outcome.state == PreviewState::ready &&
              rig.pump_until(5s,
                             [&] {
                               const auto shown = preview.checked_preview().preview;
                               return shown && shown->context == context &&
                                      shown->state == PreviewState::ready;
                             }),
          outcome.error ? outcome.error->text : "");
    return text;
  };
  const auto start = [&](int line, std::uint64_t reviewed, std::string_view expected) {
    return rig.send(std::format(
        R"("type":"run","preview_context":{},"display_units":"mm","breakpoint":{{"line":{},"text":{}}})",
        reviewed, line, Json(expected).dump()));
  };
  const auto at_breakpoint = [&](int line) {
    const auto status = oracle.source.read().status;
    return status.program.file == oracle.program && status.program.breakpoint &&
           status.program.breakpoint->line == line && status.program.breakpoint->reached;
  };

  const auto contents = load("S650 M3\nG0 Z20\nG0 X8 Y4\nG1 Z2 F900\nG1 X11 F1200\n");
  const auto revision = oracle.source.read().status.program.revision;
  const auto stale = start(5, context + 1, "G1 Z2 F900");
  const auto nonmotion = start(2, context, "S650 M3");
  const auto wrong_text = start(5, context, "G1 Z3 F900");
  check("breakpoint_rejects_stale_and_nonmotion_lines",
        stale.outcome == "rejected" && nonmotion.outcome == "rejected" &&
            wrong_text.outcome == "rejected" && !std::filesystem::exists(copy),
        stale.text() + " " + nonmotion.text() + " " + wrong_text.text());
  const auto started = start(5, context, "G1 Z2 F900");
  const bool reached = rig.pump_until(5s, [&] { return at_breakpoint(5); });
  const auto raw = rig.status();
  const auto panel = oracle.source.read().status;
  check("breakpoint_pauses_before_plunge_with_spindle_running",
        started.accepted() && reached && raw.controller.in_position &&
            raw.controller.spindle_active && near(raw.commanded_position[2], 20, 0.02) &&
            near(raw.commanded_position[0], 8, 0.02) && panel.program.revision == revision &&
            std::filesystem::exists(copy),
        started.text() + " raw line " + std::to_string(raw.current_line));
  const auto resumed = rig.send(R"("type":"resume")");
  const bool mapped =
      rig.pump_until(3s, [&] { return oracle.source.read().status.program.motion_line == 5; });
  check("breakpoint_resume_maps_source_line_after_pause", resumed.accepted() && mapped,
        resumed.text());
  check("breakpoint_completion_reopens_untouched_source", rig.pump_until(5s, [&] {
    return restored() && near(rig.status().commanded_position[2], 2, 0.02) &&
           near(rig.status().commanded_position[0], 11, 0.02);
  }) && read_text_file(oracle.program, 4096).value_or("") == contents);

  static_cast<void>(load("M0\nG0 Z15\nG1 Z4 F900\n"));
  const auto second = start(4, context, "G1 Z4 F900");
  const bool earlier = rig.pump_until(4s, [&] {
    const auto s = oracle.source.read().status;
    return s.interp == InterpState::paused && s.program.breakpoint &&
           !s.program.breakpoint->reached;
  });
  check("breakpoint_does_not_claim_an_original_m0", second.accepted() && earlier, second.text());
  const auto continue_to_breakpoint = rig.send(R"("type":"resume")");
  check("breakpoint_reached_after_existing_m0",
        continue_to_breakpoint.accepted() && rig.pump_until(4s, [&] { return at_breakpoint(4); }),
        continue_to_breakpoint.text());
  rig.stop();
  auto restarted = NmlPort::connect(rig.config, &rig.shared, &loads);
  check("breakpoint_restart_connect", restarted.has_value());
  if (restarted) {
    Rig recovered{rig.config, rig.hal, rig.shared, **restarted, rig.reader, rig.view, rig.probe};
    check("breakpoint_restart_preserves_original_file_and_pause",
          recovered.pump_until(4s, [&] { return at_breakpoint(4); }));
    {
      std::ofstream changed{oracle.program, std::ios::app};
      changed << "(Changed on disk after the pause)\n";
    }
    const auto refused_resume = recovered.send(R"("type":"resume")");
    check("breakpoint_recovery_refuses_changed_source",
          refused_resume.outcome == "failed" &&
              refused_resume.reason.find("original program changed") != std::string::npos &&
              at_breakpoint(4),
          refused_resume.text());
    const auto stopped = recovered.send(R"("type":"abort")");
    check("breakpoint_stop_reopens_original_after_restart",
          stopped.accepted() && recovered.pump_until(5s, restored), stopped.text());
    recovered.stop();
  }
  rig.restart_preview = nullptr;
  rig.start();
}

void paused_z_jog(Rig& rig) {
  constexpr auto lift = R"("type":"jog_paused_z","distance":3)";
  constexpr auto lower = R"("type":"jog_paused_z","distance":-1)";
  constexpr auto stop = R"("type":"abort")";
  constexpr auto resume = R"("type":"resume")";
  const auto file = rig.config.program_prefix + "/coffeecnc_paused_z.ngc";
  const auto at_pause = [&](std::string_view spindle) {
    std::ofstream{file} << "G21 G90 G54 G49 G94 G97\n"
                        << spindle << "\nG53 G0 X0 Y0 Z-10\nM0\nG53 G1 X5 F600\nM30\n";
    const auto open = rig.send(std::format(R"("type":"open_program","path":"{}")", file));
    const auto run = rig.send(R"("type":"run")");
    const bool paused = rig.pump_until(7s, [&] {
      const auto status = rig.status();
      return status.paused && status.controller.in_position;
    });
    check("paused_z_program_pause", open.accepted() && run.accepted() && paused,
          open.text() + " " + run.text());
    return paused && run.accepted();
  };
  const auto offset_at = [&](double value) {
    const auto z = rig.status().controller.paused_z;
    return z && near(z->offset, value, 0.001) && z->displaced == (value != 0);
  };
  if (!at_pause("M5"))
    return;
  const auto unhomed = rig.send(lift);
  check("paused_z_requires_homing",
        !unhomed.accepted() && unhomed.reason.find("Home all axes") != std::string::npos,
        unhomed.text());
  rig.send(stop);
  for (const auto axis : {"x", "y", "z"}) {
    const auto homed = rig.send(std::format(R"("type":"home","axis":"{}")", axis));
    check("paused_z_home_in_place", homed.accepted(), homed.text());
  }
  if (!at_pause("S650 M3"))
    return;
  const auto spinning = rig.send(lift);
  check("paused_z_requires_spindle_off",
        !spinning.accepted() && spinning.reason.find("spindle") != std::string::npos &&
            offset_at(0),
        spinning.text());
  rig.send(stop);
  if (!at_pause("M5"))
    return;

  const double base = rig.machine(2);
  auto raised = rig.send(lift);
  check("paused_z_lifts_only_z",
        raised.accepted() && offset_at(3) && near(rig.machine(2), base + 3) &&
            near(rig.machine(0), 0) && near(rig.machine(1), 0) && rig.status().paused,
        std::format("{} z={:.4f} base={:.4f} permit={} reason={}", raised.text(), rig.machine(2),
                    base, hal_signal("coffeecnc-eoffset-permit"), rig.status().paused_z_interlock));
  if (!raised.accepted())
    return;
  const auto too_high = rig.send(R"("type":"jog_paused_z","distance":8)");
  const auto too_low = rig.send(R"("type":"jog_paused_z","distance":-4)");
  const auto step = rig.send(R"("type":"step")");
  check("paused_z_stays_in_range",
        !too_high.accepted() && !too_low.accepted() && !step.accepted() && offset_at(3),
        too_high.text() + " " + too_low.text() + " " + step.text());
  const auto lowered = rig.send(lower);
  check("paused_z_lowers_toward_base",
        lowered.accepted() && offset_at(2) && near(rig.machine(2), base + 2), lowered.text());
  const auto continued = rig.send(resume);
  check("paused_z_returns_before_resume",
        continued.accepted() && offset_at(0) && near(rig.machine(2), base) &&
            rig.pump_until(5s, [&] { return rig.idle() && near(rig.machine(0), 5); }) &&
            near(rig.machine(2), base),
        continued.text());

  if (!at_pause("M5"))
    return;
  rig.pump(120ms);
  raised = rig.send(lift);
  const bool lifted_before_stop = raised.accepted() && offset_at(3);
  check("paused_z_lift_before_cancel", lifted_before_stop, raised.text());
  if (!lifted_before_stop)
    return;
  const auto returned = rig.post(resume);
  const bool lowering = rig.pump_until(3s, [&] {
    return rig.status().paused && rig.machine(2) < base + 2.7 && rig.machine(2) > base + 1;
  });
  const auto stopped = rig.send(stop);
  const auto cancelled = rig.await(returned);
  check("paused_z_stop_cancels_return",
        raised.accepted() && lowering && stopped.accepted() && !cancelled.accepted() &&
            rig.pump_until(2s, [&] { return rig.idle() && rig.status().controller.in_position; }) &&
            rig.machine(2) > base + 0.5 && near(rig.machine(0), 0),
        std::format("{} {} lowering={} z={:.3f} base={:.3f}", stopped.text(), cancelled.text(),
                    lowering, rig.machine(2), base));
  const double held = rig.machine(2);
  const auto blocked = rig.send(R"("type":"run")");
  rig.pump(250ms);
  check("paused_z_stop_holds_height",
        !blocked.accepted() && blocked.reason.find("still lifted") != std::string::npos &&
            near(rig.machine(2), held) && held > base + 0.5,
        blocked.text());
  const auto off = rig.send(R"("type":"set_machine","state":"off")");
  const auto on = rig.send(R"("type":"set_machine","state":"on")");
  check("paused_z_power_cycle_clears_without_motion",
        off.accepted() && on.accepted() && rig.pump_until(3s, [&] { return offset_at(0); }) &&
            near(rig.machine(2), held),
        std::format("{} {} held={:.4f} z={:.4f}", off.text(), on.text(), held, rig.machine(2)));

  // Feed hold leaves the interrupted move in the queue even after the axes have stopped.
  std::ofstream{file} << "G21 G90 G54 G49 G94 G97\nM5\nG53 G0 X0 Y0 Z-10\n"
                         "G53 G1 X30 F600\nM30\n";
  const auto opened = rig.send(std::format(R"("type":"open_program","path":"{}")", file));
  const auto started = rig.send(R"("type":"run")");
  const bool traversing = rig.pump_until(6s, [&] { return rig.machine(0) > 2; });
  const auto hold = rig.send(R"("type":"pause")");
  const bool at_rest = rig.pump_until(4s, [&] {
    const auto status = rig.status();
    return status.paused && status.paused_z_still;
  });
  const double x = rig.machine(0);
  const auto held_status = rig.status();
  const auto fed_lift = rig.send(lift);
  check("paused_z_after_feed_hold",
        opened.accepted() && started.accepted() && traversing && hold.accepted() && at_rest &&
            fed_lift.accepted() && offset_at(3) && near(rig.machine(0), x) && x < 25,
        std::format("{} {} codes={} rpm={} spinning={} x={:.3f}", hold.text(), fed_lift.text(),
                    Json(held_status.active_codes).dump(),
                    held_status.spindle_actual_rpm.value_or(-1),
                    held_status.controller.spindle_active, x));
  if (!fed_lift.accepted())
    return;
  const auto returning = rig.post(resume);
  const bool moving_down = rig.pump_until(3s, [&] {
    return rig.status().paused && rig.machine(2) < base + 2.7 && rig.machine(2) > base + 1;
  });
  const auto estop = rig.send(R"("type":"set_machine","state":"estop")");
  const auto interrupted = rig.await(returning);
  rig.pump(250ms);
  check("paused_z_estop_prevents_resume",
        moving_down && estop.accepted() && !interrupted.accepted() &&
            rig.snapshot().machine == MachineState::estop && rig.machine(2) > base + 0.5 &&
            near(rig.machine(0), x),
        estop.text() + " " + interrupted.text());
}

void paused_spindle(Rig& rig, NmlLoads& loads) {
  constexpr auto lift = R"("type":"jog_paused_z","distance":2,"stop_spindle":true)";
  constexpr auto resume = R"("type":"resume")";
  constexpr auto abort = R"("type":"abort")";
  const auto file = rig.config.program_prefix + "/coffeecnc_paused_spindle.ngc";
  const auto marker = std::filesystem::path(rig.config.directory) / ".coffeecnc-spindle-hold";
  std::ofstream{file} << "G21 G90 G54 G49 G94 G97\nS650 M4\nG53 G0 X0 Y0 Z-10\n"
                         "M0\nG53 G1 X5 F600\nM5\nM30\n";
  for (const auto axis : {"x", "y", "z"}) {
    const auto homed = rig.send(std::format(R"("type":"home","axis":"{}")", axis));
    check("paused_spindle_home_in_place", homed.accepted(), homed.text());
  }
  const auto paused_with_spindle = [&] {
    halcmd("sets spindle-at-speed TRUE");
    halcmd("sets spindle-vel-fb-rpm-abs 650");
    const auto opened = rig.send(std::format(R"("type":"open_program","path":"{}")", file));
    const auto started = rig.send(R"("type":"run")");
    const bool eligible = rig.pump_until(7s, [&] {
      const auto status = rig.status();
      return status.paused && status.controller.paused_z &&
             status.controller.paused_z->spindle == PausedSpindle::can_stop;
    });
    check("paused_spindle_offer", opened.accepted() && started.accepted() && eligible,
          std::format("{} {} codes={} speed={} actual={} reason={}", opened.text(), started.text(),
                      Json(rig.status().active_codes).dump(), rig.status().spindle_rpm,
                      rig.status().spindle_actual_rpm.value_or(-1),
                      rig.status().paused_z_interlock));
    return eligible;
  };
  const auto held = [&] {
    const auto jog = rig.post(lift);
    const bool off = rig.pump_until(3s, [] { return hal_signal("spindle-enable") == "FALSE"; });
    rig.pump(150ms);
    check("paused_spindle_waits_for_measured_stop",
          off && !rig.result(jog) && near(rig.machine(2), -10) && std::filesystem::exists(marker));
    halcmd("sets spindle-vel-fb-rpm-abs 0");
    const auto raised = rig.await(jog);
    const auto status = rig.status();
    check("paused_spindle_stays_off_for_z_lift",
          raised.accepted() && near(rig.machine(2), -8) && status.paused &&
              status.spindle_direction == SpindleDirection::stopped,
          raised.text());
    return raised.accepted();
  };
  if (!paused_with_spindle())
    return;
  const auto declined = rig.send(R"("type":"jog_paused_z","distance":2)");
  check("paused_spindle_requires_confirmation",
        !declined.accepted() && hal_signal("spindle-enable") == "TRUE" && near(rig.machine(2), -10),
        declined.text());
  if (!held())
    return;
  const auto step = rig.send(R"("type":"step")");
  check("paused_spindle_disallows_step", !step.accepted(), step.text());
  halcmd("sets spindle-at-speed FALSE");
  const auto continuing = rig.post(resume);
  const bool starting = rig.pump_until(6s, [] { return hal_signal("spindle-enable") == "TRUE"; });
  halcmd("sets spindle-vel-fb-rpm-abs 650");
  rig.pump(200ms);
  const auto restoring = rig.status();
  check("paused_spindle_starts_only_after_verified_return",
        starting && restoring.paused && near(rig.machine(0), 0) && near(rig.machine(2), -10) &&
            restoring.spindle_direction == SpindleDirection::reverse &&
            near(restoring.spindle_rpm, 650) && !rig.result(continuing));
  halcmd("sets spindle-at-speed TRUE");
  const auto continued = rig.await(continuing);
  const bool past_pause = rig.pump_until(5s, [&] { return rig.idle() && near(rig.machine(0), 5); });
  check("paused_spindle_resumes_after_at_speed",
        continued.accepted() && past_pause && !std::filesystem::exists(marker), continued.text());
  halcmd("sets spindle-vel-fb-rpm-abs 0");

  for (const auto stop : {abort, R"("type":"set_machine","state":"estop")"}) {
    if (!paused_with_spindle() || !held())
      return;
    halcmd("sets spindle-at-speed FALSE");
    const auto retry = rig.post(resume);
    const bool spinning = rig.pump_until(6s, [] { return hal_signal("spindle-enable") == "TRUE"; });
    const auto stopped = rig.send(stop);
    const auto cancelled = rig.await(retry);
    const bool idle = rig.pump_until(3s, [&] {
      return rig.idle() && hal_signal("spindle-enable") == "FALSE" &&
             !std::filesystem::exists(marker);
    });
    check("paused_spindle_stop_interrupts_spinup",
          spinning && stopped.accepted() && !cancelled.accepted() && idle &&
              near(rig.machine(0), 0),
          stopped.text() + " " + cancelled.text());
    if (rig.status().controller.machine == MachineState::estop) {
      rig.send(R"("type":"set_machine","state":"off")");
      rig.send(R"("type":"set_machine","state":"on")");
    }
  }

  if (!paused_with_spindle() || !held())
    return;
  rig.stop();
  auto restarted = NmlPort::connect(rig.config, &rig.shared, &loads);
  check("paused_spindle_reconnect_port", restarted.has_value());
  if (restarted) {
    Rig recovered{rig.config, rig.hal, rig.shared, **restarted, rig.reader, rig.view, rig.probe};
    recovered.pump(120ms);
    const auto interrupted = recovered.send(resume);
    check("paused_spindle_restart_requires_stop",
          !interrupted.accepted() && interrupted.reason.find("Press Stop") != std::string::npos &&
              near(recovered.machine(2), -8) && hal_signal("spindle-enable") == "FALSE",
          interrupted.text());
    const auto stopped = recovered.send(abort);
    const bool cleared = recovered.pump_until(
        3s, [&] { return recovered.idle() && !std::filesystem::exists(marker); });
    check("paused_spindle_restart_stop_recovers",
          stopped.accepted() && cleared && near(recovered.machine(2), -8) &&
              near(recovered.machine(0), 0) && hal_signal("spindle-enable") == "FALSE",
          stopped.text());
    recovered.stop();
  }
  rig.start();
}

auto run(const std::string& ini_file, bool g0759, bool contact_only, bool restore_only,
         bool air_only, bool restart_only, bool preflight_only, bool raised_only,
         bool breakpoint_only, bool paused_z_only, bool paused_spindle_only) -> int {
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
  const bool air_pins = !air_only ||
      (halcmd("net x-pos-cmd coffeecnc-test.x-pos") &&
       halcmd("net y-pos-cmd coffeecnc-test.y-pos") &&
       halcmd("net z-pos-cmd coffeecnc-test.z-pos"));
  check("test_pins", watching && air_pins);
  HalShared shared;
  NmlLoads loads;
  if (restore_only) {
    const auto file = std::filesystem::path(config->tool_table).parent_path() /
                      "coffeecnc-spindle-tool.txt";
    std::ofstream seed(file);
    seed << "1\n";
    seed.close();
    if (!seed)
      throw std::runtime_error("Could not seed the saved spindle tool.");
  }
  auto port = NmlPort::connect(*config, &shared, &loads);
  check("nml_connect", port.has_value(), port ? config->nml_file : port.error());
  // One status channel for the runtime's status thread, one for this thread.
  NmlStatusMailbox mailbox;
  auto reader = NmlStatusReader::connect(*config, &shared, &mailbox);
  auto view = NmlStatusReader::connect(*config, &shared);
  check("nml_status_readers", reader.has_value() && view.has_value(),
        reader ? (view ? std::string("two") : view.error()) : reader.error());
  if (wired != 0 || !watching || !air_pins || !port || !reader || !view)
    return 1;
  if (restore_only && g0759) {
    const auto populated = (*port)->set_tool({1, 1, 50, 6, "Saved end mill"}, always());
    check("startup_test_tool_has_length", populated.status == ActionStatus::accepted,
          populated.reason);
    if (populated.status != ActionStatus::accepted)
      return 1;
  }

  Rig rig{*config, *hal, shared, **port, **reader, **view, *probe};
  rig.mailbox = &mailbox;
  rig.loads = &loads;
  rig.pump(300ms);
  power(rig);
  if (paused_spindle_only) {
    paused_spindle(rig, loads);
    power_off(rig);
    return failures == 0 ? 0 : 1;
  }
  if (paused_z_only) {
    paused_z_jog(rig);
    power_off(rig);
    return failures == 0 ? 0 : 1;
  }
  if (breakpoint_only) {
    program_breakpoint(rig, loads);
    power_off(rig);
    return failures == 0 ? 0 : 1;
  }
  if (raised_only) {
    raised_run(rig, loads);
    power_off(rig);
    return failures == 0 ? 0 : 1;
  }
  if (preflight_only) {
    program_preflight(rig);
    power_off(rig);
    return failures == 0 ? 0 : 1;
  }
  if (restart_only) {
    restart_cuts(rig);
    power_off(rig);
    return failures == 0 ? 0 : 1;
  }
  if (air_only) {
    air_cuts(rig);
    power_off(rig);
    return failures == 0 ? 0 : 1;
  }
  if (restore_only) {
    restore_spindle_tool(rig);
    return failures == 0 ? 0 : 1;
  }
  if (contact_only) {
    jog_contact(rig);
    return failures == 0 ? 0 : 1;
  }
  if (g0759) {
    g0759_signals(rig);
    g0759_unhomed(rig);
    g0759_jogs(rig);
    jog_taps(rig);
    g0759_limits(rig);
    g0759_program(rig);
    g0759_homing(rig);
    if (machine_hal)
      g0759_estop(rig);
    if (machine_hal)
      machine_tool_models(rig, ini_file);
    else
      tool_models(rig, ini_file);
    power_off(rig);
    for (const auto& message : rig.all_messages())
      fact("operator_message", "text=" + word(message));
    return failures == 0 ? 0 : 1;
  }
  signals_not_configured(rig);
  unhomed(rig);
  homing(rig);
  auto table = LoadedToolTable::attach(config->millimetres_per_unit);
  check("tool_data_attached", table.has_value(), table ? "tool.mmap" : table.error());
  if (!table)
    return 1;
  PreviewOracle oracle{rig, *table};
  // Nothing has been interpreted since LinuxCNC started.
  oracle_first_block(oracle, "fresh_start");
  axis_jogs(rig);
  jog_taps(rig);
  if (config->machine_units == LengthUnit::mm)
    jog_rapid_taps(rig);
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
  static_cast<void>(rig.send(R"("type":"mdi","text":"G21 G90 G54 G1 X3 F600")"));
  rig.pump_until(10s, [&]() -> bool { return rig.idle(); });
  oracle_first_block(oracle, "after_mdi_g1");
  overrides(rig);
  program(rig);
  // program() ends with an abort in a G1 move of a G20 section.
  oracle_first_block(oracle, "after_aborted_g1_program");
  // That one ran to its M2.
  oracle_first_block(oracle, "after_finished_program");
  lines_across_open(rig);
  adapter_cancellation(rig);
  home_withdrawn_in_its_wait(rig);
  half_sent_action_is_reported(rig);
  abort_overtakes_what_waits_for_the_gate(rig);
  tools(rig);
  oracle_path(oracle);
  preview_context_during_a_run(oracle);
  preview_attached_mid_program(oracle);
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
  bool models_only = false;
  bool contact_only = false;
  bool restore_only = false;
  bool air_only = false;
  bool restart_only = false;
  bool preflight_only = false;
  bool raised_only = false;
  bool breakpoint_only = false;
  bool paused_z_only = false;
  bool paused_spindle_only = false;
  for (int i = 1; i < argc; ++i) {
    if (std::string_view(argv[i]) == "-ini" && i + 1 < argc)
      ini_file = argv[i + 1];
    // The scenario for run.sh -p g0759, and the same on the machine's own files (run.sh -m).
    machine_hal = machine_hal || std::string_view(argv[i]) == "g0759-machine";
    g0759 = g0759 || machine_hal || std::string_view(argv[i]) == "g0759";
    models_only = models_only || std::string_view(argv[i]) == "--tool-models";
    contact_only = contact_only || std::string_view(argv[i]) == "jog-contact";
    restore_only = restore_only || std::string_view(argv[i]) == "restore-spindle-tool";
    air_only = air_only || std::string_view(argv[i]) == "air-cuts";
    restart_only = restart_only || std::string_view(argv[i]) == "restart-cuts";
    preflight_only = preflight_only || std::string_view(argv[i]) == "program-preflight";
    raised_only = raised_only || std::string_view(argv[i]) == "raised-run";
    paused_spindle_only = paused_spindle_only || std::string_view(argv[i]) == "paused-spindle";
    paused_z_only = paused_z_only || std::string_view(argv[i]) == "paused-z";
    breakpoint_only = breakpoint_only || std::string_view(argv[i]) == "program-breakpoint";
  }
  if (models_only && !ini_file.empty())
    return print_tool_models(ini_file);
  int code = 1;
  try {
    if (ini_file.empty())
      throw std::runtime_error(
          "usage: coffeecnc_sim_tests -ini <file> [g0759|g0759-machine|jog-contact|restore-spindle-tool]");
    code = run(ini_file, g0759, contact_only, restore_only, air_only, restart_only, preflight_only,
               raised_only, breakpoint_only, paused_z_only, paused_spindle_only);
  } catch (const std::exception& error) {
    check("no_exception", false, error.what());
  }
  // LinuxCNC's start script ends with its own cleanup: this line carries the exit code.
  std::cout << "exit=" << code << '\n' << std::flush;
  return code;
}
