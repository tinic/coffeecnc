#ifndef COFFEECNC_APP_SESSION_HPP
#define COFFEECNC_APP_SESSION_HPP

#include <coffeecnc/app/preview.hpp>
#include <coffeecnc/app/program_store.hpp>
#include <coffeecnc/app/request.hpp>
#include <coffeecnc/app/response.hpp>
#include <coffeecnc/core/command_policy.hpp>
#include <coffeecnc/core/controller_port.hpp>
#include <coffeecnc/core/diagnostics.hpp>
#include <coffeecnc/core/jog_lease.hpp>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace coffeecnc {
class MachineDataService;
class OffsetHistoryStore;
class ProgramWriter;


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
      -> std::optional<LocalizedReason> = 0;
  [[nodiscard]] virtual auto submit_operator_settings(PageToken, RequestId, SetOperatorSettings&&)
      -> std::optional<std::string> {
    return "Operator settings storage is not part of this session.";
  }
  // This page and every one before it has ended: what waits for them is dropped, and what
  // they are still owed is not sent.
  virtual void page_ended(PageToken page) = 0;
  // A saved probe keeps its protection until explicitly reclassified, even if the table row
  // is absent or has changed. Null means a configured catalog cannot be checked yet.
  [[nodiscard]] virtual auto touch_probe_loaded(int) -> std::optional<bool> { return false; }

protected:
  ToolModelService(const ToolModelService&) = default;
  ToolModelService(ToolModelService&&) = default;
  auto operator=(const ToolModelService&) -> ToolModelService& = default;
  auto operator=(ToolModelService&&) -> ToolModelService& = default;
};

// Where the two diagnostics commands go: HAL readings on a thread of their own (HalDiagnostics).
class DiagnosticsService {
public:
  DiagnosticsService() = default;
  virtual ~DiagnosticsService() = default;

  // Start (`on`) or stop sending `diagnostics` messages to `page`; a page watched before is no
  // longer. Called with the session's mutex held: must not block or call the session.
  virtual void watch(PageToken page, bool on) = 0;
  // Starts the tool as a program of its own. A reason for the operator when it is not started.
  // Called without the session's mutex: it may take as long as starting a process does.
  [[nodiscard]] virtual auto launch(HalTool tool) -> std::optional<std::string> = 0;
  // This page and every one before it has ended: nothing more is sent for them.
  virtual void page_ended(PageToken page) = 0;

protected:
  DiagnosticsService(const DiagnosticsService&) = default;
  DiagnosticsService(DiagnosticsService&&) = default;
  auto operator=(const DiagnosticsService&) -> DiagnosticsService& = default;
  auto operator=(DiagnosticsService&&) -> DiagnosticsService& = default;
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
  // A `quit` was accepted: close the application once `outcome` has been sent.
  bool quit{};
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
// Every queued request carries its page, the jog epoch and the cancellation count it was queued
// in. The worker asks whether a request is still wanted after every call that can block (a
// status reading, the program store, a command) and before every command it sends: a request of
// an ended page sends nothing more and gets no result; one overtaken by a stop or a stale panel
// sends nothing more and is answered as cancelled: `rejected` when nothing at all was sent
// for it, `failed` with what was sent and what was not when part of it was (a mode change, the
// first of two commands); a jog start or increment of an earlier
// epoch is not sent. The port is handed the same question (`Wanted`) with every action and asks
// it down to the moment it writes each command. Requests that only stop something (abort,
// E-stop, jog release, stop-all, spindle stop, coolant off) are executed whatever became of
// their page. The watched panel's power-off request requires a current frame because the
// protocol uses the same request to reset E-stop.
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
// A start in flight when a stop decision is made can reach motion after the level has risen,
// and motion refuses it under motion.jog-inhibit ("Cannot jog while jog-inhibit is active.").
// That start is answered `accepted`, with one `jog_late_start` line on stderr: the press ended
// before its jog began, and nothing moved. When a control period read the watchdog tripped
// after the start was sent, the answer is `failed` with watchdog_stop_text. A refusal without a
// stop decision after the send, before any control period raised the level for that decision,
// or while the last control period is older than control_fresh, stays `failed` with LinuxCNC's
// text: its cause is not known to be CoffeeCNC's. A control thread that has not run for longer
// may have let the watchdog trip without reading it. An increment refused that
// way is `failed` with LinuxCNC's text and is a stop decision, as every increment that is not
// accepted.
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
// `restore_offset` writes back the `before` values of an offset history entry through MDI:
// G10 L2 for a work system, G52 for G92/G52, G10 L1 for a tool's length and radius. Refused for
// an entry the history no longer holds or one with an unknown or removed value. A length of the
// tool in the spindle is applied again in the same request, as `after_tool_length_write` says:
// `G43 H<tool>` after the G10 L1 when the loaded tool's length is in effect.
//
// `set_tool_model_defaults` and `set_tool_model_assignment` are answered by the tool-model
// service, whatever the machine and the link are doing: they pass the page, ready and ID checks
// in `submit` and are handed over there. They never wait in the request queues, so the worker
// and the controller never see them. Without a service they are `rejected`.
//
// `watch_diagnostics` and `launch_hal_tool` are answered at once by the diagnostics service,
// whatever the machine and the link are doing; without a service they are `rejected`. The
// diagnostics of a page stop when it ends.
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
  static constexpr auto panel_frame_timeout = std::chrono::milliseconds{500};

  // The port, the store and the tool-model service (may be null) must outlive the session.
  // The offset history (may be null) is told which panel command may change offsets while it
  // runs, and serves `restore_offset`. Throws std::invalid_argument for invalid limits.
  Session(ControllerPort& port, const Limits& limits, const ProgramStore& programs,
          ToolModelService* tool_models = nullptr, PreviewWorker* preview = nullptr,
          PreviewEngine* restart_engine = nullptr, ProgramWriter* program_writer = nullptr,
          OffsetHistoryStore* history = nullptr, DiagnosticsService* diagnostics = nullptr,
          bool asynchronous_copies = false, MachineDataService* machine_data = nullptr);

  // Call on every page load. A stop decision; forgets the previous page's ready and ID state and
  // its waiting requests. Messages carrying an earlier token are dropped from here on.
  [[nodiscard]] auto begin_page(bool watch_frames = false,
                                std::optional<Instant> frame_clock = {}) -> PageToken;

  // Call when the page unloads or the window closes without a replacement. A stop decision;
  // every message is dropped until the next begin_page. Ignored for a token that is not current.
  void end_page(PageToken page);

  // An animation-frame timestamp from the current document, outside the command stream.
  void panel_frame(PageToken page, std::chrono::milliseconds rendered_at, Instant now);

  // A request from the page. The outcome holds what is answered at once; the results of queued
  // requests come out of `work`.
  [[nodiscard]] auto submit(PageToken page, std::string_view json, Instant now,
                            std::chrono::milliseconds rendered_at = {}) -> Outcome;

  // The controller state as just read.
  void observe(const ControllerState& state, JogLease::Time now);

  // The program LinuxCNC has open, as last observed; empty before the first state or with none.
  // Any thread; does not wait for the controller.
  [[nodiscard]] auto loaded_program() const -> std::string;

  // A disk worker has already written and synchronized a temporary program. Send only its
  // atomic publication through the controller worker, serialized against program start.
  using Publication = std::function<std::expected<void, std::string>()>;
  [[nodiscard]] auto publish_program(std::string path, Publication publish)
      -> std::future<std::expected<void, std::string>>;

  // The dead-man, once per control period, well inside JogLease::timeout. True while the
  // jog-stop level must be high.
  [[nodiscard]] auto control(Instant now, const ControlInput& input) -> bool;

  // Executes at most one piece of work: a stop request, the jog stops that are due, or the next
  // request. Blocks as long as the port does. The clock is read when messages are stamped. Also
  // collects the controller's operator messages for the page.
  [[nodiscard]] auto work(const Clock& clock) -> Work;

  // On the disk thread, waits for one copy. A disengaged result means shutdown.
  [[nodiscard]] auto copy_work(ProgramWriter& writer) -> std::optional<Outcome>;

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
    std::optional<RefusalMessage> refusal{};
    std::optional<MessageKey> localization{};
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
    // A stop that is carried out even if its page is gone.
    bool stop{};
  };
  struct ProgramPublication {
    std::string path;
    Publication publish;
    std::promise<std::expected<void, std::string>> result;
  };
  struct ProgramCopy {
    PageToken page;
    RequestId id;
    ManageProgramFile command;
  };
  void publish_program(ProgramPublication& request, const Clock& clock);
  enum class StageKind : std::uint8_t { reply, cancelled, blocked };
  struct Stage {
    StageKind kind{StageKind::reply};
    Reply reply;
    std::vector<std::string> produced; // messages sent before the result
    std::optional<SetterState> setter{};
    bool quit{};                       // belongs only to this request and its page
    std::optional<RestartPlan> restart{};
  };
  struct PreparedRestart {
    RestartPlan plan;
    double programmed_clearance{};
    std::uint64_t context{};
    Queued owner;
    RestartSource source;
    PreviewComputation::Watch files_changed;
    std::optional<std::uint64_t> tool_request;
    CheckedPreview measured_run{}; // original program replayed with the new tool table
  };

  [[nodiscard]] static auto reply(ActionResult result) -> Reply;
  [[nodiscard]] static auto answer(Reply reply) -> Stage;
  [[nodiscard]] static auto refuse(std::string reason, RefusalMessage message) -> Stage;
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
  void expire_panel_frame(JogLease::Time now);
  [[nodiscard]] auto frame_age(Instant now, std::chrono::milliseconds rendered_at) const
      -> std::chrono::milliseconds;
  [[nodiscard]] auto execute(Queued& request, const Clock& clock) -> Stage;
  [[nodiscard]] auto check_program_start(const Queued& request, const Run& command,
                                         const ControllerState& state,
                                         const CheckedPreview& check) const
      -> std::optional<ProgramCheckIssue>;
  [[nodiscard]] auto probe_block(const Command& command, const ControllerState& state) const -> std::string;
  [[nodiscard]] auto plan_restart(Queued& request, PlanRestart command,
                                  const ControllerState& state) -> Stage;
  [[nodiscard]] auto start_restart(const Queued& request, StartRestart command) -> Stage;
  [[nodiscard]] auto resume_restart(const Queued& request, const ControllerState& state) -> Stage;
  [[nodiscard]] auto measure_tool_change(Queued& request, const ControllerState& state) -> Stage;
  [[nodiscard]] auto send_restart(const Queued& request, const PreparedRestart& restart,
                                  bool after_change) -> Stage;
  [[nodiscard]] auto jog_step(Queued& request, const JogStep& command, const Decision& decision)
      -> Stage;
  [[nodiscard]] auto perform(Queued& request, const Decision& decision,
                             const ControllerState& state, const Clock& clock,
                             const std::optional<CheckedPreview>& verified) -> Stage;
  [[nodiscard]] auto jog(Queued& request, const Jog& command, const Decision& decision,
                         const Clock& clock) -> Stage;
  [[nodiscard]] auto start_gate(Queued& request, std::size_t index, bool held)
      -> std::optional<Stage>;
  [[nodiscard]] auto stop_all() -> ActionResult;
  [[nodiscard]] auto tracked_stop(std::size_t index) -> ActionResult;
  [[nodiscard]] auto mdi(const Queued& request, const std::string& text) const -> Stage;
  [[nodiscard]] auto restore_offset(const Queued& request, const RestoreOffset& command,
                                    const ControllerState& state, const Clock& clock) -> Stage;
  [[nodiscard]] auto list_directory(const ListDirectory& request, Stage& step) -> Reply;
  void flush_stops();
  void cancel_jogs(Queued* keep_press);
  void forget_press(const Queued& request);
  void reject_press(const Queued& request);

  // Ordered by size: the analyzer counts padding.
  ControllerPort& port_;
  const ProgramStore& programs_;
  ToolModelService* tool_models_;
  PreviewWorker* preview_;
  PreviewEngine* restart_engine_;
  ProgramWriter* program_writer_;
  OffsetHistoryStore* history_;
  DiagnosticsService* diagnostics_;
  MachineDataService* machine_data_; // backup and restore, on the worker thread
  std::optional<PreparedRestart> restart_; // worker-owned
  std::uint64_t restart_id_{};
  // Worker-owned. A paused run is eligible only after CoffeeCNC checked its initial start.
  std::string verified_probe_program_;
  CheckedPreview started_program_; // worker-owned exact source of a checked AUTO start
  std::uint64_t pages_{};
  JogLease::Time status_at_{};
  // Goes up with every stop decision.
  std::uint64_t epoch_{};
  // Goes up with every abort, E-stop, power off and panel watchdog trip.
  std::uint64_t cancels_{};
  std::uint64_t setter_datum_{};
  std::uint64_t setter_tool_{};
  // The epoch in which a control period last read the watchdog tripped; 0 before any.
  std::uint64_t watchdog_epoch_{};
  // The epoch in which a control period last asked for the jog-stop level high; 0 before any.
  // Only a period after a stop decision raises the level that can refuse a start in flight.
  std::uint64_t level_epoch_{};
  // When the last control period ran.
  JogLease::Time control_at_{};
  JogLease::Time last_panel_frame_received_{};
  std::optional<Instant> panel_frame_clock_;
  std::optional<Instant> last_panel_frame_;
  std::optional<std::chrono::milliseconds> panel_recovered_at_;
  std::optional<RequestId> last_id_;
  // Failed stops that are not the result of a request; sent as operator messages.
  std::vector<ControllerMessage> problems_;
  mutable std::mutex mutex_;
  std::condition_variable work_ready_;
  JogLease leases_;
  const CommandPolicy policy_;
  std::deque<Queued> stops_;
  std::deque<Queued> requests_;
  std::optional<ProgramCopy> copy_;
  std::optional<ProgramPublication> publication_;
  ControllerState status_;
  std::array<AxisJog, 3> jog_{};
  // Consecutive control periods that found the level low, up to settled_periods.
  int low_periods_{settled_periods};
  bool woken_{};
  bool closing_{};
  bool copying_{};
  const bool asynchronous_copies_;
  bool page_open_{};
  bool ready_{};
  bool watch_frames_{};
  bool setter_busy_{};
  bool restart_busy_{};
  bool have_status_{};
  bool hold_{};
  bool jog_stop_high_{};
  // The last thing the worker did was retrying stops: a waiting request is served next.
  bool retried_{};
};

} // namespace coffeecnc

#endif // COFFEECNC_APP_SESSION_HPP
