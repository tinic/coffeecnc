#include <coffeecnc/linuxcnc/nml_port.hpp>

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
#include <iterator>
#include <mutex>
#include <numbers>
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
  MachineConfig config;
  HalShared* hal{};
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

  explicit Impl(MachineConfig machine)
      : config(std::move(machine)),
        command(emcFormat, "emcCommand", "xemc", config.nml_file.c_str()), status(config.nml_file),
        errors(emcFormat, "emcError", "xemc", config.nml_file.c_str()) {}

  [[nodiscard]] auto stat() -> const EMC_STAT* { return status.stat(); }

  void add(bool error, std::span<const char> field) {
    if (auto text = text_of(field); !text.empty())
      messages.push_back({error, std::move(text)});
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
        add(true, static_cast<EMC_OPERATOR_ERROR*>(message)->error);
      else if (type == EMC_OPERATOR_TEXT_TYPE)
        add(false, static_cast<EMC_OPERATOR_TEXT*>(message)->text);
      else if (type == EMC_OPERATOR_DISPLAY_TYPE)
        add(false, static_cast<EMC_OPERATOR_DISPLAY*>(message)->display);
      else if (type == NML_ERROR_TYPE)
        add(true, static_cast<NML_ERROR*>(message)->error);
      else if (type == NML_TEXT_TYPE)
        add(false, static_cast<NML_TEXT*>(message)->text);
      else if (type == NML_DISPLAY_TYPE)
        add(false, static_cast<NML_DISPLAY*>(message)->display);
    }
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
    const bool echo = until(echo_timeout, [&]() -> bool { return answered(stat()); });
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
    std::ifstream in{config.tool_table, std::ios::binary};
    if (!in)
      return std::unexpected(std::format("Cannot read tool table {}.", config.tool_table));
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
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
    return loaded;
  }
};

auto NmlPort::connect(const MachineConfig& config, HalShared* hal)
    -> std::expected<std::unique_ptr<NmlPort>, std::string> {
  auto impl = std::make_unique<Impl>(config);
  impl->hal = hal;
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
  return impl_->start(message, from);
}

auto NmlPort::open_program(std::string_view path, const Wanted& wanted) -> ActionResult {
  const auto from = impl_->begin(&wanted);
  const Impl::Scope scope{*impl_};
  if (!impl_->still())
    return withdrawn();
  EMC_TASK_PLAN_OPEN message;
  if (!put(message.file, path))
    return failed("The program path is too long for LinuxCNC.");
  if (auto done = impl_->run(message, from); !accepted(done))
    return done;
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
