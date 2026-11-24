#include <coffeecnc/app/session.hpp>

#include <coffeecnc/app/machine_data.hpp>
#include <coffeecnc/app/offset_history.hpp>
#include <coffeecnc/app/program_edit.hpp>

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

// The offset history's name for a panel command that can change offsets; none for the others.
auto history_source(const Command& command) -> std::optional<HistorySource> {
  return std::visit(
      [](const auto& c) -> std::optional<HistorySource> {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, TouchOff>) {
          return c.origin == TouchOrigin::zero   ? HistorySource::zero
                 : c.origin == TouchOrigin::half ? HistorySource::half
                                                 : HistorySource::touch_off;
        } else if constexpr (std::is_same_v<T, SetWorkOffset> || std::is_same_v<T, SetWorkRotation> ||
                             std::is_same_v<T, ClearG92>) {
          return HistorySource::work_offset_table;
        } else if constexpr (std::is_same_v<T, ToolTouchOff>) {
          return HistorySource::tool_touch_off;
        } else if constexpr (std::is_same_v<T, FixedSetterRequest>) {
          if (c.action == SetterAction::status)
            return std::nullopt;
          return c.action == SetterAction::touch_off ? HistorySource::tool_setter
                                                     : HistorySource::probe;
        } else if constexpr (std::is_same_v<T, ToolChanged>) {
          return c.measure_request ? std::optional{HistorySource::tool_setter} : std::nullopt;
        } else if constexpr (std::is_same_v<T, SetTool> || std::is_same_v<T, RemoveTool>) {
          return HistorySource::tool_table;
        } else if constexpr (std::is_same_v<T, Mdi>) {
          return HistorySource::mdi;
        } else if constexpr (std::is_same_v<T, RestoreOffset>) {
          return HistorySource::restore;
        } else {
          return std::nullopt;
        }
      },
      command);
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
constexpr auto panel_frame_reason =
    "Panel display is waiting for fresh frames. Try again when it updates.";
constexpr auto panel_frame_stop =
    "Panel stopped drawing. Jogging is locked until the display recovers.";
// The page posts at most every 80 ms. Require two distinct, recent frames across a real
// interval after a gap so buffered copies cannot release the hold together.
constexpr auto panel_frame_max_transit = std::chrono::milliseconds{200};
constexpr auto panel_frame_min_spacing = std::chrono::milliseconds{40};
constexpr auto panel_clock_slop = std::chrono::milliseconds{20};
// How old the last control period may be for the watchdog reading it made to count as current:
// ten 10 ms control periods, below the 300 ms timeout of the HAL watchdog in the post-GUI file.
constexpr auto control_fresh = std::chrono::milliseconds{100};
constexpr auto no_tool_models_reason = "Tool model storage is not part of this session.";
constexpr auto no_diagnostics_reason = "HAL diagnostics are not part of this session.";
constexpr auto probe_loaded_reason = "A touch probe is loaded";
constexpr std::string_view probe_code_unknown =
    "A touch probe is loaded. CoffeeCNC could not verify that the code leaves the spindle off.";

// Check the accepted subset of RS274 before dispatch. LinuxCNC ignores case and spaces between
// digits. O-words, indirect M-codes, state restore and custom M-codes can hide spindle actions.
struct ProbeCode {
  std::string_view refusal;
  bool changes_tool{};
  bool relabels_tool{};
  bool stops_program{};
};

auto inspect_probe_code(std::string_view line) -> ProbeCode {
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
      return {probe_code_unknown};
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
        return {probe_code_unknown};
      if (c != ')') {
        comment += c;
        continue;
      }
      if (active_comment())
        return {probe_code_unknown};
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
    return {probe_code_unknown};
  ProbeCode result;
  if (code.find_first_of("o#[]<>") != std::string::npos)
    result.refusal = probe_code_unknown;
  for (std::size_t pos = code.find('g'); pos != std::string::npos; pos = code.find('g', pos + 1)) {
    const char* first = code.data() + pos + 1;
    const char* last = code.data() + code.size();
    if (first != last && *first == '+')
      ++first;
    double value = 0;
    const auto [end, error] = std::from_chars(first, last, value, std::chars_format::fixed);
    if (error != std::errc{} || end == first || !std::isfinite(value) || value < 0 || value > 999) {
      result.refusal = probe_code_unknown;
      continue;
    }
    const auto number = static_cast<int>(std::round(value * 10));
    // Spindle-synchronized, tapping and boring motion requires a cutting tool.
    constexpr std::array spindle_cycles{330, 331, 740, 760, 840, 842, 843, 860, 880};
    if (std::ranges::contains(spindle_cycles, number) && result.refusal.empty())
      result.refusal = probe_loaded_reason;
  }
  constexpr std::array ordinary_m_codes{0, 1, 2, 5, 6, 7, 8, 9, 30, 48, 49, 50, 51, 52, 53,
                                       60, 62, 63, 64, 65, 66, 67, 68, 70, 71};
  for (std::size_t pos = code.find('m'); pos != std::string::npos; pos = code.find('m', pos + 1)) {
    const char* first = code.data() + pos + 1;
    const char* last = code.data() + code.size();
    if (first != last && *first == '+')
      ++first;
    double value = 0;
    const auto [end, error] = std::from_chars(first, last, value, std::chars_format::fixed);
    if (error != std::errc{} || end == first || !std::isfinite(value) || value < 0 || value > 199) {
      result.refusal = probe_code_unknown;
      continue;
    }
    const auto number = static_cast<int>(std::round(value));
    if (value == number && number == 61) {
      result.relabels_tool = true;
      continue;
    }
    if (value == number && number == 6)
      result.changes_tool = true;
    if (value == number && (number == 0 || number == 1 || number == 60))
      result.stops_program = true;
    if (number == 3 || number == 4 || number == 19) {
      if (result.refusal.empty())
        result.refusal = probe_loaded_reason;
    } else if (value != number || !std::ranges::contains(ordinary_m_codes, number)) {
      result.refusal = probe_code_unknown;
    }
  }
  return result;
}

auto probe_code_block(std::string_view line) -> std::string_view {
  const auto block = inspect_probe_code(line);
  return block.relabels_tool ? probe_loaded_reason : block.refusal;
}

auto program_line(std::string_view& remaining) -> std::string_view {
  const auto end = remaining.find('\n');
  auto line = remaining.substr(0, end);
  remaining.remove_prefix(end == std::string_view::npos ? remaining.size() : end + 1);
  if (line.ends_with('\r'))
    line.remove_suffix(1);
  return line;
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

auto needs_panel_frame(const Command& command) -> bool {
  // Protocol "off" can reset an asserted E-stop. It needs a current view of the machine.
  if (const auto* machine = std::get_if<SetMachine>(&command))
    return machine->state != MachineState::estop;
  if (const auto* setter = std::get_if<FixedSetterRequest>(&command))
    return setter->action != SetterAction::status;
  // Reading HAL and opening a HAL tool move nothing.
  if (std::holds_alternative<WatchDiagnostics>(command) ||
      std::holds_alternative<LaunchHalTool>(command))
    return false;
  return !only_stops(command) && !std::holds_alternative<Pause>(command);
}

// A held jog or an increment: motion that the jog-stop level stops.
auto starts_jog(const Command& command) -> bool {
  if (const auto* jog = std::get_if<Jog>(&command))
    return jog->held;
  return std::holds_alternative<JogStep>(command) || std::holds_alternative<JogPausedZ>(command);
}

auto idle_for_tool_measurement(const ControllerState& state) -> bool {
  return state.link == Link::live && state.interp == InterpState::idle &&
         state.mode == TaskMode::mdi && !state.requested_tool;
}

} // namespace

Session::Session(ControllerPort& port, const Limits& limits, const ProgramStore& programs,
                 ToolModelService* tool_models, PreviewWorker* preview,
                 PreviewEngine* restart_engine, ProgramWriter* program_writer,
                 OffsetHistoryStore* history, DiagnosticsService* diagnostics,
                 bool asynchronous_copies, MachineDataService* machine_data)
    : port_(port), programs_(programs), tool_models_(tool_models), preview_(preview),
      restart_engine_(restart_engine), program_writer_(program_writer), history_(history),
      diagnostics_(diagnostics), machine_data_(machine_data), policy_(limits),
      asynchronous_copies_(asynchronous_copies) {}

auto Session::reply(ActionResult result) -> Reply {
  switch (result.status) {
  case ActionStatus::accepted:
    return {};
  case ActionStatus::timed_out:
    if (result.reason.empty())
      return {CommandOutcome::timed_out,
              "LinuxCNC did not answer.",
              {},
              MessageKey{"messages.session.linuxcncNoAnswer"}};
    return {
        CommandOutcome::timed_out, std::move(result.reason), {}, std::move(result.localization)};
  case ActionStatus::cancelled:
    // Only where the port withdrew something that is never withdrawn; not `rejected`, since
    // something may have been sent.
    return {CommandOutcome::failed, cancelled_reason, {}, MessageKey{"messages.session.cancelled"}};
  case ActionStatus::failed:
    break;
  }
  if (result.reason.empty())
    return {CommandOutcome::failed,
            "LinuxCNC reported an error.",
            {},
            MessageKey{"messages.session.linuxcncError"}};
  return {CommandOutcome::failed, std::move(result.reason), {}, std::move(result.localization)};
}

auto Session::answer(Reply reply) -> Stage { return {StageKind::reply, std::move(reply), {}}; }
auto Session::refuse(std::string reason, RefusalMessage message) -> Stage {
  return answer({CommandOutcome::rejected, std::move(reason), std::move(message)});
}
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

auto Session::begin_page(bool watch_frames, std::optional<Instant> frame_clock) -> PageToken {
  const std::scoped_lock lock{mutex_};
  problems_.clear(); // The page they were for is gone.
  stop_jogs();       // A stop that fails is reported to the new page and retried.
  // Whatever the old page held, the new page has pressed nothing yet.
  for (auto& axis : jog_)
    axis.latched = false;
  drop_requests();
  ready_ = false;
  watch_frames_ = watch_frames;
  panel_frame_clock_ = frame_clock;
  last_panel_frame_.reset();
  panel_recovered_at_.reset();
  last_id_.reset();
  page_open_ = !closing_;
  if (tool_models_ != nullptr)
    tool_models_->page_ended(PageToken{pages_});
  if (diagnostics_ != nullptr)
    diagnostics_->page_ended(PageToken{pages_});
  const PageToken page{++pages_};
  // A restore staged for a quit of the page this one replaces: the window drops that close (a
  // close is for the page that asked), so no exit comes for it now, and a later exit must not
  // write it. After the page changed and under the lock: a staging that ends before this is
  // abandoned here, one that ends after it finds the page changed and abandons itself. The
  // restore never takes this lock while it holds its own (lock order: this, then the
  // restore's).
  if (machine_data_ != nullptr)
    machine_data_->abandon_restore();
  return page;
}

void Session::end_page(PageToken page) {
  const std::scoped_lock lock{mutex_};
  if (!page_open_ || page != PageToken{pages_})
    return;
  problems_.clear();
  stop_jogs();
  drop_requests();
  ready_ = false;
  last_panel_frame_.reset();
  panel_recovered_at_.reset();
  last_id_.reset();
  page_open_ = false;
  if (tool_models_ != nullptr)
    tool_models_->page_ended(page);
  if (diagnostics_ != nullptr)
    diagnostics_->page_ended(page);
}

void Session::close() {
  const std::scoped_lock lock{mutex_};
  closing_ = true;
  if (publication_) {
    publication_->result.set_value(std::unexpected("CoffeeCNC is closing."));
    publication_.reset();
  }
  problems_.clear();
  stop_jogs();
  drop_requests();
  page_open_ = false;
  if (tool_models_ != nullptr)
    tool_models_->page_ended(PageToken{pages_});
  if (diagnostics_ != nullptr)
    diagnostics_->page_ended(PageToken{pages_});
  wake();
}

void Session::expire_panel_frame(JogLease::Time now) {
  if (!last_panel_frame_ || now - last_panel_frame_->monotonic < panel_frame_timeout)
    return;
  last_panel_frame_.reset();
  if (!panel_recovered_at_)
    return;
  panel_recovered_at_.reset();
  stop_jogs();
  // Withdraw queued and in-flight work through the same cancellation checks used by Abort.
  ++cancels_;
  if (ready_)
    problems_.push_back(
        {true, std::string(panel_frame_stop), MessageKey{"messages.session.panelStoppedDrawing"}});
}

auto Session::frame_age(Instant now, std::chrono::milliseconds rendered_at) const
    -> std::chrono::milliseconds {
  // Compare the document's epoch-based monotonic timestamp to the native clock
  // projected from the start of the document. Wall-clock corrections cannot renew
  // or invalidate a motion lease.
  const auto clock = panel_frame_clock_.value_or(now);
  return clock.unix_time +
         std::chrono::duration_cast<std::chrono::milliseconds>(now.monotonic - clock.monotonic) -
         rendered_at;
}

void Session::panel_frame(PageToken page, std::chrono::milliseconds rendered_at, Instant now) {
  const std::scoped_lock lock{mutex_};
  if (!watch_frames_ || !page_open_ || page != PageToken{pages_})
    return;
  expire_panel_frame(now.monotonic);
  const auto age = frame_age(now, rendered_at);
  if (rendered_at.count() <= 0 || age < -panel_clock_slop || age > panel_frame_max_transit ||
      (last_panel_frame_ && rendered_at <= last_panel_frame_->unix_time))
    return;
  if (!panel_frame_clock_)
    panel_frame_clock_ = now;
  const Instant frame{now.monotonic - std::max(age, std::chrono::milliseconds::zero()),
                      rendered_at};
  if (!panel_recovered_at_ && last_panel_frame_ &&
      frame.monotonic - last_panel_frame_->monotonic >= panel_frame_min_spacing &&
      now.monotonic - last_panel_frame_received_ >= panel_frame_min_spacing)
    panel_recovered_at_ = rendered_at;
  last_panel_frame_ = frame;
  last_panel_frame_received_ = now.monotonic;
}

auto Session::submit(PageToken page, std::string_view json, Instant now,
                     std::chrono::milliseconds rendered_at) -> Outcome {
  Outcome outcome;
  outcome.page = page;
  auto decoded = decode_request(json);
  std::unique_lock lock{mutex_};
  if (!page_open_ || page != PageToken{pages_})
    return outcome;
  if (const auto* error = std::get_if<DecodeError>(&decoded)) {
    if (!error->id)
      outcome.messages.push_back(encode_message(
          MessageLevel::error, std::format("Panel request ignored: {}", error->reason),
          now.unix_time,
          MessageKey{"messages.session.requestIgnored", {{"detail", error->reason}}}));
    else if (in_order(*error->id))
      outcome.messages.push_back(
          encode_result(*error->id, CommandOutcome::rejected, error->reason, {}, {},
                        error->localization));
    return outcome;
  }
  auto& request = std::get<Request>(decoded);
  if (!in_order(request.id))
    return outcome;
  const auto result = [&](CommandOutcome verdict, std::string_view reason,
                          const std::optional<RefusalMessage>& refusal = {}) -> void {
    outcome.messages.push_back(encode_result(request.id, verdict, reason, {}, {}, refusal));
    report(outcome, now.unix_time);
  };
  if (std::holds_alternative<Ready>(request.command)) {
    ready_ = true;
    outcome.send_snapshots = true;
    result(CommandOutcome::accepted, "");
    return outcome;
  }
  if (!ready_) {
    result(CommandOutcome::rejected, "Panel has not finished loading.",
           RefusalMessage{"messages.session.panelNotLoaded"});
    return outcome;
  }
  expire_panel_frame(now.monotonic);
  const bool requires_frame = watch_frames_ && needs_panel_frame(request.command);
  if (requires_frame) {
    const auto age = frame_age(now, rendered_at);
    if (!panel_recovered_at_ || rendered_at < *panel_recovered_at_ ||
        age >= panel_frame_timeout || age < -panel_clock_slop) {
      result(CommandOutcome::rejected, panel_frame_reason,
             RefusalMessage{"messages.session.panelFramesWaiting"});
      return outcome;
    }
  }

  if (auto* settings = std::get_if<SetOperatorSettings>(&request.command)) {
    const auto decision = policy_.evaluate(request.command, status(now.monotonic));
    if (!decision.allowed)
      result(CommandOutcome::rejected, decision.reason, decision.refusal);
    else if (tool_models_ == nullptr)
      result(CommandOutcome::rejected, "Operator settings storage is not part of this session.",
             RefusalMessage{"messages.session.noOperatorSettings"});
    else if (const auto refused =
                 tool_models_->submit_operator_settings(page, request.id, std::move(*settings)))
      result(CommandOutcome::rejected, refused->text, refused->localization);
    return outcome;
  }

  // HAL diagnostics: not the controller's business either. A HAL tool starts outside the
  // mutex: starting a process is not instant, and the shell's thread is the only caller.
  if (std::holds_alternative<WatchDiagnostics>(request.command) ||
      std::holds_alternative<LaunchHalTool>(request.command)) {
    const auto decision = policy_.evaluate(request.command, status(now.monotonic));
    if (!decision.allowed) {
      result(CommandOutcome::rejected, decision.reason, decision.refusal);
    } else if (diagnostics_ == nullptr) {
      result(CommandOutcome::rejected, no_diagnostics_reason,
             RefusalMessage{"messages.session.noDiagnostics"});
    } else if (const auto* watch = std::get_if<WatchDiagnostics>(&request.command)) {
      diagnostics_->watch(page, watch->on);
      result(CommandOutcome::accepted, "");
    } else {
      report(outcome, now.unix_time);
      lock.unlock();
      const auto refused = diagnostics_->launch(std::get<LaunchHalTool>(request.command).tool);
      outcome.messages.push_back(refused
                                     ? encode_result(request.id, CommandOutcome::rejected,
                                                     refused->text, {}, {}, refused->localization)
                                     : encode_result(request.id, CommandOutcome::accepted, ""));
    }
    return outcome;
  }

  // Visual preferences: not the controller's business and not gated by its state. The policy
  // is asked all the same, so that it stays the one place that says what is permitted.
  if (std::holds_alternative<SetToolModelDefaults>(request.command) ||
      std::holds_alternative<SetToolModelAssignment>(request.command)) {
    auto decision = policy_.evaluate(request.command, status(now.monotonic));
    auto models = tool_model_command(request.command);
    if (!decision.allowed || !models)
      result(CommandOutcome::rejected, decision.reason, decision.refusal);
    else if (tool_models_ == nullptr)
      result(CommandOutcome::rejected, no_tool_models_reason,
             RefusalMessage{"messages.session.noToolModels"});
    else if (const auto refused = tool_models_->submit(page, request.id, std::move(*models)))
      result(CommandOutcome::rejected, refused->text, refused->localization);
    return outcome;
  }

  if (auto* file = std::get_if<ManageProgramFile>(&request.command);
      file && file->operation == ProgramFileOperation::copy) {
    if (!asynchronous_copies_)
      result(CommandOutcome::rejected, "Program file management is unavailable.",
             RefusalMessage{"backend.programFiles.unavailable"});
    else if (copy_ || copying_)
      result(CommandOutcome::rejected, busy_reason, RefusalMessage{"messages.session.busy"});
    else {
      copy_.emplace(ProgramCopy{page, request.id, std::move(*file)});
      work_ready_.notify_all();
    }
    return outcome;
  }

  // Carried out whatever becomes of its page; in order with the other requests unless it is
  // one of those that go first.
  if ((setter_busy_ || restart_busy_) && std::holds_alternative<Pause>(request.command))
    request.command = Abort{};
  const bool stops = only_stops(request.command) && !requires_frame;
  if (setter_busy_ && !stops) {
    result(CommandOutcome::rejected, "A fixed setter operation is in progress.",
           RefusalMessage{"messages.session.setterBusy"});
    return outcome;
  }
  if (restart_busy_ && !stops) {
    result(CommandOutcome::rejected, "A restart approach is in progress.",
           RefusalMessage{"messages.session.restartBusy"});
    return outcome;
  }
  bool stop_request = cancels_requests(request.command);
  if (const auto* asked = std::get_if<Jog>(&request.command)) {
    if (const auto index = axis_index(asked->axis); index && asked->held) {
      auto& axis = jog_.at(*index);
      // A jog that was stopped for the operator is not started again by the refreshes of the
      // press that began it: only a press made after a release starts a jog.
      if (axis.latched) {
        result(CommandOutcome::rejected, release_reason,
               RefusalMessage{"messages.session.releaseJog"});
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
        if (decision.allowed)
          result(CommandOutcome::rejected, release_reason,
                 RefusalMessage{"messages.session.releaseJog"});
        else
          result(CommandOutcome::rejected, decision.reason, decision.refusal);
        return outcome;
      }
      if (axis.pressed || axis.running) {
        stop_jogs();
        result(CommandOutcome::rejected, release_reason,
               RefusalMessage{"messages.session.releaseJog"});
        return outcome;
      }
      if (requests_.size() >= max_queued) {
        result(CommandOutcome::rejected, busy_reason, RefusalMessage{"messages.session.busy"});
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
    for (const auto& waiting : requests_) {
      if (waiting.stop)
        continue;
      if (waiting.sent)
        outcome.messages.push_back(encode_result(waiting.id, CommandOutcome::failed,
                                                 mode_changed_reason, {}, {}, {},
                                                 MessageKey{"messages.session.modeChanged"}));
      else
        outcome.messages.push_back(encode_result(waiting.id, CommandOutcome::rejected,
                                                 cancelled_reason, {}, {},
                                                 RefusalMessage{"messages.session.cancelled"}));
    }
    drop_requests();
  }
  auto& queue = stop_request ? stops_ : requests_;
  if (queue.size() >= (stop_request ? max_queued_stops : max_queued)) {
    // The stop decision above stands; only the controller command is not queued again.
    result(CommandOutcome::rejected, busy_reason, RefusalMessage{"messages.session.busy"});
    return outcome;
  }
  queue.push_back({page, epoch_, cancels_, request.id, std::move(request.command), false, stops});
  wake();
  return outcome;
}

auto Session::loaded_program() const -> std::string {
  const std::scoped_lock lock{mutex_};
  return have_status_ ? status_.program_file : std::string{};
}

auto Session::publish_program(std::string path, Publication publish)
    -> std::future<std::expected<void, std::string>> {
  ProgramPublication request{std::move(path), std::move(publish), {}};
  auto result = request.result.get_future();
  const std::scoped_lock lock{mutex_};
  if (closing_) {
    request.result.set_value(std::unexpected("CoffeeCNC is closing."));
  } else if (publication_) {
    request.result.set_value(std::unexpected("Another program upload is in progress."));
  } else {
    publication_.emplace(std::move(request));
    wake();
  }
  return result;
}

void Session::observe(const ControllerState& state, JogLease::Time now) {
  const std::scoped_lock lock{mutex_};
  if (state.link != Link::live || state.machine != MachineState::on ||
      (have_status_ && now - status_at_ > status_max_age) ||
      state.any_enabled_axis([](const auto& axis) { return axis.homing; }) ||
      (have_status_ &&
       (!std::ranges::equal(state.axes, status_.axes, {}, &AxisState::homed, &AxisState::homed) ||
        !std::ranges::equal(state.rotary_axes, status_.rotary_axes, {},
                            &AxisState::homed, &AxisState::homed))))
    ++setter_datum_;
  const bool tool_changed = state.tool_in_spindle != status_.tool_in_spindle ||
                            state.tool_request != status_.tool_request;
  // M61 can report the new tool before it reports idle MDI. Wake the measurement wait
  // on the later transition too, even though the spindle and the HAL request are unchanged.
  const bool measurement_ready =
      idle_for_tool_measurement(state) && !idle_for_tool_measurement(status_);
  if (tool_changed)
    ++setter_tool_;
  status_ = state;
  status_at_ = now;
  have_status_ = true;
  if (tool_changed || measurement_ready)
    work_ready_.notify_all();
}

auto Session::control(Instant now, const ControlInput& input) -> bool {
  const std::scoped_lock lock{mutex_};
  expire_panel_frame(now.monotonic);
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
          policy_.evaluate(Jog{axes[i], moving.direction, moving.velocity, true}, state,
                           !jog_[i].running && jog_[i].stepping);
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
  // Startup, a stale panel, and shutdown hold the level even after the stops were reconciled.
  const bool hold = hold_ || closing_ || (watch_frames_ && (!ready_ || !panel_recovered_at_));
  if (hold)
    level_epoch_ = epoch_;
  return hold;
}

void Session::wait_for_work(std::chrono::milliseconds timeout) {
  std::unique_lock lock{mutex_};
  work_ready_.wait_for(lock, timeout, [&]() -> bool { return woken_; });
  woken_ = false;
}

auto Session::copy_work(ProgramWriter& writer) -> std::optional<Outcome> {
  std::unique_lock lock{mutex_};
  work_ready_.wait(lock, [&] { return closing_ || copy_.has_value(); });
  if (closing_ || !copy_)
    return std::nullopt;
  auto request = std::move(*copy_);
  copy_.reset();
  const auto page_is_current = [&] { return page_open_ && request.page == PageToken{pages_}; };
  if (!page_is_current())
    return Outcome{};
  copying_ = true;
  lock.unlock();
  const auto result = writer.manage(request.command, "");
  lock.lock();
  copying_ = false;
  if (!page_is_current())
    return Outcome{};
  Outcome outcome;
  outcome.page = request.page;
  outcome.messages.push_back(
      encode_result(request.id, result ? CommandOutcome::accepted : CommandOutcome::failed,
                    result ? std::string{} : result.error().text, {}, {}, {},
                    result ? std::nullopt : result.error().localization));
  return outcome;
}

auto Session::work(const Clock& clock) -> Work {
  std::optional<Queued> request;
  std::optional<ProgramPublication> publication;
  std::optional<PageToken> idle_page;
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
    } else if (publication_) {
      publication = std::exchange(publication_, std::nullopt);
    } else if (!requests_.empty()) {
      request = std::move(requests_.front());
      requests_.pop_front();
      retried_ = false;
    } else if (page_open_ && ready_) {
      idle_page = PageToken{pages_};
    }
  }
  if (request)
    return run(std::move(*request), clock);
  if (publication) {
    publish_program(*publication, clock);
    return {WorkStatus::done, {}, false};
  }
  if (restart_ && restart_->tool_request && !wanted(restart_->owner)) {
    restart_.reset();
    static_cast<void>(port_.abort());
  }
  Work result;
  std::vector<ControllerMessage> published;
  if (stopping) {
    flush_stops();
    result.status = WorkStatus::done;
  } else {
    const auto available = [&] {
      const std::scoped_lock lock{mutex_};
      return idle_page == PageToken{pages_} && page_open_ && ready_ && !closing_ && !hold_ &&
             stops_.empty() && requests_.empty() && !stops_due(true);
    };
    if (available())
      port_.idle_maintenance(available);
    if (idle_page)
      published = port_.take_messages();
  }
  const auto now = clock();
  const std::scoped_lock lock{mutex_};
  if (!page_open_ || !ready_)
    return result;
  result.outcome.page = PageToken{pages_};
  if (idle_page && idle_page != result.outcome.page)
    published.clear();
  for (const auto& message : published)
    result.outcome.messages.push_back(
        encode_message(message.error ? MessageLevel::error : MessageLevel::info, message.text,
                       now.unix_time, message.localization));
  report(result.outcome, now.unix_time);
  return result;
}

void Session::publish_program(ProgramPublication& request, const Clock& clock) {
  const auto controller = port_.snapshot();
  const auto [observed, epoch] = [&] {
    const std::scoped_lock lock{mutex_};
    return std::pair{status(clock().monotonic), epoch_};
  }();
  if (observed.link != Link::live) {
    request.result.set_value(std::unexpected("Cannot verify the loaded program while the controller is offline."));
    return;
  }
  const auto active = [](const ControllerState& state) -> bool {
    return state.interp != InterpState::idle || state.requested_tool || state.breakpoint_program ||
           (state.mode == TaskMode::automatic && !state.in_position);
  };
  const auto loaded = [&](const ControllerState& state) -> bool {
    return !state.program_file.empty() && (state.program_file == request.path ||
           programs_.resolve_program(state.program_file).value_or("") == request.path);
  };
  const auto using_file = [&](const ControllerState& state) -> bool {
    return active(state) && (loaded(state) ||
           (state.breakpoint_program && started_program_.source.inputs &&
            started_program_.source.inputs->program_file == request.path));
  };
  if (using_file(controller) || using_file(observed)) {
    request.result.set_value(std::unexpected("Stop the loaded program before replacing it."));
    return;
  }
  auto published = request.publish();
  if (published) {
    const auto now = port_.snapshot();
    if (loaded(now)) {
      started_program_ = {};
      verified_probe_program_.clear();
      const auto reopen_current = [this, epoch] {
        const std::scoped_lock lock{mutex_};
        return !closing_ && epoch == epoch_;
      };
      const auto reopened = active(now) ? ActionResult{ActionStatus::failed, "The program is running."}
                                        : port_.open_program(request.path, reopen_current);
      if (reopened.status != ActionStatus::accepted)
        published = std::unexpected("Saved on disk, but not reopened. " + reopened.reason);
    }
  }
  request.result.set_value(std::move(published));
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
    step = refuse(stopped_reason, RefusalMessage{"messages.session.jogStoppedBeforeStart"});
  } else {
    const auto* setter = std::get_if<FixedSetterRequest>(&request.command);
    const auto* changed = std::get_if<ToolChanged>(&request.command);
    const bool exclusive = (setter && setter->action != SetterAction::status) ||
                           (changed && changed->measure_request);
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
    if (current(request)) {
      requests_.push_front(std::move(request));
      result.status = WorkStatus::blocked;
      return result;
    }
    step = cancelled();
  }
  if (step.kind == StageKind::cancelled) {
    forget_press(request);
    // `rejected` only when nothing at all was sent for the request; otherwise `failed`, with
    // what was sent and what was not.
    if (step.reply.reason.empty() && request.sent)
      step.reply = {CommandOutcome::failed,
                    mode_changed_reason,
                    {},
                    MessageKey{"messages.session.modeChanged"}};
    if (step.reply.reason.empty())
      step.reply = {CommandOutcome::rejected, cancelled_reason,
                    RefusalMessage{"messages.session.cancelled"}};
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
  std::unique_lock lock{mutex_};
  const bool shown = page_open_ && request.page == PageToken{pages_};
  const bool quits =
      shown && step.quit && current(request) && step.reply.outcome == CommandOutcome::accepted;
  // A restore staged for an exit that does not come now (the page ended, a stop came first) is
  // dropped: a later exit must not write it.
  if (step.quit && !quits && machine_data_ != nullptr &&
      std::holds_alternative<RestoreBackup>(request.command)) {
    lock.unlock();
    machine_data_->abandon_restore();
    lock.lock();
  }
  if (!page_open_)
    return result;
  result.outcome.page = PageToken{pages_};
  if (request.page == result.outcome.page) {
    result.quit = quits;
    for (auto& message : step.produced)
      result.outcome.messages.push_back(std::move(message));
    result.outcome.messages.push_back(encode_result(request.id, step.reply.outcome,
                                                    step.reply.reason, step.setter, step.restart,
                                                    step.reply.refusal, step.reply.localization));
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
  if (!ready_)
    return;
  for (const auto& problem : problems_)
    outcome.messages.push_back(
        encode_message(MessageLevel::error, problem.text, unix_time, problem.localization));
  problems_.clear();
}

// The request is still to be carried out: its page shows and no cancellation happened since.
// A stop request always is. Asked after every call that can block and before every
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
    return refuse("The fixed setter is unavailable.",
                  RefusalMessage{"messages.session.setterUnavailable"});
  auto state = port_.snapshot();
  if (!wanted(request))
    return cancelled();
  if (restart_ && restart_->tool_request) {
    if (!wanted(restart_->owner) || state.machine != MachineState::on ||
        state.requested_tool != restart_->plan.point.tool ||
        state.tool_request != *restart_->tool_request) {
      const bool page_changed = !wanted(restart_->owner) && !cancels_requests(command);
      restart_.reset();
      if (page_changed)
        static_cast<void>(port_.abort());
    } else if (std::holds_alternative<Pause>(command)) {
      restart_.reset();
      return answer(reply(port_.abort()));
    } else if (!only_stops(command) && !std::holds_alternative<ToolChanged>(command)) {
      reject_press(request);
      return refuse("Finish or stop the restart tool change first.",
                    RefusalMessage{"messages.session.finishRestartToolChange"});
    }
  }
  if (restart_ && !restart_->tool_request && !std::holds_alternative<StartRestart>(command) &&
      !std::holds_alternative<PlanRestart>(command) &&
      !std::holds_alternative<ListDirectory>(command) &&
      !std::holds_alternative<ManageProgramFile>(command) &&
      !(std::holds_alternative<SaveProgram>(command) && !std::get<SaveProgram>(command).stamp))
    restart_.reset();
  auto decision = policy_.evaluate(command, state);
  std::optional<CheckedPreview> verified;
  if (preview_ != nullptr && decision.allowed) {
    if (const auto* run_command = std::get_if<Run>(&command)) {
      verified = preview_->checked_preview();
      if (auto issue = check_program_start(request, *run_command, state, *verified)) {
        if (!wanted(request))
          return cancelled();
        decision = {false, {}, std::move(issue->text), {}, std::move(issue->localization)};
      }
    }
  } else if (decision.allowed) {
    if (const auto* run_command = std::get_if<Run>(&command); run_command && run_command->breakpoint)
      decision = {false,
                  {},
                  "A breakpoint needs a complete program check.",
                  {},
                  RefusalMessage{"messages.session.breakpointNeedsCheck"}};
  }
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
      return answer({CommandOutcome::failed,
                     "LinuxCNC did not enter the required mode.",
                     {},
                     MessageKey{"messages.session.modeNotEntered"}});
    }
  }
  if (!decision.allowed) {
    // A press that is not valid must not leave a jog running on its axis.
    reject_press(request);
    // After a mode change was sent this is not `rejected` (nothing sent) any more.
    return answer({request.sent ? CommandOutcome::failed : CommandOutcome::rejected,
                   std::move(decision.reason), std::move(decision.refusal)});
  }
  const bool fresh_run = state.interp == InterpState::idle &&
      (std::holds_alternative<Run>(command) || std::holds_alternative<Step>(command));
  if (fresh_run || std::holds_alternative<OpenProgram>(command) ||
      std::holds_alternative<Mdi>(command) || cancels_requests(command))
    verified_probe_program_.clear();
  const auto* option = std::get_if<SetProgramOption>(&command);
  const auto* run_command = std::get_if<Run>(&command);
  if (fresh_run || std::holds_alternative<OpenProgram>(command) ||
      std::holds_alternative<Mdi>(command) || std::holds_alternative<StartRestart>(command) ||
      (option && option->option == ProgramOption::block_delete) || cancels_requests(command))
    started_program_ = {};
  const bool checked_run = fresh_run && tool_models_ != nullptr &&
      tool_models_->touch_probe_loaded(state.tool_in_spindle) == true;
  if (checked_run && !verified) {
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
  auto result = perform(request, decision, state, clock, verified);
  if (checked_run && result.kind == StageKind::reply && result.reply.outcome == CommandOutcome::accepted)
    verified_probe_program_ = state.program_file;
  if (fresh_run && run_command && !run_command->breakpoint && verified && result.kind == StageKind::reply &&
      result.reply.outcome == CommandOutcome::accepted)
    started_program_ = std::move(*verified);
  return result;
}

auto Session::check_program_start(const Queued& request, const Run& command,
                                  const ControllerState& state, const CheckedPreview& check) const
    -> std::optional<ProgramCheckIssue> {
  if (!check.preview || !check.source.inputs ||
      check.source.inputs->program_file != state.program_file)
    return ProgramCheckIssue{"Wait for the program check to finish.",
                             MessageKey{"messages.session.waitProgramCheck"}};
  if (command.preview_context && *command.preview_context != check.preview->context)
    return ProgramCheckIssue{"The program check changed. Review the current results.",
                             MessageKey{"messages.session.programCheckChanged"}};
  const auto& program_check = check.preview->check;
  if (program_check && !program_check->errors.empty())
    return program_check->errors.front();
  if (command.breakpoint) {
    // The path up to where a preview ends is the program's own: a breakpoint on it pauses the
    // run before anything the preview could not follow. Run itself needs the review then.
    if (!check.source.program_snapshot || !check.files_changed || !program_check ||
        (check.preview->state != PreviewState::ready &&
         check.preview->state != PreviewState::failed))
      return ProgramCheckIssue{"A breakpoint needs a complete program check.",
                               MessageKey{"messages.session.breakpointNeedsCheck"}};
    if (std::ranges::none_of(check.preview->segments, [&](const auto& segment) {
          return segment.line == command.breakpoint->line;
        }))
      return ProgramCheckIssue{"Choose a motion line in the current program preview.",
                               MessageKey{"messages.session.chooseMotionLine"}};
    // Bind the operator's picked text to the checked bytes, including when a program on disk
    // was replaced after the panel loaded its text.
    std::string_view remaining = *check.source.program_snapshot;
    for (int line = 1; line < command.breakpoint->line && !remaining.empty(); ++line)
      static_cast<void>(program_line(remaining));
    const auto selected = program_line(remaining);
    if (selected.empty() || selected != command.breakpoint->text)
      return ProgramCheckIssue{
          "The selected program line changed. Reopen the program and choose the line again.",
          MessageKey{"messages.session.selectedLineChanged"}};
  }
  const bool units_mismatch =
      program_check && command.display_units && std::ranges::any_of(program_check->units, [&](LengthUnit unit) {
        return unit != *command.display_units;
      });
  if ((!program_check || !program_check->warnings.empty() || units_mismatch) && !command.confirmed)
    return ProgramCheckIssue{"Review the program check warnings before starting.",
                             MessageKey{"messages.session.reviewWarnings"}};
  if (check.files_changed && check.files_changed([&] { return !wanted(request); }))
    return ProgramCheckIssue{"The program or tool data changed. Review the new program check.",
                             MessageKey{"messages.session.programOrToolsChanged"}};
  if (preview_->checked_preview().preview != check.preview)
    return ProgramCheckIssue{"The program check changed. Review the current results.",
                             MessageKey{"messages.session.programCheckChanged"}};
  auto reason = port_.validate_program_inputs(*check.source.inputs);
  if (!reason.empty())
    return ProgramCheckIssue{std::move(reason.text), std::move(reason.localization)};
  return std::nullopt;
}

auto Session::plan_restart(Queued& request, PlanRestart command, const ControllerState& state)
    -> Stage {
  restart_.reset();
  if (preview_ == nullptr || restart_engine_ == nullptr)
    return refuse("Restart planning is unavailable.",
                  RefusalMessage{"messages.session.restartUnavailable"});
  const auto source = preview_->restart_source(command.preview_context);
  if (!source.inputs || !source.program_snapshot ||
      source.inputs->program_file != state.program_file) {
    const auto shown = preview_->latest();
    if (shown && shown->context == command.preview_context) {
      if (const auto& error = shown->error)
        return answer({CommandOutcome::rejected, error->text, error->localization});
    }
    return refuse("Wait for an up-to-date program preview.",
                  RefusalMessage{"messages.session.waitPreview"});
  }
  auto inputs = *source.inputs;
  inputs.restart_line = command.line;
  auto computed = restart_engine_->compute(inputs, [&] { return !wanted(request); });
  if (computed.cancelled || !wanted(request))
    return cancelled();
  if (!computed.program_snapshot || *computed.program_snapshot != *source.program_snapshot ||
      preview_->restart_source(command.preview_context).inputs != source.inputs)
    return refuse("The program preview changed. Choose the line again.",
                  RefusalMessage{"messages.session.previewChanged"});
  const auto& outcome = computed.outcome;
  if (!outcome.restart || outcome.restart->line != command.line || !computed.files_changed) {
    if (!outcome.restart_blocked.empty())
      return answer({CommandOutcome::rejected, outcome.restart_blocked});
    if (const auto& error = outcome.error)
      return answer({CommandOutcome::rejected, error->text, error->localization});
    return refuse("No verified motion is available at that line.",
                  RefusalMessage{"messages.session.noVerifiedMotion"});
  }
  auto point = *outcome.restart;
  if (tool_models_ != nullptr && tool_models_->touch_probe_loaded(point.tool) != false)
    return refuse("Restart requires a tool verified for machining.",
                  RefusalMessage{"messages.session.restartNeedsTool"});
  if (auto reason = port_.validate_restart(point, inputs); !reason.empty())
    return answer({CommandOutcome::rejected, std::move(reason)});
  if (!wanted(request))
    return cancelled();
  RestartPlan plan{++restart_id_, point.tool != state.tool_in_spindle, point};
  restart_ = PreparedRestart{plan, outcome.restart->clearance_z, command.preview_context, request, source,
                              std::move(computed.files_changed), {}};
  Stage result;
  result.restart = plan;
  return result;
}

auto Session::send_restart(const Queued& request, const PreparedRestart& restart, bool after_change)
    -> Stage {
  const auto still = token(request);
  const auto cutting_tool = [&] {
    return tool_models_ == nullptr || tool_models_->touch_probe_loaded(restart.plan.point.tool) == false;
  };
  if (!cutting_tool())
    return refuse("Restart requires a tool verified for machining.",
                  RefusalMessage{"messages.session.restartNeedsTool"});
  const auto current_files = [&] {
    return still() && cutting_tool() && !restart.files_changed([&] { return !still(); });
  };
  if (!current_files())
    return refuse("The restart was cancelled or the program changed.",
                  RefusalMessage{"messages.session.restartCancelled"});
  {
    const std::scoped_lock lock{mutex_};
    restart_busy_ = true;
    ++setter_tool_;
  }
  auto result = port_.start_restart(restart.plan.point, *restart.source.inputs, after_change,
                                   current_files, still);
  {
    const std::scoped_lock lock{mutex_};
    restart_busy_ = false;
  }
  return staged(std::move(result));
}

auto Session::start_restart(const Queued& request, StartRestart command) -> Stage {
  if (!restart_ || restart_->tool_request || restart_->plan.plan_id != command.plan_id ||
      !wanted(restart_->owner))
    return refuse("Choose a line and review the restart plan again.",
                  RefusalMessage{"messages.session.reviewRestartPlan"});
  auto restart = std::move(*restart_);
  restart_.reset();
  if (preview_->restart_source(restart.context).inputs != restart.source.inputs)
    return refuse("The program preview changed. Choose the line again.",
                  RefusalMessage{"messages.session.previewChanged"});
  auto check = restart.plan.point;
  check.clearance_z = restart.programmed_clearance;
  if (auto reason = port_.validate_restart(check, *restart.source.inputs); !reason.empty())
    return answer({CommandOutcome::rejected, std::move(reason)});
  if (check.clearance_z > restart.plan.point.clearance_z + 1e-6)
    return refuse("The required clearance changed. Review a new plan.",
                  RefusalMessage{"messages.session.clearanceChanged"});
  cancel_jogs(nullptr);
  if (!wanted(request))
    return cancelled();
  auto result = send_restart(request, restart, false);
  if (result.kind != StageKind::reply || result.reply.outcome != CommandOutcome::accepted)
    return result;
  if (!restart.plan.needs_tool_change) {
    started_program_ = std::move(restart.measured_run);
    return result;
  }
  const auto pending = port_.snapshot();
  if (pending.requested_tool != restart.plan.point.tool || !wanted(request)) {
    static_cast<void>(port_.abort());
    return answer({CommandOutcome::failed,
                   "Restart stopped before the tool change was confirmed.",
                   {},
                   MessageKey{"messages.session.restartStoppedBeforeToolChange"}});
  }
  restart.owner = request;
  restart.tool_request = pending.tool_request;
  restart_ = std::move(restart);
  return result;
}

auto Session::resume_restart(const Queued& request, const ControllerState& state) -> Stage {
  auto restart = std::exchange(restart_, std::nullopt);
  if (!restart || restart->tool_request != state.tool_request ||
      restart->plan.point.tool != state.requested_tool || !wanted(restart->owner))
    return refuse("The restart tool change is no longer pending.",
                  RefusalMessage{"messages.session.restartToolChangeGone"});
  const auto still = token(request);
  if (tool_models_ != nullptr && tool_models_->touch_probe_loaded(restart->plan.point.tool) != false) {
    static_cast<void>(port_.abort());
    return answer({CommandOutcome::failed,
                   "Restart requires a tool verified for machining.",
                   {},
                   MessageKey{"messages.session.restartNeedsTool"}});
  }
  if (!still() || restart->files_changed([&] { return !still(); })) {
    static_cast<void>(port_.abort());
    return answer({CommandOutcome::failed,
                   "The restart was cancelled or the program changed.",
                   {},
                   MessageKey{"messages.session.restartCancelled"}});
  }
  auto ack = port_.tool_changed(restart->plan.point.tool, state.tool_request, still);
  if (ack.status != ActionStatus::accepted) {
    static_cast<void>(port_.abort());
    return staged(std::move(ack));
  }
  return send_restart(request, *restart, true);
}

auto Session::measure_tool_change(Queued& request, const ControllerState& state) -> Stage {
  const auto& source = started_program_.source;
  const auto& preview = started_program_.preview;
  if (preview_ == nullptr || restart_engine_ == nullptr || !port_.fixed_setter_available() ||
      !preview || preview->state != PreviewState::ready || !source.inputs ||
      !source.program_snapshot || !started_program_.files_changed ||
      source.inputs->program_file != state.program_file || !state.tool_change_line ||
      !state.requested_tool)
    return refuse("Measuring at M6 requires a complete check of the running program.",
                  RefusalMessage{"backend.toolChangeMeasurement.needsProgramCheck"});
  const auto tool = *state.requested_tool;
  if (tool_models_ != nullptr && tool_models_->touch_probe_loaded(tool) != false)
    return refuse("Restart requires a tool verified for machining.",
                  RefusalMessage{"messages.session.restartNeedsTool"});
  const auto line = *state.tool_change_line;
  const auto target = std::ranges::find_if(preview->segments, [line](const auto& segment) {
    return segment.line > line;
  });
  if (target == preview->segments.end() || target->tool != tool)
    return refuse("There is no verified motion to restart after this tool change.",
                  RefusalMessage{"backend.toolChangeMeasurement.noMotionAfterChange"});

  std::string_view remaining = *source.program_snapshot;
  for (int index = 1; index < line && !remaining.empty(); ++index)
    static_cast<void>(program_line(remaining));
  const auto exchange = inspect_probe_code(program_line(remaining));
  if (!exchange.changes_tool || exchange.relabels_tool || exchange.stops_program ||
      !exchange.refusal.empty())
    return refuse("The original M6 block could not be verified.",
                  RefusalMessage{"backend.toolChangeMeasurement.notVerifiedM6"});
  for (int index = line + 1; index < target->line; ++index) {
    const auto block = inspect_probe_code(program_line(remaining));
    if (block.changes_tool || block.relabels_tool || block.stops_program ||
        block.refusal == probe_code_unknown)
      return refuse("Restart would skip a tool change, stop, or unsupported block.",
                    RefusalMessage{"backend.toolChangeMeasurement.skipUnsupported"});
  }
  const auto context = [&]() -> SetterEpoch {
    const std::scoped_lock lock{mutex_};
    return {request.page.value, setter_datum_, setter_tool_, cancels_};
  };
  const auto before = context();
  const auto still = [&] { return wanted(request) && context().datum == before.datum; };
  const auto setter = port_.fixed_setter({SetterAction::status}, before, still);
  if (!setter || !setter->calibration)
    return refuse("Teach the fixed setter before measuring at a program tool change.",
                  RefusalMessage{"backend.toolChangeMeasurement.teachFirst"});

  const auto anchored = [&](const RestartPoint& point, double length) {
    return point.line == target->line && point.tool == tool && point.h == tool &&
           std::abs(point.tool_offset[2] - length) <= 0.001 &&
           std::ranges::equal(restart_machine_position(point), state.tool_change_position,
                              [](double expected, double actual) {
                                return std::abs(expected - actual) <= 0.001;
                              });
  };
  const auto old_tool = std::ranges::find(source.inputs->tools, tool, &Tool::number);
  if (old_tool == source.inputs->tools.end())
    return refuse("A cutting tool in the tool table and a fixed setter are required.",
                  RefusalMessage{"backend.toolChangeMeasurement.requiresToolAndSetter"});
  auto trial_inputs = *source.inputs;
  trial_inputs.restart_line = target->line;
  trial_inputs.restart_loaded_tool = tool;
  const auto trial = restart_engine_->compute(trial_inputs, [&] { return !still(); });
  if (!still() || trial.cancelled || !trial.program_snapshot ||
      *trial.program_snapshot != *source.program_snapshot ||
      started_program_.files_changed([&] { return !still(); }))
    return refuse("The program or tool data changed before measurement.",
                  RefusalMessage{"backend.toolChangeMeasurement.dataChangedBeforeMeasure"});
  const auto& trial_point = trial.outcome.restart;
  if (!trial_point || !anchored(*trial_point, old_tool->length) || !trial.files_changed) {
    if (!trial.outcome.restart_blocked.empty())
      return answer({CommandOutcome::rejected, trial.outcome.restart_blocked});
    return refuse("There is no verified motion to restart after this tool change.",
                  RefusalMessage{"backend.toolChangeMeasurement.noMotionAfterChange"});
  }

  auto prepared = port_.prepare_tool_measurement(tool, state.tool_request, line,
                                                 *trial_point, *source.inputs, still);
  if (prepared.status != ActionStatus::accepted)
    return staged(std::move(prepared));
  auto original = std::exchange(started_program_, {});
  request.sent = true;
  {
    std::unique_lock lock{mutex_};
    const auto loaded = [&] {
      return idle_for_tool_measurement(status_) && status_.tool_in_spindle == tool;
    };
    work_ready_.wait_for(lock, std::chrono::seconds{2}, [&] {
      return loaded() || !current(request);
    });
    if (!current(request) || !loaded())
      return answer({CommandOutcome::failed,
                     "The program stopped before the tool could be measured.",
                     {},
                     MessageKey{"backend.toolChangeMeasurement.stoppedBeforeMeasure"}});
  }
  const auto measurement_epoch = context();
  const auto maintain = [&] { return still() && context() == measurement_epoch; };
  if (!maintain() || original.files_changed([&] { return !maintain(); }))
    return answer({CommandOutcome::failed,
                   "The program stopped before the tool could be measured.",
                   {},
                   MessageKey{"backend.toolChangeMeasurement.stoppedBeforeMeasure"}});
  const auto ready = port_.snapshot();
  const FixedSetterRequest measurement{SetterAction::touch_off, tool,
                                        {ready.axes[0].machine, ready.axes[1].machine, ready.axes[2].machine}};
  const auto measured = port_.fixed_setter(measurement, measurement_epoch, maintain);
  if (!measured)
    return answer({CommandOutcome::failed, measured.error()});
  const auto& recorded = measured->measurement;
  if (!recorded || recorded->calibration)
    return answer({CommandOutcome::failed,
                   "LinuxCNC did not measure the tool length.",
                   {},
                   MessageKey{"backend.toolChangeMeasurement.noMeasurement"}});
  const auto length = recorded->value;

  Stage result;
  result.setter = *measured;
  const auto cannot_restart = [&](std::string reason, std::optional<MessageKey> localization = {}) {
    result.reply.outcome = CommandOutcome::failed;
    result.reply.reason = std::move(reason);
    result.reply.localization = std::move(localization);
    return result;
  };
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
  CheckedPreview fresh;
  for (;;) {
    if (!maintain())
      return cannot_restart("Tool measured. Restart preparation was cancelled.",
                            MessageKey{"backend.toolChangeMeasurement.measuredCancelled"});
    fresh = preview_->checked_preview();
    if (fresh.preview && fresh.preview->state == PreviewState::ready && fresh.source.inputs &&
        fresh.source.inputs->program_file == state.program_file && fresh.files_changed &&
        fresh.source.program_snapshot && *fresh.source.program_snapshot == *original.source.program_snapshot &&
        fresh.source.inputs->block_delete == original.source.inputs->block_delete &&
        fresh.source.inputs->start.tool_in_spindle == tool &&
        std::ranges::any_of(fresh.source.inputs->tools, [&](const auto& row) {
          return row.number == tool && std::abs(row.length - length) <= 0.001;
        }))
      break;
    if (std::chrono::steady_clock::now() >= deadline)
      return cannot_restart("Tool measured. The updated program check is unavailable.",
                            MessageKey{"backend.toolChangeMeasurement.measuredCheckUnavailable"});
    std::unique_lock lock{mutex_};
    work_ready_.wait_for(lock, std::chrono::milliseconds{50}, [&] { return !current(request); });
  }
  // Reproduce the planned path from before the M6. A preview started at the setter can
  // inherit its XY on the first Z-only motion. Validate the approach from the live pose.
  auto inputs = *original.source.inputs;
  inputs.tools = fresh.source.inputs->tools;
  inputs.raw_tools = fresh.source.inputs->raw_tools;
  // The helper seeds slot zero from the original spindle tool. The live slot now holds M6's tool.
  std::erase_if(inputs.raw_tools, [](const auto& entry) { return entry.index == 0; });
  auto replay = restart_engine_->compute(inputs, [&] { return !maintain(); });
  if (!maintain() || replay.cancelled || !replay.program_snapshot ||
      *replay.program_snapshot != *original.source.program_snapshot)
    return cannot_restart("Tool measured. The restart source changed.",
                          MessageKey{"backend.toolChangeMeasurement.measuredSourceChanged"});
  if (replay.outcome.state != PreviewState::ready || !replay.outcome.check || !replay.files_changed)
    return cannot_restart("Tool measured. The updated program check is unavailable.",
                          MessageKey{"backend.toolChangeMeasurement.measuredCheckUnavailable"});
  if (!replay.outcome.check->errors.empty()) {
    const auto& issue = replay.outcome.check->errors.front();
    return cannot_restart(issue.text, issue.localization);
  }
  inputs.restart_line = target->line;
  inputs.restart_loaded_tool = tool;
  auto computed = restart_engine_->compute(inputs, [&] { return !maintain(); });
  if (!maintain() || computed.cancelled || !computed.program_snapshot ||
      *computed.program_snapshot != *original.source.program_snapshot ||
      preview_->restart_source(fresh.preview->context).inputs != fresh.source.inputs)
    return cannot_restart("Tool measured. The restart source changed.",
                          MessageKey{"backend.toolChangeMeasurement.measuredSourceChanged"});
  auto point = computed.outcome.restart;
  if (!point || !anchored(*point, length) || !computed.files_changed) {
    if (!computed.outcome.restart_blocked.empty())
      return cannot_restart(computed.outcome.restart_blocked);
    return cannot_restart("Tool measured. There is no verified restart at the next motion.",
                          MessageKey{"backend.toolChangeMeasurement.measuredNoRestart"});
  }
  const double clearance = point->clearance_z;
  if (auto reason = port_.validate_restart(*point, *fresh.source.inputs); !reason.empty())
    return cannot_restart(std::move(reason));
  if (!maintain())
    return cannot_restart("Tool measured. Restart preparation was cancelled.",
                          MessageKey{"backend.toolChangeMeasurement.measuredCancelled"});
  RestartPlan plan{++restart_id_, false, *point};
  inputs.restart_line.reset();
  inputs.restart_loaded_tool.reset();
  auto& outcome = replay.outcome;
  CheckedPreview new_run{
      std::make_shared<const Preview>(
          Preview{fresh.preview->context, outcome.state, std::move(outcome.error),
                  std::move(outcome.segments), outcome.bounds, std::move(outcome.dwells),
                  std::move(outcome.check)}),
      {std::make_shared<const PreviewInputs>(std::move(inputs)), std::move(replay.program_snapshot)},
      std::move(replay.files_changed)};
  restart_ = PreparedRestart{plan, clearance, fresh.preview->context, request, fresh.source,
                              std::move(computed.files_changed), {}, std::move(new_run)};
  result.restart = plan;
  return result;
}

auto Session::probe_block(const Command& command, const ControllerState& state) const -> std::string {
  const auto* spindle = std::get_if<SetSpindle>(&command);
  const auto* mdi_command = std::get_if<Mdi>(&command);
  const auto* setter = std::get_if<FixedSetterRequest>(&command);
  const bool check_tool = setter && setter->action == SetterAction::check;
  const bool program = std::holds_alternative<Run>(command) || std::holds_alternative<Step>(command) ||
                       std::holds_alternative<Resume>(command);
  const bool confirms_probe_candidate = std::holds_alternative<ToolChanged>(command) &&
      state.requested_tool && !(restart_ && restart_->tool_request);
  if (tool_models_ == nullptr || (!confirms_probe_candidate && !check_tool && !program && mdi_command == nullptr &&
                                 (spindle == nullptr || spindle->direction == SpindleDirection::stopped)))
    return {};
  if (mdi_command) {
    const auto block = inspect_probe_code(mdi_command->text);
    if (block.relabels_tool && (block.changes_tool || !block.refusal.empty()))
      return "Separate M61 from tool changes and spindle commands.";
  }
  const auto loaded = tool_models_->touch_probe_loaded(
      confirms_probe_candidate ? *state.requested_tool : state.tool_in_spindle);
  if (!loaded)
    return "Cannot verify the loaded tool type. Check tool model storage.";
  if (!*loaded)
    return {};
  if (confirms_probe_candidate) {
    if (state.spindle_active)
      return probe_loaded_reason;
    if (state.mode == TaskMode::mdi) {
      const auto block = inspect_probe_code(state.active_mdi);
      if (block.changes_tool && !block.relabels_tool && block.refusal.empty() && !state.mdi_queued)
        return {};
      return "Press Stop, then load the touch probe with a separate tool change.";
    }
    if (state.mode != TaskMode::automatic || state.program_file.empty() ||
        state.program_file != verified_probe_program_)
      return "Stop the program and load the touch probe from Tools before running.";
  }
  if (check_tool)
    return "Load a cutting tool before checking its length.";
  if (mdi_command != nullptr)
    return std::string(probe_code_block(mdi_command->text));
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
      now.tool_in_spindle != state.tool_in_spindle || now.spindle_active ||
      (confirms_probe_candidate && (now.tool_request != state.tool_request ||
                                    now.requested_tool != state.requested_tool)))
    return "A touch probe is loaded. Machine state changed during the spindle check.";
  return {};
}

auto Session::perform(Queued& request, const Decision& decision, const ControllerState& state,
                      const Clock& clock, const std::optional<CheckedPreview>& verified) -> Stage {
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
      std::holds_alternative<SetWorkRotation>(request.command) ||
      std::holds_alternative<ClearG92>(request.command) ||
      std::holds_alternative<RestoreOffset>(request.command) ||
      std::holds_alternative<TouchOff>(request.command) ||
      std::holds_alternative<ToolTouchOff>(request.command) ||
      std::holds_alternative<SetTool>(request.command) ||
      std::holds_alternative<RemoveTool>(request.command) ||
      std::holds_alternative<OpenProgram>(request.command) ||
      std::holds_alternative<EditProgram>(request.command) ||
      (std::holds_alternative<SaveProgram>(request.command) &&
       std::get<SaveProgram>(request.command).stamp)) {
    const std::scoped_lock lock{mutex_};
    if (changes_datum)
      ++setter_datum_;
    ++setter_tool_;
  }
  // The offset history attributes what changes while this runs, and for a moment after, to it.
  // A restore marks itself, with the entry it restores.
  const auto source = history_source(request.command);
  const auto mark = history_ != nullptr && source && *source != HistorySource::restore
                        ? std::optional<std::uint64_t>{history_->begin(*source)}
                        : std::nullopt;
  auto stage = std::visit(
      [&](const auto& c) -> Stage {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, Ready>) {
          return {};
        } else if constexpr (std::is_same_v<T, RestoreOffset>) {
          return restore_offset(request, c, state, clock);
        } else if constexpr (std::is_same_v<T, ListDirectory>) {
          Stage step;
          step.reply = list_directory(c, step);
          return step;
        } else if constexpr (std::is_same_v<T, ManageProgramFile>) {
          if (program_writer_ == nullptr)
            return refuse("Program file management is unavailable.",
                          RefusalMessage{"backend.programFiles.unavailable"});
          const auto observed = loaded_program();
          const auto result = program_writer_->manage(c, observed.empty() ? state.program_file : observed);
          return answer(result ? Reply{CommandOutcome::accepted, ""}
                               : Reply{CommandOutcome::failed,
                                       result.error().text,
                                       {},
                                       result.error().localization});
        } else if constexpr (std::is_same_v<T, SetMachine>) {
          // E-stop and power-off go out first; stops for held jogs follow.
          auto result = act(request, [&](const Wanted& still) -> ActionResult {
            return port_.set_machine_state(c.state, still);
          });
          if (c.state != MachineState::on)
            cancel_jogs(nullptr);
          return result;
        } else if constexpr (std::is_same_v<T, Backup> || std::is_same_v<T, CheckRestore> ||
                             std::is_same_v<T, RestoreBackup>) {
          if (machine_data_ == nullptr)
            return answer({CommandOutcome::rejected,
                           "This CoffeeCNC cannot back up or restore.",
                           {},
                           MessageKey{"messages.session.noBackup"}});
          MachineDataAnswer done;
          if constexpr (std::is_same_v<T, Backup>) {
            done = machine_data_->backup(c.directory);
          } else if constexpr (std::is_same_v<T, CheckRestore>) {
            done = machine_data_->check_restore(c.path);
          } else {
            // The machine as it is now, just before anything is staged.
            done = machine_data_->restore(
                c.path, c.token, request.page.value, [&]() -> LocalizedReason {
                  auto recheck = policy_.evaluate(request.command, port_.snapshot());
                  return recheck.allowed ? LocalizedReason{}
                                         : LocalizedReason{std::move(recheck.reason),
                                                           std::move(recheck.refusal)};
                });
          }
          Stage step;
          step.reply.outcome = done.outcome;
          step.reply.reason = std::move(done.reason);
          step.reply.localization = std::move(done.localization);
          step.produced = std::move(done.messages);
          step.quit = done.quit;
          return step;
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
          // Retire any previously acknowledged increment before homing. It has to be fenced
          // even while motion reports at rest because it may have been queued without moving yet.
          bool had_jog = false;
          {
            const std::scoped_lock lock{mutex_};
            had_jog = std::ranges::any_of(jog_, [](const AxisJog& axis) {
              return axis.running || axis.stepping || axis.in_flight || axis.pressed;
            });
          }
          if (had_jog)
            cancel_jogs(nullptr);
          {
            const std::scoped_lock lock{mutex_};
            if (hold_ || low_periods_ < settled_periods || !reconciled())
              return Stage{StageKind::blocked, {}, {}};
          }
          return act(request, [&](const Wanted& still) -> ActionResult {
            return port_.home(c.axis, still);
          });
        } else if constexpr (std::is_same_v<T, Jog>) {
          return jog(request, c, decision, clock);
        } else if constexpr (std::is_same_v<T, JogStep>) {
          return jog_step(request, c, decision);
        } else if constexpr (std::is_same_v<T, JogPausedZ>) {
          return act(request, [&](const Wanted& still) {
            return port_.jog_paused_z(c.distance, c.stop_spindle, still);
          });
        } else if constexpr (std::is_same_v<T, JogStopAll>) {
          return answer(reply(stop_all()));
        } else if constexpr (std::is_same_v<T, Mdi>) {
          return mdi(request, c.text);
        } else if constexpr (std::is_same_v<T, OpenProgram>) {
          // LinuxCNC gets the resolved path. It opens the file itself, later: a path that is
          // changed between this check and that open is not caught here.
          const auto file = programs_.resolve_program(c.path);
          if (!file)
            return answer(
                {CommandOutcome::rejected, file.error().text, {}, file.error().localization});
          return act(request, [&](const Wanted& still) -> ActionResult {
            return port_.open_program(*file, still);
          });
        } else if constexpr (std::is_same_v<T, EditProgram>) {
          if (program_writer_ == nullptr)
            return refuse("This CoffeeCNC cannot edit programs.",
                          RefusalMessage{"messages.session.cannotEdit"});
          const auto file = programs_.resolve_program(c.path);
          if (!file)
            return answer(
                {CommandOutcome::rejected, file.error().text, {}, file.error().localization});
          const auto loaded = programs_.resolve_program(state.program_file);
          if (!loaded || *file != *loaded)
            return refuse("Only the loaded program can be edited.",
                          RefusalMessage{"messages.session.onlyLoadedEditable"});
          return act(request, [&](const Wanted& still) -> ActionResult {
            auto written = program_writer_->replace_line(*file, c.line, c.before, c.after);
            if (!written)
              return {ActionStatus::failed, written.error().text, written.error().localization};
            // LinuxCNC reads the new text only when the program is opened again.
            return port_.open_program(*written, still);
          });
        } else if constexpr (std::is_same_v<T, SaveProgram>) {
          if (program_writer_ == nullptr)
            return refuse("This CoffeeCNC cannot save programs.",
                          RefusalMessage{"messages.session.cannotSave"});
          if (c.stamp) {
            const auto file = programs_.resolve_program(c.path);
            if (!file)
              return answer(
                  {CommandOutcome::rejected, file.error().text, {}, file.error().localization});
            const auto loaded = programs_.resolve_program(state.program_file);
            if (!loaded || *file != *loaded)
              return refuse("Only the loaded program can be edited.",
                            RefusalMessage{"messages.session.onlyLoadedEditable"});
            return act(request, [&](const Wanted& still) -> ActionResult {
              const auto check_controller = [&]() -> LocalizedReason {
                if (!still())
                  return {"The save was cancelled before the program was replaced.",
                          MessageKey{"messages.session.saveCancelled"}};
                const auto now = port_.snapshot();
                if (const auto recheck = policy_.evaluate(request.command, now); !recheck.allowed)
                  return {recheck.reason, recheck.refusal};
                const auto active = programs_.resolve_program(now.program_file);
                if (!active || *active != *file)
                  return {"The loaded program changed before the save finished.",
                          MessageKey{"messages.session.programChangedDuringSave"}};
                return {};
              };
              auto written = program_writer_->replace(*file, c.text, *c.stamp, check_controller);
              if (!written)
                return {ActionStatus::failed, written.error().text, written.error().localization};
              // LinuxCNC reads the new text only when the program is opened again.
              auto reopened = port_.open_program(*written, still);
              if (reopened.status != ActionStatus::accepted)
                return {
                    ActionStatus::failed,
                    std::format("Saved on disk, but not reopened. {}", reopened.reason),
                    MessageKey{"messages.session.savedNotReopened", {{"detail", reopened.reason}}}};
              return reopened;
            });
          }
          // Through a link too: the file LinuxCNC has open is never written over.
          if (c.replace && !state.program_file.empty()) {
            const auto target = programs_.resolve_new_program(c.path);
            const auto loaded = programs_.resolve_program(state.program_file);
            if (target && target->exists &&
                (target->path == state.program_file ||
                 (loaded && programs_.resolve_program(target->path).value_or("") == *loaded)))
              return refuse("The loaded program is not replaced. Choose another name.",
                            RefusalMessage{"messages.session.loadedNotReplaced"});
          }
          auto written = program_writer_->create(c.path, c.text, c.replace);
          if (!written)
            return answer(
                {CommandOutcome::failed, written.error().text, {}, written.error().localization});
          return answer({CommandOutcome::accepted, ""});
        } else if constexpr (std::is_same_v<T, Run>) {
          return act(request, [&](const Wanted& still) -> ActionResult {
            if (!verified)
              return port_.run(still);
            const auto current = [&] {
              return still() &&
                     (verified->files_changed
                          ? !verified->files_changed([&] { return !still(); })
                          : probe_block(request.command, port_.snapshot()).empty()) &&
                     preview_->checked_preview().preview == verified->preview;
            };
            if (c.breakpoint)
              return port_.run_to_breakpoint(*verified->source.inputs,
                                             *verified->source.program_snapshot, c.breakpoint->line,
                                             current, still);
            return port_.run_checked(*verified->source.inputs, current, still);
          });
        } else if constexpr (std::is_same_v<T, PlanRestart>) {
          return plan_restart(request, c, state);
        } else if constexpr (std::is_same_v<T, StartRestart>) {
          verified_probe_program_.clear();
          return start_restart(request, c);
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
            return refuse("Work coordinate system must be G54 through G59.3.",
                          RefusalMessage{"messages.policy.wcsRange"});
          return mdi(request, std::string(wcs_words[static_cast<std::size_t>(c.wcs) - 1]));
        } else if constexpr (std::is_same_v<T, SetWorkOffset>) {
          // An explicit P index: editing an inactive system must not select it.
          // Omit R so XYZ editing, copying and clearing preserve the XY rotation.
          auto written = mdi(request, "G10 L2 P" + std::to_string(c.wcs) +
                                          " X" + length(c.offset[0], state) +
                                          " Y" + length(c.offset[1], state) +
                                          " Z" + length(c.offset[2], state));
          if (mark && written.kind == StageKind::reply &&
              written.reply.outcome == CommandOutcome::accepted)
            history_->written(*mark, c.wcs, {c.offset[0], c.offset[1], c.offset[2], std::nullopt},
                              clock().unix_time);
          return written;
        } else if constexpr (std::is_same_v<T, SetWorkRotation>) {
          // R alone: LinuxCNC keeps the XYZ of the system. Degrees in every unit mode.
          auto written = mdi(request, "G10 L2 P" + std::to_string(c.wcs) + " R" + real(c.degrees));
          if (mark && written.kind == StageKind::reply &&
              written.reply.outcome == CommandOutcome::accepted)
            history_->written(*mark, c.wcs, {std::nullopt, std::nullopt, std::nullopt, c.degrees},
                              clock().unix_time);
          return written;
        } else if constexpr (std::is_same_v<T, ClearG92>) {
          // G92 and G52 are one offset in LinuxCNC: G92.1 zeroes it, and the values G92.3 would
          // bring back. The offset history records the change from the status and can restore it.
          return mdi(request, "G92.1");
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
          if (c.measure_request)
            return measure_tool_change(request, state);
          if (restart_ && restart_->tool_request)
            return resume_restart(request, state);
          // The tool the decision was made for: not another one asked for since.
          return act(request, [&](const Wanted& still) -> ActionResult {
            return port_.tool_changed(state.requested_tool.value_or(-1), state.tool_request, still);
          });
        } else {
          constexpr bool served_in_submit =
              std::is_same_v<T, SetToolModelDefaults> || std::is_same_v<T, SetToolModelAssignment> ||
              std::is_same_v<T, SetOperatorSettings> || std::is_same_v<T, WatchDiagnostics> ||
              std::is_same_v<T, LaunchHalTool>;
          static_assert(served_in_submit, "Every command needs an action.");
          // `submit` hands these to the tool-model or diagnostics service: none is ever queued.
          return refuse(no_tool_models_reason, RefusalMessage{"messages.session.noToolModels"});
        }
      },
      request.command);
  if (mark)
    history_->end(*mark, clock().monotonic);
  return stage;
}

auto Session::restore_offset(const Queued& request, const RestoreOffset& command,
                             const ControllerState& state, const Clock& clock) -> Stage {
  if (history_ == nullptr)
    return refuse("This CoffeeCNC keeps no offset history.",
                  RefusalMessage{"messages.session.noOffsetHistory"});
  const auto entry = history_->entry(command.entry);
  if (!entry)
    return refuse("That history entry is no longer kept.",
                  RefusalMessage{"messages.session.historyEntryGone"});
  std::string words;
  std::array<std::optional<double>, 4> values{};
  for (const auto& change : entry->changes) {
    if (!change.before || !change.after)
      return refuse("This entry has no earlier value to restore.",
                    RefusalMessage{"messages.session.noEarlierValue"});
    const auto value = *change.before;
    switch (change.field) {
    case HistoryField::x:
      words += " X" + length(value, state);
      values[0] = value;
      break;
    case HistoryField::y:
      words += " Y" + length(value, state);
      values[1] = value;
      break;
    case HistoryField::z:
      words += " Z" + length(value, state);
      values[2] = value;
      break;
    case HistoryField::rotation_xy:
      words += " R" + real(value);
      values[3] = value;
      break;
    case HistoryField::length:
      words += " Z" + length(value, state);
      break;
    case HistoryField::diameter:
      // G10 L1 R is the radius.
      words += " R" + length(value / 2, state);
      break;
    }
  }
  std::string block;
  switch (entry->target) {
  case HistoryTarget::wcs:
    block = "G10 L2 P" + std::to_string(entry->number) + words;
    break;
  case HistoryTarget::g92: {
    // G52 rewrites omitted offsets with their current value. While G92.2 suspends them,
    // that value is zero, erasing the saved shift used by G92.3. Guard omitted XYZ and
    // all three rotary axes, which the history entry never writes.
    std::string unstored;
    for (std::size_t i = 0; i < 6; ++i)
      if (i >= 3 || !values.at(i))
        unstored += std::format(" AND [#{} EQ 0]", 5211 + i);
    block = "G52";
    for (std::size_t i = 0; i < 3; ++i) {
      const auto& offset = values.at(i);
      if (!offset)
        continue;
      auto value = length(*offset, state);
      if (!unstored.empty())
        value = std::format("[{}+0/[#5210+[1{}]]]", value, std::exchange(unstored, {}));
      block += std::string(" ") + "XYZ"[i] + value;
    }
    break;
  }
  case HistoryTarget::tool:
    block = "G10 L1 P" + std::to_string(entry->number) + words;
    break;
  }
  // A length written for the tool in the spindle: the offset in effect follows it (#287).
  std::optional<std::string> follow_up;
  const bool writes_length = std::ranges::contains(entry->changes, HistoryField::length,
                                                   &HistoryChange::field);
  if (entry->target == HistoryTarget::tool && writes_length &&
      entry->number == state.tool_in_spindle) {
    const auto effect = port_.tool_length_in_effect();
    if (!effect)
      return answer(
          {CommandOutcome::rejected, effect.error().text, {}, effect.error().localization});
    follow_up = after_tool_length_write(entry->number, state.tool_in_spindle, *effect);
  }
  const auto mark = history_->begin(HistorySource::restore, entry->id);
  auto restored = mdi(request, block);
  if (follow_up && restored.kind == StageKind::reply &&
      restored.reply.outcome == CommandOutcome::accepted) {
    auto applied = mdi(request, *follow_up);
    restored = applied.kind == StageKind::cancelled ? cancelled(touch_off_reason) : std::move(applied);
  }
  if (entry->target == HistoryTarget::wcs && restored.kind == StageKind::reply &&
      restored.reply.outcome == CommandOutcome::accepted)
    history_->written(mark, entry->number, values, clock().unix_time);
  history_->end(mark, clock().monotonic);
  return restored;
}

// An increment is motion nobody holds: from the moment it is sent the axis counts as possibly
// moving, until a jog stop written after it has been accepted. Nothing else ends that: not time,
// and not what the status shows, which cannot tell a finished increment from one the controller
// has not taken up yet.
auto Session::jog_step(Queued& request, const JogStep& command, const Decision& decision) -> Stage {
  const auto index = axis_index(command.axis);
  if (!decision.jog_space || !index)
    return refuse("Jog space is undecided.", RefusalMessage{"messages.session.jogSpaceUndecided"});
  {
    // An increment does not wait for a jog stop to finish: it is refused.
    const std::scoped_lock lock{mutex_};
    if (hold_ || low_periods_ < settled_periods || !reconciled())
      return answer({request.sent ? CommandOutcome::failed : CommandOutcome::rejected, hold_reason,
                     RefusalMessage{"messages.session.jogStopInProgress"}});
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
    return answer(
        {CommandOutcome::failed, "Unknown axis.", {}, MessageKey{"messages.policy.unknownAxis"}});
  if (!command.held)
    return answer(reply(tracked_stop(*index)));
  if (!wanted(request))
    return cancelled();
  if (!decision.jog_space) {
    forget_press(request);
    return answer({CommandOutcome::failed,
                   "Jog space is undecided.",
                   {},
                   MessageKey{"messages.session.jogSpaceUndecided"}});
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
               ? refuse(stopped_reason, RefusalMessage{"messages.session.jogStoppedBeforeStart"})
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
        return answer({CommandOutcome::failed,
                       std::string(watchdog_stop_text),
                       {},
                       MessageKey{"backend.jogStop.watchdog"}});
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
      const auto detail = reply(std::move(stopped)).reason;
      if (i == index)
        return answer(
            reply({failure,
                   std::format("The previous jog stop on this axis is not confirmed: {}", detail),
                   MessageKey{"messages.session.previousStopThisAxis", {{"detail", detail}}}}));
      const std::string axis(1, letter(axes.at(i)));
      return answer(reply(
          {failure, std::format("The previous jog stop on {} is not confirmed: {}", axis, detail),
           MessageKey{"messages.session.previousStopAxis", {{"axis", axis}, {"detail", detail}}}}));
    }
  }
  const std::scoped_lock lock{mutex_};
  auto& axis = jog_.at(index);
  if (!current(request))
    return cancelled();
  if (request.epoch != epoch_ || (held && (axis.latched || !leases_.active(axes.at(index))))) {
    if (request.epoch == epoch_)
      axis.pressed = false;
    const auto outcome = request.sent ? CommandOutcome::failed : CommandOutcome::rejected;
    if (held && axis.latched)
      return answer({outcome, release_reason, {}, MessageKey{"messages.session.releaseJog"}});
    return answer(
        {outcome, stopped_reason, {}, MessageKey{"messages.session.jogStoppedBeforeStart"}});
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
    return {CommandOutcome::rejected, directory.error().text, {}, directory.error().localization};
  auto message = encode_directory(*directory);
  if (!message)
    return {CommandOutcome::rejected,
            std::format("Directory cannot be sent: {}", message.error()),
            {},
            MessageKey{"messages.session.directoryNotSent", {{"detail", message.error()}}}};
  step.produced.push_back(std::move(*message)); // `truncated` in it tells of a cut listing.
  return {};
}

auto Session::mdi(const Queued& request, const std::string& text) const -> Stage {
  if (text.size() > max_mdi_bytes)
    return refuse("MDI requires one block of at most 254 bytes.",
                  RefusalMessage{"messages.session.mdiTooLong"});
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
      const auto axis = std::string(1, letter(axes.at(i)));
      const auto detail = reply(stopped).reason;
      problems_.push_back(
          {true, std::format("Jog stop failed on {}: {}", axis, detail),
           MessageKey{"messages.session.jogStopFailed", {{"axis", axis}, {"detail", detail}}}});
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
