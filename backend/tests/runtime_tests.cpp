// The session's steps taken one at a time with a port call that really blocks, and the runtime on
// its real threads, both against fakes: no LinuxCNC. Time in the stepped tests is the test's own.

#include <coffeecnc/app/runtime.hpp>
#include <coffeecnc/app/session.hpp>

#include "fake_file_system.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <format>
#include <functional>
#include <future>
#include <iostream>
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

using namespace coffeecnc;
using namespace std::chrono_literals;

namespace {
using Json = nlohmann::json;
using Calls = std::vector<std::string>;
using Clock = std::chrono::steady_clock;

int checks = 0;
void require(bool condition, const std::string& message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

constexpr auto epoch = 1700000000000ms;
constexpr auto busy_reason = "Too many requests are waiting for LinuxCNC.";
constexpr auto cancelled_reason = "Cancelled by a stop request before it was sent.";
constexpr auto stopped_reason = "The jog was stopped before it started.";
constexpr auto hold_x = R"("type":"jog","axis":"x","direction":1,"velocity":600,"held":true)";
constexpr auto hold_y = R"("type":"jog","axis":"y","direction":-1,"velocity":600,"held":true)";
constexpr auto release_x = R"("type":"jog","axis":"x","direction":1,"velocity":600,"held":false)";
constexpr auto slow = R"("type":"set_coolant","flood":true,"mist":false)";

auto at(std::chrono::milliseconds elapsed) -> Instant {
  return {JogLease::Time{} + elapsed, epoch + elapsed};
}

auto name(Axis axis) -> std::string_view {
  return std::array<std::string_view, 3>{"x", "y", "z"}.at(axis_index(axis).value_or(0));
}

auto homed_machine() -> ControllerState {
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

auto listed(const Calls& calls) -> std::string {
  std::string all;
  for (const auto& call : calls)
    all += "[" + call + "]";
  return all;
}

// Records every action; one named action can be made to block inside the port until released,
// as NmlPort does while LinuxCNC's task does not answer. Safe to use from the worker and the
// test at once.
class BlockingPort final : public ControllerPort, public StatusSource {
public:
  void set(const ControllerState& state) {
    const std::scoped_lock lock{mutex_};
    state_ = state;
  }
  void change(const std::function<void(ControllerState&)>& edit) {
    const std::scoped_lock lock{mutex_};
    edit(state_);
  }
  void script(const std::string& action, std::deque<ActionResult> results) {
    const std::scoped_lock lock{mutex_};
    script_[action] = std::move(results);
  }
  // The next call of this action stays inside the port until release().
  void block(const std::string& action) {
    const std::scoped_lock lock{mutex_};
    block_on_ = action;
  }
  // Returns once a call is inside the port.
  void wait_blocked() {
    std::unique_lock lock{mutex_};
    if (!changed_.wait_for(lock, 20s, [&]() -> bool { return blocked_; }))
      throw std::runtime_error("No call blocked in the port");
  }
  void release() {
    const std::scoped_lock lock{mutex_};
    released_ = true;
    changed_.notify_all();
  }
  auto calls() -> Calls {
    const std::scoped_lock lock{mutex_};
    return calls_;
  }
  auto take_calls() -> Calls {
    const std::scoped_lock lock{mutex_};
    return std::exchange(calls_, {});
  }
  // The calls since the last look are exactly these.
  void expect(const Calls& expected, const std::string& message) {
    const auto got = take_calls();
    require(got == expected, message + ": " + listed(got));
  }
  auto called(std::string_view action) -> bool {
    const std::scoped_lock lock{mutex_};
    return std::ranges::any_of(
        calls_, [&](const std::string& call) -> bool { return call.starts_with(action); });
  }
  auto wait_called(std::string_view action) -> bool {
    for (int i = 0; i < 4000 && !called(action); ++i)
      std::this_thread::sleep_for(5ms);
    return called(action);
  }

  auto read() -> ControllerState override { return snapshot(); }
  auto snapshot() -> ControllerState override {
    const std::scoped_lock lock{mutex_};
    return state_;
  }
  auto take_messages() -> std::vector<ControllerMessage> override { return {}; }
  auto set_machine_state(MachineState machine, const Wanted&) -> ActionResult override {
    return record("set_machine_state", machine == MachineState::on    ? "on"
                                       : machine == MachineState::off ? "off"
                                                                      : "estop");
  }
  auto power_off() -> ActionResult override { return record("power_off", ""); }
  auto set_task_mode(TaskMode mode, const Wanted&) -> ActionResult override {
    auto result = record("set_task_mode", mode == TaskMode::manual ? "manual"
                                          : mode == TaskMode::mdi  ? "mdi"
                                                                   : "auto");
    const std::scoped_lock lock{mutex_};
    if (result.status == ActionStatus::accepted)
      state_.mode = mode;
    return result;
  }
  auto home(std::optional<Axis> axis, const Wanted&) -> ActionResult override {
    return record("home", axis ? name(*axis) : "all");
  }
  auto start_jog(Axis axis, int direction, double velocity, JogSpace, const Wanted&)
      -> ActionResult override {
    return record("start_jog", std::format("{} {} {}", name(axis), direction, velocity));
  }
  auto stop_jog(Axis axis) -> ActionResult override { return record("stop_jog", name(axis)); }
  auto jog_increment(Axis axis, double distance, double velocity, JogSpace, const Wanted&)
      -> ActionResult override {
    return record("jog_increment", std::format("{} {} {}", name(axis), distance, velocity));
  }
  auto mdi(std::string_view text, const Wanted&) -> ActionResult override {
    return record("mdi", text);
  }
  auto open_program(std::string_view path, const Wanted&) -> ActionResult override {
    return record("open_program", path);
  }
  auto run(const Wanted&) -> ActionResult override { return record("run", ""); }
  auto pause(const Wanted&) -> ActionResult override { return record("pause", ""); }
  auto resume(const Wanted&) -> ActionResult override { return record("resume", ""); }
  auto step(const Wanted&) -> ActionResult override { return record("step", ""); }
  auto abort() -> ActionResult override { return record("abort", ""); }
  auto set_override(Override, double value, const Wanted&) -> ActionResult override {
    return record("set_override", std::format("{}", value));
  }
  auto set_spindle(SpindleDirection, double rpm, const Wanted&) -> ActionResult override {
    return record("set_spindle", std::format("{}", rpm));
  }
  auto set_coolant(bool flood, bool mist, const Wanted&) -> ActionResult override {
    return record("set_coolant", std::format("{} {}", flood, mist));
  }
  auto set_program_option(ProgramOption, bool on, const Wanted&) -> ActionResult override {
    return record("set_program_option", std::format("{}", on));
  }
  auto tool_changed(int tool, std::uint64_t request, const Wanted&) -> ActionResult override {
    return record("tool_changed", std::format("{} {}", tool, request));
  }
  auto set_tool(const Tool& tool, const Wanted&) -> ActionResult override {
    return record("set_tool", std::to_string(tool.number));
  }
  auto remove_tool(int number, const Wanted&) -> ActionResult override {
    return record("remove_tool", std::to_string(number));
  }

private:
  auto record(const std::string& action, std::string_view detail) -> ActionResult {
    std::unique_lock lock{mutex_};
    calls_.push_back(detail.empty() ? action : action + ' ' + std::string(detail));
    if (action == block_on_) {
      block_on_.clear();
      blocked_ = true;
      changed_.notify_all();
      changed_.wait(lock, [&]() -> bool { return released_; });
      released_ = false;
      blocked_ = false;
    }
    if (auto& scripted = script_[action]; !scripted.empty()) {
      auto result = scripted.front();
      scripted.pop_front();
      return result;
    }
    return {};
  }

  std::mutex mutex_;
  std::condition_variable changed_;
  ControllerState state_{homed_machine()};
  Calls calls_;
  std::map<std::string, std::deque<ActionResult>> script_;
  std::string block_on_;
  bool blocked_{};
  bool released_{};
};

// The heartbeat and the jog-stop level, with the times the runtime tests ask about.
class FakeOutputs final : public SafetyOutputs {
public:
  void beat() override {
    const auto now = Clock::now().time_since_epoch().count();
    if (const auto last = last_beat_.exchange(now); last != 0)
      longest_gap_ = std::max(longest_gap_.load(), now - last);
    ++beats_;
  }
  void hold_jog_stop(bool on) override {
    if (on && !level_)
      raised_at_ = Clock::now().time_since_epoch().count();
    if (!on && level_)
      ++lowered_;
    level_ = on;
  }
  [[nodiscard]] auto jog_stop_high() const -> bool override { return level_; }
  [[nodiscard]] auto watchdog_ok() const -> bool override { return watchdog_; }

  [[nodiscard]] auto beats() const -> std::uint64_t { return beats_; }
  // How long ago the last beat was.
  [[nodiscard]] auto since_last_beat() const -> Clock::duration {
    return Clock::now() - Clock::time_point{Clock::duration{last_beat_.load()}};
  }
  // How often the level fell.
  [[nodiscard]] auto lowered() const -> std::uint64_t { return lowered_; }
  [[nodiscard]] auto longest_gap() const -> std::chrono::nanoseconds {
    return std::chrono::nanoseconds{longest_gap_.load()};
  }
  [[nodiscard]] auto raised_at() const -> Clock::time_point {
    return Clock::time_point{Clock::duration{raised_at_.load()}};
  }
  void trip_watchdog() { watchdog_ = false; }
  auto wait_level(bool level, std::chrono::milliseconds timeout) const -> bool {
    for (const auto end = Clock::now() + timeout; Clock::now() < end;) {
      if (level_ == level)
        return true;
      std::this_thread::sleep_for(200us);
    }
    return level_ == level;
  }

private:
  std::atomic<bool> level_{};
  std::atomic<bool> watchdog_{true};
  std::atomic<std::uint64_t> beats_{};
  std::atomic<std::uint64_t> lowered_{};
  std::atomic<Clock::rep> last_beat_{};
  std::atomic<Clock::rep> longest_gap_{};
  std::atomic<Clock::rep> raised_at_{};
};

// Everything that came out for the pages, from any thread.
class Delivered {
public:
  void add(const Outcome& outcome) {
    const std::scoped_lock lock{mutex_};
    for (const auto& text : outcome.messages) {
      messages_.emplace_back(outcome.page, Json::parse(text));
      if (const auto& message = messages_.back().second; message.at("type") == "result") {
        const auto id = message.at("id").get<RequestId>();
        results_.emplace(std::pair{outcome.page.value, id}, messages_.size() - 1);
        ++counts_[id];
      }
    }
    changed_.notify_all();
  }
  // The result of the request, if one was delivered to that page.
  auto result(PageToken page, RequestId id) -> std::optional<Json> {
    const std::scoped_lock lock{mutex_};
    return find(page, id);
  }
  auto wait_result(PageToken page, RequestId id) -> Json {
    std::unique_lock lock{mutex_};
    std::optional<Json> found;
    changed_.wait_for(lock, 20s, [&]() -> bool {
      found = find(page, id);
      return found.has_value();
    });
    if (!found)
      throw std::runtime_error(std::format("No result for request {}", id));
    return *found;
  }
  // Results with this ID, whatever page they went to.
  auto results(RequestId id) -> std::size_t {
    const std::scoped_lock lock{mutex_};
    const auto found = counts_.find(id);
    return found == counts_.end() ? 0 : found->second;
  }
  auto all() -> std::vector<std::pair<PageToken, Json>> {
    const std::scoped_lock lock{mutex_};
    return messages_;
  }
  auto has_text(std::string_view part) -> bool {
    const std::scoped_lock lock{mutex_};
    return std::ranges::any_of(messages_, [&](const auto& m) -> bool {
      return m.second.at("type") == "message" &&
             m.second.at("text").template get<std::string>().find(part) != std::string::npos;
    });
  }

private:
  // The first result of the request delivered to that page.
  auto find(PageToken page, RequestId id) -> std::optional<Json> {
    const auto found = results_.find({page.value, id});
    return found == results_.end() ? std::nullopt
                                   : std::optional<Json>{messages_.at(found->second).second};
  }
  std::mutex mutex_;
  std::condition_variable changed_;
  std::vector<std::pair<PageToken, Json>> messages_;
  std::map<std::pair<std::uint64_t, RequestId>, std::size_t> results_;
  std::map<RequestId, std::size_t> counts_;
};

// A session with one ready page on a homed machine. The test is the shell, the status thread and
// the control thread; the worker is the test too, or a thread of its own for a call that blocks.
struct Bench {
  BlockingPort port;
  FakeFileSystem files;
  ProgramStore programs{files, "/home/cnc/nc_files"};
  Session session{port, Limits{3000, 1.2, 0.5, 1.2, 24000}, programs};
  FakeOutputs outputs;
  Delivered delivered;
  PageToken page;
  RequestId next_id{1};
  std::atomic<std::chrono::milliseconds> clock{0ms};

  Bench() : page(session.begin_page()) { static_cast<void>(send(R"("type":"ready")")); }

  // Queues or answers the request; returns its ID.
  auto send(std::string_view body) -> RequestId {
    const auto id = next_id++;
    session.observe(port.snapshot(), at(clock).monotonic);
    delivered.add(
        session.submit(page, std::format("{{\"id\":{},{}}}", id, body), at(clock.load())));
    return id;
  }
  // One control period at the bench's time, on the state the port shows unless `observe` is off.
  auto control(bool observe = true) -> bool {
    if (observe)
      session.observe(port.snapshot(), at(clock).monotonic);
    return control_step(session, outputs, at(clock.load()));
  }
  auto work() -> WorkStatus {
    const auto work = session.work([this]() -> Instant { return at(clock.load()); });
    delivered.add(work.outcome);
    return work.status;
  }
  // The worker until it has nothing left; control periods where it waits for the level.
  void drain() {
    for (int steps = 0;; ++steps) {
      if (steps > 1000)
        throw std::runtime_error("The worker does not come to rest");
      const auto status = work();
      if (status == WorkStatus::idle)
        return;
      if (status == WorkStatus::blocked)
        static_cast<void>(control());
    }
  }
  // The worker on a thread of its own, until it has nothing left or waits for the level.
  auto worker() -> std::future<WorkStatus> {
    return std::async(std::launch::async, [this]() -> WorkStatus {
      auto status = WorkStatus::done;
      while (status == WorkStatus::done)
        status = work();
      return status;
    });
  }
  auto outcome(RequestId id) -> std::string {
    const auto result = delivered.result(page, id);
    return result ? result->at("outcome").get<std::string>() + ":" +
                        result->at("reason").get<std::string>()
                  : std::string("none");
  }
  // A jog on X that the controller has accepted.
  void jog_x() {
    const auto id = send(hold_x);
    drain();
    require(outcome(id) == "accepted:", "The jog starts: " + outcome(id));
    static_cast<void>(port.take_calls());
  }
  // A request that stays inside the port, with the worker in it.
  auto block_worker() -> std::future<WorkStatus> {
    port.block("set_coolant");
    static_cast<void>(send(slow));
    auto working = worker();
    port.wait_blocked();
    return working;
  }
};

// Property 3: every stop decision raises the level in the next control period while the worker
// is inside a port call, and the NML stop follows when the worker is free.
void dead_man_does_not_wait_for_the_worker() {
  struct Cause {
    std::string name;
    std::function<void(Bench&)> happen;
    bool observe{true};
  };
  const std::vector<Cause> causes{
      {"lease expiry", [](Bench& bench) -> void { bench.clock = 150ms; }},
      {"release", [](Bench& bench) -> void { static_cast<void>(bench.send(release_x)); }},
      {"stop all",
       [](Bench& bench) -> void { static_cast<void>(bench.send(R"("type":"jog_stop_all")")); }},
      {"abort", [](Bench& bench) -> void { static_cast<void>(bench.send(R"("type":"abort")")); }},
      {"E-stop",
       [](Bench& bench) -> void {
         static_cast<void>(bench.send(R"("type":"set_machine","state":"estop")"));
       }},
      {"page end", [](Bench& bench) -> void { bench.session.end_page(bench.page); }},
      {"page reload", [](Bench& bench) -> void { bench.page = bench.session.begin_page(); }},
      {"link loss",
       [](Bench& bench) -> void {
         bench.port.change([](ControllerState& state) -> void { state.link = Link::stale; });
       }},
      {"machine off",
       [](Bench& bench) -> void {
         bench.port.change(
             [](ControllerState& state) -> void { state.machine = MachineState::off; });
       }},
      {"mode changed outside",
       [](Bench& bench) -> void {
         bench.port.change([](ControllerState& state) -> void { state.mode = TaskMode::mdi; });
       }},
      {"watchdog tripped", [](Bench& bench) -> void { bench.outputs.trip_watchdog(); }},
      // Nothing observed for longer than Session::status_max_age, the lease refreshed meanwhile.
      {"status too old",
       [](Bench& bench) -> void {
         bench.clock = 90ms;
         static_cast<void>(bench.send(hold_x));
         bench.clock = 191ms;
       },
       false},
  };
  for (const auto& cause : causes) {
    Bench bench;
    bench.jog_x();
    auto working = bench.block_worker();
    bench.clock = 149ms;
    require(!bench.control() && !bench.outputs.jog_stop_high(),
            "Held and nothing decided: the level is low before " + cause.name);
    const auto beats = bench.outputs.beats();
    if (cause.name != "lease expiry" && cause.name != "status too old")
      bench.clock = 10ms;
    cause.happen(bench);
    require(bench.control(cause.observe) && bench.outputs.jog_stop_high(),
            "The level is high one control period after " + cause.name);
    require(bench.outputs.beats() == beats + 1, "The heartbeat goes on: " + cause.name);
    require(!bench.port.called("stop_jog"),
            "The worker is still inside its call, no NML stop was sent: " + cause.name);
    for (int period = 0; period < 5; ++period)
      require(bench.control(cause.observe), "The level stays high while the stop is not sent");
    bench.port.release();
    require(working.get() == WorkStatus::idle, "The worker finishes: " + cause.name);
    require(bench.port.called("stop_jog x"), "The NML stop follows the call: " + cause.name);
    require(!bench.control(cause.observe), "Stop accepted: the level is released, " + cause.name);
    require(!bench.outputs.jog_stop_high(), "And low: " + cause.name);
  }
}

// Property 6: a refresh is answered in submit and keeps the lease, whatever the worker does.
void refresh_does_not_wait_for_the_worker() {
  Bench bench;
  bench.jog_x();
  auto working = bench.block_worker();
  for (int i = 1; i <= 20; ++i) {
    bench.clock = i * 100ms;
    const auto id = bench.send(hold_x);
    require(bench.outcome(id) == "accepted:", "A refresh is answered at once");
    bench.clock = i * 100ms + 99ms;
    require(!bench.control(), "A refreshed lease does not expire behind a blocked worker");
  }
  bench.port.expect(Calls{"set_coolant true false"}, "No refresh reached the port or the queue");
  bench.clock = 2s + 150ms;
  require(bench.control(), "Without a refresh the lease expires as ever");
  bench.port.release();
  static_cast<void>(working.get());
}

// Property 4: what is queued for a page, or for a jog that was stopped, is never sent.
void stale_actions_are_dropped_before_sending() {
  {
    Bench bench;
    auto working = bench.block_worker();
    const auto press = bench.send(hold_y);
    const auto move = bench.send(R"("type":"mdi","text":"G0 X1")");
    const auto step = bench.send(R"("type":"jog_step","axis":"z","distance":1,"velocity":600)");
    bench.session.end_page(bench.page);
    bench.port.release();
    static_cast<void>(working.get());
    bench.drain();
    bench.port.expect(Calls{"set_coolant true false"},
                      "Nothing queued by a page that ended is sent");
    require(bench.delivered.results(press) + bench.delivered.results(move) +
                    bench.delivered.results(step) ==
                0,
            "And none of it is answered");
  }
  {
    Bench bench;
    auto working = bench.block_worker();
    const auto press = bench.send(hold_y);
    const auto step = bench.send(R"("type":"jog_step","axis":"z","distance":1,"velocity":600)");
    const auto release =
        bench.send(R"("type":"jog","axis":"y","direction":-1,"velocity":600,"held":false)");
    bench.port.release();
    static_cast<void>(working.get());
    bench.drain();
    bench.port.expect(Calls{"set_coolant true false", "stop_jog y"},
                      "A press and an increment queued before a release are not sent after it");
    require(bench.outcome(press) == std::string("rejected:") + stopped_reason &&
                bench.outcome(step) == std::string("rejected:") + stopped_reason &&
                bench.outcome(release) == "accepted:",
            "Each still gets its one result: " + bench.outcome(press));
  }
  {
    Bench bench;
    auto working = bench.block_worker();
    const auto press = bench.send(hold_y);
    bench.clock = 150ms;
    require(!bench.control(), "Nothing was sent for the press: no level is needed");
    bench.port.release();
    static_cast<void>(working.get());
    bench.drain();
    require(bench.port.take_calls() == Calls{"set_coolant true false"} &&
                bench.outcome(press) == std::string("rejected:") + stopped_reason,
            "A press whose lease ran out in the queue is not sent");
  }
}

// Property 4: a start in flight when the stop is decided. The level is held until the worker has
// the start back and an NML stop accepted, however long that takes, and nothing starts
// meanwhile.
void hold_until_reconciled() {
  Bench bench;
  bench.port.block("start_jog");
  bench.port.script("start_jog", {{ActionStatus::timed_out, ""}});
  bench.port.script("stop_jog", {{ActionStatus::timed_out, ""}});
  const auto press = bench.send(hold_x);
  auto working = bench.worker();
  bench.port.wait_blocked();
  require(!bench.control(), "A start in flight alone needs no level");
  bench.session.end_page(bench.page);
  require(bench.control() && bench.outputs.jog_stop_high(),
          "The page ended with a start in flight: the level is high in the next period");
  for (int period = 1; period <= 300; ++period) {
    bench.clock = period * 10ms;
    require(bench.control(), "The level is held for as long as the start is in flight");
  }
  bench.port.release();
  require(working.get() == WorkStatus::idle, "The start comes back");
  bench.port.expect(Calls{"start_jog x 1 600", "stop_jog x"},
                    "The NML stop is sent as soon as the start is back");
  require(bench.delivered.results(press) == 0 && bench.delivered.all().size() == 1,
          "Nothing is delivered to the page that ended");
  require(bench.control(), "The stop was not accepted: the level stays high");
  bench.drain();
  bench.port.expect(Calls{"stop_jog x"}, "The stop is retried after a control period");
  require(!bench.control() && !bench.outputs.jog_stop_high(),
          "Accepted: the level is released, 3 s after the start was sent");

  // The same with a new page watching, and the stop failing for longer.
  Bench second;
  second.port.block("start_jog");
  second.port.script("start_jog", {{ActionStatus::timed_out, ""}});
  second.port.script("stop_jog", {{ActionStatus::timed_out, ""}, {ActionStatus::failed, "No."}});
  static_cast<void>(second.send(hold_x));
  auto second_working = second.worker();
  second.port.wait_blocked();
  second.page = second.session.begin_page();
  second.next_id = 1;
  static_cast<void>(second.send(R"("type":"ready")"));
  require(second.control(), "A reload with a start in flight raises the level");
  second.port.release();
  static_cast<void>(second_working.get());
  require(second.delivered.has_text("Jog stop failed on X: LinuxCNC did not answer."),
          "The operator of the new page is told that the stop failed");
  static_cast<void>(second.port.take_calls());
  const auto refused = second.send(hold_y);
  second.drain();
  require(second.outcome(refused) == "failed:The previous jog stop on X is not confirmed: No." &&
              second.port.take_calls() == Calls{"stop_jog x"},
          "No jog starts on any axis while a stop is not confirmed: " + second.outcome(refused));
  const auto denial = second.delivered.result(second.page, refused);
  require(denial &&
              denial->at("localization") == Json{{"key", "messages.session.previousStopAxis"},
                                                 {"params", {{"axis", "X"}, {"detail", "No."}}}},
          "The jog denial identifies the axis with an unconfirmed stop");
  require(second.control() && second.outputs.jog_stop_high(), "The level is still high");
  const auto wanted = second.send(hold_y);
  require(second.work() == WorkStatus::done, "The retry that is due goes before the request");
  second.port.expect({"stop_jog x"}, "The stop is accepted now");
  require(second.work() == WorkStatus::blocked && second.port.calls().empty(),
          "The start waits for the level to be released");
  require(second.outputs.jog_stop_high(), "Which is still high");
  require(!second.control(), "Reconciled: this period releases the level");
  require(second.work() == WorkStatus::blocked && !second.port.called("start_jog"),
          "No start before a period has seen the level low");
  require(!second.control() && !second.outputs.jog_stop_high(), "The level is low");
  second.drain();
  require(second.outcome(wanted) == "accepted:" &&
              second.port.take_calls() == Calls{"start_jog y -1 600"},
          "Then the jog starts: " + second.outcome(wanted));
}

// The level is not released because a stop was accepted quickly: a period must have seen it high.
void level_is_seen_high_before_release() {
  Bench bench;
  bench.jog_x();
  static_cast<void>(bench.send(release_x));
  bench.drain();
  require(bench.port.take_calls() == Calls{"stop_jog x"}, "The stop is accepted at once");
  require(bench.control() && bench.outputs.jog_stop_high(),
          "The first period after the decision still raises the level");
  require(!bench.control() && !bench.outputs.jog_stop_high(), "The next one releases it");
}

// Property 5: results go to the page that asked, never to a later one.
void results_reach_only_their_page() {
  Bench bench;
  const auto first = bench.page;
  auto working = bench.block_worker();
  const auto blocked_id = bench.next_id - 1;
  const auto queued = bench.send(R"("type":"set_override","which":"feed","value":0.5)");
  bench.page = bench.session.begin_page();
  bench.next_id = 1;
  const auto ready = bench.send(R"("type":"ready")");
  bench.port.release();
  static_cast<void>(working.get());
  bench.drain();
  bench.port.expect(Calls{"set_coolant true false"}, "The old page's waiting request is not sent");
  require(!bench.delivered.result(first, blocked_id) && !bench.delivered.result(first, queued),
          "A request in the port when its page ended gets no result");
  require(!bench.delivered.result(bench.page, blocked_id) || blocked_id == ready,
          "And the new page does not get it either");
  for (const auto& [page, message] : bench.delivered.all())
    if (page == bench.page)
      require(message.at("type") == "result" && message.at("id") == ready,
              "The new page gets nothing but its own result: " + message.dump());
}

// Property 6: stops are served before requests that wait, and abort cancels those.
void stops_have_priority() {
  {
    Bench bench;
    bench.jog_x();
    auto working = bench.block_worker();
    const auto move = bench.send(R"("type":"set_override","which":"feed","value":0.5)");
    const auto stop = bench.send(R"("type":"jog_stop_all")");
    bench.port.release();
    static_cast<void>(working.get());
    bench.drain();
    bench.port.expect(Calls{"set_coolant true false", "stop_jog x", "stop_jog y", "stop_jog z",
                            "set_override 0.5"},
                      "A jog stop overtakes the request that waited before it");
    require(bench.outcome(stop) == "accepted:" && bench.outcome(move) == "accepted:",
            "Both are answered");
  }
  {
    Bench bench;
    bench.jog_x();
    auto working = bench.block_worker();
    const auto move = bench.send(R"("type":"mdi","text":"G0 X100")");
    const auto feed = bench.send(R"("type":"set_override","which":"feed","value":0.5)");
    const auto abort = bench.send(R"("type":"abort")");
    require(bench.outcome(move) == std::string("rejected:") + cancelled_reason &&
                bench.outcome(feed) == std::string("rejected:") + cancelled_reason,
            "Abort answers what waited before it at once: " + bench.outcome(move));
    const auto after = bench.send(R"("type":"set_override","which":"feed","value":0.7)");
    bench.port.release();
    static_cast<void>(working.get());
    bench.drain();
    require(bench.port.take_calls() ==
                Calls{"set_coolant true false", "abort", "stop_jog x", "set_override 0.7"},
            "Nothing asked before the abort runs after it; what is asked later does");
    require(bench.outcome(abort) == "accepted:" && bench.outcome(after) == "accepted:",
            "Abort and the later request are answered");
  }
  for (const auto body : {R"("type":"set_machine","state":"estop")",
                          R"("type":"set_machine","state":"off")", R"("type":"abort")"}) {
    // A request that was already past its first command when the stop was asked.
    Bench bench;
    bench.port.block("set_task_mode");
    const auto run = bench.send(R"("type":"run")");
    auto working = bench.worker();
    bench.port.wait_blocked();
    static_cast<void>(bench.send(body));
    bench.port.release();
    static_cast<void>(working.get());
    bench.drain();
    require(!bench.port.called("run") && bench.port.calls().size() == 2,
            std::string("The run is not sent after ") + body);
    require(bench.outcome(run) == "failed:The mode was changed, but a stop request cancelled the "
                                  "command before it was sent.",
            "It is failed, not rejected, and says so: its mode change was sent");
  }
  {
    // What only stops something is not dropped with its page, nor by an abort, and keeps its
    // place among the requests.
    Bench bench;
    auto working = bench.block_worker();
    static_cast<void>(bench.send(R"("type":"set_override","which":"feed","value":0.5)"));
    static_cast<void>(bench.send(R"("type":"set_spindle","direction":"stopped","rpm":0)"));
    static_cast<void>(bench.send(R"("type":"set_coolant","flood":false,"mist":false)"));
    static_cast<void>(bench.send(R"("type":"abort")"));
    bench.session.end_page(bench.page);
    bench.port.release();
    static_cast<void>(working.get());
    bench.drain();
    bench.port.expect(
        {"set_coolant true false", "abort", "set_spindle 0", "set_coolant false false"},
        "Spindle stop and coolant off are sent after the abort and the page's end");
  }
  {
    // A stop outlives its page.
    Bench bench;
    auto working = bench.block_worker();
    const auto abort = bench.send(R"("type":"abort")");
    bench.session.end_page(bench.page);
    bench.port.release();
    static_cast<void>(working.get());
    bench.drain();
    require(bench.port.called("abort") && bench.delivered.results(abort) == 0,
            "An abort is sent although its page ended; nobody is left to answer");
  }
}

// Property 7: the queues are bounded and say so.
void queues_are_bounded() {
  Bench bench;
  bench.jog_x();
  std::vector<RequestId> queued;
  queued.reserve(Session::max_queued);
  for (std::size_t i = 0; i < Session::max_queued; ++i)
    queued.push_back(bench.send(R"("type":"set_override","which":"feed","value":0.5)"));
  for (const auto id : queued)
    require(bench.outcome(id) == "none", "A queued request is not answered yet");
  for (int i = 0; i < 1000; ++i) {
    const auto id = bench.send(R"("type":"set_override","which":"feed","value":0.5)");
    require(bench.outcome(id) == std::string("rejected:") + busy_reason,
            "A request beyond the queue is rejected at once");
  }
  const auto press = bench.send(hold_y);
  require(bench.outcome(press) == std::string("rejected:") + busy_reason, "So is a jog press");
  for (int i = 0; i < 1000; ++i) {
    const auto id = bench.send(hold_x);
    require(bench.outcome(id) == "accepted:", "Refreshes need no queue");
  }
  std::vector<RequestId> stops;
  stops.reserve(Session::max_queued_stops);
  for (std::size_t i = 0; i < Session::max_queued_stops; ++i)
    stops.push_back(bench.send(R"("type":"jog_stop_all")"));
  const auto late = bench.send(R"("type":"jog_stop_all")");
  require(bench.outcome(late) == std::string("rejected:") + busy_reason,
          "Stops have a bounded queue of their own");
  require(bench.control(), "A stop that found its queue full has still stopped the jog");
  bench.drain();
  for (const auto id : stops)
    require(bench.outcome(id) == "accepted:", "Every queued stop is executed");
  for (const auto id : queued)
    require(bench.outcome(id) == "accepted:", "And every queued request after them");
  require(bench.port.calls().size() == 3 * Session::max_queued_stops + Session::max_queued,
          "Nothing that was rejected reached the port");
}

// A session that was closed still stops what jogs, once, and then has nothing to do.
void close_stops_once() {
  Bench bench;
  bench.jog_x();
  bench.port.script("stop_jog", {{ActionStatus::timed_out, ""}});
  const auto waiting = bench.send(R"("type":"set_override","which":"feed","value":0.5)");
  bench.session.close();
  require(bench.control(), "Closing is a stop decision");
  bench.drain();
  require(bench.port.take_calls() == Calls{"stop_jog x"} && bench.delivered.results(waiting) == 0,
          "The jog is stopped; what waited is dropped");
  for (int period = 0; period < 3; ++period) {
    require(bench.control(), "The stop failed: the level stays high after the session");
    bench.drain();
  }
  require(bench.port.take_calls().empty(), "A closed session does not retry for ever");
  require(bench.delivered.all().size() == 2, "And tells no page");
}

struct Threads {
  BlockingPort port;
  FakeFileSystem files;
  ProgramStore programs{files, "/home/cnc/nc_files"};
  FakeOutputs outputs;
  Delivered delivered;
  std::unique_ptr<Runtime> runtime{std::make_unique<Runtime>(
      port, port, outputs, Limits{3000, 1.2, 0.5, 1.2, 24000}, programs,
      [this](const Outcome& outcome) -> void { delivered.add(outcome); })};
  PageToken page{runtime->begin_page()};
  RequestId next_id{1};

  Threads() { static_cast<void>(result(R"("type":"ready")")); }
  auto send(std::string_view body) -> RequestId {
    const auto id = next_id++;
    delivered.add(runtime->submit(page, std::format("{{\"id\":{},{}}}", id, body)));
    return id;
  }
  auto result(std::string_view body) -> std::string {
    return delivered.wait_result(page, send(body)).at("outcome").get<std::string>();
  }
};

auto milliseconds(Clock::duration time) -> double {
  return std::chrono::duration<double, std::milli>(time).count();
}

// Properties 2, 3 and 6 on the real threads and the real clock: a port call that blocks for
// seconds while the lease runs out.
void threads_keep_the_dead_man_on_time() {
  Threads threads;
  require(threads.result(hold_x) == "accepted", "The jog starts");
  threads.port.block("set_coolant");
  const auto blocked = threads.send(slow);
  threads.port.wait_blocked();
  const auto beats = threads.outputs.beats();
  Clock::time_point refreshed;
  for (int i = 0; i < 10; ++i) {
    std::this_thread::sleep_for(50ms);
    const auto id = threads.send(hold_x);
    refreshed = Clock::now();
    require(threads.delivered.result(threads.page, id).has_value(),
            "A refresh is answered in submit while the worker is blocked");
  }
  require(!threads.outputs.jog_stop_high(), "Refreshed for 500 ms: still held");
  require(threads.outputs.wait_level(true, 2s), "The lease runs out: the level rises");
  const auto after = milliseconds(threads.outputs.raised_at() - refreshed);
  std::this_thread::sleep_for(1500ms);
  require(threads.outputs.jog_stop_high() && !threads.port.called("stop_jog"),
          "The worker is still blocked: the level is held and no stop was sent");
  const auto longest = milliseconds(threads.outputs.longest_gap());
  const auto beaten = threads.outputs.beats() - beats;
  threads.port.release();
  require(threads.delivered.wait_result(threads.page, blocked).at("outcome") == "accepted",
          "The blocked request is answered when the port returns");
  require(threads.port.wait_called("stop_jog x"), "And the NML stop follows");
  require(threads.outputs.wait_level(false, 2s), "Reconciled: the level is released");
  std::cout << std::format("measured dead_man_ms={:.1f} longest_heartbeat_gap_ms={:.1f} "
                           "beats_in_2s={}\n",
                           after, longest, beaten);
  // 150 ms lease, one 10 ms period, and what a loaded host adds to a sleeping thread.
  require(after >= 149 && after < 150 + 10 + 100,
          std::format("The level rose {:.1f} ms after the last refresh", after));
  require(
      longest < 150 && beaten > 100,
      std::format("The heartbeat never paused: longest gap {:.1f} ms, {} beats", longest, beaten));
}

// Property 8: destroying the runtime with the worker inside a call.
void shutdown_is_safe_and_bounded() {
  Threads threads;
  require(threads.result(hold_x) == "accepted", "The jog starts");
  threads.port.block("set_coolant");
  static_cast<void>(threads.send(slow));
  static_cast<void>(threads.send(R"("type":"mdi","text":"G0 X1")"));
  threads.port.wait_blocked();
  const auto started = Clock::now();
  auto destroyed = std::async(std::launch::async, [&]() -> void { threads.runtime.reset(); });
  require(threads.outputs.wait_level(true, 2s), "The level rises without the worker");
  const auto raised = milliseconds(threads.outputs.raised_at() - started);
  const auto beats = threads.outputs.beats();
  std::this_thread::sleep_for(300ms);
  require(destroyed.wait_for(0ms) == std::future_status::timeout,
          "The destructor waits for the call the worker is in");
  require(threads.outputs.beats() > beats + 10 && threads.outputs.jog_stop_high(),
          "The control thread beats and holds the level meanwhile");
  threads.port.release();
  require(destroyed.wait_for(5s) == std::future_status::ready, "Then every thread ends");
  const auto joined = milliseconds(Clock::now() - started);
  const auto calls = threads.port.take_calls();
  require(calls.back() == "stop_jog x" && !threads.port.called("mdi"),
          "The jog was stopped over NML; what waited was not sent");
  std::this_thread::sleep_for(60ms);
  require(threads.outputs.since_last_beat() >= 50ms && threads.outputs.jog_stop_high() &&
              threads.outputs.lowered() == 0,
          "No thread is left, and the level is left high without ever having fallen");
  std::cout << std::format("measured shutdown_level_ms={:.1f} shutdown_join_ms={:.1f}\n", raised,
                           joined);
  require(raised < 10 + 100, std::format("The level rose {:.1f} ms into the shutdown", raised));

  // No jog at all and the worker stuck in a call: nothing is left to reconcile, and the level
  // is high all the same, from the first control period of the shutdown to its end.
  Threads stuck;
  stuck.port.block("set_coolant");
  static_cast<void>(stuck.send(slow));
  stuck.port.wait_blocked();
  const auto lowered = stuck.outputs.lowered();
  const auto stuck_started = Clock::now();
  auto stuck_destroyed = std::async(std::launch::async, [&]() -> void { stuck.runtime.reset(); });
  require(stuck.outputs.wait_level(true, 2s), "The level rises with nothing jogging");
  const auto stuck_raised = milliseconds(stuck.outputs.raised_at() - stuck_started);
  for (int i = 0; i < 100; ++i) {
    require(stuck.outputs.jog_stop_high(), "The level is high during all of the shutdown");
    std::this_thread::sleep_for(5ms);
  }
  stuck.port.release();
  require(stuck_destroyed.wait_for(5s) == std::future_status::ready, "The shutdown ends");
  require(stuck.outputs.jog_stop_high() && stuck.outputs.lowered() == lowered,
          "The level never fell and is high at the end");
  std::cout << std::format("measured stuck_shutdown_level_ms={:.1f}\n", stuck_raised);
  require(stuck_raised < 10 + 100, "It rose within a control period of the shutdown's start");

  // Nothing in flight: the whole shutdown is quick.
  Threads idle;
  const auto idle_started = Clock::now();
  idle.runtime.reset();
  const auto idle_joined = milliseconds(Clock::now() - idle_started);
  std::cout << std::format("measured idle_shutdown_join_ms={:.1f}\n", idle_joined);
  require(idle_joined < 1000 && idle.outputs.jog_stop_high(), "An idle runtime ends at once");
}

// Property 7 on the threads: a flood is answered, every request exactly once.
void threads_survive_a_flood() {
  Threads threads;
  threads.port.block("set_coolant");
  const auto first = threads.send(slow);
  threads.port.wait_blocked();
  std::vector<RequestId> ids;
  ids.reserve(5000);
  for (int i = 0; i < 5000; ++i)
    ids.push_back(threads.send(R"("type":"set_override","which":"feed","value":0.5)"));
  std::size_t rejected = 0;
  for (const auto id : ids)
    if (const auto result = threads.delivered.result(threads.page, id);
        result && result->at("reason") == busy_reason)
      ++rejected;
  require(rejected == ids.size() - Session::max_queued,
          std::format("All but the queue's length are rejected at once: {}", rejected));
  threads.port.release();
  static_cast<void>(threads.delivered.wait_result(threads.page, first));
  for (const auto id : ids) {
    static_cast<void>(threads.delivered.wait_result(threads.page, id));
    require(threads.delivered.results(id) == 1, "Exactly one result per request");
  }
  require(threads.port.calls().size() == 1 + Session::max_queued,
          "Only the queue reached the port");
}

} // namespace

auto main() -> int {
  try {
    dead_man_does_not_wait_for_the_worker();
    refresh_does_not_wait_for_the_worker();
    stale_actions_are_dropped_before_sending();
    hold_until_reconciled();
    level_is_seen_high_before_release();
    results_reach_only_their_page();
    stops_have_priority();
    queues_are_bounded();
    close_stops_once();
    threads_keep_the_dead_man_on_time();
    shutdown_is_safe_and_bounded();
    threads_survive_a_flood();
    std::cout << checks << " runtime checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Check " << checks << " failed: " << error.what() << '\n';
    return 1;
  }
}
