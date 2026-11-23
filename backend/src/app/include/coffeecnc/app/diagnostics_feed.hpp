#ifndef COFFEECNC_APP_DIAGNOSTICS_FEED_HPP
#define COFFEECNC_APP_DIAGNOSTICS_FEED_HPP

#include <coffeecnc/app/session.hpp>
#include <coffeecnc/core/diagnostics.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace coffeecnc {

// The diagnostics view's service. While a page watches, a thread of its own takes a reading
// every period and posts it to that page as one `diagnostics` message; while no page watches it
// reads nothing. One page at a time: a new watch replaces the one before. Never the control
// thread's business: the readings are the source's, on this thread.
//
// After watch(page, false) or page_ended(page) has returned, nothing more is posted for that
// page. The sink is called with the feed's mutex held, on the feed's thread only: it must not
// block or call the feed. Thread-safe.
class DiagnosticsFeed final : public DiagnosticsService {
public:
  struct Source {
    // A page has begun to watch: called before its first reading, on the feed's thread.
    std::function<void()> begin;
    std::function<Diagnostics()> read;
    // DiagnosticsService::launch, on the caller's thread.
    std::function<std::optional<std::string>(HalTool)> launch;
  };
  using Sink = std::function<void(PageToken, std::vector<std::string>)>;
  // Ten readings a second: a switch is seen as it is pressed, and HAL is asked little.
  static constexpr auto default_period = std::chrono::milliseconds{100};

  DiagnosticsFeed(Source source, Sink sink, std::chrono::milliseconds period = default_period);
  DiagnosticsFeed(const DiagnosticsFeed&) = delete;
  DiagnosticsFeed(DiagnosticsFeed&&) = delete;
  auto operator=(const DiagnosticsFeed&) -> DiagnosticsFeed& = delete;
  auto operator=(DiagnosticsFeed&&) -> DiagnosticsFeed& = delete;
  // Ends the thread once the reading it is in has ended.
  ~DiagnosticsFeed() override;

  void watch(PageToken page, bool on) override;
  [[nodiscard]] auto launch(HalTool tool) -> std::optional<std::string> override;
  void page_ended(PageToken page) override;

private:
  void loop();

  Source source_;
  Sink sink_;
  std::chrono::milliseconds period_;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::optional<PageToken> watched_;
  std::uint64_t watches_{}; // goes up with every watch(page, true)
  bool closing_{};
  std::thread thread_;
};

} // namespace coffeecnc

#endif // COFFEECNC_APP_DIAGNOSTICS_FEED_HPP
