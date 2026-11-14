#include <coffeecnc/app/session.hpp>

#include "fake_file_system.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <format>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
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
  // Handed out by the next take_messages.
  std::vector<ControllerMessage> published;

  auto snapshot() -> ControllerState override {
    ++snapshots;
    if (next_states.empty())
      return state;
    auto next = next_states.front();
    next_states.pop_front();
    return next;
  }
  auto take_messages() -> std::vector<ControllerMessage> override {
    return std::exchange(published, {});
  }
  auto set_machine_state(MachineState machine) -> ActionResult override {
    return record("set_machine_state", machine == MachineState::on    ? "on"
                                       : machine == MachineState::off ? "off"
                                                                      : "estop");
  }
  auto set_task_mode(TaskMode mode) -> ActionResult override {
    auto result = record("set_task_mode", name(mode));
    if (result.status == ActionStatus::accepted && follows_mode)
      state.mode = mode;
    return result;
  }
  auto home(std::optional<Axis> axis) -> ActionResult override {
    return record("home", axis ? name(*axis) : "all");
  }
  auto start_jog(Axis axis, int direction, double velocity, JogSpace space)
      -> ActionResult override {
    return record("start_jog",
                  std::format("{} {} {} {}", name(axis), direction, velocity, name(space)));
  }
  auto stop_jog(Axis axis) -> ActionResult override { return record("stop_jog", name(axis)); }
  auto jog_increment(Axis axis, double distance, double velocity, JogSpace space)
      -> ActionResult override {
    return record("jog_increment",
                  std::format("{} {} {} {}", name(axis), distance, velocity, name(space)));
  }
  auto mdi(std::string_view text) -> ActionResult override { return record("mdi", text); }
  auto open_program(std::string_view path) -> ActionResult override {
    return record("open_program", path);
  }
  auto run() -> ActionResult override { return record("run", ""); }
  auto pause() -> ActionResult override { return record("pause", ""); }
  auto resume() -> ActionResult override { return record("resume", ""); }
  auto step() -> ActionResult override { return record("step", ""); }
  auto abort() -> ActionResult override { return record("abort", ""); }
  auto set_override(Override which, double value) -> ActionResult override {
    return record("set_override", std::format("{} {}",
                                              which == Override::feed    ? "feed"
                                              : which == Override::rapid ? "rapid"
                                                                         : "spindle",
                                              value));
  }
  auto set_spindle(SpindleDirection direction, double rpm) -> ActionResult override {
    return record("set_spindle", std::format("{} {}",
                                             direction == SpindleDirection::forward   ? "forward"
                                             : direction == SpindleDirection::reverse ? "reverse"
                                                                                      : "stopped",
                                             rpm));
  }
  auto set_coolant(bool flood, bool mist) -> ActionResult override {
    return record("set_coolant", std::format("{} {}", flood, mist));
  }
  auto set_program_option(ProgramOption option, bool on) -> ActionResult override {
    return record(
        "set_program_option",
        std::format("{} {}",
                    option == ProgramOption::optional_stop ? "optional_stop" : "block_delete", on));
  }
  auto tool_changed(int tool) -> ActionResult override {
    return record("tool_changed", std::to_string(tool));
  }
  auto set_tool(const Tool& tool) -> ActionResult override {
    return record("set_tool", std::format("{} {} {} {} {}", tool.number, tool.pocket, tool.length,
                                          tool.diameter, tool.comment));
  }
  auto remove_tool(int number) -> ActionResult override {
    return record("remove_tool", std::to_string(number));
  }

private:
  auto record(const std::string& action, std::string_view detail) -> ActionResult {
    calls.push_back(detail.empty() ? action : action + ' ' + std::string(detail));
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
  // Messages of work done outside a request or period (page changes): sent with the next.
  std::vector<std::string> backlog;

  void control() { level = session.control(now, {level, true}); }

  void drain(Outcome& outcome) {
    for (int steps = 0;; ++steps) {
      if (steps > 1000)
        throw std::runtime_error("The worker does not come to rest");
      auto work = session.work([this]() -> Instant { return now; });
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
    for (auto& message : outcome.messages)
      backlog.push_back(std::move(message));
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
    outcome.messages = std::exchange(backlog, {});
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

  explicit Rig(bool ready = true, const Limits& limits = Limits{3000, 1.2, 0.5, 1.2, 24000})
      : session(port, limits, programs), page(driver.begin_page()) {
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
    require(result.at("type") == "result" && result.at("id") == id && result.size() == 4,
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

void ready_gate() {
  Rig rig{false};
  rig.expect(R"("type":"run")", "rejected", gate_reason);
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
                message.at("time") == (epoch + 250ms).count() && message.size() == 4 &&
                message.at("text").get<std::string>().starts_with("Panel request ignored: "),
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
  rig.expect(R"("type":"jog_step","axis":"z","distance":1,"velocity":900)", "rejected",
             "Jog velocity is above the Z axis limit of 750 mm/min.");
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
  rig.expect(R"("type":"run")", "timed_out", "LinuxCNC did not answer.");
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
      {changing, {R"("type":"tool_changed")", {"tool_changed 4"}}},
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
                                                     {"truncated", false}},
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

  rig.accepted(R"("type":"jog","axis":"x","direction":-1,"velocity":600,"held":true)");
  rig.acted({"start_jog x -1 600 axis"}, "A changed direction is a new press");
  rig.accepted(R"("type":"jog","axis":"x","direction":-1,"velocity":900,"held":true)");
  rig.acted({"start_jog x -1 900 axis"}, "A changed velocity is a new press");
  rig.accepted(R"("type":"jog","axis":"x","direction":-1,"velocity":900,"held":true)");
  rig.accepted(hold_y);
  rig.acted({"start_jog y -1 600 axis"}, "Axes are held independently");

  rig.accepted(R"("type":"jog","axis":"x","direction":-1,"velocity":900,"held":false)");
  // The jog-stop level that every stop raises stops all axes: the session stops them all.
  rig.acted({"stop_jog x", "stop_jog y"}, "Release stops the axis, and with it every jog");
  rig.clock += 100ms;
  rig.accepted(hold_y);
  rig.acted({"start_jog y -1 600 axis"}, "The next refresh of another axis is a new press");
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
  rig.accepted(hold_x);
  rig.acted({"start_jog x 1 600 axis"}, "No lease was recorded for the failed start");

  rig.port.script["start_jog"] = {{ActionStatus::timed_out, ""}};
  rig.expect(R"("type":"jog","axis":"x","direction":-1,"velocity":600,"held":true)", "timed_out",
             "LinuxCNC did not answer.");
  rig.acted({"start_jog x -1 600 axis", "stop_jog x"},
            "A lost change of direction does not leave the old jog running");
  rig.clock += 1s;
  static_cast<void>(rig.tick(at(rig.clock)));
  rig.acted({}, "Its lease is gone");

  rig.accepted(hold_x);
  rig.port.calls.clear();
  rig.port.state.axes[0].machine = 1000;
  rig.expect(hold_x, "rejected", "Jog points beyond the configured travel.");
  rig.acted({"stop_jog x"}, "A refresh that is no longer valid stops its jog at once");
  rig.expect(hold_x, "rejected", "Jog points beyond the configured travel.");
  rig.acted({}, "A rejected press with no jog held sends nothing");

  rig.port.state = homed_machine();
  rig.port.state.mode = TaskMode::mdi;
  rig.accepted(hold_x);
  rig.acted({"set_task_mode manual", "start_jog x 1 600 axis"}, "A press switches to manual first");
  rig.port.state.mode = TaskMode::mdi; // changed behind the session's back
  rig.accepted(hold_x);
  rig.acted({"stop_jog x", "set_task_mode manual", "start_jog x 1 600 axis"},
            "A refresh that needs a mode change is a new press");
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

  rig.accepted(hold_x);
  rig.port.calls.clear();
  rig.accepted(R"("type":"abort")");
  rig.acted({"abort", "stop_jog x"}, "Abort goes out first, then held jogs are stopped");
  rig.accepted(hold_x);
  rig.accepted(hold_y);
  rig.port.calls.clear();
  rig.accepted(R"("type":"set_machine","state":"estop")");
  rig.acted({"set_machine_state estop", "stop_jog x", "stop_jog y"},
            "E-stop goes out first, then held jogs are stopped");
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
  rig.accepted(hold_x);
  rig.clock = 11s + 140ms;
  rig.accepted(hold_x);
  rig.port.calls.clear();
  static_cast<void>(rig.tick(at(11s + 289ms)));
  rig.acted({}, "The lease runs from the last refresh");
  static_cast<void>(rig.tick(at(11s + 290ms)));
  rig.acted({"stop_jog x"}, "And ends 150 ms after it");

  rig.clock = 20s;
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
    rig.accepted(R"("type":"jog","axis":"z","direction":1,"velocity":600,"held":true)");
    rig.acted({"start_jog z 1 600 axis"}, "The next press starts again");
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
  require(outcome.messages.size() == 1 &&
              outcome.messages.front().find("Jog stop failed on X") != std::string::npos,
          "A stop that failed at the page boundary is reported to the new page");
  rig.acted({"stop_jog x"}, "The tick retries the failed stop without a lease");
  outcome = rig.tick(at(2ms));
  require(outcome.messages.empty(), "A retry that fails again does not repeat the message");
  rig.acted({"stop_jog x"}, "It keeps retrying");
  outcome = rig.tick(at(3ms));
  require(outcome.messages.empty(), "Nothing to report once the stop is accepted");
  rig.acted({}, "An accepted stop ends the retries");
}

void new_page() {
  Rig rig;
  rig.accepted(hold_x);
  rig.accepted(hold_y);
  rig.accepted(R"("type":"abort")");
  rig.accepted(hold_x);
  rig.port.calls.clear();
  const auto old_page = rig.page;
  rig.page = rig.begin_page();
  rig.acted({"stop_jog x"}, "A new page stops the jogs the old page held");
  require(rig.handle(old_page, R"({"id":1000,"type":"jog_stop_all"})", at(0ms))
              .messages.empty(),
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
    require(result.at("id") == id && result.size() == 4, "The result carries the request's ID");
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
} // namespace

// A failing run prints the seed of its random sequence; pass it as the only argument to repeat.
auto main(int argc, char** argv) -> int {
  std::uint32_t seed = 0;
  try {
    seed = argc > 1 ? static_cast<std::uint32_t>(std::stoul(argv[1])) : std::random_device{}();
    stale_pages_are_dropped();
    ready_gate();
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
    jog_presses_and_refreshes();
    jog_failures();
    stop_all_and_cancellation();
    lease_expiry();
    lost_machine_cancels_jogs();
    new_page();
    failed_stop_survives_the_page_and_is_retried();
    explicit_stop_failures_are_retried();
    no_start_over_an_unconfirmed_stop();
    outside_changes_stop_held_jogs();
    end_of_page();
    exactly_one_result(seed);
    std::cout << checks << " session lifecycle checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Check " << checks << " failed: " << error.what() << " (seed " << seed << ")\n";
    return 1;
  }
}
