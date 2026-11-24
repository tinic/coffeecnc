#include <coffeecnc/app/machine_data.hpp>
#include <coffeecnc/app/offset_history.hpp>
#include <coffeecnc/app/program_edit.hpp>
#include <coffeecnc/app/session.hpp>

#include "fake_file_system.hpp"
#include "fake_tool_model_files.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <format>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace coffeecnc;
using namespace std::chrono_literals;

namespace {
using Json = nlohmann::json;
using Calls = std::vector<std::string>;

int checks = 0;
void require(bool condition, const std::string& message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

constexpr auto gate_reason = "Panel has not finished loading.";
constexpr auto epoch = 1700000000000ms;
constexpr auto program_directory = "/home/cnc/nc_files";
constexpr auto outside_reason = "Path is outside the program directory.";

auto at(std::chrono::milliseconds elapsed) -> Instant {
  return {JogLease::Time{} + elapsed, epoch + elapsed};
}

auto name(Axis axis) -> std::string_view {
  return std::array<std::string_view, 3>{"x", "y", "z"}.at(axis_index(axis).value_or(0));
}
auto name(TaskMode mode) -> std::string_view {
  return mode == TaskMode::manual ? "manual" : mode == TaskMode::mdi ? "mdi" : "auto";
}
auto name(JogSpace space) -> std::string_view { return space == JogSpace::axis ? "axis" : "joint"; }

// Records every action as text and answers from a script; unscripted actions are accepted.
class FakePort final : public ControllerPort {
public:
  ControllerState state;
  // Returned by the next snapshots, before `state`.
  std::deque<ControllerState> next_states;
  // Results for the next calls of an action, by the first word of its record.
  std::map<std::string, std::deque<ActionResult>> script;
  // When set, unscripted actions get a random result.
  std::mt19937* chaos{};
  // A real controller reports the mode it was switched to.
  bool follows_mode{true};
  Calls calls;
  int snapshots{};
  // Called once inside the next snapshot: what happens while the worker waits for LinuxCNC.
  std::function<void()> during_snapshot;
  // The same inside the next action of the given name.
  std::map<std::string, std::function<void()>> during;
  // What the port was told when it asked, after the hook of the action, whether the action
  // is still wanted.
  std::vector<std::pair<std::string, bool>> answers;
  // Handed out by the next take_messages.
  std::vector<ControllerMessage> published;
  std::function<void(const Wanted&)> maintenance;

  auto snapshot() -> ControllerState override {
    ++snapshots;
    if (during_snapshot)
      std::exchange(during_snapshot, {})();
    if (next_states.empty())
      return state;
    auto next = next_states.front();
    next_states.pop_front();
    return next;
  }
  auto take_messages() -> std::vector<ControllerMessage> override {
    return std::exchange(published, {});
  }
  void idle_maintenance(const Wanted& wanted) override {
    if (maintenance)
      maintenance(wanted);
  }
  auto set_machine_state(MachineState machine, const Wanted&) -> ActionResult override {
    return record("set_machine_state", machine == MachineState::on    ? "on"
                                       : machine == MachineState::off ? "off"
                                                                      : "estop");
  }
  auto power_off() -> ActionResult override { return record("power_off", ""); }
  auto set_task_mode(TaskMode mode, const Wanted&) -> ActionResult override {
    auto result = record("set_task_mode", name(mode));
    if (result.status == ActionStatus::accepted && follows_mode)
      state.mode = mode;
    return result;
  }
  auto home(std::optional<Axis> axis, const Wanted&) -> ActionResult override {
    return record("home", axis ? name(*axis) : "all");
  }
  auto start_jog(Axis axis, int direction, double velocity, JogSpace space, const Wanted&)
      -> ActionResult override {
    return record("start_jog",
                  std::format("{} {} {} {}", name(axis), direction, velocity, name(space)));
  }
  auto stop_jog(Axis axis) -> ActionResult override { return record("stop_jog", name(axis)); }
  auto jog_increment(Axis axis, double distance, double velocity, JogSpace space, const Wanted&)
      -> ActionResult override {
    return record("jog_increment",
                  std::format("{} {} {} {}", name(axis), distance, velocity, name(space)));
  }
  auto mdi(std::string_view text, const Wanted& wanted) -> ActionResult override {
    return asked("mdi", record("mdi", text), wanted);
  }
  auto open_program(std::string_view path, const Wanted&) -> ActionResult override {
    return record("open_program", path);
  }
  auto run(const Wanted& wanted) -> ActionResult override {
    return asked("run", record("run", ""), wanted);
  }
  auto pause(const Wanted&) -> ActionResult override { return record("pause", ""); }
  auto resume(const Wanted&) -> ActionResult override { return record("resume", ""); }
  auto step(const Wanted&) -> ActionResult override { return record("step", ""); }
  auto abort() -> ActionResult override { return record("abort", ""); }
  auto set_override(Override which, double value, const Wanted&) -> ActionResult override {
    return record("set_override", std::format("{} {}",
                                              which == Override::feed    ? "feed"
                                              : which == Override::rapid ? "rapid"
                                                                         : "spindle",
                                              value));
  }
  auto set_spindle(SpindleDirection direction, double rpm, const Wanted&) -> ActionResult override {
    return record("set_spindle", std::format("{} {}",
                                             direction == SpindleDirection::forward   ? "forward"
                                             : direction == SpindleDirection::reverse ? "reverse"
                                                                                      : "stopped",
                                             rpm));
  }
  auto set_coolant(bool flood, bool mist, const Wanted&) -> ActionResult override {
    return record("set_coolant", std::format("{} {}", flood, mist));
  }
  auto set_program_option(ProgramOption option, bool on, const Wanted&) -> ActionResult override {
    return record(
        "set_program_option",
        std::format("{} {}",
                    option == ProgramOption::optional_stop ? "optional_stop" : "block_delete", on));
  }
  auto tool_changed(int tool, std::uint64_t request, const Wanted&) -> ActionResult override {
    return record("tool_changed", std::format("tool {} request {}", tool, request));
  }
  auto set_tool(const Tool& tool, const Wanted&) -> ActionResult override {
    return record("set_tool", std::format("{} {} {} {} {}", tool.number, tool.pocket, tool.length,
                                          tool.diameter, tool.comment));
  }
  auto remove_tool(int number, const Wanted&) -> ActionResult override {
    return record("remove_tool", std::to_string(number));
  }
  // Read by a restore of the loaded tool's length; not an action, so not recorded.
  std::expected<ToolLengthInEffect, std::string> length_in_effect{ToolLengthInEffect{}};
  auto tool_length_in_effect() -> std::expected<ToolLengthInEffect, std::string> override {
    return length_in_effect;
  }

private:
  auto asked(const std::string& action, const ActionResult& result, const Wanted& wanted)
      -> ActionResult {
    answers.emplace_back(action, wanted());
    return result;
  }
  auto record(const std::string& action, std::string_view detail) -> ActionResult {
    calls.push_back(detail.empty() ? action : action + ' ' + std::string(detail));
    if (auto hook = std::exchange(during[action], {}))
      hook();
    if (auto& scripted = script[action]; !scripted.empty()) {
      auto result = scripted.front();
      scripted.pop_front();
      return result;
    }
    if (chaos != nullptr) {
      switch ((*chaos)() % 8) {
      case 0:
        return {ActionStatus::failed, "Scripted failure."};
      case 1:
        return {ActionStatus::timed_out, ""};
      default:
        break;
      }
    }
    return {};
  }
};

auto homed_machine() -> ControllerState {
  ControllerState s;
  s.link = Link::live;
  s.machine = MachineState::on;
  s.program_loaded = true;
  s.tool_in_spindle = 3;
  for (auto& a : s.axes) {
    a.enabled = true;
    a.homed = true;
    a.min_limit = -1000;
    a.max_limit = 1000;
  }
  return s;
}

auto results(const Outcome& outcome) -> std::vector<Json> {
  std::vector<Json> found;
  for (const auto& text : outcome.messages)
    if (auto message = Json::parse(text); message.at("type") == "result")
      found.push_back(std::move(message));
  return found;
}

// The session's three threads as one: every call observes the controller, takes its own step
// and then lets the worker finish everything that waits, with control periods where the worker
// waits for the jog-stop level. What a request or a period leads to is then in one outcome, as
// it was when the session did everything in the caller's thread.
struct Driver {
  Driver(Session& driven, FakePort& controller) : session(driven), port(controller) {}
  Session& session;
  FakePort& port;
  // The jog-stop level: follows what the control step asks for.
  bool level{};
  Instant now{};
  // A worker pass asked to close the application.
  bool quit{};
  // Messages of work done outside a request or period (page changes): sent with the next.
  std::vector<std::string> backlog;

  void control() { level = session.control(now, {level, true}); }

  void drain(Outcome& outcome) {
    for (int steps = 0;; ++steps) {
      if (steps > 1000)
        throw std::runtime_error("The worker does not come to rest");
      auto work = session.work([this]() -> Instant { return now; });
      quit = quit || work.quit;
      if (work.outcome.page != PageToken{})
        outcome.page = work.outcome.page;
      for (auto& message : work.outcome.messages)
        outcome.messages.push_back(std::move(message));
      if (work.status == WorkStatus::idle)
        return;
      if (work.status == WorkStatus::blocked)
        control();
    }
  }

  void later() {
    Outcome outcome;
    drain(outcome);
    std::ranges::move(outcome.messages, std::back_inserter(backlog));
  }

  auto begin_page() -> PageToken {
    backlog.clear();
    const auto page = session.begin_page();
    later();
    return page;
  }
  void end_page(PageToken page) {
    session.end_page(page);
    later();
    backlog.clear(); // No page is left to tell.
  }

  auto handle(PageToken page, std::string_view json, Instant at) -> Outcome {
    now = at;
    session.observe(port.state, now.monotonic);
    auto outcome = session.submit(page, json, now);
    drain(outcome);
    // As the session reports: with the result of a request, never to a request it dropped.
    const bool answered = std::ranges::any_of(outcome.messages, [](const auto& text) -> bool {
      return Json::parse(text).at("type") == "result";
    });
    if (answered)
      for (auto& message : std::exchange(backlog, {}))
        outcome.messages.push_back(std::move(message));
    return outcome;
  }

  auto tick(Instant at) -> Outcome {
    now = at;
    session.observe(port.state, now.monotonic);
    Outcome outcome;
    // A fresh outcome has no messages: the swap leaves the backlog empty.
    outcome.messages.swap(backlog);
    control();
    drain(outcome);
    return outcome;
  }
};

// A session with one page that has sent `ready`, on a homed, idle machine in manual mode, with a
// program directory that holds one program and one directory.
struct Rig {
  FakePort port;
  FakeFileSystem files;
  ProgramStore programs{files, program_directory};
  Session session;
  Driver driver{session, port};
  PageToken page;
  RequestId next_id{1};
  std::chrono::milliseconds clock{};

  explicit Rig(bool ready = true, const Limits& limits = Limits{3000, 1.2, 0.5, 1.2, 24000},
               ToolModelService* models = nullptr, OffsetHistoryStore* history = nullptr,
               MachineDataService* machine_data = nullptr)
      : session(port, limits, programs, models, nullptr, nullptr, nullptr, history, nullptr, false,
                machine_data),
        page(driver.begin_page()) {
    port.state = homed_machine();
    files.file("/home/cnc/nc_files/part.ngc", "G0 X1\nM2\n", 1700000123456);
    files.directory("/home/cnc/nc_files/fixtures", 1700000000000);
    files.file("/etc/passwd", "root:x:0:0\n");
    if (ready)
      static_cast<void>(send(R"("type":"ready")"));
    port.calls.clear();
    port.snapshots = 0;
  }

  auto begin_page() -> PageToken { return driver.begin_page(); }
  void end_page(PageToken ended) { driver.end_page(ended); }
  auto handle(PageToken from, std::string_view json, Instant now) -> Outcome {
    return driver.handle(from, json, now);
  }
  auto tick(Instant now) -> Outcome { return driver.tick(now); }

  // `body` is the request without braces and ID. IDs increase by one.
  auto send(std::string_view body) -> Outcome {
    return handle(page, std::format("{{\"id\":{},{}}}", next_id++, body), at(clock));
  }
  // Sends a request that must get exactly one result, for its own ID, and nothing else.
  auto result(std::string_view body) -> Json {
    const auto id = next_id;
    const auto outcome = send(body);
    require(outcome.messages.size() == 1, "Exactly one message for: " + std::string(body));
    auto result = Json::parse(outcome.messages.front());
    const auto expected_fields = 4U + result.count("refusal") + result.count("localization");
    require(result.at("type") == "result" && result.at("id") == id && result.size() == expected_fields,
            "The message is the result of: " + std::string(body));
    return result;
  }
  void expect(std::string_view body, std::string_view outcome, std::string_view reason = "") {
    const auto got = result(body);
    require(got.at("outcome") == outcome && got.at("reason") == reason,
            std::format("Expected {} \"{}\" for {} but got {}", outcome, reason, body, got.dump()));
  }
  void accepted(std::string_view body) { expect(body, "accepted"); }
  void acted(const Calls& expected, const std::string& message) {
    require(std::exchange(port.calls, {}) == expected, message);
  }
};

constexpr auto hold_x = R"("type":"jog","axis":"x","direction":1,"velocity":600,"held":true)";
constexpr auto hold_y = R"("type":"jog","axis":"y","direction":-1,"velocity":600,"held":true)";
constexpr auto release_x = R"("type":"jog","axis":"x","direction":1,"velocity":600,"held":false)";
constexpr auto release_y = R"("type":"jog","axis":"y","direction":-1,"velocity":600,"held":false)";
constexpr auto release_reason = "Release the jog control before jogging again.";

void stale_pages_are_dropped() {
  FakePort port;
  port.state = homed_machine();
  const FakeFileSystem files;
  const ProgramStore programs{files, program_directory};
  Session session{port, Limits{3000, 1.2, 0.5, 1.2, 24000}, programs};
  Driver driver{session, port};
  const auto ready = R"({"id":1,"type":"ready"})";
  require(driver.handle(PageToken{}, ready, at(0ms)).messages.empty() &&
              driver.handle(PageToken{1}, ready, at(0ms)).messages.empty(),
          "No page has begun: nothing is answered");
  const auto first = driver.begin_page();
  const auto second = driver.begin_page();
  require(first != second && first != PageToken{} && second != PageToken{},
          "Every page load gets its own token");
  for (const auto wire : {ready, R"({"id":2,"type":"abort"})", "not json", R"({"id":3})"}) {
    const auto outcome = driver.handle(first, wire, at(0ms));
    require(outcome.messages.empty() && !outcome.send_snapshots,
            "A replaced page gets no result and no message");
  }
  require(port.calls.empty() && port.snapshots == 0, "A replaced page reaches no controller");
  const auto outcome = driver.handle(second, ready, at(0ms));
  require(results(outcome).size() == 1 && outcome.send_snapshots,
          "The current page is unaffected by the stale page's IDs");
}

void controller_messages_wait_for_ready() {
  Rig rig{false};
  rig.port.published.push_back({true, "Motor drive reports a fault."});
  const auto clock = [] { return at(0ms); };
  require(rig.session.work(clock).outcome.messages.empty() && rig.port.published.size() == 1,
          "Controller messages wait while the new page is loading");
  require(rig.session.submit(rig.page, R"({"id":1,"type":"ready"})", at(0ms)).send_snapshots,
          "The document is now ready");
  const auto message = rig.session.work(clock).outcome.messages;
  require(message.size() == 1 && Json::parse(message.front()).at("text") == "Motor drive reports a fault.",
          "The ready document gets the retained controller fault");
  rig.port.published.push_back(
      {false, "Jog stopped: probe contact", MessageKey{"backend.jogStop.probeContact"}});
  const auto localized = rig.session.work(clock).outcome.messages;
  require(localized.size() == 1 && Json::parse(localized.front()).at("localization").at("key") ==
                                       "backend.jogStop.probeContact",
          "The session preserves an operator notice key from the controller port");
}

void panel_watchdog_uses_a_monotonic_page_clock() {
  Rig rig{false};
  auto& session = rig.session;
  const auto page = session.begin_page(true, at(0ms));
  require(session.submit(page, R"({"id":1,"type":"ready"})", at(0ms)).send_snapshots,
          "Ready before the first frame");
  session.panel_frame(page, at(80ms).unix_time, at(80ms));
  session.panel_frame(page, at(160ms).unix_time, {at(160ms).monotonic, at(160ms).unix_time - 1h});
  require(!session.control({at(161ms).monotonic, at(161ms).unix_time - 1h}, {}),
          "A backward correction does not put a fresh document on hold");
  const auto queued = session.submit(page, R"({"id":2,"type":"mdi","text":"G55"})",
                                     {at(180ms).monotonic, at(180ms).unix_time + 1h}, at(160ms).unix_time);
  require(results(queued).empty(), "A forward correction does not make fresh commands stale");
  session.panel_frame(page, at(300ms).unix_time, {at(690ms).monotonic, at(690ms).unix_time + 1h});
  require(session.control({at(690ms).monotonic, at(690ms).unix_time + 1h}, {}),
          "A delayed animation frame remains stale after a wall-clock correction");
}

void panel_watchdog_cancels_waiting_work() {
  Rig rig{false};
  auto& session = rig.session;
  const auto page = session.begin_page(true);
  const auto control = [&](std::chrono::milliseconds elapsed) -> bool {
    return session.control(at(elapsed), {});
  };
  const auto frame = [&](PageToken from, std::chrono::milliseconds drawn,
                         std::chrono::milliseconds elapsed) -> void {
    session.panel_frame(from, at(drawn).unix_time, at(elapsed));
  };
  require(session.submit(page, R"({"id":1,"type":"ready"})", at(0ms)).send_snapshots,
          "Bootstrap is allowed before any frames have been drawn");
  require(control(0ms), "Without frames the HAL jog stop remains high");
  frame(rig.page, 0ms, 0ms);
  frame(page, 80ms, 80ms);
  require(control(80ms), "A frame from the previous page cannot help re-arm jogging");
  frame(page, 160ms, 160ms);
  require(!control(160ms), "Two fresh frames of the current page release the hold");
  const auto queued =
      session.submit(page, R"({"id":2,"type":"mdi","text":"G55"})", at(165ms), at(160ms).unix_time);
  require(results(queued).empty(), "Operator work is admitted while the frame stream is healthy");
  require(control(660ms), "The jog-stop level goes high after a missing frame");
  frame(page, 580ms, 740ms);
  frame(page, 660ms, 741ms);
  require(control(741ms), "Two buffered frames arriving together cannot re-arm jogging");
  frame(page, 820ms, 820ms);
  require(!control(820ms), "Uninterrupted new frames re-arm jogging");
  const auto work = session.work([]() -> Instant { return at(820ms); });
  const auto cancelled = results(work.outcome);
  require(cancelled.size() == 1 && cancelled.front().at("outcome") == "rejected" &&
              rig.port.calls.empty(),
          "Work queued before the frame stall never runs after re-arming");
  const auto old =
      session.submit(page, R"({"id":3,"type":"mdi","text":"G55"})", at(830ms), at(660ms).unix_time);
  require(results(old).at(0).at("outcome") == "rejected",
          "A command for a frame that predates recovery is refused at the bridge");
  require(control(1320ms), "The jog hold returns when the recovered page stalls again");
  rig.driver.now = at(1330ms);
  const auto reset =
      session.submit(page, R"({"id":4,"type":"set_machine","state":"off"})", rig.driver.now);
  require(results(reset).at(0).at("outcome") == "rejected",
          "A stale power request cannot reset E-stop");
  const auto stop = session.submit(page, R"({"id":5,"type":"abort"})", rig.driver.now);
  require(results(stop).empty(), "Abort is admitted with no fresh frame");
  const auto aborted = session.work([&]() -> Instant { return rig.driver.now; });
  require(results(aborted.outcome).at(0).at("outcome") == "accepted" && !rig.port.calls.empty(),
          "Abort reaches LinuxCNC while the frame watchdog holds the jog-stop pin");
}

// Records what the session hands over and answers as told.
class FakeToolModels final : public ToolModelService {
public:
  struct Submitted {
    PageToken page;
    RequestId id{};
    ToolModelCommand command;
  };
  std::vector<Submitted> submitted;
  std::vector<SetOperatorSettings> operator_submitted;
  auto submit_operator_settings(PageToken, RequestId, SetOperatorSettings&& command)
      -> std::optional<std::string> override {
    operator_submitted.push_back(std::move(command));
    return refusal;
  }
  std::vector<std::uint64_t> ended;
  // Not empty: the next commands are refused with it.
  std::optional<std::string> refusal;
  std::optional<int> probe;

  auto submit(PageToken page, RequestId id, ToolModelCommand command)
      -> std::optional<LocalizedReason> override {
    submitted.push_back({page, id, std::move(command)});
    return refusal.transform([](const auto& text) -> LocalizedReason { return {text}; });
  }
  void page_ended(PageToken page) override { ended.push_back(page.value); }
  auto touch_probe_loaded(int number) -> std::optional<bool> override { return probe == number; }
  [[nodiscard]] auto taken() const -> std::size_t { return submitted.size(); }
  [[nodiscard]] auto ends() const -> std::size_t { return ended.size(); }
  [[nodiscard]] auto last_ended() const -> std::uint64_t {
    return ended.empty() ? 0 : ended.back();
  }
};

void touch_probe_blocks_spindle_commands_before_dispatch() {
  FakeToolModels models;
  models.probe = 99;
  Rig rig{true, Limits{3000, 1.2, 0.5, 1.2, 24000}, &models};
  rig.port.state.tool_in_spindle = 99;
  rig.port.state.program_file = "/home/cnc/nc_files/part.ngc";
  constexpr auto reason = "A touch probe is loaded";
  for (const auto* body : {R"("type":"set_spindle","direction":"forward","rpm":200)",
                           R"("type":"set_spindle","direction":"reverse","rpm":200)",
                           R"("type":"mdi","text":"S200 m +0 3")",
                           R"("type":"mdi","text":"M61 Q1")",
                           R"("type":"mdi","text":"G33 Z-1 K1")",
                           R"("type":"mdi","text":"G76 P1 Z-1 I-1 J1 K1")",
                           R"("type":"mdi","text":"G84.2 Z-1")",
                           R"("type":"mdi","text":"G84.3 Z-1")"}) {
    rig.expect(body, "rejected", reason);
    rig.acted({}, "A probe rejects spindle starts before mode change or dispatch");
  }
  rig.accepted(R"("type":"set_spindle","direction":"stopped","rpm":0)");
  rig.acted({"set_spindle stopped 0"}, "Spindle stop remains available");

  rig.port.state.spindle_active = true;
  rig.expect(R"("type":"run")", "rejected", reason);
  rig.acted({}, "An already-running spindle blocks Cycle Start");
  rig.port.state.spindle_active = false;
  rig.files.file(rig.port.state.program_file, "G0 X1\n/ N10 S200 m 0 4\nM2\n");
  rig.expect(R"("type":"run")", "rejected", reason);
  rig.expect(R"("type":"step")", "rejected", reason);
  rig.acted({}, "Run and Single Block both check the entire program");
  rig.files.file(rig.port.state.program_file, "O<other> call\nM2\n");
  rig.expect(R"("type":"run")", "rejected",
             "A touch probe is loaded. CoffeeCNC could not verify that the code leaves the spindle off.");
  rig.acted({}, "Unverifiable subroutine calls cannot run with a probe");
  rig.files.file(rig.port.state.program_file, "G 8 4 X1 Z-1 R2 F20\nM2\n");
  rig.expect(R"("type":"run")", "rejected", reason);
  rig.acted({}, "A tapping cycle that can reverse the spindle cannot run with a probe");
  rig.files.file(rig.port.state.program_file, "S200 (M3 in a comment)\nG0 X1 ; M4\nM05\nM2\n");
  rig.accepted(R"("type":"run")");
  rig.acted({"set_task_mode auto", "open_program /home/cnc/nc_files/part.ngc", "run"},
            "The checked path is reopened and a speed preset, comments and spindle stop are allowed");
  rig.port.state.interp = InterpState::paused;
  rig.accepted(R"("type":"resume")");
  rig.acted({"resume"}, "A checked program can resume with the probe loaded");
  rig.port.state.interp = InterpState::idle;

  rig.port.state.tool_in_spindle = 3;
  rig.files.file(rig.port.state.program_file, "M3 S200\nM2\n");
  rig.accepted(R"("type":"run")");
  rig.acted({"run"}, "An ordinary tool retains existing program behavior");
  rig.port.state.tool_in_spindle = 99;
  rig.port.state.interp = InterpState::paused;
  rig.files.file(rig.port.state.program_file, "G0 X1\nM2\n");
  rig.expect(R"("type":"resume")", "rejected",
             "A touch probe is loaded. CoffeeCNC could not verify that the code leaves the spindle off.");
  rig.acted({}, "Editing the source cannot certify a paused run that started without a probe");
}

void pending_probe_tool_changes() {
  FakeToolModels models;
  models.probe = 99;
  Rig rig{true, Limits{3000, 1.2, 0.5, 1.2, 24000}, &models};
  auto& state = rig.port.state;
  state.tool_in_spindle = 1;
  rig.expect(R"("type":"mdi","text":"M61 Q99 S200 M3")", "rejected",
             "Separate M61 from tool changes and spindle commands.");
  rig.acted({}, "An identity change cannot share an MDI block with spindle actuation");
  rig.accepted(R"("type":"mdi","text":"M61 Q1 G43 H1")");
  rig.acted({"set_task_mode mdi", "mdi M61 Q1 G43 H1"}, "Ordinary spindle-off identification still works");

  state.requested_tool = 99;
  state.tool_request = 10;
  state.active_mdi = "T99 M6 S200 M3";
  rig.expect(R"("type":"tool_changed")", "rejected",
             "Press Stop, then load the touch probe with a separate tool change.");
  rig.acted({}, "A pending MDI exchange cannot acknowledge a probe before a spindle command");
  state.active_mdi = "T99 M6 G43 H99";
  state.mdi_queued = true;
  rig.expect(R"("type":"tool_changed")", "rejected",
             "Press Stop, then load the touch probe with a separate tool change.");
  rig.acted({}, "Pending MDI cannot be certified when blocks are queued behind it");
  state.mdi_queued = false;
  rig.accepted(R"("type":"tool_changed")");
  rig.acted({"tool_changed tool 99 request 10"}, "A stand-alone tool change can mount the probe");

  state.mode = TaskMode::automatic;
  state.program_file = "/home/cnc/nc_files/part.ngc";
  state.active_mdi.clear();
  state.requested_tool = 99;
  state.tool_request = 11;
  rig.expect(R"("type":"tool_changed")", "rejected",
             "Stop the program and load the touch probe from Tools before running.");
  rig.acted({}, "A program started with a cutting tool cannot mount a probe without a safe plan");
  state.requested_tool = 2;
  rig.accepted(R"("type":"tool_changed")");
  rig.acted({"tool_changed tool 2 request 11"}, "Ordinary AUTO tool changes remain available");
}

constexpr auto remove_model =
    R"("type":"set_tool_model_assignment","machine_id":"mill-1","base_revision":3,)"
    R"("tool":{"number":2,"pocket":2,"diameter":6,"comment":"drill"},"family":null,"overrides":{})";
constexpr auto assign_model =
    R"("type":"set_tool_model_assignment","machine_id":"mill-1","base_revision":3,)"
    R"("tool":{"number":2,"pocket":2,"diameter":6,"comment":"drill"},"family":"drill",)"
    R"("overrides":{"tipAngle":135})";

void operator_settings_go_to_storage() {
  FakePort port;
  const FakeFileSystem files;
  const ProgramStore programs{files, program_directory};
  FakeToolModels models;
  Session session{port, Limits{3000, 1.2, 0.5, 1.2, 24000}, programs, &models};
  Driver driver{session, port};
  const auto page = driver.begin_page();
  static_cast<void>(driver.handle(page, R"({"id":1,"type":"ready"})", at(0ms)));
  const auto outcome = driver.handle(
      page,
      R"({"id":2,"type":"set_operator_settings","machine_id":"mill-1","base_revision":0,"document":)"
      R"({"version":1,"startup_units":"inch","enabled_wcs":[1,9],"default_spindle_rpm":1800,"default_jog_velocity":600}})",
      at(0ms));
  require(outcome.messages.empty() && models.operator_submitted.size() == 1,
          "Operator preferences go directly to storage");
  require(port.calls.empty() && port.snapshots == 0,
          "Saving preferences issues no controller commands or reads");
}

class FakeDiagnostics final : public DiagnosticsService {
public:
  std::vector<std::pair<std::uint64_t, bool>> watches;
  std::vector<HalTool> launched;
  std::vector<std::uint64_t> ended;
  std::optional<std::string> refusal;
  void watch(PageToken page, bool on) override { watches.emplace_back(page.value, on); }
  auto launch(HalTool tool) -> std::optional<std::string> override {
    launched.push_back(tool);
    return refusal;
  }
  void page_ended(PageToken page) override { ended.push_back(page.value); }
};

// #251: the diagnostics view's requests go to their service at once, in E-stop with the link
// down, and never to the controller.
void diagnostics_go_to_their_service() {
  FakePort port;
  port.state = ControllerState{};
  const FakeFileSystem files;
  const ProgramStore programs{files, program_directory};
  FakeDiagnostics diagnostics;
  Session session{port,    Limits{3000, 1.2, 0.5, 1.2, 24000}, programs, nullptr, nullptr,
                  nullptr, nullptr,                            nullptr,  &diagnostics};
  Driver driver{session, port};
  RequestId id = 0;
  const auto send = [&](PageToken page, std::string_view body) -> Json {
    const auto outcome = driver.handle(page, std::format("{{\"id\":{},{}}}", ++id, body), at(0ms));
    const auto found = results(outcome);
    return found.size() == 1 ? found.front() : Json();
  };
  const auto page = driver.begin_page();
  require(send(page, R"("type":"watch_diagnostics","on":true)").at("reason") == gate_reason &&
              diagnostics.watches.empty(),
          "Before ready the watch is rejected like any other request");
  static_cast<void>(send(page, R"("type":"ready")"));
  auto answer = send(page, R"("type":"watch_diagnostics","on":true)");
  require(answer.at("outcome") == "accepted" &&
              diagnostics.watches == std::vector<std::pair<std::uint64_t, bool>>{{page.value, true}},
          "The watch is accepted at once, in E-stop with the link down");
  answer = send(page, R"("type":"launch_hal_tool","tool":"halscope")");
  require(answer.at("outcome") == "accepted" &&
              diagnostics.launched == std::vector<HalTool>{HalTool::halscope},
          "A HAL tool is started by the service");
  diagnostics.refusal = "halscope is already open.";
  answer = send(page, R"("type":"launch_hal_tool","tool":"halscope")");
  require(answer.at("outcome") == "rejected" && answer.at("reason") == "halscope is already open.",
          "The service's refusal is the result");
  answer = send(page, R"("type":"watch_diagnostics","on":false)");
  require(answer.at("outcome") == "accepted" && diagnostics.watches.back() ==
                                                    std::pair<std::uint64_t, bool>{page.value, false},
          "The watch ends");
  require(port.calls.empty(), "Nothing reached the controller");
  driver.end_page(page);
  require(!diagnostics.ended.empty() && diagnostics.ended.back() == page.value,
          "The end of the page ends its diagnostics");

  Session without{port, Limits{3000, 1.2, 0.5, 1.2, 24000}, programs};
  Driver plain{without, port};
  const auto other = plain.begin_page();
  static_cast<void>(plain.handle(other, R"({"id":1,"type":"ready"})", at(0ms)));
  const auto refused = results(
      plain.handle(other, R"({"id":2,"type":"launch_hal_tool","tool":"halshow"})", at(0ms)));
  require(refused.size() == 1 && refused.front().at("outcome") == "rejected" &&
              refused.front().at("reason") == "HAL diagnostics are not part of this session.",
          "Without a service the requests are rejected");
}

void tool_model_commands_go_to_their_service() {
  FakePort port;
  const FakeFileSystem files;
  const ProgramStore programs{files, program_directory};
  FakeToolModels models;
  Session session{port, Limits{3000, 1.2, 0.5, 1.2, 24000}, programs, &models};
  Driver driver{session, port};
  RequestId id = 0;
  const auto send = [&](PageToken page, std::string_view body) -> Outcome {
    return driver.handle(page, std::format("{{\"id\":{},{}}}", ++id, body), at(0ms));
  };
  // The machine is in E-stop and the link is down, and stays so.
  port.state = ControllerState{};
  const auto page = driver.begin_page();
  require(models.ended == std::vector<std::uint64_t>{0}, "A new page ends every page before it");

  auto outcome = send(page, assign_model);
  require(results(outcome).size() == 1 && results(outcome).front().at("outcome") == "rejected" &&
              results(outcome).front().at("reason") == gate_reason && models.submitted.empty(),
          "Before ready a tool-model command is rejected like any other");
  static_cast<void>(send(page, R"("type":"ready")"));

  outcome = send(page, assign_model);
  require(outcome.messages.empty() && models.taken() == 1 && models.submitted.back().page == page &&
              models.submitted.back().id == id,
          "After ready it is handed to the service, and answered from there");
  const auto* handed = std::get_if<SetToolModelAssignment>(&models.submitted.back().command);
  require(handed != nullptr && handed->machine_id == "mill-1" && handed->base_revision == 3 &&
              handed->tool == ToolBinding{2, 2, 6, "drill"} &&
              handed->family == ToolFamily::drill && handed->overrides.tip_angle == 135,
          "as decoded");
  outcome = send(page, remove_model);
  require(outcome.messages.empty() && models.taken() == 2, "A removal goes the same way");
  require(port.calls.empty() && port.snapshots == 0,
          "Neither reached the controller: no snapshot, no command, with the link down");
  const auto work = session.work([]() -> Instant { return at(0ms); });
  require(work.status == WorkStatus::idle && work.outcome.messages.empty() && port.calls.empty(),
          "and nothing of them waits for the worker");

  // The machine running a program changes nothing.
  port.state = homed_machine();
  port.state.mode = TaskMode::automatic;
  port.state.interp = InterpState::reading;
  port.state.in_position = false;
  outcome = send(page, assign_model);
  require(outcome.messages.empty() && models.taken() == 3 && port.calls.empty(),
          "While a program runs it is handed over just the same");

  // What the service refuses at once is the one result.
  models.refusal = "Too many tool model changes are waiting.";
  outcome = send(page, assign_model);
  require(outcome.messages.size() == 1 && results(outcome).size() == 1 &&
              results(outcome).front().at("id") == id &&
              results(outcome).front().at("outcome") == "rejected" &&
              results(outcome).front().at("reason") == *models.refusal,
          "A refusal of the service is the command's rejected result");
  models.refusal.reset();

  // The ID rule and the decoder's refusals are the session's, as for every command.
  // Four commands were handed over so far.
  const auto replay =
      driver.handle(page, std::format("{{\"id\":{},{}}}", id, assign_model), at(0ms));
  require(replay.messages.empty() && models.taken() == 4,
          "A replayed ID is dropped before the service sees it");
  outcome = send(page, R"("type":"set_tool_model_assignment","machine_id":"mill-1")");
  require(results(outcome).size() == 1 && results(outcome).front().at("outcome") == "rejected" &&
              models.taken() == 4,
          "A malformed command is rejected by the decoder");
  outcome = send(page, R"("type":"abort")");
  require(results(outcome).size() == 1 && models.taken() == 4,
          "An abort cancels nothing of the service's: it has no part in it");

  // Pages.
  models.ended.clear();
  const auto next = driver.begin_page();
  require(models.ended == std::vector<std::uint64_t>{page.value},
          "A replaced page is ended for the service");
  outcome = send(page, assign_model);
  require(outcome.messages.empty() && models.taken() == 4,
          "A command of the replaced page is dropped");
  id = 0;
  static_cast<void>(send(next, R"("type":"ready")"));
  outcome = send(next, assign_model);
  require(outcome.messages.empty() && models.taken() == 5 && models.submitted.back().page == next,
          "The new page's command carries the new page");
  driver.end_page(next);
  require(models.last_ended() == next.value, "A page that ends is ended for the service");
  outcome = send(next, assign_model);
  require(outcome.messages.empty() && models.taken() == 5, "After its end nothing is handed over");
  session.close();
  require(models.last_ended() == next.value && models.ends() == 3,
          "Closing ends the last page for the service");

  // Without a service.
  Rig rig;
  rig.expect(assign_model, "rejected", "Tool model storage is not part of this session.");
  rig.acted({}, "and nothing reaches the controller");
}

void ready_gate() {
  Rig rig{false};
  const auto early = rig.result(R"("type":"run")");
  require(early.at("outcome") == "rejected" && early.at("reason") == gate_reason &&
              early.at("refusal").at("key") == "messages.session.panelNotLoaded" &&
              early.at("refusal").at("params") == Json::object(),
          "The ready gate sends a catalog key and its English reason.");
  rig.expect(R"("type":"abort")", "rejected", gate_reason);
  rig.expect(R"("type":"jog_stop_all")", "rejected", gate_reason);
  require(rig.port.calls.empty() && rig.port.snapshots == 0, "Nothing runs before ready");

  auto outcome = rig.send(R"("type":"ready")");
  require(outcome.send_snapshots && results(outcome).size() == 1 &&
              results(outcome).front().at("outcome") == "accepted",
          "Ready is accepted and asks for the snapshots");
  rig.accepted(R"("type":"abort")");
  rig.acted({"abort"}, "Commands run after ready");
  require(!rig.send(R"("type":"abort")").send_snapshots, "Only ready asks for the snapshots");

  outcome = rig.send(R"("type":"ready")");
  require(outcome.send_snapshots && results(outcome).size() == 1,
          "A repeated ready asks for the snapshots again");
  const auto replayed = rig.handle(rig.page, R"({"id":1,"type":"run"})", at(0ms));
  require(replayed.messages.empty(), "A repeated ready does not reset the ID state");
  rig.accepted(R"("type":"abort")");
}

void idle_maintenance_yields_to_an_operator_request() {
  Rig rig{false};
  int calls = 0;
  rig.port.maintenance = [&](const Wanted& wanted) {
    if (++calls != 1)
      return;
    require(wanted(), "Startup work is admitted on a ready idle page");
    static_cast<void>(rig.session.submit(rig.page, R"({"id":2,"type":"abort"})", at(0ms)));
    require(!wanted(), "An operator stop overtakes startup work before its next write");
  };
  static_cast<void>(rig.tick(at(0ms)));
  require(calls == 0, "Startup work waits for the page's ready message");
  static_cast<void>(rig.send(R"("type":"ready")"));
  require(calls == 1, "Startup maintenance ran on the ready page");
  rig.port.maintenance = {};
  static_cast<void>(rig.tick(at(0ms)));
  require(rig.port.calls == Calls{"abort"}, "The queued stop was delivered first");
}

void ids_strictly_increase() {
  Rig rig;
  const auto send = [&rig](RequestId id) -> auto {
    return rig.handle(rig.page, std::format(R"({{"id":{},"type":"abort"}})", id), at(0ms));
  };
  rig.port.calls.clear();
  require(results(send(10)).size() == 1, "A higher ID is handled");
  for (const auto id : {RequestId{10}, RequestId{9}, RequestId{1}, RequestId{0}, RequestId{-5}}) {
    const auto outcome = send(id);
    require(outcome.messages.empty() && !outcome.send_snapshots,
            "A duplicate or lower ID is dropped without a result");
  }
  rig.acted({"abort"}, "A dropped request is not executed");
  require(results(send(12)).size() == 1 && results(send(11)).empty(),
          "A gap is allowed; the skipped ID is then too late");
  require(results(send(max_request_id)).size() == 1 && results(send(max_request_id)).empty(),
          "The largest ID is handled once");

  Rig negative{false};
  const auto ready = [&negative](RequestId id) -> auto {
    return negative.handle(negative.page, std::format(R"({{"id":{},"type":"ready"}})", id),
                           at(0ms));
  };
  require(results(ready(-max_request_id)).size() == 1 && results(ready(-3)).size() == 1 &&
              results(ready(0)).size() == 1 && results(ready(-1)).empty(),
          "Any ID starts a page; order is numeric, not by sign");
}

void decode_errors() {
  Rig rig;
  rig.expect(R"("type":"set_mode","mode":"auto")", "rejected", "Unknown protocol v1 command.");
  rig.expect(R"("type":"run","line":100)", "rejected", "Unexpected field in request.");
  const auto missing = results(rig.send(R"("type":"open_program")"));
  require(missing.size() == 1 && missing.front().at("outcome") == "rejected" &&
              missing.front().at("reason") == "Missing required field: path." &&
              missing.front().at("refusal") ==
                  Json{{"key", "messages.request.missingField"}, {"params", {{"field", "path"}}}},
          "An invalid command with an ID carries its field as a catalog parameter");
  require(rig.port.calls.empty() && rig.port.snapshots == 0,
          "Invalid requests reach no controller");
  const auto seen = rig.next_id - 1;
  const auto replay =
      rig.handle(rig.page, std::format(R"({{"id":{},"type":"abort"}})", seen), at(0ms));
  require(replay.messages.empty() && rig.port.calls.empty(),
          "The ID of a rejected request counts as seen");

  for (const auto wire : {"", "not json", "[]", R"({"type":"abort"})",
                          R"({"id":"7","type":"abort"})", R"({"id":99,"type":"abort","id":99})"}) {
    const auto outcome = rig.handle(rig.page, wire, at(250ms));
    require(outcome.messages.size() == 1 && !outcome.send_snapshots,
            "A request without a usable ID gets one message");
    const auto message = Json::parse(outcome.messages.front());
    require(message.at("type") == "message" && message.at("level") == "error" &&
                message.at("time") == (epoch + 250ms).count() && message.size() == 5 &&
                message.at("localization").at("key") == "messages.session.requestIgnored" &&
                message.at("text") ==
                    "Panel request ignored: " +
                        message.at("localization").at("params").at("detail").get<std::string>(),
            "It is an operator error stamped with the wall clock");
  }
  require(rig.port.calls.empty(), "A request without an ID is not executed");
  rig.accepted(R"("type":"abort")");

  Rig loading{false};
  loading.expect(R"("type":"nonsense")", "rejected", "Unknown protocol v1 command.");
}

void policy_rejections_pass_through() {
  Rig rig;
  rig.port.state.program_loaded = false;
  rig.expect(R"("type":"run")", "rejected", "Load a program first.");
  rig.port.state = homed_machine();
  rig.port.state.axes[2].homed = false;
  rig.expect(R"("type":"mdi","text":"G0 X1")", "rejected", "Home all configured axes first.");
  rig.port.state = homed_machine();
  rig.port.state.link = Link::stale;
  rig.expect(R"("type":"home","axis":"all")", "rejected",
             "Controller status is unavailable or stale.");
  rig.port.state = homed_machine();
  rig.expect(R"("type":"select_wcs","wcs":10)", "rejected",
             "Work coordinate system must be G54 through G59.3.");
  rig.expect(R"("type":"set_override","which":"feed","value":5)", "rejected",
             "Feed override is out of range.");
  rig.acted({}, "A rejected request sends nothing to the controller");
  require(rig.port.snapshots == 5, "Every request is judged on a fresh snapshot");
}

// A machine that does not force homing and has a slower Z: the session acts on its limits.
void machine_capabilities() {
  Rig rig{true, Limits{1500, {1500, 1500, 750}, 2.0, 0.5, 1.0, 1500, false}};
  for (auto& axis : rig.port.state.axes)
    axis.homed = false;
  rig.accepted(R"("type":"mdi","text":"G0 X1")");
  rig.acted({"set_task_mode mdi", "mdi G0 X1"}, "Unhomed MDI is sent where homing is not forced");
  rig.accepted(R"("type":"touch_off","axis":"x","value":2.5)");
  rig.acted({"mdi G10 L20 P0 X2.5"}, "Unhomed touch-off is sent");
  rig.accepted(R"("type":"run")");
  rig.acted({"set_task_mode auto", "run"}, "An unhomed run is sent");
  rig.expect(R"("type":"jog","axis":"z","direction":1,"velocity":900,"held":true)", "rejected",
             "Jog velocity is above the Z axis limit of 750 mm/min.");
  const auto fast = rig.result(R"("type":"jog_step","axis":"z","distance":1,"velocity":900)");
  require(fast.at("outcome") == "rejected" &&
              fast.at("reason") == "Jog velocity is above the Z axis limit of 750 mm/min." &&
              fast.at("refusal") == Json{{"key", "messages.policy.jogVelocityAxisLimit"},
                                         {"params", {{"axis", "Z"}, {"limit", "750"}}}},
          "The worker forwards the policy refusal with its named arguments.");
  rig.acted({}, "A jog above its axis ceiling sends nothing");
  rig.accepted(R"("type":"jog","axis":"x","direction":1,"velocity":1500,"held":true)");
  rig.acted({"set_task_mode manual", "start_jog x 1 1500 joint"},
            "An unhomed jog within its ceiling is a joint jog");
}

void mode_switching() {
  Rig rig;
  rig.accepted(R"("type":"run")");
  rig.acted({"set_task_mode auto", "run"}, "The required mode is set before the action");
  rig.accepted(R"("type":"step")");
  rig.acted({"step"}, "No mode change when the controller is already in the mode");
  rig.accepted(R"("type":"mdi","text":"G0 X10 Y-5.5")");
  rig.acted({"set_task_mode mdi", "mdi G0 X10 Y-5.5"}, "MDI switches from auto");
  rig.accepted(R"("type":"home","axis":"z")");
  rig.acted({"set_task_mode manual", "home z"}, "Homing switches to manual");
  rig.accepted(R"("type":"set_coolant","flood":true,"mist":false)");
  rig.acted({"set_coolant true false"}, "A command without a mode leaves the mode alone");

  rig.port.script["set_task_mode"] = {{ActionStatus::failed, "Can't switch mode while moving."}};
  rig.expect(R"("type":"run")", "failed", "Can't switch mode while moving.");
  rig.acted({"set_task_mode auto"}, "A failed mode change is the result; the action is not sent");
  rig.port.script["set_task_mode"] = {{ActionStatus::timed_out, ""}};
  rig.expect(R"("type":"run")", "timed_out", "LinuxCNC did not answer.");
  rig.acted({"set_task_mode auto"}, "A mode change without an answer is the result");

  rig.port.follows_mode = false;
  rig.expect(R"("type":"run")", "failed", "LinuxCNC did not enter the required mode.");
  rig.acted({"set_task_mode auto"}, "The action needs the mode observed, not only requested");
  rig.port.follows_mode = true;

  auto busy = homed_machine();
  busy.mode = TaskMode::automatic;
  busy.interp = InterpState::reading;
  rig.port.next_states = {rig.port.state, busy};
  // A mode change went out, so the refusal that follows is `failed`, not `rejected`.
  rig.expect(R"("type":"run")", "failed", "Wait for the interpreter and motion to become idle.");
  rig.acted({"set_task_mode auto"}, "The request is judged again on the state after the change");
}

void outcome_mapping() {
  Rig rig;
  rig.port.state.mode = TaskMode::automatic;
  rig.accepted(R"("type":"run")");
  rig.port.script["run"] = {{ActionStatus::failed, "Program is not open."},
                            {ActionStatus::timed_out, "No answer within 5 s."},
                            {ActionStatus::failed, ""},
                            {ActionStatus::timed_out, ""}};
  rig.expect(R"("type":"run")", "failed", "Program is not open.");
  rig.expect(R"("type":"run")", "timed_out", "No answer within 5 s.");
  rig.expect(R"("type":"run")", "failed", "LinuxCNC reported an error.");
  const auto timeout = rig.result(R"("type":"run")");
  require(timeout.at("outcome") == "timed_out" &&
              timeout.at("reason") == "LinuxCNC did not answer." &&
              timeout.at("localization").at("key") == "messages.session.linuxcncNoAnswer",
          "The timeout fallback has a key as well as English text");
  rig.accepted(R"("type":"run")");
  rig.acted({"run", "run", "run", "run", "run", "run"}, "Each request is sent once");
}

void every_action() {
  struct Row {
    std::string_view body;
    Calls calls;
  };
  auto paused = homed_machine();
  paused.mode = TaskMode::automatic;
  paused.interp = InterpState::paused;
  auto reading = paused;
  reading.interp = InterpState::reading;
  auto changing = paused;
  changing.requested_tool = 4;
  changing.tool_request = 17;
  auto unhomed = homed_machine();
  unhomed.axes[1].homed = false;
  const auto idle = homed_machine();
  const std::vector<std::pair<ControllerState, Row>> rows{
      {idle, {R"("type":"set_machine","state":"on")", {"set_machine_state on"}}},
      {idle, {R"("type":"set_machine","state":"off")", {"set_machine_state off"}}},
      {idle, {R"("type":"set_machine","state":"estop")", {"set_machine_state estop"}}},
      {idle, {R"("type":"home","axis":"all")", {"home all"}}},
      {idle, {R"("type":"home","axis":"y")", {"home y"}}},
      {idle,
       {R"("type":"jog","axis":"x","direction":-1,"velocity":1500,"held":true)",
        {"start_jog x -1 1500 axis"}}},
      {unhomed,
       {R"("type":"jog","axis":"z","direction":1,"velocity":100,"held":true)",
        {"start_jog z 1 100 joint"}}},
      {idle,
       {R"("type":"jog_step","axis":"z","distance":-0.1,"velocity":600)",
        {"jog_increment z -0.1 600 axis"}}},
      {unhomed,
       {R"("type":"jog_step","axis":"y","distance":2.5,"velocity":60)",
        {"jog_increment y 2.5 60 joint"}}},
      {idle, {R"("type":"jog_stop_all")", {"stop_jog x", "stop_jog y", "stop_jog z"}}},
      {idle, {R"("type":"mdi","text":"G0 X10 Y-5.5")", {"set_task_mode mdi", "mdi G0 X10 Y-5.5"}}},
      {idle,
       {R"("type":"open_program","path":"/home/cnc/nc_files/part.ngc")",
        {"open_program /home/cnc/nc_files/part.ngc"}}},
      {idle, {R"("type":"run")", {"set_task_mode auto", "run"}}},
      {reading, {R"("type":"pause")", {"pause"}}},
      {paused, {R"("type":"resume")", {"resume"}}},
      {paused, {R"("type":"step")", {"step"}}},
      {idle, {R"("type":"step")", {"set_task_mode auto", "step"}}},
      {reading, {R"("type":"abort")", {"abort"}}},
      {idle, {R"("type":"set_override","which":"feed","value":0.85)", {"set_override feed 0.85"}}},
      {idle, {R"("type":"set_override","which":"rapid","value":0.5)", {"set_override rapid 0.5"}}},
      {idle,
       {R"("type":"set_override","which":"spindle","value":1.1)", {"set_override spindle 1.1"}}},
      {idle,
       {R"("type":"set_spindle","direction":"forward","rpm":6000)", {"set_spindle forward 6000"}}},
      {idle,
       {R"("type":"set_spindle","direction":"reverse","rpm":800)", {"set_spindle reverse 800"}}},
      {reading,
       {R"("type":"set_spindle","direction":"stopped","rpm":0)", {"set_spindle stopped 0"}}},
      {idle, {R"("type":"set_coolant","flood":false,"mist":true)", {"set_coolant false true"}}},
      {idle,
       {R"("type":"set_program_option","option":"optional_stop","on":true)",
        {"set_program_option optional_stop true"}}},
      {idle,
       {R"("type":"set_program_option","option":"block_delete","on":false)",
        {"set_program_option block_delete false"}}},
      {idle,
       {R"("type":"set_tool","tool":{"number":3,"pocket":3,"length":48.2,"diameter":6,"comment":"End mill 6 mm"})",
        {"set_tool 3 3 48.2 6 End mill 6 mm"}}},
      {idle, {R"("type":"remove_tool","number":3)", {"remove_tool 3"}}},
      // The confirmation names the request of the snapshot it was decided on.
      {changing, {R"("type":"tool_changed")", {"tool_changed tool 4 request 17"}}},
  };
  for (const auto& [state, row] : rows) {
    Rig rig;
    rig.port.state = state;
    rig.accepted(row.body);
    rig.acted(row.calls, "Controller action for: " + std::string(row.body));
  }
}

void directory_listing() {
  Rig rig;
  const auto listed = rig.send(R"("type":"list_directory","path":null)");
  require(listed.messages.size() == 2, "A listing is a directory and its result");
  require(Json::parse(listed.messages.at(0)) == Json{{"type", "directory"},
                                                     {"path", program_directory},
                                                     {"parent", nullptr},
                                                     {"entries",
                                                      {{{"name", "fixtures"},
                                                        {"kind", "directory"},
                                                        {"bytes", 0},
                                                        {"modified", 1700000000000}},
                                                       {{"name", "part.ngc"},
                                                        {"kind", "file"},
                                                        {"bytes", 9},
                                                        {"modified", 1700000123456}}}},
                                                     {"truncated", false},
                                                     {"writable", true}},
          "A null path lists the program directory: " + listed.messages.at(0));
  require(Json::parse(listed.messages.at(1)) ==
              Json{{"type", "result"}, {"id", 2}, {"outcome", "accepted"}, {"reason", ""}},
          "The accepted result follows the directory");
  rig.acted({}, "Directory listing is not a controller action");

  for (const auto* path : {"fixtures", "/home/cnc/nc_files/fixtures", "fixtures/../fixtures/"}) {
    const auto below = rig.send(std::format(R"("type":"list_directory","path":"{}")", path));
    require(below.messages.size() == 2, "A directory below the root lists");
    const auto directory = Json::parse(below.messages.front());
    require(directory.at("path") == "/home/cnc/nc_files/fixtures" &&
                directory.at("parent") == program_directory && directory.at("entries").empty(),
            std::string("Resolved path and parent for: ") + path);
  }

  rig.files.link("/home/cnc/nc_files/escape", "/etc");
  rig.files.directory("/home/cnc/nc_files/locked");
  rig.files.unreadable.emplace_back("/home/cnc/nc_files/locked");
  rig.files.calls.clear();
  rig.expect(R"("type":"list_directory","path":"..")", "rejected", outside_reason);
  rig.expect(R"("type":"list_directory","path":"/etc")", "rejected", outside_reason);
  rig.expect(R"("type":"list_directory","path":"escape")", "rejected", outside_reason);
  rig.expect(R"("type":"list_directory","path":"/home/cnc/nc_files/escape")", "rejected",
             outside_reason);
  for (const auto& call : rig.files.calls)
    require(!call.starts_with("names "), "A refused directory is not read: " + call);
  rig.expect(R"("type":"list_directory","path":"missing")", "rejected",
             "Path is not available: No such file or directory");
  rig.expect(R"("type":"list_directory","path":"part.ngc")", "rejected",
             "Path is not a directory.");
  rig.expect(R"("type":"list_directory","path":"locked")", "rejected",
             "Directory cannot be read: Permission denied");
  rig.acted({}, "A refused listing is not a controller action either");

  // The listing does not need the machine.
  rig.port.state.link = Link::down;
  rig.port.state.machine = MachineState::estop;
  require(rig.send(R"("type":"list_directory","path":null)").messages.size() == 2,
          "The program directory lists without a controller link");
  rig.port.state = homed_machine();

  // A listing that was cut says so in the directory message itself.
  for (std::size_t i = 0; i < ProgramStore::max_directory_entries + 1; ++i)
    rig.files.file(std::format("/home/cnc/nc_files/many/{}.ngc", i), "");
  rig.clock = 40ms;
  const auto cut = rig.send(R"("type":"list_directory","path":"many")");
  require(cut.messages.size() == 2, "A truncated listing is a directory and its result");
  const auto listing = Json::parse(cut.messages.at(0));
  require(listing.at("entries").size() == ProgramStore::max_directory_entries &&
              listing.at("truncated") == true &&
              Json::parse(cut.messages.at(1)).at("outcome") == "accepted",
          "The directory message carries the truncation: " + cut.messages.at(1));

  Rig loading{false};
  loading.expect(R"("type":"list_directory","path":null)", "rejected", gate_reason);
  require(loading.files.calls.empty(), "Nothing is read before ready");
}

void program_paths() {
  Rig rig;
  require(rig.session.loaded_program().empty(),
          "No program is loaded while LinuxCNC has none open");
  rig.port.state.program_file = "/srv/jobs/bracket.ngc";
  rig.session.observe(rig.port.state, rig.driver.now.monotonic);
  require(rig.session.loaded_program() == "/srv/jobs/bracket.ngc",
          "The loaded program is LinuxCNC's, wherever it is");
  rig.port.state.program_file.clear();
  rig.session.observe(rig.port.state, rig.driver.now.monotonic);
  require(rig.session.loaded_program().empty(), "None once LinuxCNC has none open");
  for (const auto* path : {"part.ngc", "/home/cnc/nc_files/part.ngc", "fixtures/../part.ngc",
                           "/home/cnc/nc_files/./fixtures/../part.ngc"}) {
    rig.accepted(std::format(R"("type":"open_program","path":"{}")", path));
    rig.acted({"open_program /home/cnc/nc_files/part.ngc"},
              std::string("LinuxCNC gets the resolved absolute path for: ") + path);
  }
  rig.files.link("/home/cnc/nc_files/alias.ngc", "/home/cnc/nc_files/part.ngc");
  rig.accepted(R"("type":"open_program","path":"alias.ngc")");
  rig.acted({"open_program /home/cnc/nc_files/part.ngc"}, "A link inside the root is its target");

  rig.files.link("/home/cnc/nc_files/escape.ngc", "/etc/passwd");
  rig.files.link("/home/cnc/nc_files/escape", "/etc");
  const std::string long_path = "/home/cnc/nc_files/" + std::string(236, 'n');
  rig.files.file(long_path, "M2\n");
  rig.files.link("/home/cnc/nc_files/long.ngc", long_path);
  require(long_path.size() == 255, "The long program path is one byte too long");
  const std::vector<std::pair<std::string, std::string>> rows{
      {"/etc/passwd", outside_reason},
      {"../../../etc/passwd", outside_reason},
      {"fixtures/../../secret.ngc", "Path is not available: No such file or directory"},
      {"escape.ngc", outside_reason},
      {"escape/passwd", outside_reason},
      {"/home/cnc/nc_files/escape/passwd", outside_reason},
      {"fixtures", "Path is not a regular file."},
      {"/home/cnc/nc_files", "Path is not a regular file."},
      {"missing.ngc", "Path is not available: No such file or directory"},
      {"long.ngc", "Program path is longer than 254 bytes."},
  };
  for (const auto& [path, reason] : rows) {
    rig.expect(std::format(R"("type":"open_program","path":"{}")", path), "rejected", reason);
    rig.acted({}, "A refused path does not reach LinuxCNC: " + path);
  }

  // The policy's rules come first and are unchanged.
  rig.port.state.interp = InterpState::reading;
  rig.expect(R"("type":"open_program","path":"part.ngc")", "rejected",
             "Stop execution before replacing the program.");
  rig.acted({}, "A running program is not replaced");
  rig.port.state = homed_machine();
  rig.port.script["open_program"] = {{ActionStatus::failed, "File not open."}};
  rig.expect(R"("type":"open_program","path":"part.ngc")", "failed", "File not open.");
  rig.acted({"open_program /home/cnc/nc_files/part.ngc"}, "LinuxCNC's own refusal is `failed`");
}

// The constructor the LinuxCNC simulation tests use.
void composed_mdi() {
  const std::array<std::string_view, 9> words{"G54", "G55",   "G56",   "G57",  "G58",
                                              "G59", "G59.1", "G59.2", "G59.3"};
  for (std::size_t i = 0; i < words.size(); ++i) {
    Rig rig;
    rig.accepted(std::format(R"("type":"select_wcs","wcs":{})", i + 1));
    rig.acted({"set_task_mode mdi", "mdi " + std::string(words.at(i))}, "Work coordinate system");
  }
  for (const auto& [value, text] : std::vector<std::pair<std::string_view, std::string_view>>{
           {"0", "0"},
           {"-0.0", "0"},
           {"-2.5", "-2.5"},
           {"0.1", "0.1"},
           {"25.4", "25.4"},
           {"1e-7", "0.0000001"},
           {"-1.5e-5", "-0.000015"},
           {"123456789.125", "123456789.125"},
           {"1e21", "1000000000000000000000"},
           {"0.30000000000000004", "0.30000000000000004"}}) {
    Rig rig;
    rig.port.state.mode = TaskMode::mdi;
    rig.accepted(std::format(R"("type":"touch_off","axis":"y","value":{})", value));
    rig.acted({std::format("mdi G10 L20 P0 Y{}", text)},
              "Touch-off writes a round-trip decimal without exponent: " + std::string(value));
  }
  Rig rig;
  rig.accepted(R"("type":"touch_off","axis":"x","value":12.5)");
  rig.acted({"set_task_mode mdi", "mdi G10 L20 P0 X12.5"}, "Touch-off of the active system");
  rig.accepted(R"("type":"touch_off","axis":"z","value":-0.05)");
  rig.acted({"mdi G10 L20 P0 Z-0.05"}, "Axis letter follows the request");
  rig.port.state.program_units = LengthUnit::inch;
  rig.accepted(R"("type":"touch_off","axis":"x","value":25.4)");
  rig.acted({"mdi G10 L20 P0 X1"}, "A G20 interpreter is given inches for the protocol's mm");
  rig.accepted(R"("type":"tool_touch_off","value":50.8)");
  rig.acted({"mdi G10 L10 P3 Z2", "mdi G43"}, "Tool touch-off converts as well");
  rig.port.state.program_units = LengthUnit::mm;

  rig.accepted(R"("type":"set_work_offset","wcs":9,"offset":[25.4,-50.8,0])");
  rig.acted({"mdi G10 L2 P9 X25.4 Y-50.8 Z0"},
            "Writes an explicit system without selecting it or replacing rotation/G92");
  rig.port.state.program_units = LengthUnit::inch;
  rig.accepted(R"("type":"set_work_offset","wcs":2,"offset":[25.4,-50.8,0])");
  rig.acted({"mdi G10 L2 P2 X1 Y-2 Z0"}, "Work origins convert protocol mm to interpreter inches");
  rig.accepted(R"("type":"set_work_offset","wcs":2,"offset":[0,0,0])");
  rig.acted({"mdi G10 L2 P2 X0 Y0 Z0"}, "Clear is the same explicit XYZ command");
  rig.port.state.program_units = LengthUnit::mm;

  rig.accepted(R"("type":"tool_touch_off","value":25.4)");
  rig.acted({"mdi G10 L10 P3 Z25.4", "mdi G43"},
            "Tool length is set for the loaded tool, then applied");
  rig.port.state.tool_in_spindle = 12;
  rig.accepted(R"("type":"tool_touch_off","value":-1.25)");
  rig.acted({"mdi G10 L10 P12 Z-1.25", "mdi G43"}, "The tool number comes from the snapshot");
  rig.port.script["mdi"] = {{ActionStatus::failed, "Tool 12 is not in the table."}};
  rig.expect(R"("type":"tool_touch_off","value":1)", "failed", "Tool 12 is not in the table.");
  rig.acted({"mdi G10 L10 P12 Z1"}, "A failed measurement is not applied");
  rig.port.script["mdi"] = {{}, {ActionStatus::timed_out, ""}};
  rig.expect(R"("type":"tool_touch_off","value":1)", "timed_out", "LinuxCNC did not answer.");
  rig.acted({"mdi G10 L10 P12 Z1", "mdi G43"}, "Both blocks belong to the one result");
  // #277: an accepted command has no reason, even when a step of it came back with words.
  rig.port.script["mdi"] = {{ActionStatus::accepted, "LinuxCNC did not acknowledge the command."},
                            {}};
  rig.accepted(R"("type":"tool_touch_off","value":1)");
  rig.acted({"mdi G10 L10 P12 Z1", "mdi G43"}, "An accepted result carries no step's reason");

  rig.expect(R"("type":"touch_off","axis":"x","value":1e300)", "rejected",
             "MDI requires one block of at most 254 bytes.");
  rig.acted({}, "A block LinuxCNC would truncate is not sent");
}

void jog_presses_and_refreshes() {
  Rig rig;
  rig.accepted(hold_x);
  rig.acted({"start_jog x 1 600 axis"}, "The first press starts the jog");
  for (int i = 0; i < 5; ++i) {
    rig.clock += 50ms;
    rig.accepted(hold_x);
    require(rig.tick(at(rig.clock)).messages.empty(), "A refreshed lease does not expire");
  }
  rig.acted({}, "Refreshes are answered without a controller action");

  constexpr auto opposite = R"("type":"jog","axis":"x","direction":-1,"velocity":600,"held":true)";
  constexpr auto faster = R"("type":"jog","axis":"x","direction":-1,"velocity":900,"held":true)";
  rig.expect(opposite, "rejected", release_reason);
  rig.acted({"stop_jog x"}, "A change in direction stops an active press");
  rig.expect(opposite, "rejected", release_reason);
  rig.accepted(release_x);
  rig.acted({"stop_jog x"}, "The original control was released");
  rig.accepted(opposite);
  rig.acted({"start_jog x -1 600 axis"}, "A new press can move in the other direction");
  rig.expect(faster, "rejected", release_reason);
  rig.acted({"stop_jog x"}, "A change in velocity also stops the active press");
  rig.accepted(release_x);
  rig.acted({"stop_jog x"}, "Release clears the latch");
  rig.accepted(faster);
  rig.acted({"start_jog x -1 900 axis"}, "A new press picks up the new speed");
  rig.accepted(hold_y);
  rig.acted({"start_jog y -1 600 axis"}, "Axes are held independently");

  rig.accepted(R"("type":"jog","axis":"x","direction":-1,"velocity":900,"held":false)");
  // The jog-stop level that every stop raises stops all axes: the session stops them all.
  rig.acted({"stop_jog x", "stop_jog y"}, "Release stops the axis, and with it every jog");
  rig.clock += 100ms;
  rig.expect(hold_y, "rejected", release_reason);
  rig.acted({}, "The refreshes of a jog that was stopped do not start it again");
  rig.accepted(release_y);
  rig.acted({"stop_jog y"}, "Its own release");
  rig.accepted(hold_y);
  rig.acted({"start_jog y -1 600 axis"}, "And a new press start it");
  rig.clock += 100ms;
  require(rig.tick(at(rig.clock)).messages.empty(), "Tick has nothing to report");
  rig.acted({}, "A released axis is not stopped again by its old lease");
  rig.accepted(release_x);
  rig.acted({"stop_jog x", "stop_jog y"}, "Release without a lease still stops");
  rig.port.state.link = Link::down;
  rig.accepted(R"("type":"jog","axis":"z","direction":1,"velocity":0,"held":false)");
  rig.acted({"stop_jog z"}, "Release is attempted without controller status");
  rig.port.script["stop_jog"] = {{ActionStatus::failed, "Not connected."}};
  rig.expect(release_x, "failed", "Not connected.");
  rig.acted({"stop_jog x"}, "A stop that failed is reported as that request's result");
}

void jog_failures() {
  Rig rig;
  rig.port.script["start_jog"] = {{ActionStatus::failed, "Joint 0 following error."}};
  rig.expect(hold_x, "failed", "Joint 0 following error.");
  rig.acted({"start_jog x 1 600 axis", "stop_jog x"}, "A start that failed is followed by a stop");
  rig.expect(hold_x, "rejected", release_reason);
  rig.acted({}, "The refreshes of the press that failed do not try again");
  rig.accepted(release_x);
  rig.acted({"stop_jog x"}, "The release");
  rig.accepted(hold_x);
  rig.acted({"start_jog x 1 600 axis"}, "A new press starts; no lease was left by the failed one");

  rig.accepted(release_x);
  rig.acted({"stop_jog x"}, "Release before starting the opposite jog");
  rig.port.script["start_jog"] = {{ActionStatus::timed_out, ""}};
  rig.expect(R"("type":"jog","axis":"x","direction":-1,"velocity":600,"held":true)", "timed_out",
             "LinuxCNC did not answer.");
  rig.acted({"start_jog x -1 600 axis", "stop_jog x"},
            "A start with no answer is independently followed by a stop");
  rig.clock += 1s;
  static_cast<void>(rig.tick(at(rig.clock)));
  rig.acted({}, "Its lease is gone");

  rig.accepted(release_x);
  rig.accepted(hold_x);
  rig.port.calls.clear();
  rig.port.state.axes[0].machine = 1000;
  rig.expect(hold_x, "rejected", "Jog points beyond the configured travel.");
  rig.acted({"stop_jog x"}, "A refresh that is no longer valid stops its jog at once");
  rig.expect(hold_x, "rejected", release_reason);
  rig.acted({}, "The refreshes that follow send nothing");
  rig.accepted(release_x);
  rig.port.calls.clear();
  rig.expect(hold_x, "rejected", "Jog points beyond the configured travel.");
  rig.expect(hold_x, "rejected", "Jog points beyond the configured travel.");
  rig.acted({}, "A rejected press with no jog held sends nothing");

  rig.port.state = homed_machine();
  rig.port.state.mode = TaskMode::mdi;
  rig.accepted(hold_x);
  rig.acted({"set_task_mode manual", "start_jog x 1 600 axis"}, "A press switches to manual first");
  rig.port.state.mode = TaskMode::mdi; // changed behind the session's back
  rig.expect(hold_x, "rejected", release_reason);
  rig.acted({"stop_jog x"}, "A refresh never changes the mode to start its jog again");
}

// A start still in flight when a stop decision raised the level reaches motion under
// jog-inhibit and is refused: the tap ended before its jog began.
void late_jog_starts() {
  constexpr auto inhibit = "Cannot jog while jog-inhibit is active.";
  struct Row {
    std::string name;
    // Runs inside start_jog, after the start was written: what happened while it was in flight.
    std::function<void(Rig&)> meanwhile;
    std::string_view outcome;
    std::string_view reason;
    Calls calls;
  };
  const std::vector<Row> rows{
      {"released in flight",
       [](Rig& rig) -> void {
         static_cast<void>(rig.session.submit(
             rig.page, std::format("{{\"id\":{},{}}}", rig.next_id++, release_x), at(rig.clock)));
         static_cast<void>(rig.session.control(at(rig.clock), {false, true}));
       },
       "accepted",
       "",
       {"start_jog x 1 600 axis", "stop_jog x"}},
      // The control thread stalls after the release: the watchdog may trip before any period
      // reads it, and no period raised CoffeeCNC's level. The refusal stays visible.
      {"released, refused before a period raised the level",
       [](Rig& rig) -> void {
         static_cast<void>(rig.session.submit(
             rig.page, std::format("{{\"id\":{},{}}}", rig.next_id++, release_x), at(rig.clock)));
       },
       "failed",
       inhibit,
       {"start_jog x 1 600 axis", "stop_jog x"}},
      // A period raised the level, then the control thread stalled past the watchdog timeout and
      // the refusal returned before another period: the trip may be unread. It stays visible.
      {"released, the level raised, then the control thread stalled",
       [](Rig& rig) -> void {
         static_cast<void>(rig.session.submit(
             rig.page, std::format("{{\"id\":{},{}}}", rig.next_id++, release_x), at(rig.clock)));
         static_cast<void>(rig.session.control(at(rig.clock), {false, true}));
         rig.driver.now = at(rig.clock + 400ms);
       },
       "failed",
       inhibit,
       {"start_jog x 1 600 axis", "stop_jog x"}},
      {"released, the last period 100 ms before the refusal",
       [](Rig& rig) -> void {
         static_cast<void>(rig.session.submit(
             rig.page, std::format("{{\"id\":{},{}}}", rig.next_id++, release_x), at(rig.clock)));
         static_cast<void>(rig.session.control(at(rig.clock), {false, true}));
         rig.driver.now = at(rig.clock + 100ms);
       },
       "accepted",
       "",
       {"start_jog x 1 600 axis", "stop_jog x"}},
      {"not released",
       [](Rig&) -> void {},
       "failed",
       inhibit,
       {"start_jog x 1 600 axis", "stop_jog x"}},
      {"the watchdog tripped in flight",
       [](Rig& rig) -> void {
         static_cast<void>(rig.session.control(at(rig.clock), {false, false}));
       },
       "failed",
       "Jog stopped by the HAL watchdog: CoffeeCNC's heartbeat stalled for longer than the "
       "watchdog timeout.",
       {"start_jog x 1 600 axis", "stop_jog x"}},
      {"released, then the watchdog tripped",
       [](Rig& rig) -> void {
         static_cast<void>(rig.session.submit(
             rig.page, std::format("{{\"id\":{},{}}}", rig.next_id++, release_x), at(rig.clock)));
         static_cast<void>(rig.session.control(at(rig.clock), {true, false}));
       },
       "failed",
       "Jog stopped by the HAL watchdog: CoffeeCNC's heartbeat stalled for longer than the "
       "watchdog timeout.",
       {"start_jog x 1 600 axis", "stop_jog x"}},
  };
  for (const auto& row : rows) {
    Rig rig;
    rig.port.script["start_jog"] = {{ActionStatus::failed, inhibit}};
    rig.port.during["start_jog"] = [&rig, &row]() -> void { row.meanwhile(rig); };
    const auto pressed = rig.next_id;
    const auto outcome = rig.send(hold_x);
    std::optional<Json> press;
    bool error = false;
    for (const auto& text : outcome.messages) {
      auto message = Json::parse(text);
      if (message.at("type") == "result" && message.at("id") == pressed)
        press = message;
      else if (message.at("type") == "result")
        require(message.at("outcome") == "accepted", row.name + ": the release is accepted");
      else
        error = true;
    }
    require(press && press->at("outcome") == row.outcome && press->at("reason") == row.reason,
            std::format("{}: the press is {} \"{}\", got {}", row.name, row.outcome, row.reason,
                        press ? press->dump() : std::string("none")));
    require(!error, row.name + ": no operator message");
    rig.acted(row.calls, row.name + ": the axis still gets its accepted stop");
    rig.clock += 10ms;
    static_cast<void>(rig.tick(at(rig.clock)));
    require(rig.driver.level, row.name + ": the level is held until a period has seen it high");
    rig.clock += 10ms;
    static_cast<void>(rig.tick(at(rig.clock)));
    require(!rig.driver.level, row.name + ": then released as for any stop");
  }
}

void stop_all_and_cancellation() {
  Rig rig;
  rig.accepted(hold_x);
  rig.accepted(hold_y);
  rig.port.calls.clear();
  rig.accepted(R"("type":"jog_stop_all")");
  rig.acted({"stop_jog x", "stop_jog y", "stop_jog z"}, "Stop-all stops every axis");
  rig.clock += 1s;
  static_cast<void>(rig.tick(at(rig.clock)));
  rig.acted({}, "Stop-all releases every lease");
  rig.port.script["stop_jog"] = {{}, {ActionStatus::failed, "Y did not stop."}};
  rig.expect(R"("type":"jog_stop_all")", "failed", "Y did not stop.");
  rig.acted({"stop_jog x", "stop_jog y", "stop_jog z"}, "A failed stop does not skip the others");

  rig.accepted(release_x);
  rig.accepted(hold_x);
  rig.port.calls.clear();
  rig.accepted(R"("type":"abort")");
  rig.acted({"abort", "stop_jog x"}, "Abort goes out first, then held jogs are stopped");
  rig.accepted(release_x);
  rig.accepted(release_y);
  rig.accepted(hold_x);
  rig.accepted(hold_y);
  rig.port.calls.clear();
  rig.accepted(R"("type":"set_machine","state":"estop")");
  rig.acted({"set_machine_state estop", "stop_jog x", "stop_jog y"},
            "E-stop goes out first, then held jogs are stopped");
  rig.accepted(release_x);
  rig.accepted(hold_x);
  rig.port.calls.clear();
  rig.accepted(R"("type":"mdi","text":"G0 X1")");
  rig.acted({"stop_jog x", "set_task_mode mdi", "mdi G0 X1"}, "No jog survives a mode change");
  rig.clock += 1s;
  static_cast<void>(rig.tick(at(rig.clock)));
  rig.acted({}, "Cancelled leases do not expire later");
}

void lease_expiry() {
  Rig rig;
  rig.accepted(hold_x);
  rig.clock = 100ms;
  rig.accepted(hold_y);
  rig.port.calls.clear();
  rig.port.snapshots = 0;
  require(rig.tick(at(149ms)).messages.empty(), "Held before the timeout");
  rig.acted({}, "Nothing stops at 149 ms");
  require(rig.tick(at(150ms)).messages.empty(), "An expired lease is not an error");
  // One lease running out is a stop decision, and the jog-stop level stops every axis.
  rig.acted({"stop_jog x", "stop_jog y"}, "At exactly 150 ms the expired lease stops every jog");
  static_cast<void>(rig.tick(at(250ms)));
  rig.acted({}, "Each is stopped once");
  static_cast<void>(rig.tick(at(10s)));
  require(rig.port.snapshots == 0 && rig.port.calls.empty(),
          "The dead-man decides on the observed state and never asks the port");

  // A refreshed axis keeps its jog however old the press is.
  rig.clock = 11s;
  rig.accepted(release_x);
  rig.accepted(hold_x);
  rig.clock = 11s + 140ms;
  rig.accepted(hold_x);
  rig.port.calls.clear();
  static_cast<void>(rig.tick(at(11s + 289ms)));
  rig.acted({}, "The lease runs from the last refresh");
  static_cast<void>(rig.tick(at(11s + 290ms)));
  rig.acted({"stop_jog x"}, "And ends 150 ms after it");

  rig.clock = 20s;
  rig.accepted(release_x);
  rig.accepted(hold_x);
  rig.port.calls.clear();
  rig.port.script["stop_jog"] = {{ActionStatus::timed_out, ""}};
  const auto outcome = rig.tick(at(20s + 150ms));
  rig.acted({"stop_jog x"}, "The expired jog is stopped");
  require(outcome.messages.size() == 1 && !outcome.send_snapshots,
          "A stop that failed is reported");
  const auto message = Json::parse(outcome.messages.front());
  require(message == Json{{"type", "message"},
                          {"level", "error"},
                          {"text", "Jog stop failed on X: LinuxCNC did not answer."},
                          {"localization",
                           {{"key", "messages.session.jogStopFailed"},
                            {"params", {{"axis", "X"}, {"detail", "LinuxCNC did not answer."}}}}},
                          {"time", (epoch + 20s + 150ms).count()}},
          "As an operator error, since no request owns it");
}

void lost_machine_cancels_jogs() {
  for (const auto link : {Link::stale, Link::down}) {
    Rig rig;
    rig.accepted(hold_x);
    rig.accepted(hold_y);
    rig.port.calls.clear();
    rig.port.state.link = link;
    static_cast<void>(rig.tick(at(10ms)));
    rig.acted({"stop_jog x", "stop_jog y"}, "Link loss stops held jogs before their leases expire");
    static_cast<void>(rig.tick(at(1s)));
    rig.acted({}, "Each is stopped once");
  }
  for (const auto machine : {MachineState::off, MachineState::estop}) {
    Rig rig;
    rig.accepted(R"("type":"jog","axis":"z","direction":1,"velocity":600,"held":true)");
    rig.port.calls.clear();
    rig.port.state.machine = machine;
    static_cast<void>(rig.tick(at(10ms)));
    rig.acted({"stop_jog z"}, "Leaving machine-on stops held jogs");
    rig.port.state.machine = MachineState::on;
    rig.expect(R"("type":"jog","axis":"z","direction":1,"velocity":600,"held":true)", "rejected",
               release_reason);
    rig.acted({}, "Power coming back does not restart a jog whose control is still held");
    rig.accepted(R"("type":"jog","axis":"z","direction":1,"velocity":600,"held":false)");
    rig.port.calls.clear();
    rig.accepted(R"("type":"jog","axis":"z","direction":1,"velocity":600,"held":true)");
    rig.acted({"start_jog z 1 600 axis"}, "The next press after a release starts again");
  }
}

void stops_held_jog(const ControllerState& changed, const std::string& what) {
  Rig rig;
  rig.accepted(hold_x);
  rig.port.calls.clear();
  rig.port.state = changed;
  static_cast<void>(rig.tick(at(10ms)));
  rig.acted({"stop_jog x"}, what + " stops the held jog before its lease expires");
  static_cast<void>(rig.tick(at(20ms)));
  rig.acted({}, what + ": stopped once");
}

void outside_changes_stop_held_jogs() {
  auto state = homed_machine();
  state.mode = TaskMode::mdi;
  stops_held_jog(state, "A mode change");
  state = homed_machine();
  state.interp = InterpState::reading;
  stops_held_jog(state, "A started interpreter");
  state = homed_machine();
  state.axes[1].homing = true;
  stops_held_jog(state, "Homing");
  state = homed_machine();
  state.axes[2].homed = false;
  stops_held_jog(state, "A change of jog space");
  Rig rig;
  rig.accepted(hold_x);
  rig.port.calls.clear();
  static_cast<void>(rig.tick(at(10ms)));
  rig.acted({}, "An unchanged state keeps the jog");
}

// A jog that was stopped for the operator stays stopped while the panel keeps sending the
// refreshes of the press that began it: only a release and a new press start it again.
void stopped_jogs_need_a_release() {
  struct Row {
    std::string name;
    std::function<void(Rig&)> stop;
    Calls stops;
  };
  const std::vector<Row> rows{
      {"lease expiry",
       [](Rig& rig) -> void {
         rig.clock += 150ms;
         static_cast<void>(rig.tick(at(rig.clock)));
       },
       {"stop_jog x"}},
      {"link loss and recovery",
       [](Rig& rig) -> void {
         rig.port.state.link = Link::stale;
         static_cast<void>(rig.tick(at(rig.clock)));
         rig.port.state.link = Link::live;
       },
       {"stop_jog x"}},
      {"machine off and on",
       [](Rig& rig) -> void {
         rig.port.state.machine = MachineState::off;
         static_cast<void>(rig.tick(at(rig.clock)));
         rig.port.state.machine = MachineState::on;
       },
       {"stop_jog x"}},
      {"a mode change outside and back",
       [](Rig& rig) -> void {
         rig.port.state.mode = TaskMode::mdi;
         static_cast<void>(rig.tick(at(rig.clock)));
         rig.port.state.mode = TaskMode::manual;
       },
       {"stop_jog x"}},
      {"abort",
       [](Rig& rig) -> void { rig.accepted(R"("type":"abort")"); },
       {"abort", "stop_jog x"}},
      {"E-stop",
       [](Rig& rig) -> void { rig.accepted(R"("type":"set_machine","state":"estop")"); },
       {"set_machine_state estop", "stop_jog x"}},
      {"stop all",
       [](Rig& rig) -> void { rig.accepted(R"("type":"jog_stop_all")"); },
       {"stop_jog x", "stop_jog y", "stop_jog z"}},
      {"the release of another axis",
       [](Rig& rig) -> void { rig.accepted(release_y); },
       {"stop_jog y", "stop_jog x"}},
      // The jog-stop level held over a stop LinuxCNC does not confirm, and its release.
      {"a hold and its release",
       [](Rig& rig) -> void {
         rig.port.script["stop_jog"] = {{ActionStatus::timed_out, ""},
                                        {ActionStatus::timed_out, ""}};
         rig.clock += 150ms;
         static_cast<void>(rig.tick(at(rig.clock)));
         rig.expect(hold_x, "rejected", release_reason);
         static_cast<void>(rig.tick(at(rig.clock)));
         rig.expect(hold_x, "rejected", release_reason);
         static_cast<void>(rig.tick(at(rig.clock)));
         require(rig.driver.level, "The stop is accepted; the level was high all the time");
         static_cast<void>(rig.tick(at(rig.clock)));
         require(!rig.driver.level, "And is released in the next period");
       },
       {"stop_jog x", "stop_jog x", "stop_jog x"}},
  };
  for (const auto& row : rows) {
    Rig rig;
    rig.accepted(hold_x);
    rig.port.calls.clear();
    row.stop(rig);
    rig.acted(row.stops, "The jog is stopped by " + row.name);
    for (int i = 0; i < 20; ++i) {
      rig.clock += 50ms;
      rig.expect(hold_x, "rejected", release_reason);
      static_cast<void>(rig.tick(at(rig.clock)));
    }
    rig.acted({}, "Refreshes that go on after " + row.name + " send nothing");
    rig.expect(hold_y, "accepted");
    rig.acted({"start_jog y -1 600 axis"}, "An axis that was not held is not affected");
    rig.accepted(release_y);
    rig.port.calls.clear();
    rig.expect(hold_x, "rejected", release_reason);
    rig.acted({}, "The release of another axis does not free it");
    rig.accepted(release_x);
    rig.acted({"stop_jog x"}, "Its release is sent as ever");
    rig.accepted(hold_x);
    rig.acted({"start_jog x 1 600 axis"}, "And the press after it starts the jog: " + row.name);
  }

  // A press that was never started is held to the same rule.
  Rig rig;
  rig.port.script["start_jog"] = {{ActionStatus::timed_out, ""}};
  rig.expect(hold_x, "timed_out", "LinuxCNC did not answer.");
  rig.port.calls.clear();
  rig.expect(hold_x, "rejected", release_reason);
  rig.acted({}, "A start that was lost is not sent again by a refresh");
  // Increments are single commands: not latched, but refused while the jog-stop level is held.
  rig.expect(R"("type":"jog_step","axis":"x","distance":1,"velocity":600)", "rejected",
             "A jog stop is in progress.");
  rig.acted({}, "No increment is sent under the level");
  // Raised, released, and found low for Session::settled_periods.
  for (int period = 0; period < 2 + Session::settled_periods; ++period) {
    require(
        rig.result(R"("type":"jog_step","axis":"x","distance":1,"velocity":600)").at("outcome") ==
            "rejected",
        "Nor before the level has been low for whole periods");
    static_cast<void>(rig.tick(at(rig.clock)));
  }
  rig.accepted(R"("type":"jog_step","axis":"x","distance":1,"velocity":600)");
  rig.acted({"jog_increment x 1 600 axis"}, "A jog step is not held back by the latch");
  // A new page has pressed nothing.
  rig.page = rig.begin_page();
  rig.acted({"stop_jog x"}, "An increment that may still be moving is stopped with its page");
  rig.next_id = 1;
  rig.accepted(R"("type":"ready")");
  rig.accepted(hold_x);
  rig.acted({"start_jog x 1 600 axis"}, "A new page starts with every axis free");
}

// The worker waits inside a status reading, the program store or a command, and meanwhile the
// request goes stale: nothing more of it is sent, however long the wait was.
void stale_requests_send_nothing() {
  struct Wait {
    std::string name;
    std::string_view body;
    // Arms the hook that runs inside the blocked call.
    std::function<void(Rig&, std::function<void()>)> arm;
    Calls sent_before; // what the request had sent when it went stale
    // The result when a stop request cancelled it: what was sent, truthfully.
    std::string_view outcome{"rejected"};
    std::string_view reason{"Cancelled by a stop request before it was sent."};
  };
  constexpr auto mode_changed =
      "The mode was changed, but a stop request cancelled the command before it was sent.";
  const auto in_auto = [](Rig& rig) -> void { rig.port.state.mode = TaskMode::automatic; };
  const std::vector<Wait> waits{
      {"the first status reading",
       R"("type":"run")",
       [&](Rig& rig, std::function<void()> hook) -> void {
         in_auto(rig);
         rig.port.during_snapshot = std::move(hook);
       },
       {}},
      {"the mode change",
       R"("type":"run")",
       [](Rig& rig, std::function<void()> hook) -> void {
         rig.port.during["set_task_mode"] = std::move(hook);
       },
       {"set_task_mode auto"},
       "failed",
       mode_changed},
      {"the status reading after the mode change",
       R"("type":"run")",
       [](Rig& rig, std::function<void()> hook) -> void {
         rig.port.during["set_task_mode"] = [&rig, hook = std::move(hook)]() -> void {
           rig.port.during_snapshot = hook;
         };
       },
       {"set_task_mode auto"},
       "failed",
       mode_changed},
      {"the program store",
       R"("type":"open_program","path":"part.ngc")",
       [](Rig& rig, std::function<void()> hook) -> void {
         rig.files.during_canonical = std::move(hook);
       },
       {}},
      {"the first block of a tool touch-off",
       R"("type":"tool_touch_off","value":1)",
       [](Rig& rig, std::function<void()> hook) -> void {
         rig.port.state.mode = TaskMode::mdi;
         rig.port.during["mdi"] = std::move(hook);
       },
       {"mdi G10 L10 P3 Z1"},
       "failed",
       "The tool length was set, but a stop request cancelled the command before it was applied "
       "(G43 was not sent)."},
      {"the first status reading of an MDI block",
       R"("type":"mdi","text":"G0 X1")",
       [](Rig& rig, std::function<void()> hook) -> void {
         rig.port.state.mode = TaskMode::mdi;
         rig.port.during_snapshot = std::move(hook);
       },
       {}},
      {"the first status reading of homing",
       R"("type":"home","axis":"x")",
       [](Rig& rig, std::function<void()> hook) -> void {
         rig.port.during_snapshot = std::move(hook);
       },
       {}},
      {"the first status reading of a spindle start",
       R"("type":"set_spindle","direction":"forward","rpm":600)",
       [](Rig& rig, std::function<void()> hook) -> void {
         rig.port.during_snapshot = std::move(hook);
       },
       {}},
      {"the first status reading of a jog press",
       hold_x,
       [](Rig& rig, std::function<void()> hook) -> void {
         rig.port.during_snapshot = std::move(hook);
       },
       {}},
      {"the first status reading of a jog step",
       R"("type":"jog_step","axis":"x","distance":1,"velocity":600)",
       [](Rig& rig, std::function<void()> hook) -> void {
         rig.port.during_snapshot = std::move(hook);
       },
       {}},
  };
  for (const auto& wait : waits) {
    {
      Rig rig;
      wait.arm(rig, [&rig]() -> void { rig.session.end_page(rig.page); });
      const auto outcome = rig.send(wait.body);
      rig.acted(wait.sent_before, "The page ended during " + wait.name + ": nothing more is sent");
      require(outcome.messages.empty(), "And nobody is answered: " + wait.name);
    }
    for (const auto stop : {R"("type":"abort")", R"("type":"set_machine","state":"estop")",
                            R"("type":"set_machine","state":"off")"}) {
      Rig rig;
      const auto id = rig.next_id;
      const auto stop_id = id + 1;
      Outcome stopped;
      wait.arm(rig, [&]() -> void {
        stopped =
            rig.session.submit(rig.page, std::format("{{\"id\":{},{}}}", stop_id, stop), at(0ms));
      });
      const auto outcome = rig.send(wait.body);
      require(stopped.messages.empty(), "The stop is queued");
      auto expected = wait.sent_before;
      expected.emplace_back(std::string(stop).find("abort") != std::string::npos ? "abort"
                            : std::string(stop).find("estop") != std::string::npos
                                ? "set_machine_state estop"
                                : "set_machine_state off");
      rig.acted(expected,
                std::string(stop) + " during " + wait.name + ": only the stop is sent after it");
      const auto found = results(outcome);
      require(found.size() == 2 && found.at(0).at("id") == id && found.at(1).at("id") == stop_id &&
                  found.at(0).at("outcome") == wait.outcome &&
                  found.at(0).at("reason") == wait.reason &&
                  found.at(1).at("outcome") == "accepted",
              "The result says what was sent and what was not; the stop is accepted: " + wait.name +
                  ": " + found.at(0).dump());
    }
  }
}

void stop_distinguishes_queued_requests_after_mode_change() {
  Rig rig;
  rig.port.state.mode = TaskMode::mdi;
  const auto clock = [] { return at(0ms); };
  rig.port.during["set_task_mode"] = [&] {
    static_cast<void>(rig.session.control(clock(), {true, true}));
  };
  const auto home = rig.session.submit(rig.page, R"({"id":2,"type":"home","axis":"x"})", clock());
  require(results(home).empty(), "The home request is queued");
  const auto blocked = rig.session.work(clock);
  require(blocked.status == WorkStatus::blocked && results(blocked.outcome).empty(),
          "The home request waits for the jog-stop level after changing to manual mode");
  rig.acted({"set_task_mode manual"}, "The mode change was sent but homing was not");

  const auto queued = rig.session.submit(rig.page, R"({"id":3,"type":"home","axis":"y"})", clock());
  require(results(queued).empty(), "The second home request is also queued");
  const auto stopped = results(rig.session.submit(rig.page, R"({"id":4,"type":"abort"})", clock()));
  require(stopped.size() == 2 && stopped.at(0).at("id") == 2 && stopped.at(1).at("id") == 3,
          "Stop immediately reports both cancelled requests");
  require(stopped.at(0).at("outcome") == "failed" && !stopped.at(0).contains("refusal") &&
              stopped.at(0).at("reason") == "The mode was changed, but a stop request cancelled "
                                            "the command before it was sent." &&
              stopped.at(0).at("localization").at("key") == "messages.session.modeChanged",
          "The partially sent request is reported as having changed the mode");
  require(stopped.at(1).at("outcome") == "rejected" && !stopped.at(1).contains("localization") &&
              stopped.at(1).at("reason") == "Cancelled by a stop request before it was sent." &&
              stopped.at(1).at("refusal").at("key") == "messages.session.cancelled",
          "The unsent request keeps its cancellation refusal");
}

// The port is handed the question with every action, and the answer follows the request: a
// real port asks it down to the moment it writes.
void port_is_told_when_a_request_goes_stale() {
  using Answers = std::vector<std::pair<std::string, bool>>;
  Rig rig;
  rig.port.state.mode = TaskMode::automatic;
  rig.accepted(R"("type":"run")");
  require(rig.port.answers == Answers{{"run", true}}, "A request that stands is wanted");
  rig.port.answers.clear();
  rig.port.during["run"] = [&rig]() -> void { rig.session.end_page(rig.page); };
  static_cast<void>(rig.send(R"("type":"run")"));
  require(rig.port.answers == Answers{{"run", false}},
          "The page ended while the port worked: the answer is no");

  for (const auto stop :
       {R"({"id":900,"type":"abort"})", R"({"id":900,"type":"set_machine","state":"estop"})"}) {
    Rig stopped;
    stopped.port.state.mode = TaskMode::mdi;
    stopped.port.during["mdi"] = [&stopped, stop]() -> void {
      static_cast<void>(stopped.session.submit(stopped.page, stop, at(0ms)));
    };
    static_cast<void>(stopped.send(R"("type":"mdi","text":"G0 X1")"));
    require(stopped.port.answers == Answers{{"mdi", false}},
            std::string("A stop was asked while the port worked: the answer is no: ") + stop);
  }
  // An action the port withdrew half way comes back `failed` with what was sent: that, not a
  // "cancelled before it was sent", is the panel's result.
  Rig coolant;
  constexpr auto half = "Cancelled after this was sent: flood on. Not sent: mist on.";
  coolant.port.script["set_coolant"] = {{ActionStatus::failed, half}};
  coolant.port.during["set_coolant"] = [&coolant]() -> void {
    static_cast<void>(
        coolant.session.submit(coolant.page, R"({"id":900,"type":"abort"})", at(0ms)));
  };
  const auto halved = results(coolant.send(R"("type":"set_coolant","flood":true,"mist":true)"));
  require(halved.size() == 2 && halved.front().at("outcome") == "failed" &&
              halved.front().at("reason") == half,
          "A half-sent action is failed with the port's account of it");
  // And one of which the port wrote nothing is rejected: nothing was sent.
  Rig nothing;
  nothing.port.script["set_coolant"] = {{ActionStatus::cancelled, ""}};
  nothing.port.during["set_coolant"] = [&nothing]() -> void {
    static_cast<void>(
        nothing.session.submit(nothing.page, R"({"id":900,"type":"abort"})", at(0ms)));
  };
  const auto none = results(nothing.send(R"("type":"set_coolant","flood":true,"mist":true)"));
  require(none.size() == 2 && none.front().at("outcome") == "rejected" &&
              none.front().at("reason") == "Cancelled by a stop request before it was sent.",
          "An action of which nothing was written is rejected");

  // What only stops something is never withdrawn, whatever happens to its page.
  Rig spindle;
  spindle.port.during["set_spindle"] = [&spindle]() -> void {
    spindle.session.end_page(spindle.page);
  };
  spindle.port.calls.clear();
  static_cast<void>(spindle.send(R"("type":"set_spindle","direction":"stopped","rpm":0)"));
  spindle.acted({"set_spindle stopped 0"}, "A spindle stop is carried out");
}

// An increment is motion nobody holds. From the moment it is sent it counts as moving, so a stop
// decision raises the jog-stop level for it, until the controller was seen at rest again.
void home_after_increment() {
  Rig rig;
  rig.port.state.axes[0].machine = rig.port.state.axes[0].max_limit - 1;
  rig.accepted(R"("type":"jog_step","axis":"x","distance":1,"velocity":300)");
  rig.acted({"jog_increment x 1 300 axis"}, "An increment may end at the configured limit");
  rig.port.state.axes[0].machine = rig.port.state.axes[0].max_limit;
  rig.clock = 10ms;
  static_cast<void>(rig.tick(at(rig.clock)));
  require(!rig.driver.level, "A completed step at the limit does not assert jog stop");
  rig.acted({}, "The limit endpoint does not schedule a jog stop");
  rig.accepted(R"("type":"home","axis":"x")");
  rig.acted({"stop_jog x", "home x"}, "Homing fences an old increment before sending Home");
  rig.port.state.axes[0].homing = true;
  rig.clock = 20ms;
  static_cast<void>(rig.tick(at(rig.clock)));
  require(!rig.driver.level, "Old increment tracking leaves homing alone");
  rig.acted({}, "No stop is sent after homing started");
}

void increments_are_tracked() {
  constexpr auto long_step = R"("type":"jog_step","axis":"x","distance":100,"velocity":600)";
  struct Row {
    std::string name;
    std::function<void(Rig&)> stop;
    Calls sent;
  };
  const std::vector<Row> rows{
      {"page end", [](Rig& rig) -> void { rig.session.end_page(rig.page); }, {"stop_jog x"}},
      {"page reload",
       [](Rig& rig) -> void { rig.page = rig.session.begin_page(); },
       {"stop_jog x"}},
      {"abort",
       [](Rig& rig) -> void {
         static_cast<void>(
             rig.session.submit(rig.page, R"({"id":500,"type":"abort"})", at(rig.clock)));
       },
       {"abort", "stop_jog x"}},
      {"a stale link",
       [](Rig& rig) -> void {
         rig.port.state.link = Link::stale;
         rig.session.observe(rig.port.state, at(rig.clock).monotonic);
       },
       {"stop_jog x"}},
      {"machine off",
       [](Rig& rig) -> void {
         rig.port.state.machine = MachineState::off;
         rig.session.observe(rig.port.state, at(rig.clock).monotonic);
       },
       {"stop_jog x"}},
      {"a mode change outside",
       [](Rig& rig) -> void {
         rig.port.state.mode = TaskMode::mdi;
         rig.session.observe(rig.port.state, at(rig.clock).monotonic);
       },
       {"stop_jog x"}},
      {"homing starting",
       [](Rig& rig) -> void {
         rig.port.state.axes[1].homing = true;
         rig.session.observe(rig.port.state, at(rig.clock).monotonic);
       },
       {"stop_jog x"}},
      {"a program starting",
       [](Rig& rig) -> void {
         rig.port.state.interp = InterpState::reading;
         rig.session.observe(rig.port.state, at(rig.clock).monotonic);
       },
       {"stop_jog x"}},
  };
  for (const auto& row : rows) {
    Rig rig;
    rig.accepted(long_step);
    rig.port.state.in_position = false;
    rig.clock = 1ms;
    rig.session.observe(rig.port.state, at(rig.clock).monotonic);
    require(!rig.session.control(at(rig.clock), {false, true}), "An increment needs no level");
    rig.port.calls.clear();
    rig.port.script["stop_jog"] = {{ActionStatus::timed_out, ""}};
    row.stop(rig);
    require(rig.session.control(at(rig.clock), {false, true}),
            "With an increment in motion the level is raised by " + row.name);
    Outcome outcome;
    rig.driver.level = true;
    rig.driver.drain(outcome);
    rig.acted(row.sent, "And the axis is stopped over NML: " + row.name);
    for (int period = 0; period < 5; ++period)
      require(rig.session.control(at(rig.clock), {true, true}),
              "The stop was not accepted: the level stays high");
    rig.driver.drain(outcome);
    rig.acted({"stop_jog x"}, "It is retried");
    require(!rig.session.control(at(rig.clock), {true, true}),
            "Accepted: the level is released after " + row.name);
  }

  // Nothing but an accepted jog stop ends the tracking: not a long time at rest, with the
  // increment never seen moving, and not the controller having moved and come to rest. The
  // status cannot tell a finished increment from one the controller has yet to take up.
  for (const bool moved : {false, true}) {
    Rig rig;
    rig.accepted(long_step);
    rig.port.calls.clear();
    if (moved) {
      rig.port.state.in_position = false;
      static_cast<void>(rig.tick(at(10ms)));
      rig.port.state.in_position = true;
      rig.port.state.axes[0].machine = 100;
    }
    for (int period = 2; period <= 500; ++period)
      static_cast<void>(rig.tick(at(period * 10ms)));
    rig.acted({}, "A tracked increment by itself asks for nothing");
    rig.port.script["stop_jog"] = {{ActionStatus::timed_out, ""}};
    rig.session.end_page(rig.page);
    require(rig.session.control(at(5s), {false, true}),
            "Five seconds later the increment is still tracked: the page end raises the level");
    Outcome outcome;
    rig.driver.level = true;
    rig.driver.drain(outcome);
    rig.acted({"stop_jog x"}, "And stops the axis over NML");
    require(rig.session.control(at(5s), {true, true}), "The level is kept until that is accepted");
    rig.driver.drain(outcome);
    rig.acted({"stop_jog x"}, "The retry");
    require(!rig.session.control(at(5s), {true, true}), "Accepted: released");
    rig.page = rig.begin_page();
    rig.acted({}, "The accepted stop ended the tracking: the next page change stops nothing");
  }
  // A new increment on the axis takes the place of the one before.
  {
    Rig rig;
    rig.accepted(long_step);
    rig.accepted(R"("type":"jog_step","axis":"x","distance":-1,"velocity":300)");
    rig.accepted(R"("type":"jog_step","axis":"z","distance":1,"velocity":300)");
    rig.port.calls.clear();
    rig.end_page(rig.page);
    rig.acted({"stop_jog x", "stop_jog z"}, "One stop for each axis that was stepped");
  }
  // An increment that came back without being accepted may have been written all the same and
  // may be taken up later: fenced like a lost start, whatever the port said.
  for (const auto& lost : {ActionResult{ActionStatus::timed_out, ""},
                           ActionResult{ActionStatus::failed, "Error after the write."}}) {
    Rig rig;
    rig.port.script["jog_increment"] = {lost};
    rig.port.script["stop_jog"] = std::deque<ActionResult>(400, {ActionStatus::timed_out, ""});
    rig.clock = 1ms;
    const auto outcome = rig.send(long_step);
    require(results(outcome).front().at("outcome") != "accepted", "The increment was not accepted");
    rig.acted({"jog_increment x 100 600 axis", "stop_jog x"}, "A stop follows it at once");
    // The controller stays at rest for a long time, far beyond any settling time, and never
    // confirms the stop: the increment is still out there, and the level stays up.
    for (int period = 1; period <= 300; ++period) {
      rig.clock = period * 10ms;
      rig.session.observe(rig.port.state, at(rig.clock).monotonic);
      require(rig.session.control(at(rig.clock), {true, true}),
              "The level is held for as long as no stop is accepted");
      Outcome retried;
      rig.driver.level = true;
      rig.driver.drain(retried);
    }
    rig.expect(long_step, "rejected", "A jog stop is in progress.");
    require(std::ranges::count(rig.port.calls, "stop_jog x") > 100 &&
                std::ranges::count_if(rig.port.calls,
                                      [](const std::string& call) -> bool {
                                        return call.starts_with("jog_increment");
                                      }) == 0,
            "Only the stop is retried; no increment is sent under the level");
    // A stop decision in between changes nothing: the level is up and stays up.
    rig.session.end_page(rig.page);
    require(rig.session.control(at(rig.clock), {true, true}), "Also across the end of the page");
    // Then the controller wakes up and the axis moves: the level has been high all along.
    rig.port.state.in_position = false;
    rig.session.observe(rig.port.state, at(rig.clock).monotonic);
    require(rig.session.control(at(rig.clock), {true, true}),
            "The delayed increment starts under a level that was never released");
    rig.port.script["stop_jog"].clear();
    rig.port.calls.clear();
    Outcome later;
    rig.driver.drain(later);
    rig.acted({"stop_jog x"}, "The stop that is accepted at last was written after the increment");
    require(!rig.session.control(at(rig.clock), {true, true}), "Only then is the level released");
  }
}

// From close on the level is high for good, with or without a jog.
void close_holds_the_level() {
  Rig rig;
  rig.session.close();
  for (int period = 0; period < 5; ++period) {
    require(rig.session.control(at(period * 10ms), {period > 0, true}),
            "A closed session holds the level although nothing jogs");
    Outcome outcome;
    rig.driver.drain(outcome);
  }
  rig.acted({}, "And sends nothing");

  Rig jogging;
  jogging.accepted(hold_x);
  jogging.port.calls.clear();
  jogging.session.close();
  Outcome outcome;
  jogging.driver.drain(outcome);
  jogging.acted({"stop_jog x"}, "A jog is stopped by the close");
  for (int period = 0; period < 5; ++period)
    require(jogging.session.control(at(period * 10ms), {true, true}),
            "The stop was accepted, and the level still stays high");
}

void end_of_page() {
  Rig rig;
  rig.accepted(hold_x);
  rig.port.calls.clear();
  rig.end_page(PageToken{});
  rig.acted({}, "Ending a page that is not current does nothing");
  rig.end_page(rig.page);
  rig.acted({"stop_jog x"}, "Ending the page stops its jogs");
  require(rig.handle(rig.page, R"({"id":900,"type":"abort"})", at(0ms)).messages.empty() &&
              rig.port.calls.empty(),
          "Nothing is handled after the page ended");
  require(rig.tick(at(1s)).page == PageToken{}, "A tick names no page while none is open");
  rig.page = rig.begin_page();
  const auto outcome = rig.handle(rig.page, R"({"id":1,"type":"ready"})", at(0ms));
  require(outcome.page == rig.page && outcome.send_snapshots,
          "The next page works and its outcome names it");
}

void explicit_stop_failures_are_retried() {
  Rig rig;
  rig.accepted(hold_x);
  rig.port.calls.clear();
  rig.port.script["stop_jog"] = {{ActionStatus::failed, "Not connected."}};
  rig.expect(release_x, "failed", "Not connected.");
  rig.acted({"stop_jog x"}, "The release asks for the stop");
  auto outcome = rig.tick(at(1ms));
  rig.acted({"stop_jog x"}, "A failed release is retried by tick");
  require(outcome.messages.empty(), "Its failure was already the result of the release");
  static_cast<void>(rig.tick(at(2ms)));
  rig.acted({}, "An accepted retry ends it");

  rig.port.script["stop_jog"] = {{}, {ActionStatus::timed_out, ""}, {}};
  rig.expect(R"("type":"jog_stop_all")", "timed_out", "LinuxCNC did not answer.");
  rig.acted({"stop_jog x", "stop_jog y", "stop_jog z"}, "Stop-all asks every axis");
  static_cast<void>(rig.tick(at(3ms)));
  rig.acted({"stop_jog y"}, "Only the axis whose stop failed is retried");
}

void no_start_over_an_unconfirmed_stop() {
  Rig rig;
  rig.accepted(hold_x);
  rig.port.calls.clear();
  rig.port.script["stop_jog"] = {{ActionStatus::failed, "Not connected."},
                                 {ActionStatus::failed, "Still not connected."}};
  rig.expect(release_x, "failed", "Not connected.");
  rig.port.calls.clear();
  rig.expect(hold_x, "failed",
             "The previous jog stop on this axis is not confirmed: Still not connected.");
  rig.acted({"stop_jog x"}, "A new press first retries the stop and does not start");
  rig.accepted(hold_x);
  rig.acted({"stop_jog x", "start_jog x 1 600 axis"},
            "Once the stop is confirmed the press starts the jog");
  static_cast<void>(rig.tick(at(1ms)));
  rig.acted({}, "The tick does not stop the new jog");
}

void failed_stop_survives_the_page_and_is_retried() {
  Rig rig;
  rig.accepted(hold_x);
  rig.port.calls.clear();
  rig.port.script["stop_jog"] = {{ActionStatus::failed, "Not connected."},
                                 {ActionStatus::timed_out, ""}};
  rig.page = rig.begin_page();
  rig.acted({"stop_jog x"}, "The page boundary asks for the stop");
  auto outcome = rig.tick(at(1ms));
  require(outcome.messages.empty(), "The loading page has no operator messages yet");
  rig.acted({"stop_jog x"}, "The tick retries the failed stop without a lease");
  rig.next_id = 1;
  outcome = rig.send(R"("type":"ready")");
  require(outcome.send_snapshots && outcome.messages.size() == 2 &&
              outcome.messages.back().find("Jog stop failed on X") != std::string::npos,
          "The failed stop is retained for the new page when it is ready");
  outcome = rig.tick(at(2ms));
  require(outcome.messages.empty(), "A retry that fails again does not repeat the message");
  rig.acted({"stop_jog x"}, "It keeps retrying");
  outcome = rig.tick(at(3ms));
  require(outcome.messages.empty(), "Nothing to report once the stop is accepted");
  rig.acted({}, "An accepted stop ends the retries");
}

// quit: refused while the interpreter is busy; else power off when on, no E-stop reset, and
// the worker asks to close only after an accepted reply.
void quit() {
  {
    Rig rig;
    rig.port.state.interp = InterpState::reading;
    rig.expect(R"("type":"quit")", "rejected", "Stop the program before quitting.");
    rig.acted({}, "A busy interpreter sends nothing");
    require(!rig.driver.quit, "and does not close");
  }
  {
    Rig rig;
    rig.accepted(R"("type":"quit")");
    rig.acted({"power_off"}, "A machine that is on is switched off");
    require(rig.driver.quit, "then the application closes");
  }
  for (const auto machine : {MachineState::off, MachineState::estop}) {
    Rig rig;
    rig.port.state.machine = machine;
    rig.accepted(R"("type":"quit")");
    rig.acted({}, "Off or in E-stop: no state change, an E-stop stays set");
    require(rig.driver.quit, "and the application closes");
  }
  {
    Rig rig;
    rig.port.script["power_off"] = {{ActionStatus::failed, "LinuxCNC did not answer."}};
    rig.expect(R"("type":"quit")", "failed", "LinuxCNC did not answer.");
    require(!rig.driver.quit, "A failed switch-off keeps the application open");
  }
  for (const auto link : {Link::stale, Link::down}) {
    Rig rig;
    rig.port.state.link = link;
    rig.expect(R"("type":"quit")", "rejected", "Controller status is unavailable or stale.");
    rig.acted({}, "An unknown controller state must not authorize Quit");
    require(!rig.driver.quit, "An unavailable controller never closes the application");
  }
  for (const bool replace_page : {false, true}) {
    Rig rig;
    rig.port.during["power_off"] = [&]() -> void {
      rig.session.end_page(rig.page);
      if (replace_page)
        rig.page = rig.session.begin_page();
    };
    require(rig.send(R"("type":"quit")").messages.empty(), "Ended page gets no Quit result");
    require(!rig.driver.quit, "Ended or replaced page cannot close the application");
    if (!replace_page)
      rig.page = rig.begin_page();
    rig.accepted(R"("type":"ready")");
    rig.accepted(R"("type":"set_override","which":"feed","value":1)");
    require(!rig.driver.quit, "Later accepted requests cannot inherit Quit intent");
  }
}

// A restore that stages its files and asks to quit, as MachineData does.
class FakeMachineData final : public MachineDataService {
public:
  std::function<void()> during;  // inside restore, before it answers
  std::function<void()> dropped; // inside abandon_restore, after it dropped the plan
  int staged = 0;
  int abandoned = 0;
  std::atomic<bool> armed{}; // a plan is staged and not abandoned
  auto backup(const std::string&) -> MachineDataAnswer override { return {}; }
  auto check_restore(const std::string&) -> MachineDataAnswer override { return {}; }
  std::uint64_t page = 0; // of the last restore
  auto restore(const std::string&, const std::string&, std::uint64_t asked,
               const std::function<LocalizedReason()>&) -> MachineDataAnswer override {
    if (during)
      during();
    ++staged;
    page = asked;
    armed = true;
    return {CommandOutcome::accepted, "", {}, true};
  }
  void abandon_restore() override {
    ++abandoned;
    armed = false;
    if (dropped)
      dropped();
  }
};

// Review #274, 3: a staged restore whose quit was not delivered (the page ended or was replaced,
// an abort came first) stayed staged, and the next normal exit wrote it from old data.
void restore_quit_not_delivered() {
  constexpr auto restore = R"("type":"restore","path":"/media/cnc/STICK/b.tar.gz","token":"0123456789abcdef")";
  {
    FakeMachineData data;
    Rig rig{true, Limits{3000, 1.2, 0.5, 1.2, 24000}, nullptr, nullptr, &data};
    rig.port.state.machine = MachineState::off;
    // The rig's first page began once: nothing was staged then.
    rig.accepted(restore);
    require(rig.driver.quit && data.staged == 1 && data.abandoned == 1 &&
                data.page == rig.page.value,
            "A delivered quit keeps the restore staged for the exit, for the page that asked");
    // Re-review #274, 1: the page is replaced before the window closes. The window drops a
    // close that is not for its page, so no exit comes now: the restore is abandoned.
    rig.page = rig.session.begin_page();
    require(data.abandoned == 2,
            "A new page abandons the restore whose close the window drops");
  }
  for (const bool replace_page : {false, true}) {
    FakeMachineData data;
    Rig rig{true, Limits{3000, 1.2, 0.5, 1.2, 24000}, nullptr, nullptr, &data};
    rig.port.state.machine = MachineState::off;
    data.during = [&]() -> void {
      rig.session.end_page(rig.page);
      if (replace_page)
        rig.page = rig.session.begin_page();
    };
    require(rig.send(restore).messages.empty(), "An ended page gets no result");
    // One for the rig's first page, one for the new page, one for the quit not delivered.
    require(!rig.driver.quit && data.staged == 1 && data.abandoned == (replace_page ? 3 : 2),
            "The quit is not delivered: the staged restore is abandoned");
  }
  {
    FakeMachineData data;
    Rig rig{true, Limits{3000, 1.2, 0.5, 1.2, 24000}, nullptr, nullptr, &data};
    rig.port.state.machine = MachineState::off;
    data.during = [&]() -> void {
      static_cast<void>(rig.session.submit(rig.page, R"({"id":99,"type":"abort"})", at(0ms)));
    };
    static_cast<void>(rig.send(restore));
    require(!rig.driver.quit && data.abandoned == 2,
            "An abort before the quit: the staged restore is abandoned");
  }
}

// Review of a2865e8, 3: begin_page abandoned the restore before it took the session's lock and
// changed the page. A staging that ended in between delivered its quit to the old page (whose
// close the window then drops) and stayed armed for a later exit. Here the staging ends while
// begin_page abandons: before the fix that is before the page changed, and the old page gets
// the quit; with it, begin_page holds the session's lock, so the worker finds the new page.
void restore_staged_while_page_begins() {
  FakeMachineData data;
  Rig rig{true, Limits{3000, 1.2, 0.5, 1.2, 24000}, nullptr, nullptr, &data};
  rig.port.state.machine = MachineState::off;
  std::mutex mutex;
  std::condition_variable changed;
  bool staging = false;
  bool go = false;
  bool sent = false;
  data.during = [&] {
    std::unique_lock lock{mutex};
    staging = true;
    changed.notify_all();
    changed.wait(lock, [&] { return go; });
  };
  std::thread worker{[&] {
    static_cast<void>(rig.send(
        R"("type":"restore","path":"/media/cnc/STICK/b.tar.gz","token":"0123456789abcdef")"));
    const std::scoped_lock lock{mutex};
    sent = true;
    changed.notify_all();
  }};
  {
    std::unique_lock lock{mutex};
    changed.wait(lock, [&] { return staging; });
  }
  std::atomic<bool> first{true};
  data.dropped = [&] {
    if (!first.exchange(false))
      return; // the worker's own abandon, once it found the page changed
    std::unique_lock lock{mutex};
    go = true;
    changed.notify_all();
    // Before the fix the worker ends here; with it, it waits for begin_page's lock.
    changed.wait_for(lock, 1s, [&] { return sent; });
  };
  const auto page = rig.session.begin_page();
  worker.join();
  require(data.staged == 1 && !data.armed && !rig.driver.quit,
          "A restore staged while a page begins is abandoned, and its quit is not delivered");
  require(page != rig.page, "The page changed");
}

void new_page() {
  Rig rig;
  rig.accepted(hold_x);
  rig.accepted(hold_y);
  rig.accepted(R"("type":"abort")");
  rig.accepted(release_x);
  rig.accepted(hold_x);
  rig.port.calls.clear();
  const auto old_page = rig.page;
  rig.page = rig.begin_page();
  rig.acted({"stop_jog x"}, "A new page stops the jogs the old page held");
  require(rig.handle(old_page, R"({"id":1000,"type":"jog_stop_all"})", at(0ms)).messages.empty(),
          "The old page is dropped");
  static_cast<void>(rig.tick(at(10s)));
  rig.acted({}, "No lease outlives its page");

  rig.next_id = 1;
  rig.expect(R"("type":"abort")", "rejected", gate_reason);
  const auto outcome = rig.send(R"("type":"ready")");
  require(outcome.send_snapshots && results(outcome).size() == 1,
          "The new page starts its IDs again and must send ready");
  rig.accepted(R"("type":"abort")");
  rig.acted({"abort"}, "The new page then works");
}

// Whatever arrives, and whatever the controller answers: a request with a new ID on the current,
// ready or not, page gets exactly one result with that ID, and every other request gets none.
// The history learns which panel command changed what, from the readings taken while it runs;
// Restore writes the earlier value back with the block of its kind and is refused where an
// edit would be.
void offset_history() {
  FakeDisk disk;
  FakeToolModelFiles files{disk};
  OffsetHistoryStore history{files, ""};
  Rig rig{true, Limits{3000, 1.2, 0.5, 1.2, 24000}, nullptr, &history};
  const auto machine = [](const Vec3& g54, std::vector<Tool> tools) -> PanelReading {
    PanelReading reading;
    reading.status.link = Link::live;
    reading.status.offsets.wcs = 1;
    reading.status.offsets.g5x = g54;
    reading.tool_table = {1, std::move(tools)};
    return reading;
  };
  const auto observe = [&](PanelReading reading) -> void {
    const auto now = at(rig.clock);
    history.observe(reading, now.unix_time, now.monotonic);
  };
  const auto newest = [&]() -> HistoryEntry { return history.snapshot()->entries.front(); };
  const auto end_mill = [](double length, double diameter) -> std::vector<Tool> {
    return {{3, 3, length, diameter, "End mill"}};
  };
  observe(machine({10, 20, -30}, end_mill(48.2, 12.7)));
  // The controller applies each block while the worker waits for it.
  const auto applies = [&](const std::string& action, const PanelReading& reading) -> void {
    rig.port.during[action] = [&observe, reading]() -> void { observe(reading); };
  };

  applies("mdi", machine({0, 20, -30}, end_mill(48.2, 12.7)));
  rig.accepted(R"("type":"touch_off","axis":"x","value":0,"origin":"zero")");
  rig.acted({"set_task_mode mdi", "mdi G10 L20 P0 X0"}, "Zero is a touch-off");
  const auto zero = newest();
  require(zero.source == HistorySource::zero && zero.number == 1 && zero.changes.size() == 1 &&
              zero.changes[0].before == 10 && zero.changes[0].after == 0,
          "Zero is named in the history");
  applies("mdi", machine({0, 10, -30}, end_mill(48.2, 12.7)));
  rig.accepted(R"("type":"touch_off","axis":"y","value":10,"origin":"half")");
  require(newest().source == HistorySource::half, "Half is named");
  applies("mdi", machine({0, 10, -25}, end_mill(48.2, 12.7)));
  rig.accepted(R"("type":"touch_off","axis":"z","value":5)");
  require(newest().source == HistorySource::touch_off, "A typed touch-off is named");
  applies("mdi", machine({0, 10, -20}, end_mill(48.2, 12.7)));
  rig.accepted(R"("type":"mdi","text":"G10 L2 P1 Z-20")");
  require(newest().source == HistorySource::mdi, "The panel's MDI line is named");
  rig.acted({"mdi G10 L20 P0 Y10", "mdi G10 L20 P0 Z5", "mdi G10 L2 P1 Z-20"},
            "The origin of a touch-off changes nothing that is sent");
  applies("set_tool", machine({0, 10, -20}, end_mill(50, 6)));
  rig.accepted(
      R"("type":"set_tool","tool":{"number":3,"pocket":3,"length":50,"diameter":6,"comment":"End mill"})");
  const auto edited = newest();
  require(edited.source == HistorySource::tool_table && edited.target == HistoryTarget::tool &&
              edited.number == 3 && edited.changes.size() == 2,
          "A tool table edit is named");
  rig.acted({"set_tool 3 3 50 6 End mill"}, "The edit itself");
  // Long after the last command, a change none of the panel's.
  rig.clock += 10s;
  observe(machine({0, 10, -21}, end_mill(50, 6)));
  require(newest().source == HistorySource::outside, "Anything else is outside the panel");

  applies("mdi", machine({10, 10, -21}, end_mill(50, 6)));
  rig.accepted(std::format(R"("type":"restore_offset","entry":{})", zero.id));
  rig.acted({"mdi G10 L2 P1 X10"}, "A work offset is restored with G10 L2, its fields only");
  const auto restored = newest();
  require(restored.source == HistorySource::restore && restored.restores == zero.id &&
              restored.changes[0].before == 0 && restored.changes[0].after == 10,
          "A restore is itself in the history, with the entry it restored");
  rig.port.state.program_units = LengthUnit::inch;
  rig.accepted(std::format(R"("type":"restore_offset","entry":{})", edited.id));
  rig.acted({"mdi G10 L1 P3 Z1.8976377952755907 R0.25"},
            "A tool is restored with G10 L1: length, and the radius of the diameter, in inches");
  rig.port.state.program_units = LengthUnit::mm;

  // A G92 shift is restored with G52, its fields only. G52 stores the offset in effect for an
  // axis it does not name, 0 while G92.2 suspends the shift: LinuxCNC is made to refuse the
  // block (a divisor of 0) when the shift is suspended and such an axis stores a value.
  auto shifted = machine({10, 10, -21}, end_mill(50, 6));
  shifted.status.offsets.g92 = {2, 9, 0};
  rig.clock += 10s;
  observe(shifted);
  const auto both = newest();
  shifted.status.offsets.g92 = {4, 9, 0};
  rig.clock += 10s;
  observe(shifted);
  const auto only_x = newest();
  require(both.target == HistoryTarget::g92 && both.changes.size() == 2 &&
              only_x.target == HistoryTarget::g92 && only_x.changes.size() == 1 &&
              only_x.changes[0].field == HistoryField::x,
          "G92 shifts are in the history");
  rig.accepted(std::format(R"("type":"restore_offset","entry":{})", only_x.id));
  rig.accepted(std::format(R"("type":"restore_offset","entry":{})", both.id));
  rig.acted({"mdi G52 X[2+0/[#5210+[1 AND [#5212 EQ 0] AND [#5213 EQ 0]"
             " AND [#5214 EQ 0] AND [#5215 EQ 0] AND [#5216 EQ 0]]]]",
             "mdi G52 X[0+0/[#5210+[1 AND [#5213 EQ 0]"
             " AND [#5214 EQ 0] AND [#5215 EQ 0] AND [#5216 EQ 0]]]] Y0"},
            "G52 names the restored axes; a suspended stored value elsewhere refuses it");

  // A work system that was never active: written, but with nothing to restore.
  rig.accepted(R"("type":"set_work_offset","wcs":2,"offset":[25.4,-50.8,0])");
  const auto inactive = newest();
  require(inactive.source == HistorySource::work_offset_table && inactive.number == 2 &&
              inactive.changes.size() == 3 && !inactive.changes[0].before,
          "An edit of an inactive system is recorded when it is written");
  rig.acted({"mdi G10 L2 P2 X25.4 Y-50.8 Z0"}, "The edit itself");
  rig.expect(std::format(R"("type":"restore_offset","entry":{})", inactive.id), "rejected",
             "This entry has no earlier value to restore.");
  rig.expect(R"("type":"restore_offset","entry":999)", "rejected",
             "That history entry is no longer kept.");
  rig.port.state.machine = MachineState::off;
  rig.expect(std::format(R"("type":"restore_offset","entry":{})", zero.id), "rejected",
             "Machine power is off or E-stop is active.");
  rig.port.state.machine = MachineState::on;
  rig.port.state.interp = InterpState::reading;
  rig.expect(std::format(R"("type":"restore_offset","entry":{})", zero.id), "rejected",
             "Wait for the interpreter and motion to become idle.");
  rig.acted({}, "A refused restore sends nothing");
  rig.port.state.interp = InterpState::idle;

  // The XY rotation from the offset editor: of an inactive system as written, of the active one
  // as the status shows it; restored with G10 L2 R.
  rig.accepted(R"("type":"set_work_rotation","wcs":4,"rotation_xy":30)");
  rig.acted({"mdi G10 L2 P4 R30"}, "The rotation of an inactive system");
  const auto rotated = newest();
  require(rotated.source == HistorySource::work_offset_table && rotated.number == 4 &&
              rotated.changes.size() == 1 && rotated.changes[0].field == HistoryField::rotation_xy &&
              !rotated.changes[0].before && rotated.changes[0].after == 30,
          "A rotation of an inactive system is recorded when it is written");
  rig.clock += 10s;
  auto turned = machine({10, 10, -21}, end_mill(50, 6));
  observe(turned);
  turned.status.offsets.rotation_xy = 15;
  applies("mdi", turned);
  rig.accepted(R"("type":"set_work_rotation","wcs":1,"rotation_xy":15)");
  rig.acted({"mdi G10 L2 P1 R15"}, "The rotation of the active system");
  const auto active_rotation = newest();
  require(active_rotation.source == HistorySource::work_offset_table &&
              active_rotation.number == 1 && active_rotation.changes.size() == 1 &&
              active_rotation.changes[0].before == 0 && active_rotation.changes[0].after == 15,
          "A rotation of the active system is recorded from the status");
  applies("mdi", machine({10, 10, -21}, end_mill(50, 6)));
  rig.accepted(std::format(R"("type":"restore_offset","entry":{})", active_rotation.id));
  rig.acted({"mdi G10 L2 P1 R0"}, "A rotation is restored with G10 L2 R");

  // The G92/G52 clear: one entry of the offset editor, undone with G52.
  rig.clock += 10s;
  auto shift = machine({10, 10, -21}, end_mill(50, 6));
  shift.status.offsets.g92 = {4, 9, 0};
  observe(shift);
  auto cleared = shift;
  cleared.status.offsets.g92 = {0, 0, 0};
  applies("mdi", cleared);
  rig.accepted(R"("type":"clear_g92")");
  rig.acted({"mdi G92.1"}, "G92.1 clears the shift");
  const auto clear = newest();
  require(clear.source == HistorySource::work_offset_table && clear.target == HistoryTarget::g92 &&
              clear.changes.size() == 2 && clear.changes[0].before == 4 &&
              clear.changes[0].after == 0 && clear.changes[1].before == 9,
          "A clear of the shift is recorded with what it cleared");
  applies("mdi", shift);
  rig.accepted(std::format(R"("type":"restore_offset","entry":{})", clear.id));
  rig.acted({"mdi G52 X[4+0/[#5210+[1 AND [#5213 EQ 0]"
             " AND [#5214 EQ 0] AND [#5215 EQ 0] AND [#5216 EQ 0]]]] Y9"},
            "and undone with G52");

  Rig plain;
  plain.expect(R"("type":"restore_offset","entry":1)", "rejected",
               "This CoffeeCNC keeps no offset history.");
}

// A restored length of the tool in the spindle is applied again in the same request when that
// tool's length is in effect (#287): G43 H<tool> after the G10 L1, through the same MDI path.
void tool_length_restore() {
  FakeDisk disk;
  FakeToolModelFiles files{disk};
  OffsetHistoryStore history{files, ""};
  Rig rig{true, Limits{3000, 1.2, 0.5, 1.2, 24000}, nullptr, &history};
  rig.port.state.mode = TaskMode::mdi;
  rig.port.state.tool_in_spindle = 3;
  const auto reading = [](double length) -> PanelReading {
    PanelReading seen;
    seen.status.link = Link::live;
    seen.status.offsets.wcs = 1;
    seen.tool_table = {1, {{3, 3, length, 6, "End mill"}}};
    return seen;
  };
  const auto observe = [&](PanelReading seen) -> void {
    const auto now = at(rig.clock);
    history.observe(seen, now.unix_time, now.monotonic);
  };
  observe(reading(48.2));
  rig.port.during["mdi"] = [&]() -> void { observe(reading(50)); };
  rig.accepted(R"("type":"mdi","text":"G10 L1 P3 Z50")");
  const auto entry = history.snapshot()->entries.front();
  require(entry.target == HistoryTarget::tool && entry.number == 3 &&
              entry.changes.front().field == HistoryField::length,
          "The length of the loaded tool is in the history");
  rig.acted({"mdi G10 L1 P3 Z50"}, "The panel's MDI line");
  const auto restore = std::format(R"("type":"restore_offset","entry":{})", entry.id);
  const auto in_effect = [&](double offset_z) -> void {
    rig.port.length_in_effect = ToolLengthInEffect{{0, 0, offset_z}, 50};
  };

  in_effect(50);
  rig.accepted(restore);
  rig.acted({"mdi G10 L1 P3 Z48.2", "mdi G43 H3"}, "G43 of the loaded tool is applied again");
  rig.port.state.program_units = LengthUnit::inch;
  rig.accepted(restore);
  rig.acted({"mdi G10 L1 P3 Z1.8976377952755907", "mdi G43 H3"},
            "In inches: the length in inches, the same G43 H3");
  rig.port.state.program_units = LengthUnit::mm;

  in_effect(0);
  rig.accepted(restore);
  rig.acted({"mdi G10 L1 P3 Z48.2"}, "No offset in effect: G10 L1 only");
  in_effect(40);
  rig.accepted(restore);
  rig.acted({"mdi G10 L1 P3 Z48.2"}, "The offset of another H stays: G10 L1 only");
  in_effect(90);
  rig.accepted(restore);
  rig.acted({"mdi G10 L1 P3 Z48.2"}, "A G43.2 sum stays: G10 L1 only");
  rig.port.length_in_effect = std::unexpected(std::string("Cannot read tool table tool.tbl."));
  rig.expect(restore, "rejected", "Cannot read tool table tool.tbl.");
  rig.acted({}, "An unreadable offset in effect sends nothing");
  rig.port.state.tool_in_spindle = 2;
  rig.accepted(restore);
  rig.acted({"mdi G10 L1 P3 Z48.2"}, "Another tool in the spindle: G10 L1 only");
  rig.port.state.tool_in_spindle = 3;

  // A stop request after the G10 L1: G43 is not sent, and the result says so.
  in_effect(50);
  rig.port.during["mdi"] = [&]() -> void {
    static_cast<void>(rig.session.submit(rig.page, R"({"id":900,"type":"abort"})", at(0ms)));
  };
  const auto found = results(rig.send(restore));
  require(found.size() == 2 && found.front().at("outcome") == "failed" &&
              found.front().at("reason") ==
                  "The tool length was set, but a stop request cancelled the command before it "
                  "was applied (G43 was not sent).",
          "A stop between the blocks: G43 was not sent");
  rig.acted({"mdi G10 L1 P3 Z48.2", "abort"}, "Only the G10 L1 and the abort are sent");
}

void exactly_one_result(std::uint32_t seed) {
  constexpr std::array<std::string_view, 30> bodies{
      R"("type":"ready")",
      R"("type":"set_machine","state":"on")",
      R"("type":"set_machine","state":"estop")",
      R"("type":"home","axis":"all")",
      R"("type":"jog","axis":"x","direction":-1,"velocity":1500,"held":true)",
      R"("type":"jog","axis":"x","direction":1,"velocity":1500,"held":true)",
      R"("type":"jog","axis":"y","direction":1,"velocity":300,"held":true)",
      R"("type":"jog","axis":"x","direction":1,"velocity":0,"held":false)",
      R"("type":"jog_step","axis":"z","distance":-0.1,"velocity":600)",
      R"("type":"jog_stop_all")",
      R"("type":"mdi","text":"G0 X10 Y-5.5")",
      R"("type":"list_directory","path":null)",
      R"("type":"open_program","path":"/home/cnc/nc_files/part.ngc")",
      R"("type":"run")",
      R"("type":"pause")",
      R"("type":"resume")",
      R"("type":"step")",
      R"("type":"abort")",
      R"("type":"set_override","which":"feed","value":0.85)",
      R"("type":"set_spindle","direction":"forward","rpm":6000)",
      R"("type":"set_coolant","flood":true,"mist":false)",
      R"("type":"set_program_option","option":"optional_stop","on":true)",
      R"("type":"select_wcs","wcs":2)",
      R"("type":"touch_off","axis":"y","value":0)",
      R"("type":"tool_touch_off","value":25.4)",
      R"("type":"set_tool","tool":{"number":3,"pocket":3,"length":48.2,"diameter":6,"comment":"End mill 6 mm"})",
      R"("type":"remove_tool","number":3)",
      R"("type":"tool_changed")",
      // Decodable ID, invalid command.
      R"("type":"warp")",
      R"("type":"jog","axis":"x","direction":2,"velocity":1,"held":true)",
  };
  constexpr std::array<std::string_view, 4> without_id{"", "{", R"({"type":"run"})",
                                                       R"({"id":1,"type":"run","id":2})"};
  std::mt19937 random{seed};
  const auto pick = [&random](std::size_t count) -> std::size_t { return random() % count; };

  Rig rig{false};
  rig.port.chaos = &random;
  auto stale = rig.page;
  std::optional<RequestId> last;
  bool ready = false;
  int answered = 0;
  int dropped = 0;
  for (int i = 0; i < 20000; ++i) {
    rig.clock += std::chrono::milliseconds{pick(80)};
    const auto now = at(rig.clock);
    auto& state = rig.port.state;
    switch (pick(12)) {
    case 0:
      state = homed_machine();
      break;
    case 1:
      state.link = std::array{Link::live, Link::live, Link::stale, Link::down}.at(pick(4));
      break;
    case 2:
      state.machine =
          std::array{MachineState::on, MachineState::on, MachineState::off, MachineState::estop}.at(
              pick(4));
      break;
    case 3:
      state.interp = std::array{InterpState::idle, InterpState::reading, InterpState::paused,
                                InterpState::waiting}
                         .at(pick(4));
      break;
    case 4:
      state.axes.at(pick(3)).homed = pick(2) == 0;
      state.requested_tool = pick(2) == 0 ? std::optional<int>{4} : std::nullopt;
      break;
    default:
      break;
    }
    rig.port.follows_mode = pick(10) != 0;

    const auto roll = pick(100);
    if (roll < 3) {
      stale = rig.page;
      rig.page = rig.begin_page();
      last.reset();
      ready = false;
      continue;
    }
    if (roll < 10) {
      require(results(rig.tick(now)).empty(), "Tick never produces a result");
      continue;
    }
    if (roll < 16) {
      const auto outcome = rig.handle(rig.page, without_id.at(pick(4)), now);
      require(results(outcome).empty() && outcome.messages.size() == 1 && !outcome.send_snapshots,
              "No result without an ID");
      continue;
    }
    RequestId id = last.value_or(0) + 1 + static_cast<RequestId>(pick(3));
    if (roll < 30 && last)
      id = *last - static_cast<RequestId>(pick(4));
    const auto body = bodies.at(pick(bodies.size()));
    const auto wire = std::format("{{\"id\":{},{}}}", id, body);
    if (roll >= 30 && roll < 36 && stale != rig.page) {
      const auto outcome = rig.handle(stale, wire, now);
      require(outcome.messages.empty() && !outcome.send_snapshots, "No result on a replaced page");
      continue;
    }
    const auto before = rig.port.calls.size();
    const auto outcome = rig.handle(rig.page, wire, now);
    const auto found = results(outcome);
    if (last && id <= *last) {
      require(outcome.messages.empty() && !outcome.send_snapshots &&
                  rig.port.calls.size() == before,
              "A duplicate or lower ID gets no result and no action: " + wire);
      ++dropped;
      continue;
    }
    last = id;
    require(found.size() == 1, "Exactly one result for: " + wire);
    const auto& result = found.front();
    const auto verdict = result.at("outcome").get<std::string>();
    const auto refusal = result.find("refusal");
    require(result.at("id") == id &&
                result.size() == 4U + result.count("refusal") + result.count("localization"),
            "The result carries the request's ID");
    if (refusal != result.end())
      require(verdict == "rejected" && refusal->at("key").is_string() && refusal->at("params").is_object(),
              "Only a rejected result can carry a refusal key and arguments");
    if (const auto entry = result.find("localization"); entry != result.end())
      require(verdict != "accepted" && entry->at("key").is_string() &&
                  entry->at("params").is_object(),
              "Only a non-accepted result can carry message localization");
    require(verdict == "accepted" || verdict == "rejected" || verdict == "failed" ||
                verdict == "timed_out",
            "The outcome is one of the protocol's four");
    require((verdict == "accepted") == result.at("reason").get<std::string>().empty(),
            "A reason is given for everything but accepted: " + result.dump());
    const bool is_ready = body == bodies.front();
    if (!ready && !is_ready && body != bodies.at(28) && body != bodies.at(29))
      require(verdict == "rejected" && result.at("reason") == gate_reason &&
                  rig.port.calls.size() == before,
              "Nothing runs before ready: " + wire);
    require(outcome.send_snapshots == (is_ready && verdict == "accepted"),
            "Only an accepted ready asks for the snapshots");
    ready = ready || outcome.send_snapshots;
    for (const auto& text : outcome.messages)
      if (const auto message = Json::parse(text); message.at("type") != "result")
        require((message.at("type") == "message" && message.at("level") == "error") ||
                    (message.at("type") == "directory" && body == bodies.at(11) &&
                     verdict == "accepted" && text == outcome.messages.front()),
                "Besides the result: operator errors, and the directory an accepted listing "
                "asked for, before its result");
    ++answered;
  }
  require(
      answered > 5000 && dropped > 500,
      std::format("The sequence exercised both paths: {} answered, {} dropped", answered, dropped));
}
// The program editor's save over the loaded program: refused while the program runs or is
// paused, only for the loaded program, only with the stamp of the file; then opened again.
void editor_saves_over_the_loaded_program() {
  constexpr auto part = "/home/cnc/nc_files/part.ngc";
  const std::string before = "G0 X1\nM2\n";
  FakePort port;
  port.state = homed_machine();
  port.state.program_file = part;
  FakeFileSystem files;
  files.file(part, before);
  files.file("/home/cnc/nc_files/other.ngc", "M2\n");
  const ProgramStore programs{files, program_directory};
  FakeDisk disk;
  disk.files[part] = before;
  FakeToolModelFiles writes{disk};
  ProgramWriter writer{programs, writes};
  const Limits limits{3000, 1.2, 0.5, 1.2, 24000};
  Session session{port, limits, programs, nullptr, nullptr, nullptr, &writer};
  Driver driver{session, port};
  const auto page = driver.begin_page();
  RequestId id = 0;
  const auto send = [&](const std::string& body) -> Json {
    const auto found =
        results(driver.handle(page, std::format("{{\"id\":{},{}}}", ++id, body), at(0ms)));
    require(found.size() == 1, "One result for: " + body);
    return found.empty() ? Json{} : found.front();
  };
  static_cast<void>(
      driver.handle(page, std::format("{{\"id\":{},\"type\":\"ready\"}}", ++id), at(0ms)));
  port.calls.clear();
  const auto save = [](std::string_view path, std::string_view stamp) -> std::string {
    return std::format(R"("type":"save_program","path":"{}","text":"G0 X2\nM2\n","stamp":"{}")",
                       path, stamp);
  };
  const auto stamp = program_stamp(before);

  for (const auto interp : {InterpState::reading, InterpState::paused}) {
    port.state.interp = interp;
    const auto refused = send(save(part, stamp));
    require(refused.at("outcome") == "rejected" &&
                refused.at("reason") == "Stop the program before saving it.",
            "Refused while the program runs or is paused: " + refused.dump());
  }
  port.state.interp = InterpState::idle;
  const auto other = send(save("/home/cnc/nc_files/other.ngc", program_stamp("M2\n")));
  require(other.at("outcome") == "rejected" &&
              other.at("reason") == "Only the loaded program can be edited.",
          "Only the loaded program is saved over: " + other.dump());
  const auto stale = send(save(part, program_stamp("G0 X0\nM2\n")));
  require(stale.at("outcome") == "failed" &&
              stale.at("reason") ==
                  "The program file changed since it was opened. Open it again to edit it.",
          "A file changed since it was opened is not written over: " + stale.dump());
  require(disk.files.at(part) == before && port.calls.empty(),
          "Nothing is written and nothing opened while refused");

  writes.during = [&](std::string_view step) {
    if (step == "sync")
      port.state.interp = InterpState::reading;
  };
  const auto started = send(save(part, stamp));
  require(started.at("outcome") == "failed" &&
              started.at("reason") == "Stop the program before saving it." &&
              disk.files.at(part) == before && port.calls.empty(),
          "A program that starts during the temporary-file sync prevents replacement: " + started.dump());
  writes.during = {};
  port.state.interp = InterpState::idle;

  const auto saved = send(save(part, stamp));
  require(saved.at("outcome") == "accepted", "The save is accepted: " + saved.dump());
  require(disk.files.at(part) == "G0 X2\nM2\n", "The edited text is on disk");
  require(port.calls == Calls{"open_program /home/cnc/nc_files/part.ngc"},
          "The program is opened again, so the preview and the program check follow");

  disk.files[part] = before;
  port.script["open_program"].push_back({ActionStatus::failed, "LinuxCNC refused the open."});
  const auto refused_reopen = send(save(part, stamp));
  require(refused_reopen.at("outcome") == "failed" &&
              refused_reopen.at("reason") ==
                  "Saved on disk, but not reopened. LinuxCNC refused the open." &&
              disk.files.at(part) == "G0 X2\nM2\n",
          "A reopen failure reports the disk save separately: " + refused_reopen.dump());
}

void uploads_do_not_replace_the_running_program() {
  constexpr auto part = "/home/cnc/nc_files/part.ngc";
  constexpr auto other = "/home/cnc/nc_files/other.ngc";
  FakePort port;
  port.state = homed_machine();
  port.state.program_file = part;
  FakeFileSystem files;
  files.file(part, "M2\n");
  const ProgramStore programs{files, program_directory};
  const Limits limits{3000, 1.2, 0.5, 1.2, 24000};
  Session session{port, limits, programs};
  const auto clock = [] { return at(0ms); };
  int published = 0;
  const auto publish = [&](std::string path) {
    auto queued = session.publish_program(std::move(path), [&]() -> std::expected<void, std::string> {
      ++published;
      return {};
    });
    require(session.work(clock).status == WorkStatus::done, "The worker takes up publication");
    return queued.get();
  };

  require(!publish(part), "An offline controller cannot verify a replacement");
  for (const auto interp : {InterpState::reading, InterpState::paused}) {
    port.state.interp = interp;
    session.observe(port.state, clock().monotonic);
    require(!publish(part), "The loaded file is protected during execution and pause");
  }
  port.state.interp = InterpState::idle;
  port.state.requested_tool = 2;
  session.observe(port.state, clock().monotonic);
  require(!publish(part) && published == 0, "The loaded file is protected while an M6 waits");
  require(publish(other).has_value() && port.calls.empty(), "Another file can be replaced without reopening during a run");
  port.state.requested_tool.reset();
  session.observe(port.state, clock().monotonic);
  require(publish(part).has_value() && published == 2 &&
              port.calls == Calls{std::string("open_program ") + part},
          "An idle replacement reopens the pathname in LinuxCNC");
  port.script["open_program"].push_back({ActionStatus::failed, "LinuxCNC refused the open."});
  const auto failed = publish(part);
  require(!failed && published == 3 &&
              failed.error() == "Saved on disk, but not reopened. LinuxCNC refused the open.",
          "A failed reopen reports the successful disk publication");
}

void usb_copy_does_not_hold_the_command_worker() {
  constexpr auto source = "/media/turo/USB/part.ngc";
  constexpr auto destination = "/home/cnc/nc_files/part.ngc";
  FakePort port;
  port.state = homed_machine();
  FakeFileSystem files;
  files.file(source, "M2\n");
  files.directory(program_directory);
  const ProgramStore programs{files, "/", program_directory};
  FakeDisk disk;
  disk.files[source] = "M2\n";
  FakeToolModelFiles writes{disk};
  ProgramWriter writer{programs, writes};
  const Limits limits{3000, 1.2, 0.5, 1.2, 24000};
  Session session{port, limits, programs, nullptr, nullptr, nullptr, nullptr, nullptr,
                  nullptr, true};
  Driver driver{session, port};
  const auto page = driver.begin_page();
  static_cast<void>(driver.handle(page, R"({"id":1,"type":"ready"})", at(0ms)));
  const auto pending = driver.handle(page,
      R"({"id":2,"type":"manage_program_file","operation":"copy","path":"/media/turo/USB/part.ngc"})", at(0ms));
  require(pending.messages.empty() && !disk.files.contains(destination),
          "The command worker returns while the copy waits for the disk worker");
  writes.during = [&](std::string_view step) {
    if (step == "sync") {
      const auto stopped = results(driver.handle(page, R"({"id":3,"type":"abort"})", at(0ms)));
      require(stopped.size() == 1 && stopped.front().at("outcome") == "accepted",
              "Stop can be handled while the copy synchronizes");
    }
  };
  const auto copied = session.copy_work(writer);
  require(copied && !copied->messages.empty() && disk.files.contains(destination),
          "The disk worker publishes the copy and returns its result");
}

} // namespace

// A failing run prints the seed of its random sequence; pass it as the only argument to repeat.
auto main(int argc, char** argv) -> int {
  std::uint32_t seed = 0;
  try {
    seed = argc > 1 ? static_cast<std::uint32_t>(std::stoul(argv[1])) : std::random_device{}();
    stale_pages_are_dropped();
    editor_saves_over_the_loaded_program();
    uploads_do_not_replace_the_running_program();
    usb_copy_does_not_hold_the_command_worker();
    controller_messages_wait_for_ready();
    panel_watchdog_uses_a_monotonic_page_clock();
    panel_watchdog_cancels_waiting_work();
    ready_gate();
    idle_maintenance_yields_to_an_operator_request();
    tool_model_commands_go_to_their_service();
    diagnostics_go_to_their_service();
    touch_probe_blocks_spindle_commands_before_dispatch();
    pending_probe_tool_changes();
    operator_settings_go_to_storage();
    ids_strictly_increase();
    decode_errors();
    policy_rejections_pass_through();
    machine_capabilities();
    mode_switching();
    outcome_mapping();
    every_action();
    directory_listing();
    program_paths();
    composed_mdi();
    offset_history();
    tool_length_restore();
    jog_presses_and_refreshes();
    jog_failures();
    late_jog_starts();
    stop_all_and_cancellation();
    lease_expiry();
    lost_machine_cancels_jogs();
    quit();
    restore_quit_not_delivered();
    restore_staged_while_page_begins();
    new_page();
    failed_stop_survives_the_page_and_is_retried();
    explicit_stop_failures_are_retried();
    no_start_over_an_unconfirmed_stop();
    outside_changes_stop_held_jogs();
    stopped_jogs_need_a_release();
    stale_requests_send_nothing();
    stop_distinguishes_queued_requests_after_mode_change();
    port_is_told_when_a_request_goes_stale();
    increments_are_tracked();
    home_after_increment();
    close_holds_the_level();
    end_of_page();
    exactly_one_result(seed);
    std::cout << checks << " session lifecycle checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Check " << checks << " failed: " << error.what() << " (seed " << seed << ")\n";
    return 1;
  }
}
