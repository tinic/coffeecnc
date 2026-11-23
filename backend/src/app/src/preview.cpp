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
    wanted_ = Request{context, std::make_shared<const PreviewInputs>(inputs)};
    newest_ = context;
    waiting_ = true;
  }
  wake_.notify_all();
}

void PreviewWorker::advance_context(std::uint64_t context) {
  const std::scoped_lock lock{mutex_};
  if (newest_ != context) {
    newest_ = context;
    wanted_.reset();
    unchecked_.reset();
    waiting_ = false;
  }
}

auto PreviewWorker::latest() const -> std::shared_ptr<const Preview> {
  const std::scoped_lock lock{mutex_};
  return latest_;
}

void PreviewWorker::revalidate(std::uint64_t context) {
  {
    const std::scoped_lock lock{mutex_};
    if (!latest_ || latest_->context != context)
      return;
    if (newest_ != context)
      return;
    unchecked_ = std::move(latest_);
    latest_.reset();
    ++revalidation_;
  }
  wake_.notify_all();
}

auto PreviewWorker::files_generation() const -> std::uint64_t { return generation_; }

auto PreviewWorker::restart_source(std::uint64_t context) const -> RestartSource {
  const std::scoped_lock lock{mutex_};
  if (!latest_ || newest_ != context || latest_->context != context ||
      latest_->state != PreviewState::ready)
    return {};
  return latest_source_;
}

auto PreviewWorker::checked_preview() const -> CheckedPreview {
  const std::scoped_lock lock{mutex_};
  if (!latest_ || latest_->context != newest_)
    return {};
  return {latest_, latest_source_, latest_files_};
}

void PreviewWorker::loop() {
  // Of the preview that is current; this thread's alone.
  PreviewComputation::Watch files_changed;
  std::uint64_t files_context{};
  std::unique_lock lock{mutex_};
  for (;;) {
    const auto due = [&]() -> bool {
      return stop_ || wanted_.has_value() || unchecked_ != nullptr;
    };
    if (files_changed)
      wake_.wait_for(lock, check_period_, due);
    else
      wake_.wait(lock, due);
    if (stop_)
      return;
    if (wanted_) {
      auto request = std::move(*wanted_);
      wanted_.reset();
      waiting_ = false;
      lock.unlock();
      auto computed = engine_.compute(*request.inputs, [&]() -> bool {
        return stop_ || newest_ != request.context;
      });
      lock.lock();
      if (computed.cancelled || stop_ || newest_ != request.context)
        continue;
      auto& outcome = computed.outcome;
      unchecked_.reset();
      latest_ = std::make_shared<const Preview>(
          Preview{request.context, outcome.state, std::move(outcome.error),
                  std::move(outcome.segments), outcome.bounds, std::move(outcome.dwells),
                  std::move(outcome.check)});
      latest_source_ = {std::move(request.inputs), std::move(computed.program_snapshot)};
      files_changed = std::move(computed.files_changed);
      files_context = request.context;
      latest_files_ = files_changed;
      continue;
    }
    if (!files_changed) {
      // A preview asked to be checked again that has nothing to compare its files with is
      // never given back unchecked: it is dropped, and the generation asks for a new one.
      if (unchecked_) {
        unchecked_.reset();
        ++generation_;
      }
      continue;
    }
    if (files_context != newest_) {
      files_changed = nullptr;
      continue;
    }
    const auto check = revalidation_;
    lock.unlock();
    // A new context or the end need not wait for the files to be compared.
    const bool changed = files_changed([&]() -> bool {
      return stop_ || waiting_ || files_context != newest_;
    });
    lock.lock();
    if (files_context != newest_ || check != revalidation_)
      continue;
    if (changed) {
      // Once: the composer makes a new context of it and submits that. The result is of
      // files that are no more: it is gone for good, and nothing can bring it back.
      ++generation_;
      files_changed = nullptr;
      unchecked_.reset();
      latest_.reset();
      latest_source_ = {};
      latest_files_ = {};
    } else if (unchecked_ && !wanted_ && !stop_) {
      // Compared again and the same: finished for its context again.
      latest_ = std::move(unchecked_);
      unchecked_.reset();
    }
  }
}

} // namespace coffeecnc
