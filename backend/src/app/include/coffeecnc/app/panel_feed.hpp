#ifndef COFFEECNC_APP_PANEL_FEED_HPP
#define COFFEECNC_APP_PANEL_FEED_HPP

#include <coffeecnc/app/session.hpp>
#include <coffeecnc/app/status_publisher.hpp>
#include <coffeecnc/core/panel_messages.hpp>
#include <coffeecnc/core/panel_source.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace coffeecnc {

// The inputs of a preview as one reading gives them. The machine position is the one the
// machine last rested at with an idle interpreter: a position in motion is not a start state.
[[nodiscard]] auto preview_inputs(const PanelReading& reading, const Vec3& rest_position)
    -> PreviewInputs;

// Turns successive readings into the messages for the page that shows. One step per reading;
// no thread and no clock of its own.
//
// Every reading counts for `preview_context`, with or without a page, so the context a page is
// told is the one a preview of that moment would have. A page gets a StatusPublisher of its own:
// its statuses are numbered from 1. `hello` and every snapshot are sent in a page's first step
// and again in the step after every further `ready` of that page; the numbering goes on.
//
// No interpreter preview exists yet. The `preview` sent is `unavailable` for the current
// context, with no segments, whatever program is loaded, and carries no_preview_reason as its
// error text.
//
// `work_offsets` always has nine rows, each as available as the reading says; its revision
// counts from 1 and goes up when a row changes.
//
// The source's own messages wait, the last max_waiting of them, until a page can be told.
//
// Pure logic, NOT thread-safe.
class PanelComposer {
public:
  static constexpr std::string_view no_preview_reason =
      "This version of CoffeeCNC does not compute a toolpath preview.";
  static constexpr std::size_t max_waiting = 32;

  explicit PanelComposer(Hello hello);

  // The messages for `page`, which has sent `ready`; `ready_again` when it has sent another
  // since the last step. When one of them cannot be encoded the result is the reason, nothing
  // counts as sent and the same is due on the next step.
  [[nodiscard]] auto compose(const PanelReading& reading, PageToken page, bool ready_again,
                             std::chrono::milliseconds unix_time)
      -> std::expected<std::vector<std::string>, std::string>;
  // A reading while no page is ready.
  void observe(const PanelReading& reading);

private:
  void count(const PanelReading& reading);

  Hello hello_;
  PreviewContext preview_context_;
  std::uint64_t context_{};
  Vec3 rest_position_{};
  std::optional<PageToken> page_;
  // hello and the snapshots are owed to the page.
  bool snapshots_due_{};
  StatusPublisher publisher_;
  WorkOffsets work_offsets_;
  std::vector<ControllerMessage> waiting_;
};

// The publisher thread: reads the source every `period`, and hands the messages for the page
// that has sent `ready` to `deliver`, with that page's token. It owns the source, a
// PanelComposer and nothing else: no HAL pin, no command channel, nothing of the session.
//
//   thread     period   owns                          may block on
//   publisher  50 ms    PanelSource (a status channel the controller's status buffer, and
//                       of its own, the files the     the file system
//                       panel is shown)
//
// `deliver` is called on the publisher thread only; it must not block and must check the token
// against the page it shows when it delivers. page_ready and page_ended are called by the
// shell's thread and return at once.
//
// A message that cannot be encoded (a number that is not finite) is not sent; the page is told
// why in an operator message, once per reason until a step succeeds again.
class PanelFeed {
public:
  using Deliver = std::function<void(PageToken, std::vector<std::string>)>;
  static constexpr auto period = std::chrono::milliseconds{50};

  // The source must outlive the feed. Starts the thread.
  PanelFeed(PanelSource& source, Deliver deliver);
  // Joins the thread: returns when the reading in progress has returned.
  ~PanelFeed();
  PanelFeed(const PanelFeed&) = delete;
  auto operator=(const PanelFeed&) -> PanelFeed& = delete;

  // The page sent `ready` (Outcome::send_snapshots): snapshots at once, then a status per
  // period. Every call asks for the snapshots, also a repeated one for the same page.
  void page_ready(PageToken page);
  // The page is gone. Ignored for a page that is not the one that was ready.
  void page_ended(PageToken page);

private:
  void loop();

  PanelSource& source_;
  Deliver deliver_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::optional<PageToken> page_;
  // A `ready` has not been answered with snapshots yet.
  bool ready_{};
  bool stop_{};
  std::thread thread_;
};

} // namespace coffeecnc

#endif // COFFEECNC_APP_PANEL_FEED_HPP
