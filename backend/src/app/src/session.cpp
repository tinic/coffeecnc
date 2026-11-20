#include <coffeecnc/app/session.hpp>

#include <coffeecnc/app/messages.hpp>
#include <coffeecnc/core/jog_stop_report.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <format>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

namespace coffeecnc {
namespace {

constexpr std::array<Axis, 3> axes{Axis::x, Axis::y, Axis::z};
constexpr std::array<std::string_view, 9> wcs_words{"G54", "G55",   "G56",   "G57",  "G58",
                                                    "G59", "G59.1", "G59.2", "G59.3"};
// LinuxCNC's LINELEN is 255 including the terminator.
constexpr std::size_t max_mdi_bytes = 254;

auto letter(Axis axis) -> char {
  switch (axis) {
  case Axis::x:
    return 'X';
  case Axis::y:
    return 'Y';
  case Axis::z:
    return 'Z';
  }
  return '?';
}

// A G-code real: the shortest decimal that reads back as the same double, never an exponent,
// independent of the locale. Empty when the value cannot be written.
auto real(double value) -> std::string {
  if (value == 0)
    return "0"; // also for negative zero
  std::array<char, 512> buffer{};
  const auto [end, error] =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::fixed);
  if (error != std::errc{})
    return {};
  return {buffer.data(), end};
}

// A protocol length (millimetres) as the interpreter will read it.
auto length(double millimetres, const ControllerState& state) -> std::string {
  constexpr double millimetres_per_inch = 25.4;
  return real(state.program_units == LengthUnit::inch ? millimetres / millimetres_per_inch
                                                      : millimetres);
}

constexpr auto busy_reason = "Too many requests are waiting for LinuxCNC.";
constexpr auto cancelled_reason = "Cancelled by a stop request before it was sent.";
constexpr auto mode_changed_reason =
    "The mode was changed, but a stop request cancelled the command before it was sent.";
constexpr auto touch_off_reason = "The tool length was set, but a stop request cancelled the "
                                  "command before it was applied (G43 was not sent).";
constexpr auto stopped_reason = "The jog was stopped before it started.";
constexpr auto release_reason = "Release the jog control before jogging again.";
constexpr auto hold_reason = "A jog stop is in progress.";
// How old the last control period may be for the watchdog reading it made to count as current:
// ten 10 ms control periods, below the 300 ms timeout of the HAL watchdog in the post-GUI file.
constexpr auto control_fresh = std::chrono::milliseconds{100};
constexpr auto no_tool_models_reason = "Tool model storage is not part of this session.";
constexpr auto probe_loaded_reason = "A touch probe is loaded";
constexpr std::string_view probe_code_unknown =
    "A touch probe is loaded. CoffeeCNC could not verify that the code leaves the spindle off.";

// Check the accepted subset of RS274 before dispatch. LinuxCNC ignores case and spaces between
// digits. O-words, indirect M-codes, state restore and custom M-codes can hide spindle actions.
auto probe_code_block(std::string_view line) -> std::string_view {
  std::string code;
  std::string comment;
  bool inside = false;
  bool semicolon = false;
  const auto active_comment = [&]() {
    return comment.starts_with("py,") || comment.starts_with("pyrun,") ||
           comment.starts_with("pyreload");
  };
  for (char c : line) {
    if (c == '\0' || c == '\r' || c == '\n')
      return probe_code_unknown;
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
    if (c == ' ' || c == '\t' || c == '\v' || c == '\f')
      continue;
    if (semicolon) {
      comment += c;
      continue;
    }
    if (inside) {
      if (c == '(')
        return probe_code_unknown;
      if (c != ')') {
        comment += c;
        continue;
      }
      if (active_comment())
        return probe_code_unknown;
      comment.clear();
      inside = false;
    } else if (c == ';') {
      // LinuxCNC can interpret the final semicolon comment as an active comment too.
      semicolon = true;
    } else if (c == '(') {
      inside = true;
    } else {
      code += c;
    }
  }
  if (inside || active_comment())
    return probe_code_unknown;
  if (code.find_first_of("o#[]<>") != std::string::npos)
    return probe_code_unknown;
  for (std::size_t pos = code.find('g'); pos != std::string::npos; pos = code.find('g', pos + 1)) {
    const char* first = code.data() + pos + 1;
    const char* last = code.data() + code.size();
    if (first != last && *first == '+')
      ++first;
    double value = 0;
    const auto [end, error] = std::from_chars(first, last, value, std::chars_format::fixed);
    if (error != std::errc{} || end == first || !std::isfinite(value) || value < 0 || value > 999)
      return probe_code_unknown;
    const auto number = static_cast<int>(std::round(value * 10));
    // Rigid tapping, tapping and boring cycles can reverse or restart the spindle themselves.
    if (number == 331 || number == 740 || number == 840 || number == 860 || number == 880)
      return probe_loaded_reason;
  }
  constexpr std::array ordinary_m_codes{0, 1, 2, 5, 6, 7, 8, 9, 30, 48, 49, 50, 51, 52, 53,
                                       60, 61, 62, 63, 64, 65, 66, 67, 68, 70, 71};
  for (std::size_t pos = code.find('m'); pos != std::string::npos; pos = code.find('m', pos + 1)) {
    const char* first = code.data() + pos + 1;
    const char* last = code.data() + code.size();
    if (first != last && *first == '+')
      ++first;
    double value = 0;
    const auto [end, error] = std::from_chars(first, last, value, std::chars_format::fixed);
    if (error != std::errc{} || end == first || !std::isfinite(value) || value < 0 || value > 199)
      return probe_code_unknown;
    const auto number = static_cast<int>(std::round(value));
    if (number == 3 || number == 4 || number == 19)
      return probe_loaded_reason;
    if (value != number || std::ranges::find(ordinary_m_codes, number) == ordinary_m_codes.end())
      return probe_code_unknown;
  }
  return {};
}

// The tool-model command in a request, if it is one.
auto tool_model_command(Command& command) -> std::optional<ToolModelCommand> {
  if (auto* defaults = std::get_if<SetToolModelDefaults>(&command))
    return std::move(*defaults);
  if (auto* assignment = std::get_if<SetToolModelAssignment>(&command))
    return std::move(*assignment);
  return {};
}

// Abort, E-stop and power off: whatever waits must not run after them.
auto cancels_requests(const Command& command) -> bool {
  if (const auto* machine = std::get_if<SetMachine>(&command))
    return machine->state != MachineState::on;
  return std::holds_alternative<Abort>(command);
}

// What stops something is carried out whatever became of the request behind it.
auto only_stops(const Command& command) -> bool {
  if (const auto* jog = std::get_if<Jog>(&command))
    return !jog->held;
  if (const auto* spindle = std::get_if<SetSpindle>(&command))
    return spindle->direction == SpindleDirection::stopped;
  if (const auto* coolant = std::get_if<SetCoolant>(&command))
    return !coolant->flood && !coolant->mist;
  return cancels_requests(command) || std::holds_alternative<JogStopAll>(command);
}

// A held jog or an increment: motion that the jog-stop level stops.
auto starts_jog(const Command& command) -> bool {
  if (const auto* jog = std::get_if<Jog>(&command))
    return jog->held;
  return std::holds_alternative<JogStep>(command);
}

} // namespace

Session::Session(ControllerPort& port, const Limits& limits, const ProgramStore& programs,
                 ToolModelService* tool_models)
    : port_(port), programs_(programs), tool_models_(tool_models), policy_(limits) {}

auto Session::reply(ActionResult result) -> Reply {
  switch (result.status) {
  case ActionStatus::accepted:
    return {};
  case ActionStatus::timed_out:
    return {CommandOutcome::timed_out,
            result.reason.empty() ? "LinuxCNC did not answer." : std::move(result.reason)};
  case ActionStatus::cancelled:
    // Only where the port withdrew something that is never withdrawn; not `rejected`, since
    // something may have been sent.
    return {CommandOutcome::failed, cancelled_reason};
  case ActionStatus::failed:
    break;
  }
  return {CommandOutcome::failed,
          result.reason.empty() ? "LinuxCNC reported an error." : std::move(result.reason)};
}

auto Session::answer(Reply reply) -> Stage { return {StageKind::reply, std::move(reply), {}}; }
// `partial` says what was sent before the request was cancelled; empty when nothing was.
auto Session::cancelled(std::string partial) -> Stage {
  return {StageKind::cancelled, {CommandOutcome::failed, std::move(partial)}, {}};
}
// What the port made of an action: its result, or the request's cancellation.
auto Session::staged(ActionResult result) -> Stage {
  if (result.status == ActionStatus::cancelled)
    return cancelled();
  return answer(reply(std::move(result)));
}

auto Session::begin_page() -> PageToken {
  const std::scoped_lock lock{mutex_};
  problems_.clear(); // The page they were for is gone.
  stop_jogs();       // A stop that fails is reported to the new page and retried.
  // Whatever the old page held, the new page has pressed nothing yet.
  for (auto& axis : jog_)
    axis.latched = false;
  drop_requests();
  ready_ = false;
  last_id_.reset();
  page_open_ = !closing_;
  if (tool_models_ != nullptr)
    tool_models_->page_ended(PageToken{pages_});
  return PageToken{++pages_};
}

void Session::end_page(PageToken page) {
  const std::scoped_lock lock{mutex_};
  if (!page_open_ || page != PageToken{pages_})
    return;
  problems_.clear();
  stop_jogs();
  drop_requests();
  ready_ = false;
  last_id_.reset();
  page_open_ = false;
  if (tool_models_ != nullptr)
    tool_models_->page_ended(page);
}

void Session::close() {
  const std::scoped_lock lock{mutex_};
  closing_ = true;
  problems_.clear();
  stop_jogs();
  drop_requests();
  page_open_ = false;
  if (tool_models_ != nullptr)
    tool_models_->page_ended(PageToken{pages_});
  wake();
}

auto Session::submit(PageToken page, std::string_view json, Instant now) -> Outcome {
  Outcome outcome;
  outcome.page = page;
  auto decoded = decode_request(json);
  const std::scoped_lock lock{mutex_};
  if (!page_open_ || page != PageToken{pages_})
    return outcome;
  if (const auto* error = std::get_if<DecodeError>(&decoded)) {
    if (!error->id)
      outcome.messages.push_back(encode_message(
          MessageLevel::error, "Panel request ignored: " + error->reason, now.unix_time));
    else if (in_order(*error->id))
      outcome.messages.push_back(
          encode_result(*error->id, CommandOutcome::rejected, error->reason));
    return outcome;
  }
  auto& request = std::get<Request>(decoded);
  if (!in_order(request.id))
    return outcome;
  const auto result = [&](CommandOutcome verdict, std::string_view reason) -> void {
    outcome.messages.push_back(encode_result(request.id, verdict, reason));
    report(outcome, now.unix_time);
  };
  if (std::holds_alternative<Ready>(request.command)) {
    ready_ = true;
    outcome.send_snapshots = true;
    result(CommandOutcome::accepted, "");
    return outcome;
  }
  if (!ready_) {
    result(CommandOutcome::rejected, "Panel has not finished loading.");
    return outcome;
  }

  if (auto* settings = std::get_if<SetOperatorSettings>(&request.command)) {
    const auto decision = policy_.evaluate(request.command, status(now.monotonic));
    if (!decision.allowed)
      result(CommandOutcome::rejected, decision.reason);
    else if (tool_models_ == nullptr)
      result(CommandOutcome::rejected, "Operator settings storage is not part of this session.");
    else if (const auto refused =
                 tool_models_->submit_operator_settings(page, request.id, std::move(*settings)))
      result(CommandOutcome::rejected, *refused);
    return outcome;
  }

  // Visual preferences: not the controller's business and not gated by its state. The policy
  // is asked all the same, so that it stays the one place that says what is permitted.
  if (std::holds_alternative<SetToolModelDefaults>(request.command) ||
      std::holds_alternative<SetToolModelAssignment>(request.command)) {
    auto decision = policy_.evaluate(request.command, status(now.monotonic));
    auto models = tool_model_command(request.command);
    if (!decision.allowed || !models)
      result(CommandOutcome::rejected, decision.reason);
    else if (tool_models_ == nullptr)
      result(CommandOutcome::rejected, no_tool_models_reason);
    else if (const auto refused = tool_models_->submit(page, request.id, std::move(*models)))
      result(CommandOutcome::rejected, *refused);
    return outcome;
  }

  // Carried out whatever becomes of its page; in order with the other requests unless it is
  // one of those that go first.
  if (setter_busy_ && std::holds_alternative<Pause>(request.command))
    request.command = Abort{};
  const bool stops = only_stops(request.command);
  if (setter_busy_ && !stops) {
    result(CommandOutcome::rejected, "A fixed setter operation is in progress.");
    return outcome;
  }
  bool stop_request = cancels_requests(request.command);
  if (const auto* asked = std::get_if<Jog>(&request.command)) {
    if (const auto index = axis_index(asked->axis); index && asked->held) {
      auto& axis = jog_.at(*index);
      // A jog that was stopped for the operator is not started again by the refreshes of the
      // press that began it: only a press made after a release starts a jog.
      if (axis.latched) {
        result(CommandOutcome::rejected, release_reason);
        return outcome;
      }
      const bool press = axis.pressed && axis.press_direction == asked->direction &&
                         axis.press_velocity == asked->velocity;
      const bool held = !axis.pressed && axis.running && axis.held.direction == asked->direction &&
                        axis.held.velocity == asked->velocity;
      if (leases_.active(asked->axis) && (press || held)) {
        // A refresh. The dead-man is renewed here, not behind whatever the worker is doing.
        const auto state = status(now.monotonic);
        auto decision = policy_.evaluate(*asked, state);
        const bool as_it_is = decision.allowed &&
                              (!decision.required_mode || *decision.required_mode == state.mode) &&
                              (press || decision.jog_space == axis.held.space);
        if (as_it_is) {
          leases_.refresh(asked->axis, now.monotonic);
          result(CommandOutcome::accepted, "");
          return outcome;
        }
        // The observed state no longer allows the jog as it is: it ends here, not when its
        // lease expires, and not by starting it anew.
        stop_jogs();
        result(CommandOutcome::rejected,
               decision.allowed ? std::string(release_reason) : std::move(decision.reason));
        return outcome;
      }
      if (requests_.size() >= max_queued) {
        result(CommandOutcome::rejected, busy_reason);
        return outcome;
      }
      // A press. Its lease runs from now: a start that has to wait is not sent once the lease
      // has run out.
      axis.pressed = true;
      axis.press_direction = asked->direction;
      axis.press_velocity = asked->velocity;
      leases_.refresh(asked->axis, now.monotonic);
    } else if (index) {
      // Always stop, with or without a lease: the release may be all that is left of a jog.
      stop_jogs({}, index);
      need_stop(*index);
      stop_request = true;
    }
  } else if (std::holds_alternative<JogStopAll>(request.command)) {
    stop_jogs();
    for (std::size_t i = 0; i < jog_.size(); ++i)
      need_stop(i);
    stop_request = true;
  } else if (stop_request) {
    stop_jogs();
    ++cancels_;
    for (const auto& waiting : requests_)
      if (!waiting.stop)
        outcome.messages.push_back(
            encode_result(waiting.id, CommandOutcome::rejected, cancelled_reason));
    drop_requests();
  }
  auto& queue = stop_request ? stops_ : requests_;
  if (queue.size() >= (stop_request ? max_queued_stops : max_queued)) {
    // The stop decision above stands; only the controller command is not queued again.
    result(CommandOutcome::rejected, busy_reason);
    return outcome;
  }
  queue.push_back({page, epoch_, cancels_, request.id, std::move(request.command), false, stops});
  wake();
  return outcome;
}

void Session::observe(const ControllerState& state, JogLease::Time now) {
  const std::scoped_lock lock{mutex_};
  if (state.link != Link::live || state.machine != MachineState::on ||
      (have_status_ && now - status_at_ > status_max_age) ||
      std::ranges::any_of(state.axes, [](const auto& axis) { return axis.homing; }) ||
      (have_status_ &&
       !std::ranges::equal(state.axes, status_.axes,
                           [](const auto& a, const auto& b) { return a.homed == b.homed; })))
    ++setter_datum_;
  if (state.tool_in_spindle != status_.tool_in_spindle ||
      state.tool_request != status_.tool_request)
    ++setter_tool_;
  status_ = state;
  status_at_ = now;
  have_status_ = true;
}

auto Session::control(Instant now, const ControlInput& input) -> bool {
  const std::scoped_lock lock{mutex_};
  jog_stop_high_ = input.jog_stop_high;
  control_at_ = now.monotonic;
  low_periods_ = jog_stop_high_ ? 0 : std::min(low_periods_ + 1, settled_periods);
  const bool leased =
      std::ranges::any_of(axes, [&](Axis axis) -> bool { return leases_.active(axis); });
  const bool stepping = std::ranges::any_of(
      jog_, [](const AxisJog& axis) -> bool { return axis.stepping || axis.in_flight; });
  // A jog without a lease has nothing that would end it.
  bool stop = false;
  for (std::size_t i = 0; i < axes.size(); ++i)
    stop = stop || (jog_[i].running && !leases_.active(axes[i]));
  if ((leased || stepping) && !stop) {
    const auto state = status(now.monotonic);
    stop = !input.watchdog_ok || state.link != Link::live || state.machine != MachineState::on;
    if (!stop) {
      const auto expired = leases_.expired(now.monotonic);
      stop = std::ranges::any_of(expired, [](bool axis) -> bool { return axis; });
    }
    // A change made outside the panel (mode, interpreter, homing, jog space) must not leave a
    // jog running until its lease expires: keep only what the policy would still start.
    // An increment that may be moving is held to the same rule, as the jog it is.
    for (std::size_t i = 0; i < axes.size() && !stop; ++i) {
      if (!jog_[i].running && !jog_[i].stepping)
        continue;
      const auto& moving = jog_[i].running ? jog_[i].held : jog_[i].step;
      const auto decision =
          policy_.evaluate(Jog{axes[i], moving.direction, moving.velocity, true}, state);
      stop = !decision.allowed || decision.jog_space != moving.space ||
             (decision.required_mode && *decision.required_mode != state.mode);
    }
  }
  if (stop)
    stop_jogs();
  // The epoch a tripped watchdog was last seen in: a start refused under jog-inhibit in an
  // earlier epoch may have been refused by the watchdog.
  if (!input.watchdog_ok)
    watchdog_epoch_ = epoch_;
  for (auto& axis : jog_) {
    if (axis.stop_needed && axis.stop_failed && !axis.retry_due && !closing_) {
      axis.retry_due = true;
      wake();
    }
  }
  // Released only after a period that saw the level high: however fast the stops were
  // accepted, the level has been on the pin.
  if (hold_ && jog_stop_high_ && reconciled()) {
    hold_ = false;
    wake();
  } else if (!hold_ && low_periods_ == settled_periods && !requests_.empty()) {
    wake(); // a start may have waited for the level to fall
  }
  // From close on the level is high for good, whatever was reconciled.
  if (hold_ || closing_)
    level_epoch_ = epoch_;
  return hold_ || closing_;
}

void Session::wait_for_work(std::chrono::milliseconds timeout) {
  std::unique_lock lock{mutex_};
  work_ready_.wait_for(lock, timeout, [&]() -> bool { return woken_; });
  woken_ = false;
}

auto Session::work(const Clock& clock) -> Work {
  std::optional<Queued> request;
  bool stopping = false;
  {
    const std::scoped_lock lock{mutex_};
    const bool fresh = stops_due(false);
    if (!stops_.empty()) {
      request = std::move(stops_.front());
      stops_.pop_front();
    } else if (fresh || (stops_due(true) && (requests_.empty() || !retried_))) {
      // A stop not tried yet goes before every request; retries take turns with them.
      stopping = true;
      retried_ = !fresh;
    } else if (!requests_.empty()) {
      request = std::move(requests_.front());
      requests_.pop_front();
      retried_ = false;
    }
  }
  if (request)
    return run(std::move(*request), clock);
  Work result;
  std::vector<ControllerMessage> published;
  if (stopping) {
    flush_stops();
    result.status = WorkStatus::done;
  } else {
    published = port_.take_messages();
  }
  const auto now = clock();
  const std::scoped_lock lock{mutex_};
  if (!page_open_)
    return result;
  result.outcome.page = PageToken{pages_};
  for (const auto& message : published)
    result.outcome.messages.push_back(encode_message(
        message.error ? MessageLevel::error : MessageLevel::info, message.text, now.unix_time));
  report(result.outcome, now.unix_time);
  return result;
}

auto Session::run(Queued request, const Clock& clock) -> Work {
  Work result;
  result.status = WorkStatus::done;
  Stage step;
  // Nothing of an ended page is sent, and nothing that a stop decision has overtaken. A stop
  // request is executed whatever became of its page.
  enum class Fate : std::uint8_t { execute, drop, cancel, stale };
  const auto fate = [&]() -> Fate {
    const std::scoped_lock lock{mutex_};
    if (!request.stop && !current(request))
      return page_open_ && request.page == PageToken{pages_} ? Fate::cancel : Fate::drop;
    return starts_jog(request.command) && request.epoch != epoch_ ? Fate::stale : Fate::execute;
  }();
  if (fate == Fate::drop)
    return result;
  if (fate == Fate::cancel) {
    step.kind = StageKind::cancelled;
  } else if (fate == Fate::stale) {
    step = answer({CommandOutcome::rejected, stopped_reason});
  } else {
    const auto* setter = std::get_if<FixedSetterRequest>(&request.command);
    const bool exclusive = setter && setter->action == SetterAction::touch_off;
    if (exclusive) {
      const std::scoped_lock lock{mutex_};
      setter_busy_ = true;
    }
    step = execute(request, clock);
    if (exclusive) {
      const std::scoped_lock lock{mutex_};
      setter_busy_ = false;
    }
  }
  if (step.kind == StageKind::blocked) {
    const std::scoped_lock lock{mutex_};
    // Still the next request: order is kept.
    if (current(request))
      requests_.push_front(std::move(request));
    result.status = WorkStatus::blocked;
    return result;
  }
  if (step.kind == StageKind::cancelled) {
    forget_press(request);
    // `rejected` only when nothing at all was sent for the request; otherwise `failed`, with
    // what was sent and what was not.
    if (step.reply.reason.empty() && request.sent)
      step.reply.reason = mode_changed_reason;
    if (step.reply.reason.empty())
      step.reply = {CommandOutcome::rejected, cancelled_reason};
  }
  // Stops decided meanwhile go out before anything else is taken up, except a stop request:
  // abort, E-stop and power off go out first, and a release stops its own axis.
  const bool stop_waits = [&]() -> bool {
    const std::scoped_lock lock{mutex_};
    return !stops_.empty();
  }();
  if (!stop_waits)
    flush_stops();
  const auto now = clock();
  const std::scoped_lock lock{mutex_};
  if (!page_open_)
    return result;
  result.outcome.page = PageToken{pages_};
  if (request.page == result.outcome.page) {
    result.quit = step.quit && current(request) && step.reply.outcome == CommandOutcome::accepted;
    for (auto& message : step.produced)
      result.outcome.messages.push_back(std::move(message));
    result.outcome.messages.push_back(
        encode_result(request.id, step.reply.outcome, step.reply.reason, step.setter));
  }
  report(result.outcome, now.unix_time);
  return result;
}

// A request at or below the last ID seen is a duplicate or a replay: its original already
// produced the one result it gets.
auto Session::in_order(RequestId id) -> bool {
  if (last_id_ && id <= *last_id_)
    return false;
  last_id_ = id;
  return true;
}

auto Session::status(JogLease::Time now) const -> ControllerState {
  auto state = status_;
  if (!have_status_)
    state.link = Link::down;
  else if (state.link == Link::live && now - status_at_ > status_max_age)
    state.link = Link::stale;
  return state;
}

auto Session::current(const Queued& request) const -> bool {
  return page_open_ && request.page == PageToken{pages_} && request.cancels == cancels_;
}

auto Session::reconciled() const -> bool {
  return std::ranges::none_of(
      jog_, [](const AxisJog& axis) -> bool { return axis.stop_needed || axis.in_flight; });
}

// A stop is waiting for the worker: one not tried yet, or with `retries` also one to try again.
auto Session::stops_due(bool retries) const -> bool {
  return std::ranges::any_of(jog_, [&](const AxisJog& axis) -> bool {
    return axis.stop_needed && (!axis.stop_failed || (retries && axis.retry_due));
  });
}

// The stop decision. With `keep_press` the press on that axis stays wanted in the new epoch;
// `released` is the axis the panel itself let go. Every other axis that had a jog or a press
// must be released before it jogs again.
void Session::stop_jogs(std::optional<std::size_t> keep_press,
                        std::optional<std::size_t> released) {
  ++epoch_;
  // The axis whose press is kept, with the lease it has; no axis when there is none.
  const std::size_t keep = keep_press.value_or(jog_.size());
  std::optional<JogLease::Time> kept;
  if (keep < jog_.size() && jog_[keep].pressed)
    kept = leases_.last_refresh(axes.at(keep));
  static_cast<void>(leases_.cancel_all());
  for (std::size_t i = 0; i < jog_.size(); ++i) {
    auto& axis = jog_[i];
    if (axis.running || axis.in_flight || axis.stepping)
      need_stop(i);
    axis.stepping = false;
    if ((axis.running || axis.pressed) && !(kept && i == keep))
      axis.latched = true;
    axis.running = false;
    axis.pressed = false;
    if (i == released)
      axis.latched = false;
    if (kept && i == keep) {
      axis.pressed = true;
      leases_.refresh(axes.at(i), *kept);
    }
  }
  wake();
}

void Session::need_stop(std::size_t index) {
  auto& axis = jog_.at(index);
  if (!axis.stop_needed) {
    axis.stop_needed = true;
    axis.stop_failed = false;
    axis.retry_due = false;
  }
  hold_ = true;
  wake();
}

void Session::wake() {
  woken_ = true;
  work_ready_.notify_all();
}

void Session::report(Outcome& outcome, std::chrono::milliseconds unix_time) {
  for (const auto& problem : problems_)
    outcome.messages.push_back(encode_message(MessageLevel::error, problem, unix_time));
  problems_.clear();
}

// The request is still to be carried out: its page shows and no abort, E-stop or power off was
// asked since. A stop request always is. Asked after every call that can block and before every
// command sent: a request that went stale while the worker waited sends nothing more.
auto Session::wanted(const Queued& request) const -> bool {
  const std::scoped_lock lock{mutex_};
  return request.stop || current(request);
}

// The same for the port to ask, down to the moment it writes. A jog start or increment is also
// not wanted any more once a stop decision has been made. Valid as long as the request is.
auto Session::token(const Queued& request) const -> Wanted {
  return [this, &request]() -> bool {
    const std::scoped_lock lock{mutex_};
    return request.stop ||
           (current(request) && (!starts_jog(request.command) || request.epoch == epoch_));
  };
}

// One action of the port, unless the request went stale before or inside it.
auto Session::act(const Queued& request,
                  const std::function<ActionResult(const Wanted&)>& send) const -> Stage {
  const auto still = token(request);
  if (!still())
    return cancelled();
  return staged(send(still));
}

// The waiting requests of a page that is gone, or that an abort overtook. What only stops
// something stays.
void Session::drop_requests() {
  std::erase_if(requests_, [](const Queued& request) -> bool { return !request.stop; });
}

auto Session::execute(Queued& request, const Clock& clock) -> Stage {
  const auto& command = request.command;
  if (std::holds_alternative<FixedSetterRequest>(command) && !port_.fixed_setter_available())
    return answer({CommandOutcome::rejected, "The fixed setter is unavailable."});
  auto state = port_.snapshot();
  if (!wanted(request))
    return cancelled();
  auto decision = policy_.evaluate(command, state);
  if (decision.allowed) {
    if (auto reason = probe_block(command, state); !reason.empty())
      decision = {false, {}, std::move(reason), {}};
  }
  if (decision.allowed && decision.required_mode && *decision.required_mode != state.mode) {
    // No jog survives a mode change; none is left running if the change fails either.
    cancel_jogs(&request);
    if (!wanted(request))
      return cancelled();
    auto changed = port_.set_task_mode(*decision.required_mode, token(request));
    if (changed.status == ActionStatus::cancelled)
      return cancelled();
    auto switched = reply(std::move(changed));
    request.sent = true;
    if (switched.outcome != CommandOutcome::accepted) {
      forget_press(request);
      return answer(std::move(switched));
    }
    if (!wanted(request))
      return cancelled();
    // The decision was made for the old mode. Decide again for the state now observed.
    state = port_.snapshot();
    if (!wanted(request))
      return cancelled();
    decision = policy_.evaluate(command, state);
    if (decision.allowed) {
      if (auto reason = probe_block(command, state); !reason.empty())
        decision = {false, {}, std::move(reason), {}};
    }
    if (decision.allowed && decision.required_mode && *decision.required_mode != state.mode) {
      forget_press(request);
      return answer({CommandOutcome::failed, "LinuxCNC did not enter the required mode."});
    }
  }
  if (!decision.allowed) {
    // A press that is not valid must not leave a jog running on its axis.
    reject_press(request);
    // After a mode change was sent this is not `rejected` (nothing sent) any more.
    return answer({request.sent ? CommandOutcome::failed : CommandOutcome::rejected,
                   std::move(decision.reason)});
  }
  const bool fresh_run = state.interp == InterpState::idle &&
      (std::holds_alternative<Run>(command) || std::holds_alternative<Step>(command));
  if (fresh_run || std::holds_alternative<OpenProgram>(command) ||
      std::holds_alternative<Mdi>(command) || cancels_requests(command))
    verified_probe_program_.clear();
  const bool checked_run = fresh_run && tool_models_ != nullptr &&
      tool_models_->touch_probe_loaded(state.tool_in_spindle) == true;
  if (checked_run) {
    // LinuxCNC can keep the interpreter's original file open across a pathname replacement.
    // Reopen the checked path before starting a fresh program and verify the source again.
    if (!wanted(request))
      return cancelled();
    auto reopened = port_.open_program(state.program_file, token(request));
    if (reopened.status == ActionStatus::cancelled)
      return cancelled();
    request.sent = true;
    if (auto opened = reply(std::move(reopened)); opened.outcome != CommandOutcome::accepted)
      return answer(std::move(opened));
    const auto now = port_.snapshot();
    if (!wanted(request))
      return cancelled();
    if (now.program_file != state.program_file || now.tool_in_spindle != state.tool_in_spindle ||
        now.spindle_active)
      return answer({CommandOutcome::failed, std::string(probe_code_unknown)});
    if (auto reason = probe_block(command, now); !reason.empty())
      return answer({CommandOutcome::failed, std::move(reason)});
    const auto recheck = policy_.evaluate(command, now);
    if (!recheck.allowed || now.mode != state.mode)
      return answer({CommandOutcome::failed, "Machine state changed during the spindle check."});
    state = now;
  }
  auto result = perform(request, decision, state, clock);
  if (checked_run && result.kind == StageKind::reply && result.reply.outcome == CommandOutcome::accepted)
    verified_probe_program_ = state.program_file;
  return result;
}

auto Session::probe_block(const Command& command, const ControllerState& state) const -> std::string {
  const auto* spindle = std::get_if<SetSpindle>(&command);
  const auto* mdi = std::get_if<Mdi>(&command);
  const bool program = std::holds_alternative<Run>(command) || std::holds_alternative<Step>(command) ||
                       std::holds_alternative<Resume>(command);
  if (tool_models_ == nullptr || (!program && mdi == nullptr &&
                                 (spindle == nullptr || spindle->direction == SpindleDirection::stopped)))
    return {};
  const auto loaded = tool_models_->touch_probe_loaded(state.tool_in_spindle);
  if (!loaded)
    return "Cannot verify the loaded tool type. Check tool model storage.";
  if (!*loaded)
    return {};
  if (mdi != nullptr)
    return std::string(probe_code_block(mdi->text));
  if (spindle != nullptr || state.spindle_active)
    return probe_loaded_reason;
  if (std::holds_alternative<Resume>(command) && state.mode != TaskMode::automatic)
    return std::string(probe_code_unknown);
  if (state.interp != InterpState::idle && state.program_file != verified_probe_program_)
    return std::string(probe_code_unknown);
  const auto text = programs_.read_program(state.program_file, 0);
  if (!text || text->truncated)
    return std::string(probe_code_unknown);
  for (const auto& line : text->lines)
    if (const auto reason = probe_code_block(line); !reason.empty())
      return std::string(reason);
  // The source can have changed while being read through a different controller client.
  const auto now = port_.snapshot();
  if (now.link != Link::live || now.program_file != state.program_file ||
      now.tool_in_spindle != state.tool_in_spindle || now.spindle_active)
    return "A touch probe is loaded. Machine state changed during the spindle check.";
  return {};
}

auto Session::perform(Queued& request, const Decision& decision, const ControllerState& state,
                      const Clock& clock) -> Stage {
  // Interpreter commands can reload the same tool number or change the work frame.
  const auto* power = std::get_if<SetMachine>(&request.command);
  const bool changes_datum =
      std::holds_alternative<Home>(request.command) || (power && power->state != MachineState::on);
  if (changes_datum || std::holds_alternative<Mdi>(request.command) ||
      std::holds_alternative<Run>(request.command) ||
      std::holds_alternative<Step>(request.command) ||
      std::holds_alternative<Resume>(request.command) ||
      std::holds_alternative<ToolChanged>(request.command) ||
      std::holds_alternative<SelectWcs>(request.command) ||
      std::holds_alternative<SetWorkOffset>(request.command) ||
      std::holds_alternative<TouchOff>(request.command)) {
    const std::scoped_lock lock{mutex_};
    if (changes_datum)
      ++setter_datum_;
    ++setter_tool_;
  }
  return std::visit(
      [&](const auto& c) -> Stage {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, Ready>) {
          return {};
        } else if constexpr (std::is_same_v<T, ListDirectory>) {
          Stage step;
          step.reply = list_directory(c, step);
          return step;
        } else if constexpr (std::is_same_v<T, SetMachine>) {
          // E-stop and power-off go out first; stops for held jogs follow.
          auto result = act(request, [&](const Wanted& still) -> ActionResult {
            return port_.set_machine_state(c.state, still);
          });
          if (c.state != MachineState::on)
            cancel_jogs(nullptr);
          return result;
        } else if constexpr (std::is_same_v<T, Quit>) {
          // Power off when on; an E-stop or a machine already off stays as it is (no reset).
          if (state.machine == MachineState::on) {
            auto off =
                act(request, [&](const Wanted&) -> ActionResult { return port_.power_off(); });
            cancel_jogs(nullptr);
            if (off.kind != StageKind::reply || off.reply.outcome != CommandOutcome::accepted)
              return off;
          }
          Stage done;
          done.quit = true;
          return done;
        } else if constexpr (std::is_same_v<T, Home>) {
          return act(request, [&](const Wanted& still) -> ActionResult {
            return port_.home(c.axis, still);
          });
        } else if constexpr (std::is_same_v<T, Jog>) {
          return jog(request, c, decision, clock);
        } else if constexpr (std::is_same_v<T, JogStep>) {
          return jog_step(request, c, decision);
        } else if constexpr (std::is_same_v<T, JogStopAll>) {
          return answer(reply(stop_all()));
        } else if constexpr (std::is_same_v<T, Mdi>) {
          return mdi(request, c.text);
        } else if constexpr (std::is_same_v<T, OpenProgram>) {
          // LinuxCNC gets the resolved path. It opens the file itself, later: a path that is
          // changed between this check and that open is not caught here.
          const auto file = programs_.resolve_program(c.path);
          if (!file)
            return answer({CommandOutcome::rejected, file.error()});
          return act(request, [&](const Wanted& still) -> ActionResult {
            return port_.open_program(*file, still);
          });
        } else if constexpr (std::is_same_v<T, Run>) {
          return act(request,
                     [&](const Wanted& still) -> ActionResult { return port_.run(still); });
        } else if constexpr (std::is_same_v<T, Pause>) {
          return act(request,
                     [&](const Wanted& still) -> ActionResult { return port_.pause(still); });
        } else if constexpr (std::is_same_v<T, Resume>) {
          return act(request,
                     [&](const Wanted& still) -> ActionResult { return port_.resume(still); });
        } else if constexpr (std::is_same_v<T, Step>) {
          return act(request,
                     [&](const Wanted& still) -> ActionResult { return port_.step(still); });
        } else if constexpr (std::is_same_v<T, Abort>) {
          auto result = port_.abort();
          cancel_jogs(nullptr);
          return answer(reply(std::move(result)));
        } else if constexpr (std::is_same_v<T, SetOverride>) {
          return act(request, [&](const Wanted& still) -> ActionResult {
            return port_.set_override(c.which, c.value, still);
          });
        } else if constexpr (std::is_same_v<T, SetSpindle>) {
          return act(request, [&](const Wanted& still) -> ActionResult {
            return port_.set_spindle(c.direction, c.rpm, still);
          });
        } else if constexpr (std::is_same_v<T, SetCoolant>) {
          return act(request, [&](const Wanted& still) -> ActionResult {
            return port_.set_coolant(c.flood, c.mist, still);
          });
        } else if constexpr (std::is_same_v<T, SetProgramOption>) {
          return act(request, [&](const Wanted& still) -> ActionResult {
            return port_.set_program_option(c.option, c.on, still);
          });
        } else if constexpr (std::is_same_v<T, SelectWcs>) {
          if (c.wcs < 1 || static_cast<std::size_t>(c.wcs) > wcs_words.size())
            return answer(
                {CommandOutcome::rejected, "Work coordinate system must be G54 through G59.3."});
          return mdi(request, std::string(wcs_words[static_cast<std::size_t>(c.wcs) - 1]));
        } else if constexpr (std::is_same_v<T, SetWorkOffset>) {
          // An explicit P index: editing an inactive system must not select it.
          // Omit R so XYZ editing, copying and clearing preserve the XY rotation.
          return mdi(request, "G10 L2 P" + std::to_string(c.wcs) +
                                  " X" + length(c.offset[0], state) +
                                  " Y" + length(c.offset[1], state) +
                                  " Z" + length(c.offset[2], state));
        } else if constexpr (std::is_same_v<T, TouchOff>) {
          // P0 is the active coordinate system.
          return mdi(request, std::string("G10 L20 P0 ") + letter(c.axis) + length(c.value, state));
        } else if constexpr (std::is_same_v<T, ToolTouchOff>) {
          auto measured = mdi(request, "G10 L10 P" + std::to_string(state.tool_in_spindle) + " Z" +
                                           length(c.value, state));
          if (measured.kind != StageKind::reply ||
              measured.reply.outcome != CommandOutcome::accepted)
            return measured;
          auto applied = mdi(request, "G43");
          if (applied.kind == StageKind::cancelled)
            return cancelled(touch_off_reason);
          return applied;
        } else if constexpr (std::is_same_v<T, FixedSetterRequest>) {
          const auto context = [&]() -> SetterEpoch {
            const std::scoped_lock lock{mutex_};
            return {request.page.value, setter_datum_, setter_tool_, cancels_};
          };
          const auto begun = context();
          const auto still = [&]() {
            if (!wanted(request))
              return false;
            const auto now = context();
            return now.datum == begun.datum && now.tool == begun.tool && now.stop == begun.stop;
          };
          auto result = port_.fixed_setter(c, begun, still);
          if (!result)
            return answer({CommandOutcome::failed, std::move(result.error())});
          Stage step;
          step.setter = std::move(*result);
          return step;
        } else if constexpr (std::is_same_v<T, SetTool>) {
          return act(request, [&](const Wanted& still) -> ActionResult {
            return port_.set_tool(c.tool, still);
          });
        } else if constexpr (std::is_same_v<T, RemoveTool>) {
          return act(request, [&](const Wanted& still) -> ActionResult {
            return port_.remove_tool(c.number, still);
          });
        } else if constexpr (std::is_same_v<T, ToolChanged>) {
          // The tool the decision was made for: not another one asked for since.
          return act(request, [&](const Wanted& still) -> ActionResult {
            return port_.tool_changed(state.requested_tool.value_or(-1), state.tool_request, still);
          });
        } else {
          static_assert(std::is_same_v<T, SetToolModelDefaults> ||
                            std::is_same_v<T, SetToolModelAssignment> ||
                            std::is_same_v<T, SetOperatorSettings>,
                        "Every command needs an action.");
          // `submit` hands these to the tool-model service: none is ever queued.
          return answer({CommandOutcome::rejected, no_tool_models_reason});
        }
      },
      request.command);
}

// An increment is motion nobody holds: from the moment it is sent the axis counts as possibly
// moving, until a jog stop written after it has been accepted. Nothing else ends that: not time,
// and not what the status shows, which cannot tell a finished increment from one the controller
// has not taken up yet.
auto Session::jog_step(Queued& request, const JogStep& command, const Decision& decision) -> Stage {
  const auto index = axis_index(command.axis);
  if (!decision.jog_space || !index)
    return answer({CommandOutcome::rejected, "Jog space is undecided."});
  {
    // An increment does not wait for a jog stop to finish: it is refused.
    const std::scoped_lock lock{mutex_};
    if (hold_ || low_periods_ < settled_periods || !reconciled())
      return answer(
          {request.sent ? CommandOutcome::failed : CommandOutcome::rejected, hold_reason});
  }
  if (auto gate = start_gate(request, *index, false))
    return std::move(*gate);
  auto result = port_.jog_increment(command.axis, command.distance, command.velocity,
                                    *decision.jog_space, token(request));
  {
    const std::scoped_lock lock{mutex_};
    auto& axis = jog_.at(*index);
    // A stop decision made meanwhile has seen it in flight and asks for the stop.
    axis.in_flight = false;
    if (result.status == ActionStatus::cancelled) {
      // The increment itself was not written.
    } else if (result.status == ActionStatus::accepted && request.epoch == epoch_) {
      axis.stepping = true;
      axis.step = {command.distance < 0 ? -1 : 1, command.velocity, *decision.jog_space};
    } else if (result.status != ActionStatus::accepted) {
      // Not acknowledged, or failed after it was written: the controller may still take it up
      // or be moving. Fenced like a start that was lost: the level is held until a jog stop
      // written after it is accepted, which proves the controller has consumed it.
      stop_jogs();
      need_stop(*index);
    }
  }
  flush_stops();
  return staged(std::move(result));
}

auto Session::jog(Queued& request, const Jog& command, const Decision& decision, const Clock& clock)
    -> Stage {
  const auto index = axis_index(command.axis);
  if (!index)
    return answer({CommandOutcome::failed, "Unknown axis."});
  if (!command.held)
    return answer(reply(tracked_stop(*index)));
  if (!wanted(request))
    return cancelled();
  if (!decision.jog_space) {
    forget_press(request);
    return answer({CommandOutcome::failed, "Jog space is undecided."});
  }
  const HeldJog asked{command.direction, command.velocity, *decision.jog_space};
  {
    const std::scoped_lock lock{mutex_};
    auto& axis = jog_.at(*index);
    // The jog asked for is the one that runs.
    if (request.epoch == epoch_ && axis.running && axis.held == asked &&
        leases_.active(command.axis)) {
      axis.pressed = false;
      return {};
    }
  }
  if (auto gate = start_gate(request, *index, true))
    return std::move(*gate);
  auto started = port_.start_jog(command.axis, command.direction, command.velocity, asked.space,
                                 token(request));
  if (started.status == ActionStatus::cancelled) {
    // The start itself was not written. The stop decision that withdrew it has the rest.
    forget_press(request);
    const std::scoped_lock lock{mutex_};
    jog_.at(*index).in_flight = false;
    return request.epoch != epoch_ && current(request)
               ? answer({CommandOutcome::rejected, stopped_reason})
               : cancelled();
  }
  std::optional<std::uint64_t> late_in;
  const auto refused_at = clock().monotonic;
  {
    const std::scoped_lock lock{mutex_};
    auto& axis = jog_.at(*index);
    axis.in_flight = false;
    // Stopped while in flight: the stop decision has seen it and asks for the stop.
    if (request.epoch != epoch_) {
      // Refused under the jog-inhibit that a stop decision made after the send has raised: a
      // tap that ended before its jog began, and nothing moved. The stop is still needed.
      if (started.status != ActionStatus::failed || !is_jog_inhibit_refusal(started.reason))
        return answer(reply(std::move(started)));
      if (watchdog_epoch_ > request.epoch)
        return answer({CommandOutcome::failed, std::string(watchdog_stop_text)});
      // No period has raised the level since the stop decision: the inhibit that refused the
      // start was not ours, or not yet. No period for longer than control_fresh: the control
      // thread is stalled and may not have read a trip.
      if (level_epoch_ <= request.epoch || refused_at - control_at_ > control_fresh)
        return answer(reply(std::move(started)));
      late_in = epoch_;
    } else {
      axis.pressed = false;
      if (started.status == ActionStatus::accepted) {
        axis.running = true;
        axis.held = asked;
        return {};
      }
      // The axis may be moving: an earlier jog, or a start that was lost on the way back.
      stop_jogs();
      need_stop(*index);
      axis.latched = true;
    }
  }
  if (late_in) {
    std::cerr << std::format("coffeecnc: jog_late_start axis={} sent_epoch={} epoch={} "
                             "text=\"{}\"\n",
                             letter(command.axis), request.epoch, *late_in, started.reason)
              << std::flush;
    return {};
  }
  flush_stops();
  return answer(reply(std::move(started)));
}

// What stands between a jog request and its start. Nothing when the start may be sent now; the
// axis is then marked in flight.
auto Session::start_gate(Queued& request, std::size_t index, bool held) -> std::optional<Stage> {
  // An earlier stop is not confirmed: nothing new starts until every one is, or the level that
  // waits for it would stop the new jog as well. This axis first.
  for (std::size_t n = 0; n < axes.size(); ++n) {
    const std::size_t i = (index + n) % axes.size();
    {
      const std::scoped_lock lock{mutex_};
      if (!jog_.at(i).stop_needed)
        continue;
    }
    if (auto stopped = tracked_stop(i); stopped.status != ActionStatus::accepted) {
      forget_press(request);
      const auto failure = stopped.status;
      auto reason =
          (i == index ? std::string("The previous jog stop on this axis is not confirmed: ")
                      : std::string("The previous jog stop on ") + letter(axes.at(i)) +
                            " is not confirmed: ") +
          reply(std::move(stopped)).reason;
      return answer(reply({failure, std::move(reason)}));
    }
  }
  const std::scoped_lock lock{mutex_};
  auto& axis = jog_.at(index);
  if (!current(request))
    return cancelled();
  if (request.epoch != epoch_ || (held && (axis.latched || !leases_.active(axes.at(index))))) {
    if (request.epoch == epoch_)
      axis.pressed = false;
    return answer({request.sent ? CommandOutcome::failed : CommandOutcome::rejected,
                   held && axis.latched ? release_reason : stopped_reason});
  }
  // Not before the level has been low for whole control periods: LinuxCNC's servo thread, and
  // the HAL logic between the pin and motion, must have seen it fall.
  if (hold_ || low_periods_ < settled_periods || !reconciled())
    return Stage{StageKind::blocked, {}, {}};
  axis.in_flight = true;
  return {};
}

auto Session::stop_all() -> ActionResult {
  ActionResult result;
  for (std::size_t i = 0; i < axes.size(); ++i) {
    auto stopped = tracked_stop(i);
    if (result.status == ActionStatus::accepted && stopped.status != ActionStatus::accepted)
      result = std::move(stopped);
  }
  return result;
}

// Not a controller action: nothing is sent to LinuxCNC.
auto Session::list_directory(const ListDirectory& request, Stage& step) -> Reply {
  const auto directory = programs_.list(request.path);
  if (!directory)
    return {CommandOutcome::rejected, directory.error()};
  auto message = encode_directory(*directory);
  if (!message)
    return {CommandOutcome::rejected, "Directory cannot be sent: " + message.error()};
  step.produced.push_back(std::move(*message)); // `truncated` in it tells of a cut listing.
  return {};
}

auto Session::mdi(const Queued& request, const std::string& text) const -> Stage {
  if (text.size() > max_mdi_bytes)
    return answer({CommandOutcome::rejected, "MDI requires one block of at most 254 bytes."});
  return act(request, [&](const Wanted& still) -> ActionResult { return port_.mdi(text, still); });
}

// Every stop goes through here, so a failed one is always left to be retried and keeps the
// jog-stop level high. The worker sends one command at a time: a stop accepted here was written
// after every start for the axis had returned.
auto Session::tracked_stop(std::size_t index) -> ActionResult {
  auto stopped = port_.stop_jog(axes.at(index));
  const std::scoped_lock lock{mutex_};
  auto& axis = jog_.at(index);
  const bool failed = stopped.status != ActionStatus::accepted;
  axis.stop_needed = failed;
  axis.stop_failed = failed;
  axis.retry_due = false;
  if (failed)
    hold_ = true;
  return stopped;
}

// Stops every axis that is due. A failure is reported once; the retries are silent.
void Session::flush_stops() {
  for (std::size_t i = 0; i < axes.size(); ++i) {
    bool known = false;
    {
      const std::scoped_lock lock{mutex_};
      const auto& axis = jog_.at(i);
      if (!axis.stop_needed || (axis.stop_failed && !axis.retry_due))
        continue;
      known = axis.stop_failed;
    }
    const auto stopped = tracked_stop(i);
    if (stopped.status != ActionStatus::accepted && !known) {
      const std::scoped_lock lock{mutex_};
      problems_.push_back(std::string("Jog stop failed on ") + letter(axes.at(i)) + ": " +
                          reply(stopped).reason);
    }
  }
}

// A stop decision of the worker's own, with its stops sent before it goes on. The request's own
// press, if it is one, moves to the new epoch.
void Session::cancel_jogs(Queued* keep_press) {
  {
    const std::scoped_lock lock{mutex_};
    const bool stale = keep_press != nullptr && keep_press->epoch != epoch_;
    std::optional<std::size_t> keep;
    if (keep_press != nullptr && !stale)
      if (const auto* press = std::get_if<Jog>(&keep_press->command);
          press != nullptr && press->held)
        keep = axis_index(press->axis);
    stop_jogs(keep);
    if (keep_press != nullptr && !stale)
      keep_press->epoch = epoch_;
  }
  flush_stops();
}

// The press of this request does not become a jog.
void Session::forget_press(const Queued& request) {
  const auto* press = std::get_if<Jog>(&request.command);
  if (press == nullptr || !press->held)
    return;
  const auto index = axis_index(press->axis);
  const std::scoped_lock lock{mutex_};
  if (!index || request.epoch != epoch_)
    return;
  auto& axis = jog_.at(*index);
  axis.pressed = false;
  if (!axis.running)
    leases_.release(press->axis);
}

// The same, and a jog that runs on the axis is stopped: the page asked for something else.
void Session::reject_press(const Queued& request) {
  const auto* press = std::get_if<Jog>(&request.command);
  if (press == nullptr || !press->held)
    return;
  forget_press(request);
  const auto index = axis_index(press->axis);
  {
    const std::scoped_lock lock{mutex_};
    if (!index || request.epoch != epoch_ || !jog_.at(*index).running)
      return;
    stop_jogs();
  }
  flush_stops();
}

} // namespace coffeecnc
