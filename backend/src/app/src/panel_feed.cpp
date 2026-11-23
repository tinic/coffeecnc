#include <coffeecnc/app/panel_feed.hpp>
#include <coffeecnc/app/offset_history.hpp>
#include <coffeecnc/app/program_progress.hpp>

#include <coffeecnc/app/response.hpp>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>
#include <utility>

namespace coffeecnc {
namespace {

auto unix_now() -> std::chrono::milliseconds {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch());
}

} // namespace

auto preview_inputs(const PanelReading& reading, const Vec3& rest_position) -> PreviewInputs {
  const auto& status = reading.status;
  PreviewInputs inputs;
  inputs.program_file = status.program.file;
  inputs.program_revision = status.program.revision;
  inputs.systems = reading.work_systems;
  inputs.g92 = status.offsets.g92;
  // A table withheld from the panel is still the machine's table.
  inputs.tools = reading.accepted_tools.value_or(reading.tool_table.tools);
  inputs.raw_tools = reading.preview_tools;
  inputs.block_delete = status.program.block_delete;
  inputs.start.wcs = status.offsets.wcs;
  inputs.start.program_units = status.program_units;
  inputs.start.position = rest_position;
  inputs.start.tool_in_spindle = status.tool.in_spindle;
  inputs.start.tool_prepared = status.tool.prepared;
  inputs.start.tool_offset = status.offsets.tool;
  inputs.start.active_codes = status.active_codes;
  inputs.start.feed = status.motion.programmed_feed;
  inputs.start.spindle_speed = status.spindle.commanded_rpm;
  inputs.start.spindle_direction = status.spindle.direction;
  inputs.start.flood = status.coolant.flood;
  inputs.start.mist = status.coolant.mist;
  return inputs;
}

PanelComposer::PanelComposer(Hello hello, PreviewService* preview)
    : hello_(std::move(hello)), preview_(preview) {}

void PanelComposer::count(const PanelReading& reading) {
  const auto& status = reading.status;
  // Without a status there are no inputs: the context stays what it was. What the machine
  // did meanwhile is not known: the program has to be seen at rest again.
  if (status.link == Link::down) {
    seen_at_rest_.reset();
    return;
  }
  // The interpreter can already be idle while AUTO still has motion or a wait outstanding.
  const bool automatic = status.mode == TaskMode::automatic && status.program.file.has_value();
  const bool running = status.program.breakpoint ||
                       status.interp != InterpState::idle ||
                       (automatic && (reading.task_executing || !status.motion.in_position ||
                                      !std::holds_alternative<WaitNone>(status.wait)));
  if (!running && status.motion.in_position) {
    if (reading.commanded_position) {
      rest_position_ = *reading.commanded_position;
    } else {
      for (const auto& axis : status.axes)
        if (const auto index = axis_index(axis.axis))
          rest_position_.at(*index) = axis.machine;
    }
  }
  auto inputs = preview_inputs(reading, rest_position_);
  if (preview_ != nullptr)
    inputs.files_generation = preview_->files_generation();
  // A coherent start: the machine at rest, its tool table accepted, the status live. Only from
  // such a reading are the machine's state taken as input and a program known to have a
  // start state.
  const bool coherent = !running && status.motion.in_position && !reading.task_executing &&
                        reading.tools_settled && status.link == Link::live;
  const auto context = preview_context_.update(inputs, !coherent);
  const std::pair program{inputs.program_file, inputs.program_revision};
  if (coherent) {
    block_delete_before_run_ = inputs.block_delete;
    seen_at_rest_ = program;
  }
  // A program that has had no coherent start since it was loaded, or since the link was
  // down, has no start state: what is read is the middle of its run, or a machine whose tool
  // table is still being read.
  const bool had_no_start_state = no_start_state_;
  no_start_state_ = seen_at_rest_ != program;
  running_without_start_ = no_start_state_ && running;
  // Block delete switched while the program runs: the rest of the run is no longer the plan
  // that was previewed, and no coherent one can be computed before the run has ended.
  // For the rest of that run, also when it is switched back: blocks already skipped or run
  // under the other setting stay so.
  plan_broken_ =
      running && (plan_broken_ || inputs.block_delete != block_delete_before_run_);
  if (context != context_) {
    submit_due_ = true;
    context_ = context;
    if (preview_ != nullptr)
      preview_->advance_context(context);
  }
  // At rest again with the inputs it had before: a preview is a function of its inputs, so
  // the result that exists for this context is the preview, and nothing is computed. It was
  // not shown meanwhile, so the service compares its files once more first.
  if (had_no_start_state && !no_start_state_ && !submit_due_ && preview_ != nullptr)
    preview_->revalidate(context_);
  if (!submit_due_ || plan_broken_ || no_start_state_)
    return;
  submit_due_ = false;
  if (preview_ != nullptr && preview_context_.inputs().program_file)
    preview_->submit(context_, preview_context_.inputs());
}

void PanelComposer::observe(const PanelReading& reading) {
  count(reading);
  std::ranges::copy(reading.messages, std::back_inserter(waiting_));
  if (waiting_.size() > max_waiting)
    waiting_.erase(waiting_.begin(),
                   waiting_.end() - static_cast<std::ptrdiff_t>(max_waiting));
}

auto PanelComposer::compose(const PanelReading& reading, PageToken page, bool ready_again,
                            std::chrono::milliseconds unix_time, std::vector<PanelRecord>* records)
    -> std::expected<std::vector<std::string>, std::string> {
  observe(reading);
  if (page_ != page) {
    page_ = page;
    publisher_ = StatusPublisher{};
    snapshots_due_ = true;
  }
  snapshots_due_ = snapshots_due_ || ready_again;
  Status status = reading.status;
  status.preview_context = context_;
  // A finished preview is sent as the service holds it; anything else is made here.
  Preview made;
  made.context = status.preview_context;
  made.state = PreviewState::unavailable;
  std::shared_ptr<const Preview> finished;
  if (preview_ == nullptr) {
    made.error = PreviewError{0, std::string(no_preview_reason)};
  } else if (status.program.file && no_start_state_) {
    made.error = PreviewError{0, std::string(running_without_start_ ? no_start_state_reason
                                                                    : not_settled_reason)};
  } else if (status.program.file && plan_broken_) {
    made.error = PreviewError{0, std::string(plan_broken_reason)};
  } else if (status.program.file) {
    finished = preview_->latest();
    if (!finished || finished->context != status.preview_context) {
      finished.reset();
      made.state = PreviewState::pending;
    }
  }
  const Preview& preview = finished ? *finished : made;
  if (const auto hint = status.program.restart_hint;
      hint && (preview.state != PreviewState::ready ||
               std::ranges::none_of(preview.segments, [&](const PreviewSegment& segment) {
                 return segment.line == hint->line && segment.tool == hint->tool;
               })))
    status.program.restart_hint.reset();
  if (work_offsets_.revision == 0 || reading.work_systems != work_offsets_.systems)
    work_offsets_ = {work_offsets_.revision + 1, reading.work_systems};
  auto messages = publisher_.publish(
      {hello_, status, reading.tool_table, work_offsets_, reading.program.get(), preview,
       reading.offset_history.get()},
      snapshots_due_, records);
  if (!messages)
    return messages; // Nothing was sent: what was due still is.
  snapshots_due_ = false;
  std::ranges::transform(waiting_, std::back_inserter(*messages),
                         [&](const ControllerMessage& message) -> std::string {
                           return encode_message(message.error ? MessageLevel::error
                                                               : MessageLevel::info,
                                                 message.text, unix_time);
                         });
  if (records != nullptr)
    records->insert(records->end(), waiting_.size(), PanelRecord::message);
  waiting_.clear();
  return messages;
}

PanelFeed::PanelFeed(PanelSource& source, Deliver deliver, PreviewService* preview,
                     SnapshotsSent snapshots_sent, ProgramProgress* progress,
                     OffsetHistoryStore* history, Mirror mirror)
    : source_(source), deliver_(std::move(deliver)), snapshots_sent_(std::move(snapshots_sent)),
      preview_(preview), progress_(progress), history_(history), mirror_(std::move(mirror)),
      thread_([this]() -> void { loop(); }) {}

PanelFeed::~PanelFeed() {
  {
    const std::scoped_lock lock{mutex_};
    stop_ = true;
  }
  wake_.notify_all();
  thread_.join();
}

void PanelFeed::page_ready(PageToken page) {
  {
    const std::scoped_lock lock{mutex_};
    page_ = page;
    ready_ = true;
  }
  wake_.notify_all();
}

void PanelFeed::page_ended(PageToken page) {
  const std::scoped_lock lock{mutex_};
  if (page_ == page)
    page_.reset();
}

void PanelFeed::loop() {
  PanelComposer composer{source_.hello(), preview_};
  std::string reported;
  // The page the composer was last asked for, and whether it still owes that page its
  // snapshots: as the composer itself counts them.
  std::optional<PageToken> composed;
  bool snapshots_owed = false;
  std::vector<PanelRecord> records;
  auto next = std::chrono::steady_clock::now();
  for (;;) {
    auto reading = source_.read();
    if (progress_)
      progress_->observe(reading, ProgramProgress::Clock::now());
    if (history_)
      history_->observe(reading, unix_now(), OffsetHistoryStore::Clock::now());
    std::optional<PageToken> page;
    bool ready = false;
    {
      const std::scoped_lock lock{mutex_};
      if (stop_)
        return;
      page = page_;
      ready = std::exchange(ready_, false);
    }
    if (page || mirror_) {
      const auto publication = page.value_or(PageToken{});
      snapshots_owed = snapshots_owed || ready || composed != publication;
      composed = publication;
      records.clear();
      auto messages = composer.compose(reading, publication, ready, unix_now(),
                                       mirror_ ? &records : nullptr);
      if (messages) {
        reported.clear();
        if (mirror_)
          mirror_(*messages, records, reading.program != nullptr);
        if (page) {
          deliver_(*page, std::move(*messages));
          if (std::exchange(snapshots_owed, false) && snapshots_sent_)
            snapshots_sent_(*page);
        }
      } else if (page && messages.error() != reported) {
        reported = messages.error();
        deliver_(*page, {encode_message(MessageLevel::error,
                                        "The machine status cannot be shown: " + reported,
                                        unix_now())});
      }
    } else {
      composer.observe(reading);
    }
    // On a fixed grid; a page that has just become ready is served at once.
    next += period;
    if (const auto after = std::chrono::steady_clock::now(); next < after)
      next = after;
    // Woken before the period is over: a page has become ready, or this is the end. After
    // the end no reading is begun.
    std::unique_lock lock{mutex_};
    if (wake_.wait_until(lock, next, [&]() -> bool { return stop_ || ready_; }))
      next = std::chrono::steady_clock::now();
    if (stop_)
      return;
  }
}

} // namespace coffeecnc
