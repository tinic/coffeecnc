#include <coffeecnc/app/preview.hpp>

#include <utility>

namespace coffeecnc {

PreviewWorker::PreviewWorker(PreviewEngine& engine, std::chrono::milliseconds check_period)
    : engine_(engine), check_period_(check_period), thread_([this]() -> void { loop(); }) {}

PreviewWorker::~PreviewWorker() {
  {
    const std::scoped_lock lock{mutex_};
    stop_ = true;
  }
  wake_.notify_all();
  thread_.join();
}

void PreviewWorker::submit(std::uint64_t context, const PreviewInputs& inputs) {
  {
    const std::scoped_lock lock{mutex_};
    wanted_ = Request{context, inputs};
    newest_ = context;
    waiting_ = true;
  }
  wake_.notify_all();
}

auto PreviewWorker::latest() const -> std::shared_ptr<const Preview> {
  const std::scoped_lock lock{mutex_};
  return latest_;
}

auto PreviewWorker::files_generation() const -> std::uint64_t { return generation_; }

void PreviewWorker::loop() {
  // Of the preview that is current; this thread's alone.
  PreviewComputation::Watch files_changed;
  std::unique_lock lock{mutex_};
  for (;;) {
    const auto due = [&]() -> bool { return stop_ || wanted_.has_value(); };
    if (files_changed)
      wake_.wait_for(lock, check_period_, due);
    else
      wake_.wait(lock, due);
    if (stop_)
      return;
    if (wanted_) {
      const auto request = std::move(*wanted_);
      wanted_.reset();
      waiting_ = false;
      lock.unlock();
      auto computed = engine_.compute(request.inputs, [&]() -> bool {
        return stop_ || newest_ != request.context;
      });
      lock.lock();
      if (computed.cancelled || stop_ || newest_ != request.context)
        continue;
      auto& outcome = computed.outcome;
      latest_ = std::make_shared<const Preview>(
          Preview{request.context, outcome.state, std::move(outcome.error),
                  std::move(outcome.segments), outcome.bounds});
      files_changed = std::move(computed.files_changed);
      continue;
    }
    if (!files_changed)
      continue;
    lock.unlock();
    // A new context or the end need not wait for the files to be compared.
    const bool changed = files_changed([&]() -> bool { return stop_ || waiting_; });
    lock.lock();
    if (changed) {
      // Once: the composer makes a new context of it and submits that.
      ++generation_;
      files_changed = nullptr;
    }
  }
}

} // namespace coffeecnc
