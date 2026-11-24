#include <coffeecnc/app/diagnostics_feed.hpp>
#include <coffeecnc/app/messages.hpp>

#include <optional>
#include <string>
#include <utility>

namespace coffeecnc {

DiagnosticsFeed::DiagnosticsFeed(Source source, Sink sink, std::chrono::milliseconds period)
    : source_(std::move(source)), sink_(std::move(sink)), period_(period),
      thread_([this]() -> void { loop(); }) {}

DiagnosticsFeed::~DiagnosticsFeed() {
  {
    const std::scoped_lock lock{mutex_};
    closing_ = true;
    watched_.reset();
  }
  changed_.notify_all();
  thread_.join();
}

void DiagnosticsFeed::watch(PageToken page, bool on) {
  {
    const std::scoped_lock lock{mutex_};
    if (on) {
      watched_ = page;
      ++watches_;
    } else if (watched_ == page) {
      watched_.reset();
    }
  }
  changed_.notify_all();
}

auto DiagnosticsFeed::launch(HalTool tool) -> std::optional<LocalizedReason> {
  if (!source_.launch)
    return LocalizedReason{"HAL tools cannot be started here.",
                           MessageKey{"backend.halDiagnostics.cannotStart"}};
  return source_.launch(tool);
}

void DiagnosticsFeed::page_ended(PageToken page) {
  {
    const std::scoped_lock lock{mutex_};
    if (watched_ && watched_->value <= page.value)
      watched_.reset();
  }
  changed_.notify_all();
}

void DiagnosticsFeed::loop() {
  using Clock = std::chrono::steady_clock;
  std::unique_lock lock{mutex_};
  std::uint64_t begun = 0;
  while (!closing_) {
    if (!watched_) {
      changed_.wait(lock, [&]() -> bool { return closing_ || watched_.has_value(); });
      continue;
    }
    const auto page = *watched_;
    const auto generation = watches_;
    const auto due = Clock::now() + period_;
    lock.unlock();
    if (begun != generation && source_.begin)
      source_.begin();
    begun = generation;
    std::optional<std::string> message;
    if (source_.read)
      if (auto encoded = encode_diagnostics(source_.read()))
        message = std::move(*encoded);
    lock.lock();
    // Posted only while that watch is still the one: never after it has ended.
    if (message && watched_ == page && watches_ == generation)
      sink_(page, {std::move(*message)});
    changed_.wait_until(lock, due,
                        [&]() -> bool { return closing_ || watched_ != page || watches_ != generation; });
  }
}

} // namespace coffeecnc
