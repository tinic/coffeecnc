#ifndef COFFEECNC_APP_SESSION_HPP
#define COFFEECNC_APP_SESSION_HPP

#include <coffeecnc/app/program_store.hpp>
#include <coffeecnc/app/request.hpp>
#include <coffeecnc/app/response.hpp>
#include <coffeecnc/core/command_policy.hpp>
#include <coffeecnc/core/controller_port.hpp>
#include <coffeecnc/core/jog_lease.hpp>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace coffeecnc {

// One page load in the native shell. Never on the wire. A default token matches no page.
struct PageToken {
  std::uint64_t value{};
  friend auto operator==(const PageToken&, const PageToken&) -> bool = default;
};

// Where the two tool-model commands go: storage with a thread of its own (ToolModelStore).
// Called with the session's mutex held, so neither call may block or call the session.
class ToolModelService {
public:
  ToolModelService() = default;
  virtual ~ToolModelService() = default;

  // A reason: the command is `rejected` with it at once. None: the command waits for the
  // writer, and its one CommandResult comes from there, unless its page ends first.
  [[nodiscard]] virtual auto submit(PageToken page, RequestId id, ToolModelCommand command)
      -> std::optional<std::string> = 0;
  // This page and every one before it has ended: what waits for them is dropped, and what
  // they are still owed is not sent.
  virtual void page_ended(PageToken page) = 0;

protected:
  ToolModelService(const ToolModelService&) = default;
  ToolModelService(ToolModelService&&) = default;
  auto operator=(const ToolModelService&) -> ToolModelService& = default;
  auto operator=(ToolModelService&&) -> ToolModelService& = default;
};

// The caller's clocks, read once per call.
struct Instant {
  JogLease::Time monotonic{};            // jog leases
  std::chrono::milliseconds unix_time{}; // stamps operator messages
};

// Messages one call produced.
struct Outcome {
  // The page the messages are for. The caller posts them only while this is still the page it
  // shows: replies queued across a reload must not reach the next page.
  PageToken page;
  // Protocol v1 JSON for that page, in order. A `directory` precedes the result of the
  // `list_directory` that asked for it.
  std::vector<std::string> messages;
  // The page sent `ready`: send hello, the tool table, the program and status.
  bool send_snapshots{};
};

// What the control thread read from its outputs in this period.
struct ControlInput {
  bool jog_stop_high{}; // SafetyOutputs::jog_stop_high
  bool watchdog_ok{true};
};

enum class WorkStatus : std::uint8_t {
  done,    // one piece of work was executed; ask again
  idle,    // nothing to do
  blocked, // the next request starts a jog and the jog-stop level is not released yet
};

struct Work {
  WorkStatus status{WorkStatus::idle};
  Outcome outcome;
};

// The single owner of request handling: page generations, the ready gate, request identity,
// policy, mode switching, controller actions, the program directory, jog leases and the jog-stop
// hold. Every request that is executed or rejected gets exactly one CommandResult; stale,
// duplicate and replayed requests get none, and neither does a request whose page has ended.
//
// Three threads share it, each through its own members; one mutex guards the state and is never
// held across a call of the port, the program store or anything else that can block:
//
//   any thread   begin_page, end_page, submit, close. Never wait for the controller. `submit`
//                answers at once what needs no controller (the ready gate, invalid requests, a
//                full queue, the refresh of a held jog) and queues the rest.
//   status       observe: the latest controller state, read on a path of its own.
//   control      control, once per period: the dead-man. Decides on the observed state alone.
//   worker       work: executes queued requests and jog stops on the port. The only caller of
//                the port, so the only writer of controller commands.
//
// Queues. Requests wait in order, at most max_queued; abort, E-stop, power off, a jog release
// and stop-all wait in a queue of their own, at most max_queued_stops, that is served first. A
// request that finds its queue full is `rejected`. Abort, E-stop and power off also reject every
// request waiting in the ordinary queue: what was asked before a stop must not run after it.
// Every queued request carries its page, the jog epoch and the count of aborts it was queued
// in. The worker asks whether a request is still wanted after every call that can block (a
// status reading, the program store, a command) and before every command it sends: a request of
// an ended page sends nothing more and gets no result; one overtaken by abort, E-stop or power
// off sends nothing more and is answered as cancelled: `rejected` when nothing at all was sent
// for it, `failed` with what was sent and what was not when part of it was (a mode change, the
// first of two commands); a jog start or increment of an earlier
// epoch is not sent. The port is handed the same question (`Wanted`) with every action and asks
// it down to the moment it writes each command. Requests that only stop something (abort,
// E-stop, power off, jog release, stop-all, spindle stop, coolant off) are executed whatever
// became of their page and are never withdrawn.
//
// Limitation: several axes can be held at once, but the jog-stop level is common to all of them,
// so releasing one stops them all, and each of the others must be released and pressed again.
//
// Jogs. A held jog has a lease from the moment its press is accepted for queueing; refreshes
// extend it without the worker. A stop decision is any of: release, stop-all, abort, E-stop,
// power off, page begin or end, a lease not refreshed for JogLease::timeout, controller link not
// live, machine not on, the watchdog tripped, a held jog the policy would no longer start, a
// mode change, a failed start. It takes effect in the same call, for every axis, because the
// jog-stop level stops every jog:
//
//   - the jog epoch goes up: no start or increment queued before it is sent;
//   - every lease ends;
//   - every axis that is jogging, has a start or increment in flight, or was sent an increment
//     that is not known to have ended, needs a jog stop acknowledged by the controller; a
//     released axis always does. An increment the controller acknowledged stays tracked until
//     a jog stop is accepted after it, and by nothing else: neither time nor the status can
//     tell a finished increment from one the controller has not taken up. So after a jog
//     increment the next stop decision raises the level and stops that axis over NML, however
//     long ago the increment ended; and while it is tracked, everything that is a stop decision
//     for a held jog but the lease is one for it. An increment that came back from the port in
//     any other way than accepted may have been written and may still be taken up: it is a
//     stop decision by itself;
//   - while any axis needs one, the jog-stop level is held (`control` returns true);
//   - every axis that had a jog or a press, except the one the panel itself released, is
//     latched: `held: true` for it is `rejected` until the panel has sent `held: false` for that
//     axis or a new page has begun. The refreshes of a press that was stopped never start it
//     again. Jog increments are not latched.
//
// Hold states, per session:
//
//   released   no axis needs a stop; the level is low; jog starts are sent.
//   held       entered by a stop decision that leaves an axis needing a stop. The level is high.
//              The worker sends no start and no increment: a start that is next waits
//              (`blocked`), an increment is refused.
//              The worker stops each such axis as soon as its present call returns. A stop that
//              fails is reported once as an operator message (or as the result of the request
//              that asked for it) and retried after every control period. A jog request then
//              retries the stops itself and fails while one is not confirmed.
//              A start is sent again once the level has been found low in settled_periods
//              control periods.
//   released   again once (a) no start or increment is in flight, (b) every axis that needed a
//              stop has had one accepted, sent after the last start for it returned, and (c) a
//              control period has seen the level high. The controller executes commands in the
//              order they were written, so an accepted stop means every start written before it
//              has been executed and stopped, or refused under the level. Until then, also with
//              a controller that never answers, the level stays high.
//
// `list_directory` is answered from the program store, not the controller. `open_program` reaches
// the controller only with a path the store resolved: a regular file inside the program
// directory; anything else is `rejected`.
//
// `set_tool_model_defaults` and `set_tool_model_assignment` are answered by the tool-model
// service, whatever the machine and the link are doing: they pass the page, ready and ID checks
// in `submit` and are handed over there. They never wait in the request queues, so the worker
// and the controller never see them. Without a service they are `rejected`.
//
// No threads of its own and time passed in: every member is a step a test can take.
class Session {
public:
  using Clock = std::function<Instant()>;
  static constexpr std::size_t max_queued = 32;
  static constexpr std::size_t max_queued_stops = 16;
  // An observed state older than this counts as a stale link.
  static constexpr auto status_max_age = std::chrono::milliseconds{100};
  // Control periods the jog-stop level must have been found low before a jog is sent: the first
  // may be the one in which the pin fell.
  static constexpr int settled_periods = 2;

  // The port, the store and the tool-model service (may be null) must outlive the session.
  // Throws std::invalid_argument for invalid limits.
  Session(ControllerPort& port, const Limits& limits, const ProgramStore& programs,
          ToolModelService* tool_models = nullptr);

  // Call on every page load. A stop decision; forgets the previous page's ready and ID state and
  // its waiting requests. Messages carrying an earlier token are dropped from here on.
  [[nodiscard]] auto begin_page() -> PageToken;

  // Call when the page unloads or the window closes without a replacement. A stop decision;
  // every message is dropped until the next begin_page. Ignored for a token that is not current.
  void end_page(PageToken page);

  // A request from the page. The outcome holds what is answered at once; the results of queued
  // requests come out of `work`.
  [[nodiscard]] auto submit(PageToken page, std::string_view json, Instant now) -> Outcome;

  // The controller state as just read.
  void observe(const ControllerState& state, JogLease::Time now);

  // The dead-man, once per control period, well inside JogLease::timeout. True while the
  // jog-stop level must be high.
  [[nodiscard]] auto control(Instant now, const ControlInput& input) -> bool;

  // Executes at most one piece of work: a stop request, the jog stops that are due, or the next
  // request. Blocks as long as the port does. The clock is read when messages are stamped. Also
  // collects the controller's operator messages for the page.
  [[nodiscard]] auto work(const Clock& clock) -> Work;

  // Waits until there may be work, at most `timeout`.
  void wait_for_work(std::chrono::milliseconds timeout);

  // The end of the session: a stop decision, no page, nothing waiting but stop requests. `work`
  // then still executes those and stops each axis that needs it once; it is finished when it
  // returns `idle`. From here on `control` returns true for good.
  void close();

private:
  struct Reply {
    CommandOutcome outcome{CommandOutcome::accepted};
    std::string reason;
  };
  struct HeldJog {
    int direction{};
    double velocity{};
    JogSpace space{};
    friend auto operator==(const HeldJog&, const HeldJog&) -> bool = default;
  };
  struct AxisJog {
    // A press waits in the queue or its start is in flight; the lease already runs.
    bool pressed{};
    int press_direction{};
    double press_velocity{};
    // The controller accepted a start and no stop decision followed.
    bool running{};
    HeldJog held;
    // The worker is inside the port with a start or increment for this axis.
    bool in_flight{};
    // The controller must still accept a jog stop for this axis.
    bool stop_needed{};
    // The last attempt failed; reported.
    bool stop_failed{};
    // A control period has passed since: the worker tries again.
    bool retry_due{};
    // An increment was acknowledged by the controller and no jog stop was accepted since: the
    // axis may be moving, or may yet begin to. A later increment takes its place.
    bool stepping{};
    HeldJog step;
    // A jog or press on this axis was ended by something other than its release: a release
    // must come before the next start.
    bool latched{};
  };
  struct Queued {
    PageToken page;
    std::uint64_t epoch{};
    std::uint64_t cancels{};
    RequestId id{};
    Command command;
    // Something was sent to the controller for it already: a refusal is `failed`, not `rejected`.
    bool sent{};
    // It only stops something (abort, E-stop, power off, jog release, stop-all, spindle stop,
    // coolant off): executed whatever became of its page, and never withdrawn.
    bool stop{};
  };
  enum class StageKind : std::uint8_t { reply, cancelled, blocked };
  struct Stage {
    StageKind kind{StageKind::reply};
    Reply reply;
    std::vector<std::string> produced; // messages sent before the result
  };

  [[nodiscard]] static auto reply(ActionResult result) -> Reply;
  [[nodiscard]] static auto answer(Reply reply) -> Stage;
  [[nodiscard]] static auto cancelled(std::string partial = {}) -> Stage;
  [[nodiscard]] auto in_order(RequestId id) -> bool;
  [[nodiscard]] auto status(JogLease::Time now) const -> ControllerState;
  [[nodiscard]] auto current(const Queued& request) const -> bool;
  [[nodiscard]] auto reconciled() const -> bool;
  [[nodiscard]] auto stops_due(bool retries) const -> bool;
  void stop_jogs(std::optional<std::size_t> keep_press = {},
                 std::optional<std::size_t> released = {});
  void need_stop(std::size_t index);
  void wake();
  void report(Outcome& outcome, std::chrono::milliseconds unix_time);

  // Worker only.
  [[nodiscard]] auto run(Queued request, const Clock& clock) -> Work;
  [[nodiscard]] auto wanted(const Queued& request) const -> bool;
  [[nodiscard]] auto token(const Queued& request) const -> Wanted;
  [[nodiscard]] auto act(const Queued& request,
                         const std::function<ActionResult(const Wanted&)>& send) const -> Stage;
  [[nodiscard]] static auto staged(ActionResult result) -> Stage;
  void drop_requests();
  [[nodiscard]] auto execute(Queued& request) -> Stage;
  [[nodiscard]] auto jog_step(Queued& request, const JogStep& command, const Decision& decision)
      -> Stage;
  [[nodiscard]] auto perform(Queued& request, const Decision& decision,
                             const ControllerState& state) -> Stage;
  [[nodiscard]] auto jog(Queued& request, const Jog& command, const Decision& decision) -> Stage;
  [[nodiscard]] auto start_gate(Queued& request, std::size_t index, bool held)
      -> std::optional<Stage>;
  [[nodiscard]] auto stop_all() -> ActionResult;
  [[nodiscard]] auto tracked_stop(std::size_t index) -> ActionResult;
  [[nodiscard]] auto mdi(const Queued& request, const std::string& text) const -> Stage;
  [[nodiscard]] auto list_directory(const ListDirectory& request, Stage& step) -> Reply;
  void flush_stops();
  void cancel_jogs(Queued* keep_press);
  void forget_press(const Queued& request);
  void reject_press(const Queued& request);

  // Ordered by size: the analyzer counts padding.
  ControllerPort& port_;
  const ProgramStore& programs_;
  ToolModelService* tool_models_;
  std::uint64_t pages_{};
  JogLease::Time status_at_{};
  // Goes up with every stop decision.
  std::uint64_t epoch_{};
  // Goes up with every abort, E-stop and power off.
  std::uint64_t cancels_{};
  std::optional<RequestId> last_id_;
  // Failed stops that are not the result of a request; sent as operator messages.
  std::vector<std::string> problems_;
  mutable std::mutex mutex_;
  std::condition_variable work_ready_;
  JogLease leases_;
  const CommandPolicy policy_;
  std::deque<Queued> stops_;
  std::deque<Queued> requests_;
  ControllerState status_;
  std::array<AxisJog, 3> jog_{};
  // Consecutive control periods that found the level low, up to settled_periods.
  int low_periods_{settled_periods};
  bool woken_{};
  bool closing_{};
  bool page_open_{};
  bool ready_{};
  bool have_status_{};
  bool hold_{};
  bool jog_stop_high_{};
  // The last thing the worker did was retrying stops: a waiting request is served next.
  bool retried_{};
};

} // namespace coffeecnc

#endif // COFFEECNC_APP_SESSION_HPP
