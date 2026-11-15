#ifndef COFFEECNC_APP_PANEL_FEED_HPP
#define COFFEECNC_APP_PANEL_FEED_HPP

#include <coffeecnc/app/preview.hpp>
#include <coffeecnc/app/session.hpp>
#include <coffeecnc/app/status_publisher.hpp>
#include <coffeecnc/core/panel_messages.hpp>
#include <coffeecnc/core/panel_source.hpp>

#include <atomic>
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
#include <utility>
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
// The `preview` sent is for the status's context, always:
//
//   no service         `unavailable`, with no_preview_reason as its error text
//   no program loaded  `unavailable`, no error
//   no coherent start  `unavailable`: a program (file and revision) has a start state only
//                      once a reading showed it at rest, with the tool table settled and the
//                      link live, since it was loaded or since the link came back. Until
//                      then nothing is computed and nothing of the machine's state is taken
//                      as input: with no_start_state_reason while it runs (what is read is
//                      the middle of its run), with not_settled_reason at rest (the tool
//                      table is still being read). Once such a reading exists the context
//                      then in force is
//                      the current one: computed, unless a finished result for that very
//                      context exists (the inputs are what they were), which is then used
//                      again after the service has compared its files once more; `pending`
//                      until it has.
//   block delete was   `unavailable`, with plan_broken_reason: the context went up when the
//   switched during    setting changed, so the path sent before is stale, and nothing is
//   the run            computed until the run has ended, also when it is switched back
//   otherwise          what the service has finished for this very context; until then
//                      `pending`, without segments. A preview of another context is never
//                      sent under this one.
//
// Every new context with a program loaded is submitted to the service, with the inputs in
// force for it. While a program runs these are the machine's state from before the run
// (PreviewContext): the preview during a run is the plan from before the run, not a
// prediction of what is left of it. The service's files_generation is one of the inputs, read
// at every reading. Optional stop is not an input: it pauses and changes no path. A reading
// whose tool table is not settled (PanelReading::tools_settled) is treated like one of a
// running program: the machine's state of the last settled reading at rest stays the input.
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
  static constexpr std::string_view plan_broken_reason =
      "Block delete was switched while the program runs: the path shown before is not what "
      "the rest of the run does. A preview is computed when the run has ended.";
  static constexpr std::string_view no_start_state_reason =
      "The program was already running when its state was first read: there is no start "
      "state to preview it from. A preview is computed when the run has ended.";
  static constexpr std::string_view not_settled_reason =
      "The machine's tool table is still being read: no preview is computed before it is "
      "settled.";
  static constexpr std::size_t max_waiting = 32;

  // `preview` may be null; it must outlive the composer.
  explicit PanelComposer(Hello hello, PreviewService* preview = nullptr);

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
  PreviewService* preview_;
  PreviewContext preview_context_;
  std::uint64_t context_{};
  // The context has not been submitted to the service yet.
  bool submit_due_{};
  bool block_delete_before_run_{};
  bool plan_broken_{};
  // The program, file and revision, that was last seen with the interpreter idle.
  std::optional<std::pair<std::optional<std::string>, std::uint64_t>> seen_at_rest_;
  bool no_start_state_{};
  bool running_without_start_{};
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
  // Called on the publisher thread right after `hello` and the snapshots were handed to
  // `deliver` for that page: what must reach a page after its `hello` is sent from here.
  using SnapshotsSent = std::function<void(PageToken)>;
  static constexpr auto period = std::chrono::milliseconds{50};

  // The source, and the preview service when there is one, must outlive the feed. Starts the
  // thread.
  PanelFeed(PanelSource& source, Deliver deliver, PreviewService* preview = nullptr,
            SnapshotsSent snapshots_sent = {});
  // Joins the thread: returns when the reading in progress has returned. No reading is begun
  // after this was called; a feed that sleeps between two readings ends at once.
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
  SnapshotsSent snapshots_sent_;
  PreviewService* preview_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::optional<PageToken> page_;
  // A `ready` has not been answered with snapshots yet.
  bool ready_{};
  // Written under the mutex, so that the sleeping thread cannot miss it.
  std::atomic<bool> stop_{};
  std::thread thread_;
};

} // namespace coffeecnc

#endif // COFFEECNC_APP_PANEL_FEED_HPP
