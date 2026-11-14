#include <coffeecnc/linuxcnc/nml_port.hpp>

#include <coffeecnc/core/task_heartbeat.hpp>
#include <coffeecnc/linuxcnc/tool_table.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
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

// Text of a fixed-size, NUL-terminated NML string field, without trailing white space.
auto text_of(std::span<const char> field) -> std::string {
  std::string text(field.begin(), std::ranges::find(field, '\0'));
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
    text.pop_back();
  return text;
}

// False when the text does not fit the field with its terminator.
auto put(std::span<char> field, std::string_view text) -> bool {
  if (text.size() >= field.size())
    return false;
  std::ranges::fill(field, '\0');
  std::ranges::copy(text, field.begin());
  return true;
}

auto xyz(const EmcPose& pose) -> std::array<double, 3> {
  return {pose.tran.x, pose.tran.y, pose.tran.z};
}

auto index_of(Axis axis) -> int { return static_cast<int>(axis_index(axis).value_or(0)); }

} // namespace

struct NmlPort::Impl {
  MachineConfig config;
  HalComponent* hal{};
  std::unique_ptr<RCS_CMD_CHANNEL> command;
  std::unique_ptr<RCS_STAT_CHANNEL> status;
  std::unique_ptr<NML> errors;
  TaskHeartbeat heartbeat;
  // From the status buffer's write counter, as of the last stat().
  Link link{Link::down};
  // The status channel's buffer holds a task status from an earlier peek.
  bool have_status{};
  int serial{};
  std::vector<ControllerMessage> messages;

  explicit Impl(MachineConfig machine) : config(std::move(machine)) {}

  // The latest status task wrote, or null before the first. peek() returns 0 when task wrote
  // nothing since the last peek; the buffer then still holds the latest status. Whether task is
  // alive is the heartbeat's question, not this one's.
  [[nodiscard]] auto stat() -> const EMC_STAT* {
    const NMLTYPE type = status->peek();
    if (type == EMC_STAT_TYPE)
      have_status = true;
    else if (type != 0)
      return nullptr; // a channel error
    if (!have_status)
      return nullptr;
    // Every reading feeds the heartbeat, so the waits of an action keep it current.
    link = heartbeat.observe(status->cms->header.write_id, Clock::now());
    // NML hands out the base class; the buffer was seen to hold a task status.
    return static_cast<const EMC_STAT*>(status->get_address());
  }

  void add(bool error, std::span<const char> field) {
    if (auto text = text_of(field); !text.empty())
      messages.push_back({error, std::move(text)});
  }

  // Moves everything on the error channel into `messages`. Reading consumes.
  void collect() {
    for (NMLTYPE type = errors->read(); type > 0; type = errors->read()) {
      // NML hands out the base class; each downcast follows the type tag just read.
      NMLmsg* message = errors->get_address();
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

  // Writes the message and waits until task has echoed its serial number.
  [[nodiscard]] auto send(RCS_CMD_MSG& message) -> ActionResult {
    if (command->write(&message) != 0)
      return failed("Cannot write to LinuxCNC's command channel.");
    serial = message.serial_number;
    const bool echoed = until(echo_timeout, [&]() -> bool {
      const EMC_STAT* s = stat();
      return s != nullptr && s->echo_serial_number - serial >= 0;
    });
    if (!echoed)
      return {ActionStatus::timed_out, "LinuxCNC did not acknowledge the command."};
    return {};
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

  // Sends and waits until task has finished the command.
  [[nodiscard]] auto run(RCS_CMD_MSG& message, std::size_t from) -> ActionResult {
    if (auto sent = send(message); !accepted(sent))
      return sent;
    const int rcs = outcome(completion_timeout);
    if (rcs == RCS_EXEC)
      return {ActionStatus::timed_out, "LinuxCNC did not finish the command."};
    if (rcs == RCS_ERROR)
      return failure(from, "LinuxCNC reported an error.");
    return {};
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
    return settled(from,
                   on ? "LinuxCNC did not enter world mode for an axis jog."
                      : "LinuxCNC did not enter joint mode.",
                   [&](const EMC_STAT& s) -> bool { return in_teleop(s) == on; });
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

  // Replaces the file in one step, then has LinuxCNC read it.
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
    std::filesystem::rename(temporary, config.tool_table, error);
    if (error)
      return failed(std::format("Cannot replace tool table {}.", config.tool_table));
    const auto from = mark();
    EMC_TOOL_LOAD_TOOL_TABLE message;
    std::ranges::fill(message.file, '\0'); // the configured table
    return run(message, from);
  }
};

auto NmlPort::connect(const MachineConfig& config, HalComponent* hal)
    -> std::expected<std::unique_ptr<NmlPort>, std::string> {
  auto impl = std::make_unique<Impl>(config);
  impl->hal = hal;
  const char* file = impl->config.nml_file.c_str();
  impl->command = std::make_unique<RCS_CMD_CHANNEL>(emcFormat, "emcCommand", "xemc", file);
  impl->status = std::make_unique<RCS_STAT_CHANNEL>(emcFormat, "emcStatus", "xemc", file);
  impl->errors = std::make_unique<NML>(emcFormat, "emcError", "xemc", file);
  if (impl->command->valid() == 0 || impl->status->valid() == 0 || impl->errors->valid() == 0)
    return std::unexpected(
        std::format("Cannot open LinuxCNC's NML channels from {}.", impl->config.nml_file));
  // The first reading of the write counter: the next can tell whether task is running.
  static_cast<void>(until(echo_timeout, [&]() -> bool { return impl->stat() != nullptr; }));
  return std::unique_ptr<NmlPort>(new NmlPort(std::move(impl)));
}

NmlPort::NmlPort(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
NmlPort::~NmlPort() = default;

auto NmlPort::snapshot() -> ControllerState { return status().controller; }

auto NmlPort::status() -> NmlStatus {
  NmlStatus out;
  const EMC_STAT* s = impl_->stat();
  if (s == nullptr)
    return out; // link down
  const double mm = impl_->config.millimetres_per_unit;
  const auto& task = s->task;
  const auto& motion = s->motion;
  auto& c = out.controller;
  c.link = impl_->link;
  c.machine = task.state == EMC_TASK_STATE_ON      ? MachineState::on
              : task.state == EMC_TASK_STATE_ESTOP ? MachineState::estop
                                                   : MachineState::off;
  c.mode = task.mode == EMC_TASK_MODE_AUTO  ? TaskMode::automatic
           : task.mode == EMC_TASK_MODE_MDI ? TaskMode::mdi
                                            : TaskMode::manual;
  // Paused is whatever waits for resume or step. After the first step from an M0/M1 stop task
  // holds the next block with task_paused set while its interpreter state says reading or
  // waiting; that is still paused for every decision made on this state.
  c.interp = task.interpState == EMC_TASK_INTERP_IDLE      ? InterpState::idle
             : task.task_paused != 0                       ? InterpState::paused
             : task.interpState == EMC_TASK_INTERP_READING ? InterpState::reading
             : task.interpState == EMC_TASK_INTERP_PAUSED  ? InterpState::paused
                                                           : InterpState::waiting;
  const auto machine = xyz(motion.traj.actualPosition);
  const auto g5x = xyz(task.g5x_offset);
  const auto g92 = xyz(task.g92_offset);
  const auto tool = xyz(task.toolOffset);
  const auto dtg = xyz(motion.traj.dtg);
  // A teleop jog leaves traj.inpos set; its velocity shows.
  bool moving = !motion.traj.inpos || motion.traj.current_vel != 0;
  // As AXIS computes it: less tool offset and G5x, rotated back, less G92.
  std::array<double, 3> work{};
  for (std::size_t i = 0; i < work.size(); ++i)
    work[i] = machine[i] - tool[i] - g5x[i];
  if (task.rotation_xy != 0) {
    const double angle = -task.rotation_xy * std::numbers::pi / 180;
    const double x = work[0] * std::cos(angle) - work[1] * std::sin(angle);
    const double y = work[0] * std::sin(angle) + work[1] * std::cos(angle);
    work[0] = x;
    work[1] = y;
  }
  for (std::size_t i = 0; i < c.axes.size(); ++i) {
    // Trivial kinematics, checked when the configuration was loaded: joint i is axis i.
    const auto& joint = motion.joint[i];
    auto& axis = c.axes[i];
    axis.enabled = true;
    axis.homed = joint.homed != 0;
    axis.homing = joint.homing != 0;
    axis.on_hard_limit = joint.minHardLimit != 0 || joint.maxHardLimit != 0;
    axis.machine = machine[i] * mm;
    axis.min_limit = motion.axis[i].minPositionLimit * mm;
    axis.max_limit = motion.axis[i].maxPositionLimit * mm;
    moving = moving || motion.axis[i].velocity != 0 || joint.velocity != 0;
    out.work[i] = (work[i] - g92[i]) * mm;
    out.dtg[i] = dtg[i] * mm;
    out.g5x[i] = g5x[i] * mm;
    out.g92[i] = g92[i] * mm;
    out.tool_offset[i] = tool[i] * mm;
  }
  c.in_position = !moving;
  c.program_loaded = task.file[0] != '\0';
  c.tool_in_spindle = s->io.tool.toolInSpindle;
  if (impl_->hal != nullptr)
    c.requested_tool = impl_->hal->tool_change_request();
  c.program_units = task.programUnits == CANON_UNITS_INCHES ? LengthUnit::inch : LengthUnit::mm;

  out.teleop = motion.traj.mode == EMC_TRAJ_MODE_TELEOP;
  out.wcs = task.g5x_index;
  out.rotation_xy = task.rotation_xy;
  out.velocity = motion.traj.current_vel * mm * seconds_per_minute;
  out.feed_override = motion.traj.scale;
  out.rapid_override = motion.traj.rapid_scale;
  const auto& spindle = motion.spindle[0];
  out.spindle_direction = spindle.direction > 0   ? SpindleDirection::forward
                          : spindle.direction < 0 ? SpindleDirection::reverse
                                                  : SpindleDirection::stopped;
  out.spindle_rpm = std::fabs(spindle.speed);
  out.spindle_override = spindle.spindle_scale;
  out.flood = s->io.coolant.flood != 0;
  out.mist = s->io.coolant.mist != 0;
  if (s->io.tool.pocketPrepped >= 0)
    out.prepared_pocket = s->io.tool.pocketPrepped;
  out.program_file = text_of(task.file);
  out.current_line = task.currentLine;
  out.motion_line = task.motionLine;
  out.optional_stop = task.optional_stop_state;
  out.block_delete = task.block_delete_state;
  out.paused = task.task_paused != 0;
  out.dwell_seconds_left = task.delayLeft;
  out.queued_mdi = task.queuedMDIcommands;
  return out;
}

auto NmlPort::take_messages() -> std::vector<ControllerMessage> {
  // Called every control cycle: also the regular reading the heartbeat needs.
  static_cast<void>(impl_->stat());
  impl_->collect();
  return std::exchange(impl_->messages, {});
}

auto NmlPort::set_machine_state(MachineState state) -> ActionResult {
  const auto from = impl_->mark();
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

auto NmlPort::set_task_mode(TaskMode mode) -> ActionResult {
  const auto from = impl_->mark();
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

auto NmlPort::home(std::optional<Axis> axis) -> ActionResult {
  const auto from = impl_->mark();
  // Task homes joints, and only in joint mode; a homed machine is in world mode.
  if (auto joint_mode = impl_->teleop(false, from); !accepted(joint_mode))
    return joint_mode;
  EMC_JOINT_HOME message;
  message.joint = axis ? index_of(*axis) : -1; // -1 homes all joints in their configured sequence
  return impl_->pass(message, from);
}

auto NmlPort::start_jog(Axis axis, int direction, double velocity, JogSpace space) -> ActionResult {
  const auto from = impl_->mark();
  if (auto mode = impl_->teleop(space == JogSpace::axis, from); !accepted(mode))
    return mode;
  EMC_JOG_CONT message;
  message.joint_or_axis = index_of(axis);
  message.vel = impl_->units_per_second(direction < 0 ? -velocity : velocity);
  message.jjogmode = space == JogSpace::joint ? 1 : 0;
  return impl_->pass(message, from);
}

auto NmlPort::stop_jog(Axis axis) -> ActionResult {
  const auto from = impl_->mark();
  const EMC_STAT* s = impl_->stat();
  // Task answers a jog stop with an error unless the machine is on, and nothing jogs outside
  // manual mode. Without any status the stop is sent and its real outcome reported.
  if (s != nullptr && (s->task.state != EMC_TASK_STATE_ON || s->task.mode != EMC_TASK_MODE_MANUAL))
    return {};
  EMC_JOG_STOP message;
  message.joint_or_axis = index_of(axis);
  // A jog runs in the space motion is in: axis jogs need world mode, joint jogs joint mode.
  message.jjogmode = s != nullptr && s->motion.traj.mode == EMC_TRAJ_MODE_TELEOP ? 0 : 1;
  return impl_->run(message, from);
}

auto NmlPort::jog_increment(Axis axis, double distance, double velocity, JogSpace space)
    -> ActionResult {
  const auto from = impl_->mark();
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

auto NmlPort::mdi(std::string_view text) -> ActionResult {
  const auto from = impl_->mark();
  EMC_TASK_PLAN_EXECUTE message;
  if (!put(message.command, text))
    return failed("The MDI block is too long for LinuxCNC.");
  return impl_->start(message, from);
}

auto NmlPort::open_program(std::string_view path) -> ActionResult {
  const auto from = impl_->mark();
  EMC_TASK_PLAN_OPEN message;
  if (!put(message.file, path))
    return failed("The program path is too long for LinuxCNC.");
  if (auto done = impl_->run(message, from); !accepted(done))
    return done;
  return impl_->settled(
      from, "LinuxCNC did not open the program.",
      [&](const EMC_STAT& now) -> bool { return text_of(now.task.file) == path; });
}

auto NmlPort::run() -> ActionResult {
  const auto from = impl_->mark();
  EMC_TASK_PLAN_RUN message;
  message.line = 0; // from the start
  return impl_->start(message, from);
}

auto NmlPort::pause() -> ActionResult {
  const auto from = impl_->mark();
  EMC_TASK_PLAN_PAUSE message;
  if (auto sent = impl_->send(message); !accepted(sent))
    return sent;
  return impl_->settled(from, "LinuxCNC did not pause.",
                        [](const EMC_STAT& now) -> bool { return now.task.task_paused != 0; });
}

auto NmlPort::resume() -> ActionResult {
  const auto from = impl_->mark();
  EMC_TASK_PLAN_RESUME message;
  if (auto sent = impl_->send(message); !accepted(sent))
    return sent;
  return impl_->settled(from, "LinuxCNC did not resume.",
                        [](const EMC_STAT& now) -> bool { return now.task.task_paused == 0; });
}

auto NmlPort::step() -> ActionResult {
  const auto from = impl_->mark();
  EMC_TASK_PLAN_STEP message;
  return impl_->pass(message, from);
}

auto NmlPort::abort() -> ActionResult {
  const auto from = impl_->mark();
  EMC_TASK_ABORT message;
  return impl_->run(message, from);
}

auto NmlPort::set_override(Override which, double value) -> ActionResult {
  const auto from = impl_->mark();
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

auto NmlPort::set_spindle(SpindleDirection direction, double rpm) -> ActionResult {
  const auto from = impl_->mark();
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

auto NmlPort::set_coolant(bool flood, bool mist) -> ActionResult {
  const auto from = impl_->mark();
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

auto NmlPort::set_program_option(ProgramOption option, bool on) -> ActionResult {
  const auto from = impl_->mark();
  if (option == ProgramOption::optional_stop) {
    EMC_TASK_PLAN_SET_OPTIONAL_STOP message;
    message.state = on;
    return impl_->run(message, from);
  }
  EMC_TASK_PLAN_SET_BLOCK_DELETE message;
  message.state = on;
  return impl_->run(message, from);
}

auto NmlPort::tool_changed() -> ActionResult {
  if (impl_->hal == nullptr)
    return failed("The CoffeeCNC HAL component is not available.");
  if (!impl_->hal->tool_change_request())
    return failed("LinuxCNC is not waiting for a tool change.");
  impl_->hal->confirm_tool_changed();
  return {};
}

auto NmlPort::set_tool(const Tool& tool) -> ActionResult {
  const auto table = impl_->read_tool_table();
  if (!table)
    return failed(table.error());
  return impl_->load_tool_table(with_tool(*table, tool, impl_->config.millimetres_per_unit));
}

auto NmlPort::remove_tool(int number) -> ActionResult {
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
