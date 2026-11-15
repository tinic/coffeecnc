// The preview in the app layer, without an interpreter: PanelComposer on a preview service of
// the test's own, and the PreviewWorker thread on an engine of the test's own.

#include <coffeecnc/app/panel_feed.hpp>
#include <coffeecnc/app/preview.hpp>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace coffeecnc;
using namespace std::chrono_literals;

namespace {
using Json = nlohmann::json;

int checks = 0;
void require(bool condition, const std::string& message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

auto wait_for(const std::function<bool()>& done, std::chrono::milliseconds limit = 5s) -> bool {
  for (const auto end = std::chrono::steady_clock::now() + limit;
       std::chrono::steady_clock::now() < end; std::this_thread::sleep_for(1ms))
    if (done())
      return true;
  return done();
}

auto hello() -> Hello {
  Hello h;
  h.machine_name = "Mill";
  h.axes = {Axis::x, Axis::y, Axis::z};
  h.axis_max_jog_velocity = {1500, 1500, 750};
  h.max_jog_velocity = 1500;
  return h;
}

// A live, idle machine at rest in G54 with an offset and a program loaded.
auto reading() -> PanelReading {
  PanelReading r;
  r.status.link = Link::live;
  r.status.machine = MachineState::on;
  r.status.axes = {{.axis = Axis::x}, {.axis = Axis::y}, {.axis = Axis::z}};
  r.status.motion.in_position = true;
  r.status.offsets.wcs = 1;
  r.status.offsets.g5x = {10, 20, 30};
  r.status.active_codes = {"G1", "G54"};
  r.status.program.file = "/nc/part.ngc";
  r.status.program.revision = 1;
  r.tool_table = {1, {{1, 1, 40, 6, "End mill"}}};
  r.work_systems.at(0) = WorkSystem{{10, 20, 30}, 0, true};
  return r;
}

// The preview and the status of one step.
struct Step {
  std::optional<Json> preview;
  Json status;
  [[nodiscard]] auto context() const -> std::uint64_t {
    return status.at("preview_context").get<std::uint64_t>();
  }
};

auto compose(PanelComposer& composer, const PanelReading& r) -> Step {
  const auto messages = composer.compose(r, PageToken{1}, false, 1234ms);
  require(messages.has_value(), "Composing succeeds: " + messages.error_or(""));
  Step step;
  for (const auto& text : messages.value_or(std::vector<std::string>{})) {
    auto message = Json::parse(text);
    if (message.at("type") == "preview")
      step.preview = std::move(message);
    else if (message.at("type") == "status")
      step.status = std::move(message);
  }
  return step;
}

// A service that finishes when the test says so.
class FakeService final : public PreviewService {
public:
  struct Submitted {
    std::uint64_t context{};
    PreviewInputs inputs;
  };
  void submit(std::uint64_t context, const PreviewInputs& inputs) override {
    submitted.push_back({context, inputs});
  }
  [[nodiscard]] auto latest() const -> std::shared_ptr<const Preview> override { return finished; }
  [[nodiscard]] auto files_generation() const -> std::uint64_t override { return generation; }

  // A path of one feed move, for `context`.
  void finish(std::uint64_t context, double x = 5) {
    Preview preview;
    preview.context = context;
    preview.state = PreviewState::ready;
    preview.segments = {{3, MoveKind::feed, 1, {{0, 0, 0}, {x, 0, 0}}}};
    preview.bounds = feed_bounds(preview.segments);
    finished = std::make_shared<const Preview>(std::move(preview));
  }

  std::vector<Submitted> submitted;
  std::shared_ptr<const Preview> finished;
  std::uint64_t generation{};
};

void composer_without_a_program() {
  FakeService service;
  PanelComposer composer{hello(), &service};
  auto r = reading();
  r.status.program = {};
  const auto step = compose(composer, r);
  require(step.preview && step.preview->at("state") == "unavailable" &&
              step.preview->at("error").is_null() &&
              step.preview->at("context") == step.context() && service.submitted.empty(),
          "Without a program the preview is unavailable, without an error, and nothing is "
          "computed");
}

void composer_pending_then_the_result() {
  FakeService service;
  PanelComposer composer{hello(), &service};
  auto r = reading();
  auto step = compose(composer, r);
  const auto context = step.context();
  require(service.submitted.size() == 1 && service.submitted[0].context == context &&
              service.submitted[0].inputs.program_file == "/nc/part.ngc" &&
              service.submitted[0].inputs.systems.at(0).offset == Vec3{10, 20, 30},
          "A context with a program is submitted once, with its inputs");
  require(step.preview && step.preview->at("state") == "pending" &&
              step.preview->at("context") == context && step.preview->at("segments").empty(),
          "Until it is computed the preview is pending for the status's context");
  step = compose(composer, r);
  require(!step.preview && service.submitted.size() == 1, "Pending is sent once, computed once");

  service.finish(context);
  step = compose(composer, r);
  require(step.preview && step.preview->at("state") == "ready" &&
              step.preview->at("context") == context && step.context() == context &&
              step.preview->at("segments").at(0).at("line") == 3 &&
              step.preview->at("segments").at(0).at("kind") == "feed" &&
              step.preview->at("segments").at(0).at("points") ==
                  Json::array({0.0, 0.0, 0.0, 5.0, 0.0, 0.0}) &&
              step.preview->at("bounds").at("max") == Json::array({5.0, 0.0, 0.0}),
          "The finished preview is sent with the context it was computed for, which is the "
          "status's");
  require(!compose(composer, r).preview, "And not again");
}

void composer_stale_and_recomputed() {
  FakeService service;
  PanelComposer composer{hello(), &service};
  auto r = reading();
  const auto first = compose(composer, r).context();
  service.finish(first);
  static_cast<void>(compose(composer, r));

  // A work offset changes: the preview shown is of an older context.
  r.status.offsets.g5x = {11, 20, 30};
  r.work_systems.at(0) = WorkSystem{{11, 20, 30}, 0, true};
  auto step = compose(composer, r);
  require(step.context() == first + 1 && service.submitted.size() == 2 &&
              service.submitted[1].context == first + 1 &&
              service.submitted[1].inputs.systems.at(0).offset == Vec3{11, 20, 30},
          "A changed work offset is a new context, submitted with the new offset");
  require(step.preview && step.preview->at("state") == "pending" &&
              step.preview->at("context") == first + 1,
          "The preview of the older context is not sent under the new one: pending");
  service.finish(first + 1, 6);
  step = compose(composer, r);
  require(step.preview && step.preview->at("state") == "ready" &&
              step.preview->at("context") == first + 1 &&
              step.preview->at("segments").at(0).at("points").at(3) == 6.0,
          "Then the preview computed for the new context");

  // A result that arrives for a context that is no longer the status's.
  r.status.offsets.g5x = {12, 20, 30};
  r.work_systems.at(0) = WorkSystem{{12, 20, 30}, 0, true};
  static_cast<void>(compose(composer, r));
  service.finish(first + 1, 7);
  require(!compose(composer, r).preview, "A late result of an older context is never sent");
}

void composer_files_generation_is_an_input() {
  FakeService service;
  PanelComposer composer{hello(), &service};
  const auto r = reading();
  const auto first = compose(composer, r).context();
  service.finish(first);
  static_cast<void>(compose(composer, r));
  service.generation = 1; // a file of the preview changed on disk
  const auto step = compose(composer, r);
  require(step.context() == first + 1 && service.submitted.size() == 2 &&
              service.submitted[1].inputs.files_generation == 1 && step.preview &&
              step.preview->at("state") == "pending",
          "A changed file is a new context: the preview shown is stale, and a new one is "
          "asked for");
  require(compose(composer, r).context() == first + 1 && service.submitted.size() == 2,
          "Once per change");
}

// The tool table a preview is computed with travels with the inputs, every field of it.
void composer_tools_are_inputs() {
  FakeService service;
  PanelComposer composer{hello(), &service};
  auto r = reading();
  r.preview_tools = {{0, 1, 1, {0, 0, 40}, 6, 0, 0, 0}, {1, 1, 1, {0, 0, 40}, 6, 0, 0, 0}};
  const auto first = compose(composer, r).context();
  require(service.submitted.size() == 1 && service.submitted[0].inputs.raw_tools == r.preview_tools,
          "The table is submitted with the context");
  // An X offset: nothing the panel's table shows.
  r.preview_tools[1].offset[0] = 0.5;
  auto step = compose(composer, r);
  require(step.context() == first + 1 && service.submitted.size() == 2 &&
              service.submitted[1].inputs.raw_tools[1].offset == Vec3{0.5, 0, 40} &&
              step.preview && step.preview->at("state") == "pending",
          "An edit of a tool's X offset is a new context, computed again with it");
  // The spindle's entry, an angle, the orientation: each counts.
  r.preview_tools[0].number = 2;
  require(compose(composer, r).context() == first + 2, "The spindle's entry counts");
  r.preview_tools[1].front_angle = 30;
  require(compose(composer, r).context() == first + 3, "An angle counts");
  r.preview_tools[1].orientation = 2;
  require(compose(composer, r).context() == first + 4 && service.submitted.size() == 5,
          "The orientation counts");
  require(compose(composer, r).context() == first + 4, "The same table: the same context");
}

void composer_plan_of_before_the_run() {
  FakeService service;
  PanelComposer composer{hello(), &service};
  auto r = reading();
  const auto first = compose(composer, r).context();
  service.finish(first);
  static_cast<void>(compose(composer, r));
  // The program runs and changes G92, the work offset, the tool: nothing of it is an input.
  r.status.interp = InterpState::reading;
  r.status.active_codes = {"G0", "G55"};
  r.status.offsets.g92 = {5, 0, 0};
  r.status.offsets.g5x = {10, 23, 30};
  r.work_systems.at(0) = WorkSystem{{10, 23, 30}, 0, true};
  r.preview_tools = {{0, 2, 2, {0, 0, 40}, 3, 0, 0, 0}};
  auto step = compose(composer, r);
  require(step.context() == first && !step.preview && service.submitted.size() == 1,
          "While the program runs its own changes are no new context: the plan of before the "
          "run stays the preview");
  // A file changed meanwhile is one, computed from the state of before the run.
  service.generation = 1;
  step = compose(composer, r);
  require(step.context() == first + 1 && service.submitted.size() == 2 &&
              service.submitted[1].inputs.g92 == Vec3{} &&
              service.submitted[1].inputs.systems.at(0).offset == Vec3{10, 20, 30} &&
              service.submitted[1].inputs.raw_tools.empty() &&
              service.submitted[1].inputs.start.active_codes ==
                  std::vector<std::string>{"G1", "G54"},
          "A new context during the run is computed from the machine as it was before the run");
  // Ended: what the run left is one new context.
  r.status.interp = InterpState::idle;
  step = compose(composer, r);
  require(step.context() == first + 2 && service.submitted.size() == 3 &&
              service.submitted[2].inputs.g92 == Vec3{5, 0, 0} &&
              service.submitted[2].inputs.raw_tools.size() == 1,
          "After the run the state it left is one new context");
  require(compose(composer, r).context() == first + 2 && service.submitted.size() == 3, "One");
}

// Block delete switched in a pause: the path shown is no longer the plan, and none can be
// computed before the run has ended.
void composer_block_delete_during_a_run() {
  FakeService service;
  PanelComposer composer{hello(), &service};
  auto r = reading();
  const auto first = compose(composer, r).context();
  service.finish(first);
  static_cast<void>(compose(composer, r));
  r.status.interp = InterpState::paused;
  static_cast<void>(compose(composer, r));
  r.status.program.block_delete = true;
  auto step = compose(composer, r);
  require(step.context() == first + 1 && step.preview &&
              step.preview->at("state") == "unavailable" &&
              step.preview->at("context") == first + 1 &&
              step.preview->at("error").at("line") == 0 &&
              step.preview->at("error").at("text") == PanelComposer::plan_broken_reason &&
              step.preview->at("segments").empty() && service.submitted.size() == 1,
          "Block delete switched while paused: the context goes up, the preview for it says "
          "why there is none, and nothing is computed");
  r.status.interp = InterpState::reading;
  step = compose(composer, r);
  require(step.context() == first + 1 && !step.preview && service.submitted.size() == 1,
          "For the rest of the run: once, and still nothing computed");
  // The run ends where it began: the same context, and now it is computed, once.
  r.status.interp = InterpState::idle;
  step = compose(composer, r);
  require(step.context() == first + 1 && service.submitted.size() == 2 &&
              service.submitted[1].context == first + 1 &&
              service.submitted[1].inputs.block_delete && step.preview &&
              step.preview->at("state") == "pending",
          "When the run has ended the context is computed, once");
  service.finish(first + 1);
  step = compose(composer, r);
  require(step.preview && step.preview->at("state") == "ready" && service.submitted.size() == 2,
          "And its preview is sent");

  // Switched, and switched back within the same pause: still no plan. Blocks already skipped
  // or run under the other setting stay so.
  r.status.interp = InterpState::paused;
  static_cast<void>(compose(composer, r));
  r.status.program.block_delete = false;
  step = compose(composer, r);
  require(step.context() == first + 2 && step.preview->at("state") == "unavailable" &&
              service.submitted.size() == 2,
          "Switched again in a pause: no plan");
  r.status.program.block_delete = true;
  step = compose(composer, r);
  require(step.context() == first + 3 && step.preview &&
              step.preview->at("state") == "unavailable" &&
              step.preview->at("error").at("text") == PanelComposer::plan_broken_reason &&
              step.preview->at("context") == first + 3 && service.submitted.size() == 2,
          "Switched back to what it was: still no plan for this run, and no return to the "
          "preview of before");
  r.status.interp = InterpState::idle;
  step = compose(composer, r);
  require(step.context() == first + 3 && service.submitted.size() == 3 &&
              step.preview->at("state") == "pending",
          "When the run has ended: one computation for the state found");
  // Optional stop is not an input of a preview.
  r.status.program.optional_stop = !r.status.program.optional_stop;
  require(compose(composer, r).context() == first + 3 && service.submitted.size() == 3,
          "Optional stop changes no context");
}

// A program first seen while it runs has no start state.
void composer_no_start_state() {
  FakeService service;
  PanelComposer composer{hello(), &service};
  auto r = reading();
  r.status.interp = InterpState::reading;
  auto step = compose(composer, r);
  const auto first = step.context();
  require(step.preview && step.preview->at("state") == "unavailable" &&
              step.preview->at("error").at("line") == 0 &&
              step.preview->at("error").at("text") == PanelComposer::no_start_state_reason &&
              step.preview->at("context") == first && service.submitted.empty(),
          "A first reading during a run: no preview, with the reason, and nothing computed");
  r.status.interp = InterpState::paused;
  require(!compose(composer, r).preview && service.submitted.empty(), "Paused: the same");
  r.status.interp = InterpState::idle;
  step = compose(composer, r);
  require(service.submitted.size() == 1 && service.submitted[0].context == step.context() &&
              step.preview && step.preview->at("state") == "pending",
          "Seen at rest: computed, once");
  static_cast<void>(compose(composer, r));
  require(service.submitted.size() == 1, "Once");
  // Run again: now there is a start state.
  r.status.interp = InterpState::reading;
  step = compose(composer, r);
  require(!step.preview && service.submitted.size() == 1, "A run seen from its start keeps its plan");

  // Another program, loaded and started by someone else, seen only while it runs.
  r.status.program.file = "/nc/other.ngc";
  r.status.program.revision = 2;
  step = compose(composer, r);
  require(step.preview && step.preview->at("state") == "unavailable" &&
              step.preview->at("error").at("text") == PanelComposer::no_start_state_reason &&
              service.submitted.size() == 1,
          "Another program first seen running: no preview and no computation");
  r.status.interp = InterpState::idle;
  static_cast<void>(compose(composer, r));
  require(service.submitted.size() == 2 &&
              service.submitted[1].inputs.program_file == "/nc/other.ngc",
          "Until it is seen at rest");

  // The link was down: whatever happened meanwhile, the program must be seen at rest again.
  auto down = r;
  down.status.link = Link::down;
  static_cast<void>(compose(composer, down));
  r.status.interp = InterpState::reading;
  r.status.offsets.g5x = {11, 20, 30};
  r.work_systems.at(0) = WorkSystem{{11, 20, 30}, 0, true};
  step = compose(composer, r);
  require(step.preview && step.preview->at("state") == "unavailable" &&
              step.preview->at("error").at("text") == PanelComposer::no_start_state_reason &&
              service.submitted.size() == 2,
          "A link that returns in the middle of a run: no start state");
  r.status.interp = InterpState::idle;
  static_cast<void>(compose(composer, r));
  require(service.submitted.size() == 3, "At rest again: computed");
}

void composer_without_a_service() {
  PanelComposer composer{hello()};
  const auto step = compose(composer, reading());
  require(step.preview && step.preview->at("state") == "unavailable" &&
              step.preview->at("error").at("text") == PanelComposer::no_preview_reason,
          "Without a service no preview is computed, and the preview says so");
}

// An engine the test steers: how long it computes, what it answers, whether files changed.
class FakeEngine final : public PreviewEngine {
public:
  [[nodiscard]] auto compute(const PreviewInputs& inputs, const std::function<bool()>& cancelled)
      -> PreviewComputation override {
    ++computations;
    while (hold) {
      if (cancelled()) {
        ++cancellations;
        return {{}, true, {}};
      }
      std::this_thread::sleep_for(1ms);
    }
    PreviewComputation computed;
    computed.outcome.state = PreviewState::ready;
    computed.outcome.segments = {
        {1, MoveKind::feed, 0, {{0, 0, 0}, {static_cast<double>(inputs.program_revision), 0, 0}}}};
    received.push_back(inputs);
    computed.files_changed = [this](const std::function<bool()>&) -> bool {
      ++file_checks;
      return files_changed;
    };
    return computed;
  }
  std::vector<PreviewInputs> received; // of finished computations; read when none runs
  std::atomic<int> computations{};
  std::atomic<int> cancellations{};
  std::atomic<int> file_checks{};
  std::atomic<bool> hold{};
  std::atomic<bool> files_changed{};
};

auto inputs(std::uint64_t revision) -> PreviewInputs {
  PreviewInputs i;
  i.program_file = "/nc/part.ngc";
  i.program_revision = revision;
  return i;
}

void worker_computes_the_newest() {
  FakeEngine engine;
  PreviewWorker worker{engine, 10ms};
  require(!worker.latest() && worker.files_generation() == 0, "Nothing before the first");
  worker.submit(4, inputs(1));
  require(wait_for([&]() -> bool { return worker.latest() != nullptr; }) &&
              worker.latest()->context == 4 && worker.latest()->state == PreviewState::ready &&
              worker.latest()->segments.at(0).points.at(1)[0] == 1.0,
          "A submitted context is computed and kept with its context");

  // A computation in progress is given up for a newer context.
  engine.hold = true;
  worker.submit(5, inputs(2));
  require(wait_for([&]() -> bool { return engine.computations == 2; }), "Computing 5");
  worker.submit(6, inputs(3));
  require(wait_for([&]() -> bool { return engine.cancellations == 1; }),
          "The older computation is told to stop");
  require(worker.latest()->context == 4, "Its result is not kept");
  engine.hold = false;
  require(wait_for([&]() -> bool { return worker.latest()->context == 6; }) &&
              worker.latest()->segments.at(0).points.at(1)[0] == 3.0 && engine.computations == 3,
          "The newest context is computed");
}

// The engine computes with the table that was submitted, whatever the table is by then.
void worker_hands_the_submitted_tools_on() {
  FakeEngine engine;
  PreviewWorker worker{engine, 10ms};
  auto submitted = inputs(1);
  submitted.raw_tools = {{1, 7, 1, {0.25, 0, 40}, 6, 0, 0, 0}};
  engine.hold = true;
  worker.submit(1, submitted);
  require(wait_for([&]() -> bool { return engine.computations == 1; }), "Computing");
  submitted.raw_tools[0].offset[0] = 9; // the caller's copy changes after the submit
  engine.hold = false;
  require(wait_for([&]() -> bool { return worker.latest() != nullptr; }) &&
              engine.received.size() == 1 &&
              engine.received[0].raw_tools ==
                  std::vector<PreviewTool>{{1, 7, 1, {0.25, 0, 40}, 6, 0, 0, 0}},
          "The engine receives the table as it was submitted");
}

void worker_watches_the_files() {
  FakeEngine engine;
  PreviewWorker worker{engine, 10ms};
  worker.submit(1, inputs(1));
  require(wait_for([&]() -> bool { return engine.file_checks >= 5; }) &&
              engine.computations == 1 && worker.files_generation() == 0,
          "While the files are unchanged they are checked every period and nothing is "
          "computed again");
  engine.files_changed = true;
  require(wait_for([&]() -> bool { return worker.files_generation() == 1; }),
          "A changed file raises the generation");
  const int checks_then = engine.file_checks;
  std::this_thread::sleep_for(60ms);
  require(worker.files_generation() == 1 && engine.file_checks == checks_then &&
              engine.computations == 1,
          "Once: the worker computes nothing by itself and stops checking that preview");
  // The composer makes a new context of the generation and submits it.
  engine.files_changed = false;
  worker.submit(2, inputs(2));
  require(wait_for([&]() -> bool { return worker.latest()->context == 2; }) &&
              wait_for([&]() -> bool { return engine.file_checks > checks_then; }) &&
              worker.files_generation() == 1,
          "The new preview is watched again");
}

void worker_ends_a_computation_when_destroyed() {
  FakeEngine engine;
  engine.hold = true;
  const auto begun = std::chrono::steady_clock::now();
  {
    PreviewWorker worker{engine, 10ms};
    worker.submit(1, inputs(1));
    require(wait_for([&]() -> bool { return engine.computations == 1; }), "Computing");
  }
  require(engine.cancellations == 1 && std::chrono::steady_clock::now() - begun < 2s,
          "Destroying the worker gives the computation up and does not wait for it");
}

void bounds_are_of_the_feed_moves() {
  const std::vector<PreviewSegment> segments{
      {1, MoveKind::rapid, 0, {{-50, -50, 50}, {0, 0, 5}}},
      {2, MoveKind::feed, 0, {{0, 0, 5}, {10, -2, 1}}},
      {3, MoveKind::feed, 0, {{10, -2, 1}, {4, 8, -3}}}};
  require(feed_bounds(segments) == Bounds{{0, -2, -3}, {10, 8, 5}} &&
              !feed_bounds({segments[0]}) && !feed_bounds({}),
          "Bounds are the extent of the feed moves; none without one");
}

} // namespace

auto main() -> int {
  try {
    composer_without_a_program();
    composer_pending_then_the_result();
    composer_stale_and_recomputed();
    composer_files_generation_is_an_input();
    composer_tools_are_inputs();
    composer_plan_of_before_the_run();
    composer_block_delete_during_a_run();
    composer_no_start_state();
    composer_without_a_service();
    worker_computes_the_newest();
    worker_hands_the_submitted_tools_on();
    worker_watches_the_files();
    worker_ends_a_computation_when_destroyed();
    bounds_are_of_the_feed_moves();
    std::cout << checks << " preview checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
