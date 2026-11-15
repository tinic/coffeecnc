#include <coffeecnc/app/panel_feed.hpp>

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
  inputs.tools = reading.tool_table.tools;
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
  return inputs;
}

PanelComposer::PanelComposer(Hello hello, PreviewService* preview)
    : hello_(std::move(hello)), preview_(preview) {}

void PanelComposer::count(const PanelReading& reading) {
  const auto& status = reading.status;
  // Without a status there are no inputs: the context stays what it was.
  if (status.link == Link::down)
    return;
  const bool running = status.interp != InterpState::idle;
  if (!running && status.motion.in_position)
    for (const auto& axis : status.axes)
      if (const auto index = axis_index(axis.axis))
        rest_position_.at(*index) = axis.machine;
  auto inputs = preview_inputs(reading, rest_position_);
  if (preview_ != nullptr)
    inputs.files_generation = preview_->files_generation();
  const auto context = preview_context_.update(inputs, running);
  if (context == context_)
    return;
  context_ = context;
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
                            std::chrono::milliseconds unix_time)
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
  } else if (status.program.file) {
    finished = preview_->latest();
    if (!finished || finished->context != status.preview_context) {
      finished.reset();
      made.state = PreviewState::pending;
    }
  }
  const Preview& preview = finished ? *finished : made;
  if (work_offsets_.revision == 0 || reading.work_systems != work_offsets_.systems)
    work_offsets_ = {work_offsets_.revision + 1, reading.work_systems};
  auto messages = publisher_.publish(
      {hello_, status, reading.tool_table, work_offsets_, reading.program.get(), preview},
      snapshots_due_);
  if (!messages)
    return messages; // Nothing was sent: what was due still is.
  snapshots_due_ = false;
  std::ranges::transform(waiting_, std::back_inserter(*messages),
                         [&](const ControllerMessage& message) -> std::string {
                           return encode_message(message.error ? MessageLevel::error
                                                               : MessageLevel::info,
                                                 message.text, unix_time);
                         });
  waiting_.clear();
  return messages;
}

PanelFeed::PanelFeed(PanelSource& source, Deliver deliver, PreviewService* preview)
    : source_(source), deliver_(std::move(deliver)), preview_(preview),
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
  auto next = std::chrono::steady_clock::now();
  for (;;) {
    const auto reading = source_.read();
    std::optional<PageToken> page;
    bool ready = false;
    {
      const std::scoped_lock lock{mutex_};
      if (stop_)
        return;
      page = page_;
      ready = std::exchange(ready_, false);
    }
    if (page) {
      auto messages = composer.compose(reading, *page, ready, unix_now());
      if (messages) {
        reported.clear();
        deliver_(*page, std::move(*messages));
      } else if (messages.error() != reported) {
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
