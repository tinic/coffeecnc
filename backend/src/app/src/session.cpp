#include <coffeecnc/app/session.hpp>

#include <coffeecnc/app/messages.hpp>

#include <array>
#include <charconv>
#include <cstddef>
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

} // namespace

Session::Session(ControllerPort& port, const Limits& limits, const ProgramStore& programs)
    : port_(port), programs_(&programs), policy_(limits) {}

Session::Session(ControllerPort& port, const Limits& limits) : port_(port), policy_(limits) {}

auto Session::reply(ActionResult result) -> Reply {
  switch (result.status) {
  case ActionStatus::accepted:
    return {};
  case ActionStatus::timed_out:
    return {CommandOutcome::timed_out,
            result.reason.empty() ? "LinuxCNC did not answer." : std::move(result.reason)};
  case ActionStatus::failed:
    break;
  }
  return {CommandOutcome::failed,
          result.reason.empty() ? "LinuxCNC reported an error." : std::move(result.reason)};
}

auto Session::begin_page() -> PageToken {
  problems_.clear(); // The page they were for is gone.
  cancel_jogs();     // A stop that fails here is reported to the new page and retried by tick.
  ready_ = false;
  last_id_.reset();
  page_open_ = true;
  return PageToken{++pages_};
}

void Session::end_page(PageToken page) {
  if (!page_open_ || page != PageToken{pages_})
    return;
  problems_.clear();
  cancel_jogs();
  ready_ = false;
  last_id_.reset();
  page_open_ = false;
}

auto Session::handle(PageToken page, std::string_view json, Instant now) -> Outcome {
  Outcome outcome;
  outcome.page = page;
  if (!page_open_ || page != PageToken{pages_})
    return outcome;
  const auto decoded = decode_request(json);
  if (const auto* error = std::get_if<DecodeError>(&decoded)) {
    if (!error->id)
      outcome.messages.push_back(encode_message(
          MessageLevel::error, "Panel request ignored: " + error->reason, now.unix_time));
    else if (in_order(*error->id))
      outcome.messages.push_back(
          encode_result(*error->id, CommandOutcome::rejected, error->reason));
    return outcome;
  }
  const auto& request = std::get<Request>(decoded);
  if (!in_order(request.id))
    return outcome;
  const bool is_ready = std::holds_alternative<Ready>(request.command);
  Reply answer;
  if (!is_ready && !ready_)
    answer = {CommandOutcome::rejected, "Panel has not finished loading."};
  else
    answer = execute(request.command, now.monotonic);
  if (is_ready && answer.outcome == CommandOutcome::accepted) {
    ready_ = true;
    outcome.send_snapshots = true;
  }
  for (auto& message : produced_)
    outcome.messages.push_back(std::move(message));
  produced_.clear();
  outcome.messages.push_back(encode_result(request.id, answer.outcome, answer.reason));
  report(outcome, now);
  return outcome;
}

auto Session::tick(Instant now) -> Outcome {
  Outcome outcome;
  if (page_open_)
    outcome.page = PageToken{pages_};
  // A jog whose stop failed earlier has no lease any more but may still be moving: ask again.
  const auto retry = stop_pending_;
  stop(retry);
  bool held = false;
  for (const auto axis : axes)
    held = held || leases_.active(axis);
  if (held) {
    const auto state = port_.snapshot();
    if (state.link != Link::live || state.machine != MachineState::on) {
      cancel_jogs();
    } else {
      stop(leases_.expired(now.monotonic));
      // A change made outside the panel (mode, interpreter, homing, jog space) must not leave a
      // jog running until its lease expires: keep only what the policy would still start.
      for (std::size_t i = 0; i < axes.size(); ++i) {
        if (!leases_.active(axes[i]))
          continue;
        const auto decision = policy_.evaluate(
            Jog{axes[i], held_[i].direction, held_[i].velocity, true}, state);
        if (!decision.allowed || decision.jog_space != held_[i].space ||
            (decision.required_mode && *decision.required_mode != state.mode))
          stop_if_held(axes[i]);
      }
    }
  }
  report(outcome, now);
  return outcome;
}

// A request at or below the last ID seen is a duplicate or a replay: its original already
// produced the one result it gets.
auto Session::in_order(RequestId id) -> bool {
  if (last_id_ && id <= *last_id_)
    return false;
  last_id_ = id;
  return true;
}

auto Session::execute(const Command& command, JogLease::Time now) -> Reply {
  const auto* jog_command = std::get_if<Jog>(&command);
  auto state = port_.snapshot();
  auto decision = policy_.evaluate(command, state);
  if (decision.allowed && decision.required_mode && *decision.required_mode != state.mode) {
    // No jog survives a mode change; none is left running if the change fails either.
    cancel_jogs();
    auto switched = reply(port_.set_task_mode(*decision.required_mode));
    if (switched.outcome != CommandOutcome::accepted)
      return switched;
    // The decision was made for the old mode. Decide again for the state now observed.
    state = port_.snapshot();
    decision = policy_.evaluate(command, state);
    if (decision.allowed && decision.required_mode && *decision.required_mode != state.mode)
      return {CommandOutcome::failed, "LinuxCNC did not enter the required mode."};
    // A mode change was sent, so this is not `rejected` (nothing sent) any more.
    if (!decision.allowed) {
      if (jog_command != nullptr)
        stop_if_held(jog_command->axis);
      return {CommandOutcome::failed, std::move(decision.reason)};
    }
  }
  if (!decision.allowed) {
    // A refresh that is no longer valid must not leave its jog running until the lease expires.
    if (jog_command != nullptr)
      stop_if_held(jog_command->axis);
    return {CommandOutcome::rejected, std::move(decision.reason)};
  }
  return perform(command, decision, state, now);
}

auto Session::perform(const Command& command, const Decision& decision,
                      const ControllerState& state, JogLease::Time now) -> Reply {
  return std::visit(
      [&](const auto& c) -> Reply {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, Ready>) {
          return {};
        } else if constexpr (std::is_same_v<T, ListDirectory>) {
          return list_directory(c);
        } else if constexpr (std::is_same_v<T, SetMachine>) {
          // E-stop and power-off go out first; stops for held jogs follow.
          auto result = port_.set_machine_state(c.state);
          if (c.state != MachineState::on)
            cancel_jogs();
          return reply(std::move(result));
        } else if constexpr (std::is_same_v<T, Home>) {
          return reply(port_.home(c.axis));
        } else if constexpr (std::is_same_v<T, Jog>) {
          return reply(jog(c, decision, now));
        } else if constexpr (std::is_same_v<T, JogStep>) {
          if (!decision.jog_space)
            return Reply{CommandOutcome::rejected, "Jog space is undecided."};
          return reply(port_.jog_increment(c.axis, c.distance, c.velocity, *decision.jog_space));
        } else if constexpr (std::is_same_v<T, JogStopAll>) {
          return reply(stop_all());
        } else if constexpr (std::is_same_v<T, Mdi>) {
          return mdi(c.text);
        } else if constexpr (std::is_same_v<T, OpenProgram>) {
          // LinuxCNC gets the resolved path: what was checked is what it opens.
          if (programs_ == nullptr)
            return reply(port_.open_program(c.path));
          const auto file = programs_->resolve_program(c.path);
          if (!file)
            return Reply{CommandOutcome::rejected, file.error()};
          return reply(port_.open_program(*file));
        } else if constexpr (std::is_same_v<T, Run>) {
          return reply(port_.run());
        } else if constexpr (std::is_same_v<T, Pause>) {
          return reply(port_.pause());
        } else if constexpr (std::is_same_v<T, Resume>) {
          return reply(port_.resume());
        } else if constexpr (std::is_same_v<T, Step>) {
          return reply(port_.step());
        } else if constexpr (std::is_same_v<T, Abort>) {
          auto result = port_.abort();
          cancel_jogs();
          return reply(std::move(result));
        } else if constexpr (std::is_same_v<T, SetOverride>) {
          return reply(port_.set_override(c.which, c.value));
        } else if constexpr (std::is_same_v<T, SetSpindle>) {
          return reply(port_.set_spindle(c.direction, c.rpm));
        } else if constexpr (std::is_same_v<T, SetCoolant>) {
          return reply(port_.set_coolant(c.flood, c.mist));
        } else if constexpr (std::is_same_v<T, SetProgramOption>) {
          return reply(port_.set_program_option(c.option, c.on));
        } else if constexpr (std::is_same_v<T, SelectWcs>) {
          if (c.wcs < 1 || static_cast<std::size_t>(c.wcs) > wcs_words.size())
            return Reply{CommandOutcome::rejected,
                         "Work coordinate system must be G54 through G59.3."};
          return mdi(std::string(wcs_words[static_cast<std::size_t>(c.wcs) - 1]));
        } else if constexpr (std::is_same_v<T, TouchOff>) {
          // P0 is the active coordinate system.
          return mdi(std::string("G10 L20 P0 ") + letter(c.axis) + length(c.value, state));
        } else if constexpr (std::is_same_v<T, ToolTouchOff>) {
          auto measured =
              mdi("G10 L10 P" + std::to_string(state.tool_in_spindle) + " Z" + length(c.value, state));
          if (measured.outcome != CommandOutcome::accepted)
            return measured;
          return mdi("G43");
        } else if constexpr (std::is_same_v<T, SetTool>) {
          return reply(port_.set_tool(c.tool));
        } else if constexpr (std::is_same_v<T, RemoveTool>) {
          return reply(port_.remove_tool(c.number));
        } else {
          static_assert(std::is_same_v<T, ToolChanged>, "Every command needs an action.");
          return reply(port_.tool_changed());
        }
      },
      command);
}

auto Session::jog(const Jog& request, const Decision& decision, JogLease::Time now)
    -> ActionResult {
  const auto index = axis_index(request.axis);
  if (!index)
    return {ActionStatus::failed, "Unknown axis."};
  if (!request.held) {
    // Always stop, with or without a lease: the release may be all that is left of a jog.
    leases_.release(request.axis);
    return tracked_stop(*index);
  }
  // An earlier stop on this axis is not confirmed: nothing new starts until one is, or the
  // retry in tick would stop the new jog instead of the old one.
  if (stop_pending_[*index]) {
    if (auto stopped = tracked_stop(*index); stopped.status != ActionStatus::accepted) {
      leases_.release(request.axis);
      return {stopped.status, "The previous jog stop on this axis is not confirmed: " +
                                  reply(std::move(stopped)).reason};
    }
  }
  if (!decision.jog_space)
    return {ActionStatus::failed, "Jog space is undecided."};
  const HeldJog wanted{request.direction, request.velocity, *decision.jog_space};
  if (leases_.active(request.axis) && held_[*index] == wanted) {
    leases_.refresh(request.axis, now);
    return {};
  }
  auto started = port_.start_jog(request.axis, request.direction, request.velocity, wanted.space);
  if (started.status != ActionStatus::accepted) {
    // The axis may be moving: an earlier jog, or a start that was lost on the way back.
    leases_.release(request.axis);
    JogLease::Stops stops{};
    stops[*index] = true;
    stop(stops);
    return started;
  }
  held_[*index] = wanted;
  leases_.refresh(request.axis, now);
  return {};
}

auto Session::stop_all() -> ActionResult {
  static_cast<void>(leases_.cancel_all());
  ActionResult result;
  for (std::size_t i = 0; i < axes.size(); ++i) {
    auto stopped = tracked_stop(i);
    if (result.status == ActionStatus::accepted && stopped.status != ActionStatus::accepted)
      result = std::move(stopped);
  }
  return result;
}

// Not a controller action: nothing is sent to LinuxCNC.
auto Session::list_directory(const ListDirectory& request) -> Reply {
  if (programs_ == nullptr)
    return {CommandOutcome::rejected, "No program directory is configured."};
  const auto directory = programs_->list(request.path);
  if (!directory)
    return {CommandOutcome::rejected, directory.error()};
  auto message = encode_directory(*directory);
  if (!message)
    return {CommandOutcome::rejected, "Directory cannot be sent: " + message.error()};
  produced_.push_back(std::move(*message)); // `truncated` in it tells the panel of a cut listing.
  return {};
}

auto Session::mdi(const std::string& text) -> Reply {
  if (text.size() > max_mdi_bytes)
    return {CommandOutcome::rejected, "MDI requires one block of at most 254 bytes."};
  return reply(port_.mdi(text));
}

// Every stop goes through here, so a failed one is always left for tick to retry.
auto Session::tracked_stop(std::size_t index) -> ActionResult {
  auto stopped = port_.stop_jog(axes.at(index));
  stop_pending_.at(index) = stopped.status != ActionStatus::accepted;
  return stopped;
}

void Session::stop(const JogLease::Stops& stops) {
  for (std::size_t i = 0; i < stops.size(); ++i) {
    if (!stops[i])
      continue;
    const bool known = stop_pending_[i];
    const auto stopped = tracked_stop(i);
    // Reported once; tick keeps retrying without repeating the message.
    if (stopped.status != ActionStatus::accepted && !known)
      problems_.push_back(std::string("Jog stop failed on ") + letter(axes[i]) + ": " +
                          reply(stopped).reason);
  }
}

void Session::stop_if_held(Axis axis) {
  const auto index = axis_index(axis);
  if (!index || !leases_.active(axis))
    return;
  leases_.release(axis);
  JogLease::Stops stops{};
  stops[*index] = true;
  stop(stops);
}

void Session::cancel_jogs() { stop(leases_.cancel_all()); }

void Session::report(Outcome& outcome, Instant now) {
  for (const auto& problem : problems_)
    outcome.messages.push_back(encode_message(MessageLevel::error, problem, now.unix_time));
  problems_.clear();
  for (const auto& notice : notices_)
    outcome.messages.push_back(encode_message(MessageLevel::info, notice, now.unix_time));
  notices_.clear();
}

} // namespace coffeecnc
