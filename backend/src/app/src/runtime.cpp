#include <coffeecnc/app/runtime.hpp>

#include <algorithm>
#include <utility>

namespace coffeecnc {
namespace {

auto now() -> Instant {
  return {JogLease::Clock::now(), std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::system_clock::now().time_since_epoch())};
}

} // namespace

auto control_step(Session& session, SafetyOutputs& outputs, Instant now) -> bool {
  outputs.beat();
  const bool hold = session.control(now, {outputs.jog_stop_high(), outputs.watchdog_ok()});
  outputs.hold_jog_stop(hold);
  return hold;
}

void status_step(Session& session, StatusSource& source) {
  // Date the local acquisition before the read, so blocking and conversion time count
  // toward sample age. The source separately checks the controller's writer heartbeat.
  const auto begun = JogLease::Clock::now();
  const auto state = source.read();
  session.observe(state, begun);
}

Runtime::Runtime(ControllerPort& port, StatusSource& status, SafetyOutputs& outputs,
                 const Limits& limits, const ProgramStore& programs, Sink sink,
                 ToolModelService* tool_models, Quit quit, PreviewWorker* preview,
                 PreviewEngine* restart_engine, ProgramWriter* program_writer,
                 OffsetHistoryStore* history, DiagnosticsService* diagnostics,
                 ProgramWriter* copy_writer)
    : status_(status), outputs_(outputs), sink_(std::move(sink)), quit_(std::move(quit)),
      session_(port, limits, programs, tool_models, preview, restart_engine, program_writer,
               history, diagnostics, copy_writer != nullptr),
      control_([this]() -> void { control_loop(); }),
      status_thread_([this]() -> void { status_loop(); }),
      worker_([this]() -> void { work_loop(); }),
      copier_(copy_writer ? std::thread([this, copy_writer]() -> void { copy_loop(*copy_writer); })
                          : std::thread{}) {}

Runtime::~Runtime() {
  session_.close();
  closing_ = true;
  // The control thread keeps beating and holding the level while the worker stops the jogs.
  worker_.join();
  if (copier_.joinable())
    copier_.join();
  stop_status_ = true;
  status_thread_.join();
  stop_control_ = true;
  control_.join();
}

auto Runtime::begin_page(bool watch_frames) -> PageToken {
  return session_.begin_page(watch_frames, now());
}
void Runtime::end_page(PageToken page) { session_.end_page(page); }
void Runtime::panel_frame(PageToken page, std::chrono::milliseconds rendered_at) {
  session_.panel_frame(page, rendered_at, now());
}
auto Runtime::submit(PageToken page, std::string_view json, std::chrono::milliseconds rendered_at)
    -> Outcome {
  return session_.submit(page, json, now(), rendered_at);
}

auto Runtime::loaded_program() const -> std::string { return session_.loaded_program(); }

auto Runtime::publish_program(const std::string& path, const Session::Publication& publish)
    -> std::expected<void, std::string> {
  return session_.publish_program(path, publish).get();
}

auto Runtime::timing() const -> Timing {
  return {control_periods_.load(), std::chrono::nanoseconds{longest_control_period_.load()}};
}

void Runtime::reset_timing() {
  control_periods_ = 0;
  longest_control_period_ = 0;
}

void Runtime::control_loop() {
  auto next = JogLease::Clock::now();
  auto last = next;
  while (!stop_control_) {
    const auto started = JogLease::Clock::now();
    const auto period =
        std::chrono::duration_cast<std::chrono::nanoseconds>(started - last).count();
    last = started;
    longest_control_period_ = std::max(longest_control_period_.load(), period);
    ++control_periods_;
    static_cast<void>(control_step(session_, outputs_, now()));
    // On a fixed grid: a late period does not delay the ones after it.
    next += control_period;
    if (const auto after = JogLease::Clock::now(); next < after)
      next = after;
    std::this_thread::sleep_until(next);
  }
  // The last word on the pin is this thread's: high, and it stays so after the runtime.
  outputs_.hold_jog_stop(true);
}

void Runtime::status_loop() {
  while (!stop_status_) {
    status_step(session_, status_);
    std::this_thread::sleep_for(status_period);
  }
}

void Runtime::work_loop() {
  for (;;) {
    // Read before the work: after close() one more pass finds everything the close left.
    const bool closing = closing_;
    const auto work = session_.work(now);
    if (sink_ && !work.outcome.messages.empty())
      sink_(work.outcome);
    if (work.quit && quit_)
      quit_(work.outcome.page);
    if (work.status == WorkStatus::done)
      continue;
    if (closing)
      return;
    session_.wait_for_work(control_period);
  }
}

void Runtime::copy_loop(ProgramWriter& writer) {
  while (const auto work = session_.copy_work(writer))
    if (sink_ && !work->messages.empty())
      sink_(*work);
}

} // namespace coffeecnc
