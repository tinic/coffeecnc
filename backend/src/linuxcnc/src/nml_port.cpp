#include <coffeecnc/linuxcnc/nml_port.hpp>
#include <coffeecnc/core/text_file.hpp>

#include <coffeecnc/core/jog_stop_report.hpp>
#include <coffeecnc/core/task_heartbeat.hpp>
#include <coffeecnc/linuxcnc/tool_table.hpp>

#include "nml_gate.hpp"
#include "nml_status.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <numbers>
#include <numeric>
#include <span>
#include <system_error>
#include <thread>
#include <utility>

#include <cmd_msg.hh>
#include <cms.hh>
#include <emc.hh>
#include <emc_nml.hh>
#include <nml.hh>
#include <nml_oi.hh>
#include <nml_type.hh>
#include <rcs.hh>
#include <stat_msg.hh>

namespace coffeecnc {
namespace {

using Clock = std::chrono::steady_clock;
// Task writes its status once per cycle, 1 ms on a usual configuration.
constexpr auto poll_interval = std::chrono::milliseconds{1};
// An operator error is published a few task cycles after the status that reports the failure.
constexpr auto error_text_time = std::chrono::milliseconds{50};
constexpr auto error_companion_time = std::chrono::milliseconds{20};
// How long a jog-stop text that is not CoffeeCNC's own waits for the control thread to read the
// pins once more: three of its 10 ms periods.
constexpr auto fresh_pins_time = std::chrono::milliseconds{30};
constexpr double seconds_per_minute = 60;

auto failed(std::string_view reason) -> ActionResult {
  return {ActionStatus::failed, std::string(reason)};
}
auto accepted(const ActionResult& result) -> bool {
  return result.status == ActionStatus::accepted;
}
// A command in words for the operator, for the reason of an action that was only half sent.
auto words(long type) -> std::string_view {
  switch (type) {
  case EMC_TRAJ_SET_TELEOP_ENABLE_TYPE:
    return "the change between world and joint mode";
  case EMC_JOINT_HOME_TYPE:
    return "the homing command";
  case EMC_JOG_CONT_TYPE:
    return "the jog start";
  case EMC_JOG_INCR_TYPE:
    return "the jog increment";
  case EMC_COOLANT_FLOOD_ON_TYPE:
    return "flood on";
  case EMC_COOLANT_FLOOD_OFF_TYPE:
    return "flood off";
  case EMC_COOLANT_MIST_ON_TYPE:
    return "mist on";
  case EMC_COOLANT_MIST_OFF_TYPE:
    return "mist off";
  case EMC_TOOL_LOAD_TOOL_TABLE_TYPE:
    return "the tool table reload";
  case EMC_TASK_PLAN_CLOSE_TYPE:
    return "closing the loaded program";
  case EMC_TASK_PLAN_OPEN_TYPE:
    return "opening the program";
  default:
    break;
  }
  return "another command";
}

auto withdrawn() -> ActionResult {
  return {ActionStatus::cancelled, "Cancelled before it was sent."};
}

// Polls until `done` holds. False when it did not within the timeout.
template <typename Predicate>
auto until(std::chrono::milliseconds timeout, Predicate done) -> bool {
  const auto deadline = Clock::now() + timeout;
  for (;;) {
    if (done())
      return true;
    if (Clock::now() >= deadline)
      return false;
    std::this_thread::sleep_for(poll_interval);
  }
}

// False when the text does not fit the field with its terminator.
auto put(std::span<char> field, std::string_view text) -> bool {
  if (text.size() >= field.size())
    return false;
  std::ranges::fill(field, '\0');
  std::ranges::copy(text, field.begin());
  return true;
}

auto index_of(Axis axis) -> int { return static_cast<int>(axis_index(axis).value_or(0)); }

} // namespace

struct NmlPort::Impl {
  struct WorkFrame {
    int wcs;
    std::array<double, 3> g5x;
    std::array<double, 3> g92;
    double rotation;
    friend auto operator==(const WorkFrame&, const WorkFrame&) -> bool = default;
  };
  struct Calibration {
    std::uint64_t datum;
    std::array<double, 3> plane;
    double clearance_z;
  };

  std::optional<Calibration> setter_calibration;

  MachineConfig config;
  HalShared* hal{};
  NmlLoads* loads{};
  Gated<RCS_CMD_CHANNEL> command;
  StatusChannel status;
  Gated<NML> errors;
  int serial{};
  // Of the action in progress: asked before every write. Null for an action that only stops
  // something. Set by begin() and used only until that action returns.
  const Wanted* wanted{};
  // It has answered no: nothing more is written for this action.
  bool cancelled{};
  // The types of the commands this action has written so far.
  std::vector<long> written_types;
  // A command has been written; `serial` is its number.
  bool written{};
  // Every write, for other threads to read.
  std::mutex trace_mutex;
  std::vector<Written> trace;
  // The joint or axis of the jog stop that is the last command written, until task echoes it.
  std::optional<int> unanswered_stop;
  std::vector<ControllerMessage> messages;
  // A jog-stop text read in the present collect(), not yet attributed.
  struct JogStopText {
    std::string text;
    Clock::time_point read_at;
  };
  std::vector<JogStopText> jog_stop_texts;
  // The watchdog trip the operator was last told about.
  std::optional<Clock::time_point> told_watchdog;

  explicit Impl(MachineConfig machine)
      : config(std::move(machine)),
        command(emcFormat, "emcCommand", "xemc", config.nml_file.c_str()), status(config.nml_file),
        errors(emcFormat, "emcError", "xemc", config.nml_file.c_str()) {}

  [[nodiscard]] auto stat() -> const EMC_STAT* { return status.stat(); }

  void add(bool error, std::span<const char> field) {
    if (auto text = text_of(field); !text.empty())
      messages.push_back({error, std::move(text)});
  }

  void add_error(std::span<const char> field) {
    auto text = text_of(field);
    if (text.empty())
      return;
    if (is_jog_stop_text(text))
      jog_stop_texts.push_back({std::move(text), Clock::now()});
    else
      messages.push_back({true, std::move(text)});
  }

  // LinuxCNC's "Jog aborted by jog-stop" texts of one collect(): CoffeeCNC's own stops go to the
  // log, a watchdog stop is told in CoffeeCNC's words, anything else is passed on unchanged.
  // None of them is ever an action's reason unless it is passed on.
  void attribute_jog_stops() {
    if (jog_stop_texts.empty())
      return;
    const auto last_read = jog_stop_texts.back().read_at;
    // CoffeeCNC's level is noted before every write of it, so a level that caused one of these
    // texts was noted before the text was read: this copy has it.
    const auto own = hal != nullptr ? hal->jog_stop_history().own_level_high : std::nullopt;
    std::optional<Clock::time_point> tripped;
    if (hal != nullptr) {
      // The watchdog is seen only by the control thread. A watchdog trips only after that thread
      // has not run for its timeout (300 ms), and it sees the trip before it re-arms it: when it
      // read the pins within jog_stop_window before the text, any trip behind the text is in
      // the history already. Otherwise it may have stalled itself: let it read the pins once
      // more, for a short time only.
      const auto read = hal->jog_stop_history().read_at;
      if (!read || last_read - *read >= jog_stop_window)
        static_cast<void>(until(fresh_pins_time, [&]() -> bool {
          const auto again = hal->jog_stop_history().read_at;
          return again && *again >= last_read;
        }));
      tripped = hal->jog_stop_history().watchdog_tripped;
    }
    // One log line per cause and text in this pass, with the count.
    struct Logged {
      JogStopCause cause;
      std::string text;
      int count;
    };
    std::vector<Logged> logged;
    for (auto& seen : jog_stop_texts) {
      const auto cause = classify_jog_stop(seen.text, seen.read_at, own, tripped);
      if (cause == JogStopCause::unattributed) {
        messages.push_back({true, std::move(seen.text)});
        continue;
      }
      if (cause == JogStopCause::watchdog &&
          (!told_watchdog || *tripped - *told_watchdog > jog_stop_window)) {
        told_watchdog = tripped;
        messages.push_back({true, std::string(watchdog_stop_text)});
      }
      if (!logged.empty() && logged.back().cause == cause && logged.back().text == seen.text)
        ++logged.back().count;
      else
        logged.push_back({cause, std::move(seen.text), 1});
    }
    jog_stop_texts.clear();
    for (const auto& line : logged) {
      const auto since = [](std::optional<Clock::time_point> at,
                            Clock::time_point read) -> std::string {
        return at ? std::format(
                        "{}",
                        std::chrono::duration_cast<std::chrono::milliseconds>(read - *at).count())
                  : std::string("none");
      };
      std::cerr << std::format("coffeecnc: jog_stop_text cause={} count={} "
                               "own_level_ms_before={} watchdog_tripped_ms_before={} text=\"{}\"\n",
                               line.cause == JogStopCause::own ? "own" : "watchdog", line.count,
                               since(own, last_read), since(tripped, last_read), line.text)
                << std::flush;
    }
  }

  // Moves everything on the error channel into `messages`. Reading consumes.
  void collect() {
    const auto read = [&]() -> NMLTYPE {
      return errors.call([](NML& channel) -> NMLTYPE { return channel.read(); });
    };
    for (NMLTYPE type = read(); type > 0; type = read()) {
      // NML hands out the base class; each downcast follows the type tag just read. The message
      // is in this channel's own buffer until the next read.
      NMLmsg* message = errors.call([](NML& channel) -> NMLmsg* { return channel.get_address(); });
      if (type == EMC_OPERATOR_ERROR_TYPE)
        add_error(static_cast<EMC_OPERATOR_ERROR*>(message)->error);
      else if (type == EMC_OPERATOR_TEXT_TYPE)
        add(false, static_cast<EMC_OPERATOR_TEXT*>(message)->text);
      else if (type == EMC_OPERATOR_DISPLAY_TYPE)
        add(false, static_cast<EMC_OPERATOR_DISPLAY*>(message)->display);
      else if (type == NML_ERROR_TYPE)
        add_error(static_cast<NML_ERROR*>(message)->error);
      else if (type == NML_TEXT_TYPE)
        add(false, static_cast<NML_TEXT*>(message)->text);
      else if (type == NML_DISPLAY_TYPE)
        add(false, static_cast<NML_DISPLAY*>(message)->display);
    }
    attribute_jog_stops();
  }

  // Where the messages of the action that starts now will begin.
  [[nodiscard]] auto mark() -> std::size_t {
    collect();
    return messages.size();
  }

  [[nodiscard]] auto error_since(std::size_t from) const -> bool {
    return std::ranges::any_of(messages.begin() + static_cast<std::ptrdiff_t>(from), messages.end(),
                               [](const auto& message) -> bool { return message.error; });
  }

  // The errors published since `from`, as one reason; they leave the message queue.
  [[nodiscard]] auto take_errors(std::size_t from) -> std::string {
    std::string reason;
    for (std::size_t i = from; i < messages.size();) {
      if (!messages[i].error) {
        ++i;
        continue;
      }
      if (!reason.empty())
        reason += "; ";
      reason += messages[i].text;
      messages.erase(messages.begin() + static_cast<std::ptrdiff_t>(i));
    }
    return reason;
  }

  // A failure whose reason is what task published about it, or `fallback`.
  [[nodiscard]] auto failure(std::size_t from, std::string_view fallback) -> ActionResult {
    static_cast<void>(until(error_text_time, [&]() -> bool {
      collect();
      return error_since(from);
    }));
    // One failure can publish several errors (the axis, the joint, the rejected command).
    static_cast<void>(until(error_companion_time, [&]() -> bool {
      collect();
      return false;
    }));
    auto reason = take_errors(from);
    return failed(reason.empty() ? std::string(fallback) : std::move(reason));
  }

  // Ends an action: its `Wanted` is the caller's and is not kept past the call.
  class Scope {
  public:
    explicit Scope(Impl& impl) : impl_(impl) {}
    Scope(const Scope&) = delete;
    auto operator=(const Scope&) -> Scope& = delete;
    ~Scope() { impl_.wanted = nullptr; }

  private:
    Impl& impl_;
  };

  // Starts an action: whose it is, and where its messages will begin.
  [[nodiscard]] auto begin(const Wanted* still_wanted) -> std::size_t {
    wanted = still_wanted;
    cancelled = false;
    written_types.clear();
    return mark();
  }

  // The action in progress may go on. Once the answer was no it stays no.
  [[nodiscard]] auto still() -> bool {
    if (!cancelled && wanted != nullptr && *wanted && !(*wanted)())
      cancelled = true;
    return !cancelled;
  }

  // What an action that stops here has to say for itself. `cancelled` means that nothing of
  // it was written. Once one of its commands has gone out it is `failed`, and the reason names
  // what was sent and that the rest was not.
  [[nodiscard]] auto withdraw(long unsent = 0) const -> ActionResult {
    if (written_types.empty())
      return withdrawn();
    std::string done;
    for (const long type : written_types)
      done += (done.empty() ? "" : ", ") + std::string(words(type));
    return failed(unsent == 0 ? std::format("Cancelled after this was sent: {}. The rest of the "
                                            "action was not sent.",
                                            done)
                              : std::format("Cancelled after this was sent: {}. Not sent: {}.",
                                            done, words(unsent)));
  }

  // The one place a command leaves this port. The gate is taken first, then the action is
  // asked a last time whether it is still wanted, then the command is written. No other NML
  // call falls between that question and the write; the answer itself can still change in that
  // moment, since the session is not kept locked while this thread writes. Once write() of
  // libnml has begun the command is committed.
  [[nodiscard]] auto write(RCS_CMD_MSG& message) -> ActionResult {
    const int wrote = command.call(
        [&](RCS_CMD_CHANNEL& channel) -> int { return still() ? channel.write(&message) : 0; });
    if (cancelled)
      return withdraw(message.type);
    if (wrote != 0)
      return failed("Cannot write to LinuxCNC's command channel.");
    written_types.push_back(message.type);
    serial = message.serial_number;
    written = true;
    unanswered_stop.reset();
    {
      const std::scoped_lock lock{trace_mutex};
      if (trace.size() == NmlPort::max_trace)
        trace.erase(trace.begin());
      trace.push_back({message.type, message.serial_number, Clock::now()});
    }
    return {};
  }

  // Task has echoed the last command written, or none was.
  [[nodiscard]] auto answered(const EMC_STAT* s) const -> bool {
    return !written || (s != nullptr && s->echo_serial_number - serial >= 0);
  }

  // Waits until task has echoed the serial number of the command written last.
  [[nodiscard]] auto echoed() -> ActionResult {
    // The error channel is read in every poll of every wait: jog_stop_window relies on it.
    const bool echo = until(echo_timeout, [&]() -> bool {
      collect();
      return answered(stat());
    });
    if (!echo)
      return {ActionStatus::timed_out, "LinuxCNC did not acknowledge the command."};
    unanswered_stop.reset();
    return {};
  }

  // Writes the message and waits until task has echoed its serial number.
  [[nodiscard]] auto send(RCS_CMD_MSG& message) -> ActionResult {
    if (auto wrote = write(message); !accepted(wrote))
      return wrote;
    return echoed();
  }

  // RCS status of the command sent last once it is no longer executing; RCS_EXEC after `timeout`.
  // A later serial number means another sender's command replaced it: it is done.
  [[nodiscard]] auto outcome(std::chrono::milliseconds timeout) -> int {
    int rcs = RCS_EXEC;
    static_cast<void>(until(timeout, [&]() -> bool {
      collect();
      const EMC_STAT* s = stat();
      if (s == nullptr)
        return false;
      rcs = s->echo_serial_number - serial > 0 ? int{RCS_DONE} : s->status;
      return rcs != RCS_EXEC;
    }));
    return rcs;
  }

  // Waits until task has echoed and finished the command written last.
  [[nodiscard]] auto finished(std::size_t from) -> ActionResult {
    if (auto echo = echoed(); !accepted(echo))
      return echo;
    const int rcs = outcome(completion_timeout);
    if (rcs == RCS_EXEC)
      return {ActionStatus::timed_out, "LinuxCNC did not finish the command."};
    if (rcs == RCS_ERROR)
      return failure(from, "LinuxCNC reported an error.");
    return {};
  }

  // Sends and waits until task has finished the command.
  [[nodiscard]] auto run(RCS_CMD_MSG& message, std::size_t from) -> ActionResult {
    if (auto wrote = write(message); !accepted(wrote))
      return wrote;
    return finished(from);
  }

  // Sends a command task only passes on (jog, home, step): its refusal arrives as an operator
  // error, not in the command's status.
  [[nodiscard]] auto pass(RCS_CMD_MSG& message, std::size_t from) -> ActionResult {
    if (auto sent = send(message); !accepted(sent))
      return sent;
    bool refused = false;
    static_cast<void>(until(error_window, [&]() -> bool {
      collect();
      const EMC_STAT* s = stat();
      refused = error_since(from) ||
                (s != nullptr && s->echo_serial_number == serial && s->status == RCS_ERROR);
      return refused;
    }));
    if (refused)
      return failure(from, "LinuxCNC reported an error.");
    return {};
  }

  // Sends a command that starts execution (MDI block, program): an error in its first moments
  // fails it; still executing after the window, it is accepted.
  [[nodiscard]] auto start(RCS_CMD_MSG& message, std::size_t from) -> ActionResult {
    if (auto sent = send(message); !accepted(sent))
      return sent;
    if (outcome(start_window) == RCS_ERROR)
      return failure(from, "LinuxCNC reported an error.");
    return {};
  }

  // Waits for a state task reports after the command is done.
  template <typename Predicate>
  [[nodiscard]] auto settled(std::size_t from, std::string_view otherwise, Predicate reached)
      -> ActionResult {
    const bool ok = until(settle_timeout, [&]() -> bool {
      collect();
      const EMC_STAT* s = stat();
      return (s != nullptr && reached(*s)) || error_since(from);
    });
    const EMC_STAT* s = stat();
    if (ok && s != nullptr && reached(*s))
      return {};
    return failure(from, otherwise);
  }

  // World (teleop) mode for axis jogs, joint mode for joint jogs and homing.
  [[nodiscard]] auto teleop(bool on, std::size_t from) -> ActionResult {
    const auto in_teleop = [](const EMC_STAT& s) -> bool {
      return s.motion.traj.mode == EMC_TRAJ_MODE_TELEOP;
    };
    if (const EMC_STAT* s = stat(); s != nullptr && in_teleop(*s) == on)
      return {};
    EMC_TRAJ_SET_TELEOP_ENABLE message;
    message.enable = on ? 1 : 0;
    if (auto done = run(message, from); !accepted(done))
      return done;
    auto switched = settled(from,
                            on ? "LinuxCNC did not enter world mode for an axis jog."
                               : "LinuxCNC did not enter joint mode.",
                            [&](const EMC_STAT& s) -> bool { return in_teleop(s) == on; });
    // The wait is over: the command this was the preparation for may not be wanted any more.
    if (accepted(switched) && !still())
      return withdraw();
    return switched;
  }

  [[nodiscard]] auto units_per_second(double millimetres_per_minute) const -> double {
    return millimetres_per_minute / config.millimetres_per_unit / seconds_per_minute;
  }

  [[nodiscard]] auto read_tool_table() const -> std::expected<std::string, std::string> {
    if (config.tool_table.empty())
      return std::unexpected(std::string("No tool table is configured ([EMCIO] TOOL_TABLE)."));
    auto text = read_text_file(config.tool_table, max_tool_table_bytes);
    if (!text)
      return std::unexpected(text.error() == TextFileError::too_large
                                 ? std::format("Tool table {} is larger than 4 MiB.",
                                               config.tool_table)
                                 : std::format("Cannot read tool table {}.", config.tool_table));
    return std::move(*text);
  }

  // Replaces the file in one step, then has LinuxCNC read it: two commits. Until the first the
  // edit can be withdrawn and leaves nothing behind; between them only half of it.
  [[nodiscard]] auto load_tool_table(const std::string& table) -> ActionResult {
    const std::string temporary = config.tool_table + ".coffeecnc";
    {
      std::ofstream out{temporary, std::ios::binary | std::ios::trunc};
      out << table;
      out.flush();
      if (!out)
        return failed(std::format("Cannot write tool table {}.", temporary));
    }
    std::error_code error;
    if (!still()) {
      std::filesystem::remove(temporary, error);
      return withdrawn();
    }
    std::filesystem::rename(temporary, config.tool_table, error);
    if (error)
      return failed(std::format("Cannot replace tool table {}.", config.tool_table));
    const auto from = mark();
    EMC_TOOL_LOAD_TOOL_TABLE message;
    std::ranges::fill(message.file, '\0'); // the configured table
    auto loaded = run(message, from);
    // Withdrawn between the two commits. Nothing is rolled back: the file may have been edited
    // again by now.
    if (loaded.status == ActionStatus::cancelled)
      return failed(std::format("The tool table file {} was changed, but the request was "
                                "cancelled before LinuxCNC was told to load it.",
                                config.tool_table));
    if (accepted(loaded) && loads != nullptr)
      ++loads->tool_tables;
    return loaded;
  }
};

auto NmlPort::connect(const MachineConfig& config, HalShared* hal, NmlLoads* loads)
    -> std::expected<std::unique_ptr<NmlPort>, std::string> {
  auto impl = std::make_unique<Impl>(config);
  impl->hal = hal;
  impl->loads = loads;
  if (impl->command.call([](RCS_CMD_CHANNEL& channel) -> int { return channel.valid(); }) == 0 ||
      !impl->status.valid() ||
      impl->errors.call([](NML& channel) -> int { return channel.valid(); }) == 0)
    return std::unexpected(
        std::format("Cannot open LinuxCNC's NML channels from {}.", impl->config.nml_file));
  // The first reading of the write counter: the next can tell whether task is running.
  static_cast<void>(until(echo_timeout, [&]() -> bool { return impl->stat() != nullptr; }));
  return std::unique_ptr<NmlPort>(new NmlPort(std::move(impl)));
}

NmlPort::NmlPort(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
NmlPort::~NmlPort() = default;

auto NmlPort::snapshot() -> ControllerState { return status().controller; }

auto NmlPort::writes() const -> std::vector<Written> {
  const std::scoped_lock lock{impl_->trace_mutex};
  return impl_->trace;
}
auto NmlPort::types() -> Types {
  return {EMC_JOG_CONT_TYPE,
          EMC_JOG_STOP_TYPE,
          EMC_JOG_INCR_TYPE,
          EMC_TRAJ_SET_TELEOP_ENABLE_TYPE,
          EMC_JOINT_HOME_TYPE,
          EMC_TASK_PLAN_RUN_TYPE,
          EMC_TASK_PLAN_EXECUTE_TYPE,
          EMC_SPINDLE_ON_TYPE,
          EMC_COOLANT_FLOOD_ON_TYPE,
          EMC_COOLANT_MIST_ON_TYPE,
          EMC_TOOL_LOAD_TOOL_TABLE_TYPE,
          EMC_TASK_ABORT_TYPE};
}

NmlGateHold::NmlGateHold() : lock_(nml_gate()) {}
NmlGateHold::~NmlGateHold() = default;

auto NmlPort::status() -> NmlStatus {
  const EMC_STAT* s = impl_->stat();
  if (s == nullptr)
    return {}; // link down
  return to_status(*s, impl_->config, impl_->hal, impl_->status.link());
}

auto NmlPort::take_messages() -> std::vector<ControllerMessage> {
  // Called every control cycle: also the regular reading the heartbeat needs.
  static_cast<void>(impl_->stat());
  impl_->collect();
  return std::exchange(impl_->messages, {});
}

auto NmlPort::set_machine_state(MachineState state, const Wanted& wanted) -> ActionResult {
  // E-stop and power off are never withdrawn.
  const auto from = impl_->begin(state == MachineState::on ? &wanted : nullptr);
  const Impl::Scope scope{*impl_};
  if (!impl_->still())
    return withdrawn();
  const EMC_STAT* s = impl_->stat();
  EMC_TASK_SET_STATE message;
  switch (state) {
  case MachineState::estop:
    message.state = EMC_TASK_STATE_ESTOP;
    break;
  case MachineState::off:
    // Protocol `off` is E-stop cleared with machine power off: from E-stop that is a reset, from
    // on a power-off. Task reports both as ESTOP_RESET.
    message.state = s != nullptr && s->task.state == EMC_TASK_STATE_ON ? EMC_TASK_STATE_OFF
                                                                       : EMC_TASK_STATE_ESTOP_RESET;
    break;
  case MachineState::on:
    message.state = EMC_TASK_STATE_ON;
    break;
  }
  if (state == MachineState::estop) {
    // An E-stop request is answered by the state LinuxCNC reports after it, whatever became of
    // the command or of a step of it: task can switch the machine off and abort (which may
    // publish errors of its own) without ever reporting E-stop, where the state is an input's.
    const auto sent = impl_->run(message, from);
    // Evidence is a status task published after it took this command, read while the link is
    // live. stat() also hands out what a stopped task left in the buffer.
    const auto evidence = [&](const EMC_STAT* now) -> bool {
      return now != nullptr && impl_->status.link() == Link::live &&
             !impl_->written_types.empty() && now->echo_serial_number - impl_->serial >= 0;
    };
    static_cast<void>(until(settle_timeout, [&]() -> bool {
      impl_->collect();
      const EMC_STAT* now = impl_->stat();
      return evidence(now) && now->task.state == EMC_TASK_STATE_ESTOP;
    }));
    // What a failed step took out of the message queue for its reason is the operator's again.
    if (!accepted(sent) && !sent.reason.empty())
      impl_->messages.push_back({true, sent.reason});
    const EMC_STAT* now = impl_->stat();
    const Link link = impl_->status.link();
    if (now == nullptr)
      return estop_request_result(std::nullopt, link, false);
    return estop_request_result(now->task.state == EMC_TASK_STATE_ESTOP ? MachineState::estop
                                : now->task.state == EMC_TASK_STATE_ON  ? MachineState::on
                                                                        : MachineState::off,
                                link, evidence(now));
  }
  if (auto done = impl_->run(message, from); !accepted(done))
    return done;
  // Task reports the command done even when it left the state as it was (on from E-stop).
  return impl_->settled(
      from, "LinuxCNC did not change the machine state.", [&](const EMC_STAT& now) -> bool {
        switch (state) {
        case MachineState::estop:
          return now.task.state == EMC_TASK_STATE_ESTOP;
        case MachineState::on:
          return now.task.state == EMC_TASK_STATE_ON;
        case MachineState::off:
          break;
        }
        return now.task.state == EMC_TASK_STATE_ESTOP_RESET || now.task.state == EMC_TASK_STATE_OFF;
      });
}

auto NmlPort::power_off() -> ActionResult {
  const auto from = impl_->begin(nullptr);
  const Impl::Scope scope{*impl_};
  // Unlike protocol set_machine/off (also the explicit Reset E-stop action), this
  // path always sends OFF. An E-stop arriving at any point must remain asserted.
  EMC_TASK_SET_STATE message;
  message.state = EMC_TASK_STATE_OFF;
  if (auto done = impl_->run(message, from); !accepted(done))
    return done;
  return impl_->settled(from, "LinuxCNC did not confirm machine power off.",
                        [&](const EMC_STAT& now) -> bool {
    return impl_->status.link() == Link::live &&
           (now.task.state == EMC_TASK_STATE_ESTOP ||
            now.task.state == EMC_TASK_STATE_ESTOP_RESET || now.task.state == EMC_TASK_STATE_OFF);
  });
}

auto NmlPort::set_task_mode(TaskMode mode, const Wanted& wanted) -> ActionResult {
  const auto from = impl_->begin(&wanted);
  const Impl::Scope scope{*impl_};
  if (!impl_->still())
    return withdrawn();
  EMC_TASK_SET_MODE message;
  message.mode = mode == TaskMode::automatic ? EMC_TASK_MODE_AUTO
                 : mode == TaskMode::mdi     ? EMC_TASK_MODE_MDI
                                             : EMC_TASK_MODE_MANUAL;
  if (auto sent = impl_->send(message); !accepted(sent))
    return sent;
  // Task ignores a mode change during a jog and refuses one while a program runs, in both cases
  // with an operator error only: the mode itself is the evidence.
  return impl_->settled(
      from, "LinuxCNC did not change the task mode.", [&](const EMC_STAT& now) -> bool {
        return now.task.mode == message.mode &&
               (now.echo_serial_number != impl_->serial || now.status != RCS_EXEC);
      });
}

auto NmlPort::home(std::optional<Axis> axis, const Wanted& wanted) -> ActionResult {
  // Motion homes only joints with a HOME_SEQUENCE on "home all" and says nothing about the rest:
  // without one the command would be accepted and do nothing.
  if (!axis && !impl_->config.home_all_configured)
    return failed("Homing all axes at once is not configured ([JOINT_n] HOME_SEQUENCE): home "
                  "each axis.");
  const auto from = impl_->begin(&wanted);
  const Impl::Scope scope{*impl_};
  if (!impl_->still())
    return withdrawn();
  // Task homes joints, and only in joint mode; a homed machine is in world mode.
  if (auto joint_mode = impl_->teleop(false, from); !accepted(joint_mode))
    return joint_mode;
  EMC_JOINT_HOME message;
  message.joint = axis ? index_of(*axis) : -1; // -1 homes all joints in their configured sequence
  return impl_->pass(message, from);
}

auto NmlPort::start_jog(Axis axis, int direction, double velocity, JogSpace space,
                        const Wanted& wanted) -> ActionResult {
  const auto from = impl_->begin(&wanted);
  const Impl::Scope scope{*impl_};
  if (!impl_->still())
    return withdrawn();
  if (auto mode = impl_->teleop(space == JogSpace::axis, from); !accepted(mode))
    return mode;
  EMC_JOG_CONT message;
  message.joint_or_axis = index_of(axis);
  message.vel = impl_->units_per_second(direction < 0 ? -velocity : velocity);
  message.jjogmode = space == JogSpace::joint ? 1 : 0;
  return impl_->pass(message, from);
}

auto NmlPort::stop_jog(Axis axis) -> ActionResult {
  const auto from = impl_->begin(nullptr);
  const Impl::Scope scope{*impl_};
  const EMC_STAT* s = impl_->stat();
  const bool answered = impl_->answered(s);
  // Task answers a jog stop with an error unless the machine is on, and nothing jogs outside
  // manual mode. That holds for what task has taken up: with a command of this port still
  // unanswered, or without any status, the stop is sent and its real outcome reported.
  if (s != nullptr && answered &&
      (s->task.state != EMC_TASK_STATE_ON || s->task.mode != EMC_TASK_MODE_MANUAL))
    return {};
  // The stop for this axis is in the command queue already: task will execute it after
  // everything written before it. Writing it again would only fill the queue.
  if (!answered && impl_->unanswered_stop == index_of(axis))
    return impl_->finished(from);
  EMC_JOG_STOP message;
  message.joint_or_axis = index_of(axis);
  // A jog runs in the space motion is in: axis jogs need world mode, joint jogs joint mode.
  message.jjogmode = s != nullptr && s->motion.traj.mode == EMC_TRAJ_MODE_TELEOP ? 0 : 1;
  if (auto wrote = impl_->write(message); !accepted(wrote))
    return wrote;
  impl_->unanswered_stop = message.joint_or_axis;
  return impl_->finished(from);
}

auto NmlPort::jog_increment(Axis axis, double distance, double velocity, JogSpace space,
                            const Wanted& wanted) -> ActionResult {
  const auto from = impl_->begin(&wanted);
  const Impl::Scope scope{*impl_};
  if (!impl_->still())
    return withdrawn();
  if (auto mode = impl_->teleop(space == JogSpace::axis, from); !accepted(mode))
    return mode;
  EMC_JOG_INCR message;
  message.joint_or_axis = index_of(axis);
  // The sign of the velocity is the direction; the increment is a magnitude.
  message.incr = std::fabs(distance) / impl_->config.millimetres_per_unit;
  message.vel = impl_->units_per_second(distance < 0 ? -velocity : velocity);
  message.jjogmode = space == JogSpace::joint ? 1 : 0;
  return impl_->pass(message, from);
}

auto NmlPort::mdi(std::string_view text, const Wanted& wanted) -> ActionResult {
  const auto from = impl_->begin(&wanted);
  const Impl::Scope scope{*impl_};
  if (!impl_->still())
    return withdrawn();
  EMC_TASK_PLAN_EXECUTE message;
  if (!put(message.command, text))
    return failed("The MDI block is too long for LinuxCNC.");
  auto started = impl_->start(message, from);
  if (accepted(started) && impl_->loads != nullptr)
    ++impl_->loads->mdi_blocks;
  return started;
}

auto NmlPort::probe_fixed_setter(const FixedSetterMove& move, const Wanted& wanted)
    -> std::expected<FixedSetterContact, std::string> {
  using Contact = std::expected<FixedSetterContact, std::string>;
  constexpr double tolerance_mm = 0.02;
  const auto limits = impl_->config.fixed_setter;
  auto* hal = impl_->hal;
  if (!limits || hal == nullptr)
    return std::unexpected("The fixed setter is not configured.");
  if (move.tool <= 0 || !std::isfinite(move.travel_mm) || move.travel_mm <= 0 ||
      move.travel_mm > limits->travel_mm || !std::isfinite(move.feed_mm_min) ||
      move.feed_mm_min <= 0 || move.feed_mm_min > limits->feed_mm_min ||
      !std::ranges::all_of(move.start, [](double value) { return std::isfinite(value); }))
    return std::unexpected("The confirmed probe move exceeds the fixed setter limits.");
  const double move_seconds = move.travel_mm / move.feed_mm_min * seconds_per_minute;
  if (move_seconds > 180)
    return std::unexpected("The probe move would take more than three minutes.");
  const auto move_timeout = std::chrono::milliseconds{static_cast<int>((move_seconds + 3) * 1000)};

  const auto from = impl_->begin(&wanted);
  const Impl::Scope scope{*impl_};
  const auto* raw = impl_->stat();
  if (raw == nullptr || impl_->status.link() != Link::live || !impl_->still())
    return std::unexpected("The probe requires a live controller and an active request.");
  const auto before = to_status(*raw, impl_->config, hal, Link::live);
  const auto normal_mode = [](const NmlStatus& status) {
    const auto& c = status.controller;
    return c.machine == MachineState::on && c.mode == TaskMode::mdi && !status.paused &&
           !c.requested_tool && !c.spindle_active && status.spindle_actual_rpm &&
           *status.spindle_actual_rpm < 1.0 &&
           std::ranges::all_of(
               c.axes,
               [](const auto& axis) { return axis.homed && !axis.homing && !axis.on_hard_limit; });
  };
  const auto near = [](double a, double b) {
    return std::isfinite(a) && std::abs(a - b) <= tolerance_mm;
  };
  const auto& c = before.controller;
  const auto search_start = move.approach.value_or(move.start);
  const double bottom_z = search_start[2] - move.travel_mm;
  if (!normal_mode(before) || !raw->motion.traj.enabled || raw->motion.external_offsets_applied ||
      c.interp != InterpState::idle || !c.in_position || before.queued_mdi != 0 ||
      c.tool_in_spindle != move.tool || !std::ranges::contains(before.active_codes, "G40") ||
      before.feed_mode != FeedMode::per_minute || !std::isfinite(before.programmed_feed) ||
      !std::isfinite(bottom_z))
    return std::unexpected(
        "The probe requires a stopped spindle, homed axes and idle G40/G94 MDI.");
  for (std::size_t axis = 0; axis < move.start.size(); ++axis)
    if (!near(c.axes[axis].machine, move.start[axis]))
      return std::unexpected("The machine moved after the probe was confirmed.");
  for (std::size_t axis = 0; axis < search_start.size(); ++axis) {
    const auto& travel = impl_->config.travel[axis];
    if (!std::isfinite(search_start[axis]) || search_start[axis] < travel.min ||
        search_start[axis] > travel.max)
      return std::unexpected("The setter position is outside the machine's configured travel.");
  }
  if (bottom_z < impl_->config.travel[2].min || search_start[2] < move.start[2])
    return std::unexpected("The setter move is outside the confirmed Z travel.");

  const bool restore_feed_override = raw->motion.traj.feed_override_enabled;
  const bool restore_adaptive = raw->motion.traj.adaptive_feed_enabled;
  const bool restore_incremental = std::ranges::contains(before.active_codes, "G91");
  const double program_scale = c.program_units == LengthUnit::inch ? 25.4 : 1.0;
  const double machine_scale = impl_->config.millimetres_per_unit;
  auto expected_xy = move.start;
  bool positioning = false;
  bool traversing_xy = false;
  const auto steady_context = [&](const EMC_STAT& current) -> bool {
    const auto s = to_status(current, impl_->config, hal, Link::live);
    return normal_mode(s) && std::ranges::contains(s.active_codes, "G40") &&
           current.motion.traj.enabled && !current.motion.traj.paused &&
           !current.motion.external_offsets_applied && !current.motion.on_soft_limit &&
           s.controller.tool_in_spindle == move.tool &&
           s.controller.program_units == c.program_units && s.feed_mode == before.feed_mode &&
           s.wcs == before.wcs && s.g5x == before.g5x && s.g92 == before.g92 &&
           s.tool_offset == before.tool_offset && s.rotation_xy == before.rotation_xy &&
           (traversing_xy ? near(s.controller.axes[2].machine, search_start[2])
                          : near(s.controller.axes[0].machine, expected_xy[0]) &&
                                near(s.controller.axes[1].machine, expected_xy[1]));
  };

  if (!hal->begin_probe_cycle(Clock::now()))
    return std::unexpected("The probe interlock could not be armed.");
  const auto interrupt = [&](std::string_view reason) -> Contact {
    hal->stop_probe_cycle();
    // Keep the guard active if status is lost. An acknowledgement from a previous status is
    // not evidence that the controller has stopped the probe.
    const bool stopped = until(std::chrono::milliseconds{1500}, [&]() {
      const auto* s = impl_->stat();
      return s != nullptr && impl_->status.link() == Link::live &&
             s->task.state != EMC_TASK_STATE_ON && !s->motion.traj.enabled &&
             s->motion.traj.inpos && s->motion.traj.current_vel == 0;
    });
    if (stopped)
      hal->finish_probe_cycle();
    auto errors = impl_->take_errors(from);
    return std::unexpected(errors.empty() ? std::string(reason) : std::move(errors));
  };
  const auto live_cycle = [&]() -> const EMC_STAT* {
    const auto* s = impl_->stat();
    if (!impl_->still() || s == nullptr || impl_->status.link() != Link::live ||
        !steady_context(*s) || !hal->renew_probe_cycle(Clock::now()))
      return nullptr;
    return s;
  };
  const auto interlock = [&]() -> std::optional<HalView> {
    const auto view = hal->view(Clock::now());
    return view && view->probe_cycle_active && view->probe_setter_selected &&
                   view->probe_motion_permitted && view->probe_input
               ? view
               : std::nullopt;
  };
  const auto armed_at = Clock::now() + settle_timeout;
  while (live_cycle() != nullptr && !interlock() && Clock::now() < armed_at)
    std::this_thread::sleep_for(poll_interval);
  const auto selected = interlock();
  raw = live_cycle();
  if (raw == nullptr || !selected || *selected->probe_input || raw->motion.traj.probeval != 0 ||
      !near(raw->motion.traj.actualPosition.tran.z * machine_scale, move.start[2]))
    return interrupt("The fixed setter is tripped or its interlock is unavailable.");

  bool saw_probe = false;
  const auto block = [&](std::string_view code, std::chrono::milliseconds timeout,
                         bool watch_probe = false, bool moves = false) -> bool {
    if (live_cycle() == nullptr || !interlock())
      return false;
    EMC_TASK_PLAN_EXECUTE command;
    if (!put(command.command, code) || !accepted(impl_->write(command)))
      return false;
    if (impl_->loads != nullptr)
      ++impl_->loads->mdi_blocks;
    bool done = false;
    static_cast<void>(until(timeout, [&]() {
      impl_->collect();
      const auto* s = live_cycle();
      const auto pins = interlock();
      if (s == nullptr || !pins || (positioning && *pins->probe_input) ||
          impl_->error_since(from) || s->echo_serial_number > impl_->serial)
        return true;
      if (s->echo_serial_number != impl_->serial)
        return false;
      const auto& traj = s->motion.traj;
      if (moves && (traj.feed_override_enabled || traj.adaptive_feed_enabled))
        return true;
      if (watch_probe && traj.probing && !traj.probe_tripped)
        saw_probe = true;
      done = s->status == RCS_DONE && s->task.interpState == EMC_TASK_INTERP_IDLE &&
             s->task.queuedMDIcommands == 0 && traj.inpos && traj.current_vel == 0;
      return done || s->status == RCS_ERROR;
    }));
    return done;
  };

  if (!block("M50 P0", completion_timeout) || !block("M52 P0", completion_timeout))
    return interrupt("Could not prepare the probing feed.");
  raw = live_cycle();
  const auto probe_pin = interlock();
  if (raw == nullptr || !probe_pin || *probe_pin->probe_input || raw->motion.traj.probeval != 0 ||
      raw->motion.traj.feed_override_enabled || raw->motion.traj.adaptive_feed_enabled ||
      !near(raw->motion.traj.actualPosition.tran.z * machine_scale, move.start[2]) ||
      !near(raw->motion.traj.position.tran.z * machine_scale, move.start[2]))
    return interrupt("Probe input, position or feed controls changed before descent.");

  if (move.approach) {
    constexpr auto positioning_timeout = std::chrono::seconds{30};
    positioning = true;
    if (search_start[2] > move.start[2] &&
        !block(std::format("G90 G53 G0 Z{:.8f}", search_start[2] / program_scale),
               positioning_timeout, false, true))
      return interrupt("Could not raise Z to setter clearance.");
    raw = live_cycle();
    if (raw == nullptr ||
        !near(raw->motion.traj.actualPosition.tran.z * machine_scale, search_start[2]))
      return interrupt("The machine did not reach the setter clearance height.");
    traversing_xy = true;
    if (!block(std::format("G90 G53 G0 X{:.8f} Y{:.8f}", search_start[0] / program_scale,
                           search_start[1] / program_scale),
               positioning_timeout, false, true))
      return interrupt("Could not position XY above the setter.");
    expected_xy = search_start;
    traversing_xy = false;
    positioning = false;
    raw = live_cycle();
    const auto positioned = interlock();
    if (raw == nullptr || !positioned || *positioned->probe_input ||
        raw->motion.traj.probeval != 0 ||
        !near(raw->motion.traj.actualPosition.tran.z * machine_scale, search_start[2]) ||
        !near(raw->motion.traj.position.tran.z * machine_scale, search_start[2]) ||
        std::hypot(raw->motion.traj.actualPosition.tran.x * machine_scale - search_start[0],
                   raw->motion.traj.actualPosition.tran.y * machine_scale - search_start[1]) >
            limits->xy_tolerance_mm)
      return interrupt("The tool must be centered above a clear setter before probing.");
  }

  const auto feed_word = std::format(" F{:.8f}", move.feed_mm_min / program_scale);
  const auto search = [&](double from_z, double travel, double feed) -> Contact {
    saw_probe = false;
    const auto timeout = std::chrono::milliseconds{
        static_cast<int>((travel / feed * seconds_per_minute + 3) * 1000)};
    if (!block(
            std::format("G91 G38.2 Z{:.8f} F{:.8f}", -travel / program_scale, feed / program_scale),
            timeout, true, true))
      return interrupt("The fixed setter descent did not complete.");
    const auto* sample = live_cycle();
    const auto at_contact = interlock();
    if (sample == nullptr || !at_contact || !*at_contact->probe_input || !saw_probe ||
        !sample->motion.traj.probe_tripped || sample->motion.traj.probing ||
        sample->motion.traj.probeval == 0 || sample->echo_serial_number != impl_->serial ||
        !sample->motion.traj.inpos || sample->motion.traj.current_vel != 0)
      return interrupt("The fixed setter did not report a fresh contact.");
    const auto position = sample->motion.traj.probedPosition.tran;
    const double travel_after_contact =
        (position.z - sample->motion.traj.actualPosition.tran.z) * machine_scale;
    if (!std::isfinite(travel_after_contact))
      return interrupt("The fixed setter did not report a settled Z position.");
    const FixedSetterContact contact{
        {position.x * machine_scale, position.y * machine_scale, position.z * machine_scale},
        std::max(0.0, travel_after_contact)};
    if (!near(contact.machine[0], search_start[0]) || !near(contact.machine[1], search_start[1]) ||
        !std::isfinite(contact.machine[2]) || contact.machine[2] >= from_z ||
        contact.machine[2] < from_z - travel - tolerance_mm)
      return interrupt("The setter contact lies outside the confirmed search move.");
    return contact;
  };
  auto contact = search(search_start[2], move.travel_mm, move.feed_mm_min);
  if (!contact)
    return contact;
  std::optional<double> unstable_spread;
  if (limits->latch_feed_mm_min > 0) {
    const auto search_stop = contact->stopping_distance;
    std::array<double, 3> latches{};
    for (double& latch : latches) {
      const double backoff_z = contact->machine[2] + limits->latch_backoff_mm;
      if (backoff_z > impl_->config.travel[2].max ||
          !block(std::format("G90 G53 G1 Z{:.8f}{}", backoff_z / program_scale, feed_word),
                 move_timeout, false, true))
        return interrupt("The fixed setter latch backoff did not complete.");
      raw = live_cycle();
      const auto released = interlock();
      if (raw == nullptr || !released || *released->probe_input || raw->motion.traj.probeval != 0 ||
          !near(raw->motion.traj.position.tran.z * machine_scale, backoff_z) ||
          !near(raw->motion.traj.actualPosition.tran.z * machine_scale, backoff_z))
        return interrupt("The setter did not release before the latch pass.");
      const double latch_travel = std::min(2 * limits->latch_backoff_mm, backoff_z - bottom_z);
      contact = search(backoff_z, latch_travel, limits->latch_feed_mm_min);
      if (!contact)
        return contact;
      latch = contact->machine[2];
    }
    const auto [low, high] = std::ranges::minmax(latches);
    constexpr double max_spread_mm = 0.01;
    if (high - low > max_spread_mm)
      unstable_spread = high - low;
    else
      contact->machine[2] = std::accumulate(latches.begin(), latches.end(), 0.0) /
                            static_cast<double>(latches.size());
    contact->stopping_distance = search_stop;
  }
  // On a failed teach the pre-probe setup height is known; no new clearance was established.
  const double retract_z = move.retract_above_contact_mm > 0 && !unstable_spread
                               ? contact->machine[2] + move.retract_above_contact_mm
                               : move.retract_z.value_or(search_start[2]);
  raw = live_cycle();
  if (raw == nullptr || !std::isfinite(retract_z) || retract_z > impl_->config.travel[2].max ||
      !std::isfinite(raw->motion.traj.actualPosition.tran.z) ||
      retract_z <= raw->motion.traj.actualPosition.tran.z * machine_scale ||
      retract_z <= contact->machine[2])
    return interrupt("The tool-change height is outside safe Z travel for this contact.");
  if (!block(std::format("G90 G53 G1 Z{:.8f}{}", retract_z / program_scale, feed_word),
             move_timeout, false, true))
    return interrupt("The fixed setter retract did not complete.");
  raw = live_cycle();
  const auto after_retract = interlock();
  if (raw == nullptr || !after_retract || *after_retract->probe_input ||
      raw->motion.traj.probeval != 0 ||
      !near(raw->motion.traj.actualPosition.tran.z * machine_scale, retract_z))
    return interrupt("The fixed setter did not release at the retract height.");

  if (!block(std::format("{} G80 F{:.8f}", restore_incremental ? "G91" : "G90",
                         before.programmed_feed / program_scale),
             completion_timeout) ||
      (restore_feed_override && !block("M50 P1", completion_timeout)) ||
      (restore_adaptive && !block("M52 P1", completion_timeout)))
    return interrupt("The probe completed, but LinuxCNC did not restore its feed settings.");
  raw = live_cycle();
  if (raw == nullptr || raw->motion.traj.feed_override_enabled != restore_feed_override ||
      raw->motion.traj.adaptive_feed_enabled != restore_adaptive)
    return interrupt("The probe completed, but LinuxCNC did not confirm its feed controls.");
  hal->finish_probe_cycle();
  if (unstable_spread)
    return std::unexpected(
        std::format("Setter latch spread {:.4f} mm exceeds 0.01 mm.", *unstable_spread));
  return contact;
}

auto NmlPort::fixed_setter_available() const -> bool {
  return impl_->config.fixed_setter.has_value() && impl_->hal != nullptr;
}

auto NmlPort::fixed_setter(const FixedSetterRequest& request, SetterEpoch epoch,
                           const Wanted& wanted) -> std::expected<SetterState, std::string> {
  const auto limits = impl_->config.fixed_setter;
  if (!limits || impl_->hal == nullptr)
    return std::unexpected("The fixed setter is unavailable.");

  auto& calibration = impl_->setter_calibration;
  if (calibration && calibration->datum != epoch.datum)
    calibration.reset();
  const auto state = [&]() -> SetterState {
    return {calibration ? std::optional{calibration->plane} : std::nullopt, std::nullopt};
  };
  if (request.action == SetterAction::status)
    return state();

  const auto* raw = impl_->stat();
  if (raw == nullptr || impl_->status.link() != Link::live || (wanted && !wanted()))
    return std::unexpected("The fixed setter requires a live controller and an active request.");
  const auto before = to_status(*raw, impl_->config, impl_->hal, Link::live);
  const auto frame_of = [](const NmlStatus& s) {
    return Impl::WorkFrame{s.wcs, s.g5x, s.g92, s.rotation_xy};
  };
  const auto frame = frame_of(before);

  const auto ready = [](const EMC_STAT& status, const NmlStatus& s) {
    const auto& c = s.controller;
    return c.machine == MachineState::on && c.in_position && c.interp == InterpState::idle &&
           !s.paused && s.queued_mdi == 0 && !s.task_executing && !c.requested_tool &&
           !c.spindle_active && s.spindle_actual_rpm && std::abs(*s.spindle_actual_rpm) < 1.0 &&
           status.motion.traj.enabled && status.motion.traj.current_vel == 0 &&
           !status.motion.external_offsets_applied && !status.motion.on_soft_limit &&
           std::ranges::all_of(
               c.axes, [](const auto& axis) { return !axis.homing && !axis.on_hard_limit; });
  };
  // Homing is not required: machine coordinates are continuous within a session, and a homing
  // or a restart forgets the taught setter (Session::observe).
  if (!ready(*raw, before))
    return std::unexpected("The fixed setter requires stationary axes and a stopped spindle.");
  const int tool = before.controller.tool_in_spindle;
  if (tool <= 0 || request.tool != tool)
    return std::unexpected("The confirmed tool must be in the spindle.");

  constexpr double position_tolerance = 0.02; // mm
  const auto near = [](double actual, double expected) {
    return std::abs(actual - expected) <= position_tolerance;
  };
  const auto at_start = [&](const NmlStatus& s, const std::array<double, 3>& start) {
    return near(s.controller.axes[0].machine, start[0]) &&
           near(s.controller.axes[1].machine, start[1]) &&
           near(s.controller.axes[2].machine, start[2]);
  };
  if (before.controller.mode != TaskMode::mdi || !at_start(before, request.start))
    return std::unexpected("Wait for idle MDI at the confirmed starting position.");
  const bool teaching = tool == limits->reference_tool;
  if (!teaching && !calibration)
    return std::unexpected(std::format("Teach the setter with T{} first.", limits->reference_tool));

  const double machine_scale = impl_->config.millimetres_per_unit;
  const auto table = impl_->read_tool_table();
  const auto previous = table ? tool_z(*table, tool) : std::nullopt;
  if (!previous || !std::isfinite(*previous * machine_scale))
    return std::unexpected("Could not read the loaded tool's length from the tool table.");
  const double previous_length = *previous * machine_scale;
  auto move = FixedSetterMove{tool, request.start, limits->travel_mm, limits->feed_mm_min};
  if (teaching) {
    move.retract_above_contact_mm = limits->clearance_mm;
    calibration.reset();
  } else {
    move.approach = std::array{calibration->plane[0], calibration->plane[1],
                               std::max(request.start[2], calibration->clearance_z)};
    if (limits->clearance_mm > 0)
      move.retract_z = calibration->clearance_z;
  }
  const auto contact = probe_fixed_setter(move, wanted);
  if (!contact)
    return std::unexpected(contact.error());
  if (wanted && !wanted())
    return std::unexpected("The fixed setter result was cancelled.");
  const double value = contact->machine[2] - (teaching ? previous_length : calibration->plane[2]);
  if (!std::isfinite(value))
    return std::unexpected("The fixed setter result is invalid.");
  const auto current_table = impl_->read_tool_table();
  if (!current_table || *current_table != *table)
    return std::unexpected("The tool table changed during the setter cycle.");

  const SetterMeasurement measurement{
      teaching, tool, contact->machine, value, previous_length, contact->stopping_distance};
  if (teaching) {
    calibration = Impl::Calibration{
        epoch.datum,
        {request.start[0], request.start[1], value},
        limits->clearance_mm > 0 ? contact->machine[2] + limits->clearance_mm : request.start[2]};
    return SetterState{calibration->plane, measurement};
  }
  auto stopped_at = move.approach.value_or(request.start);
  if (move.retract_z)
    stopped_at[2] = *move.retract_z;
  const auto from = impl_->begin(&wanted);
  const Impl::Scope scope{*impl_};
  const auto same_context = [&](const EMC_STAT& current) {
    const auto s = to_status(current, impl_->config, impl_->hal, Link::live);
    return ready(current, s) && s.controller.mode == TaskMode::mdi &&
           s.controller.program_units == before.controller.program_units &&
           s.controller.tool_in_spindle == tool && frame_of(s) == frame && at_start(s, stopped_at);
  };
  const auto block = [&](std::string_view code) {
    const auto* current = impl_->stat();
    if (current == nullptr || impl_->status.link() != Link::live || !same_context(*current))
      return false;
    EMC_TASK_PLAN_EXECUTE command;
    if (!put(command.command, code) || !accepted(impl_->write(command)))
      return false;
    if (impl_->loads != nullptr)
      ++impl_->loads->mdi_blocks;
    bool done = false;
    static_cast<void>(until(completion_timeout, [&]() {
      impl_->collect();
      const auto* s = impl_->stat();
      if (s == nullptr || impl_->status.link() != Link::live || impl_->error_since(from) ||
          s->echo_serial_number > impl_->serial)
        return true;
      if (s->echo_serial_number != impl_->serial || s->status == RCS_EXEC)
        return false;
      done = s->status == RCS_DONE && same_context(*s);
      return done || s->status == RCS_ERROR;
    }));
    return done;
  };
  const double scale = before.controller.program_units == LengthUnit::inch ? 25.4 : 1.0;
  if (!block(std::format("G10 L1 P{} Z{:.8f}", tool, value / scale))) {
    auto error = impl_->take_errors(from);
    return std::unexpected(error.empty() ? "LinuxCNC did not confirm the tool table write. Check "
                                           "the tool table before retrying."
                                         : std::move(error));
  }
  const auto changed = impl_->read_tool_table();
  const auto saved = changed ? tool_z(*changed, tool) : std::nullopt;
  constexpr double table_tolerance = 0.001; // mm, allows LinuxCNC's file rounding
  if (!saved || std::abs(*saved * machine_scale - value) > table_tolerance)
    return std::unexpected(
        "Could not verify the new tool table length. Check the tool table before retrying.");
  if (!block(std::format("G43 H{}", tool))) {
    const auto error = impl_->take_errors(from);
    return std::unexpected(error.empty()
                               ? "Length saved. LinuxCNC did not confirm applying compensation."
                               : "Length saved. Compensation failed. " + error);
  }
  const auto* applied = impl_->stat();
  if (applied == nullptr || impl_->status.link() != Link::live ||
      applied->echo_serial_number != impl_->serial ||
      !std::isfinite(applied->task.toolOffset.tran.z) ||
      std::abs(applied->task.toolOffset.tran.z * machine_scale - value) > table_tolerance)
    return std::unexpected("Length saved. Could not verify the applied compensation.");
  return SetterState{calibration->plane, measurement};
}

auto NmlPort::open_program(std::string_view path, const Wanted& wanted) -> ActionResult {
  const auto from = impl_->begin(&wanted);
  const Impl::Scope scope{*impl_};
  if (!impl_->still())
    return withdrawn();
  EMC_TASK_PLAN_OPEN message;
  if (!put(message.file, path))
    return failed("The program path is too long for LinuxCNC.");
  // The interpreter keeps a program open until it has run to its end or was aborted, and
  // refuses another while it does ("A file is already open"): close first. Closing with none
  // open does nothing.
  EMC_TASK_PLAN_CLOSE close;
  if (auto closed = impl_->run(close, from); !accepted(closed))
    return closed;
  if (auto done = impl_->run(message, from); !accepted(done))
    return done;
  // LinuxCNC has taken the file up: a load, also of the file that was loaded before.
  if (impl_->loads != nullptr)
    ++impl_->loads->programs;
  return impl_->settled(
      from, "LinuxCNC did not open the program.",
      [&](const EMC_STAT& now) -> bool { return text_of(now.task.file) == path; });
}

auto NmlPort::run(const Wanted& wanted) -> ActionResult {
  const auto from = impl_->begin(&wanted);
  const Impl::Scope scope{*impl_};
  if (!impl_->still())
    return withdrawn();
  EMC_TASK_PLAN_RUN message;
  message.line = 0; // from the start
  return impl_->start(message, from);
}

auto NmlPort::pause(const Wanted& wanted) -> ActionResult {
  const auto from = impl_->begin(&wanted);
  const Impl::Scope scope{*impl_};
  if (!impl_->still())
    return withdrawn();
  EMC_TASK_PLAN_PAUSE message;
  if (auto sent = impl_->send(message); !accepted(sent))
    return sent;
  return impl_->settled(from, "LinuxCNC did not pause.",
                        [](const EMC_STAT& now) -> bool { return now.task.task_paused != 0; });
}

auto NmlPort::resume(const Wanted& wanted) -> ActionResult {
  const auto from = impl_->begin(&wanted);
  const Impl::Scope scope{*impl_};
  if (!impl_->still())
    return withdrawn();
  EMC_TASK_PLAN_RESUME message;
  if (auto sent = impl_->send(message); !accepted(sent))
    return sent;
  return impl_->settled(from, "LinuxCNC did not resume.",
                        [](const EMC_STAT& now) -> bool { return now.task.task_paused == 0; });
}

auto NmlPort::step(const Wanted& wanted) -> ActionResult {
  const auto from = impl_->begin(&wanted);
  const Impl::Scope scope{*impl_};
  if (!impl_->still())
    return withdrawn();
  EMC_TASK_PLAN_STEP message;
  return impl_->pass(message, from);
}

auto NmlPort::abort() -> ActionResult {
  const auto from = impl_->begin(nullptr);
  const Impl::Scope scope{*impl_};
  EMC_TASK_ABORT message;
  return impl_->run(message, from);
}

auto NmlPort::set_override(Override which, double value, const Wanted& wanted) -> ActionResult {
  const auto from = impl_->begin(&wanted);
  const Impl::Scope scope{*impl_};
  if (!impl_->still())
    return withdrawn();
  switch (which) {
  case Override::feed: {
    EMC_TRAJ_SET_SCALE message;
    message.scale = value;
    return impl_->run(message, from);
  }
  case Override::rapid: {
    EMC_TRAJ_SET_RAPID_SCALE message;
    message.scale = value;
    return impl_->run(message, from);
  }
  case Override::spindle:
    break;
  }
  EMC_TRAJ_SET_SPINDLE_SCALE message;
  message.spindle = 0;
  message.scale = value;
  return impl_->run(message, from);
}

auto NmlPort::set_spindle(SpindleDirection direction, double rpm, const Wanted& wanted)
    -> ActionResult {
  // A spindle stop is never withdrawn.
  const auto from = impl_->begin(direction == SpindleDirection::stopped ? nullptr : &wanted);
  const Impl::Scope scope{*impl_};
  if (!impl_->still())
    return withdrawn();
  if (direction == SpindleDirection::stopped) {
    EMC_SPINDLE_OFF message;
    message.spindle = 0;
    return impl_->run(message, from);
  }
  EMC_SPINDLE_ON message;
  message.spindle = 0;
  // Reverse is a negative speed.
  message.speed = direction == SpindleDirection::reverse ? -rpm : rpm;
  message.factor = 0;
  message.xoffset = 0;
  message.wait_for_spindle_at_speed = 1;
  return impl_->run(message, from);
}

auto NmlPort::set_coolant(bool flood, bool mist, const Wanted& wanted) -> ActionResult {
  // Coolant off is never withdrawn.
  const auto from = impl_->begin(flood || mist ? &wanted : nullptr);
  const Impl::Scope scope{*impl_};
  if (!impl_->still())
    return withdrawn();
  const EMC_STAT* s = impl_->stat();
  // Only what differs from the observed state is sent; without a status, both.
  const bool send_flood = s == nullptr || (s->io.coolant.flood != 0) != flood;
  const bool send_mist = s == nullptr || (s->io.coolant.mist != 0) != mist;
  if (send_flood) {
    EMC_COOLANT_FLOOD_ON on;
    EMC_COOLANT_FLOOD_OFF off;
    auto done = flood ? impl_->run(on, from) : impl_->run(off, from);
    if (!accepted(done))
      return done;
  }
  if (send_mist) {
    EMC_COOLANT_MIST_ON on;
    EMC_COOLANT_MIST_OFF off;
    return mist ? impl_->run(on, from) : impl_->run(off, from);
  }
  return {};
}

auto NmlPort::set_program_option(ProgramOption option, bool on, const Wanted& wanted)
    -> ActionResult {
  const auto from = impl_->begin(&wanted);
  const Impl::Scope scope{*impl_};
  if (!impl_->still())
    return withdrawn();
  if (option == ProgramOption::optional_stop) {
    EMC_TASK_PLAN_SET_OPTIONAL_STOP message;
    message.state = on;
    return impl_->run(message, from);
  }
  EMC_TASK_PLAN_SET_BLOCK_DELETE message;
  message.state = on;
  return impl_->run(message, from);
}

auto NmlPort::tool_changed(int tool, std::uint64_t request, const Wanted& wanted) -> ActionResult {
  static_cast<void>(impl_->begin(&wanted));
  const Impl::Scope scope{*impl_};
  if (impl_->hal == nullptr)
    return failed("The CoffeeCNC HAL component is not available.");
  if (!impl_->hal->view(HalShared::Clock::now()))
    return failed("The CoffeeCNC HAL component is not being serviced.");
  // Asked at the moment the confirmation is handed to the control thread.
  if (!impl_->still())
    return withdrawn();
  // The request the decision was made on, not whatever the pins show by now.
  if (!impl_->hal->confirm_tool_changed({request, tool}))
    return failed("LinuxCNC is not waiting for that tool change any more.");
  return {};
}

auto NmlPort::set_tool(const Tool& tool, const Wanted& wanted) -> ActionResult {
  static_cast<void>(impl_->begin(&wanted));
  const Impl::Scope scope{*impl_};
  if (!impl_->still())
    return withdrawn();
  const auto table = impl_->read_tool_table();
  if (!table)
    return failed(table.error());
  return impl_->load_tool_table(with_tool(*table, tool, impl_->config.millimetres_per_unit));
}

auto NmlPort::remove_tool(int number, const Wanted& wanted) -> ActionResult {
  static_cast<void>(impl_->begin(&wanted));
  const Impl::Scope scope{*impl_};
  if (!impl_->still())
    return withdrawn();
  if (const EMC_STAT* s = impl_->stat(); s != nullptr && s->io.tool.toolInSpindle == number)
    return failed(std::format("Tool {} is in the spindle.", number));
  const auto table = impl_->read_tool_table();
  if (!table)
    return failed(table.error());
  const auto edited = without_tool(*table, number);
  if (!edited)
    return failed(std::format("Tool {} is not in the tool table.", number));
  return impl_->load_tool_table(*edited);
}

} // namespace coffeecnc
