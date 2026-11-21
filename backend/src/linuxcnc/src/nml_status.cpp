#include "nml_status.hpp"

#include <coffeecnc/core/task_heartbeat.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <numbers>
#include <thread>
#include <utility>

#include <cms.hh>
#include <emc.hh>
#include <nml.hh>
#include <stat_msg.hh>

namespace coffeecnc {

void map_breakpoint(NmlStatus& status, const BreakpointSource* source) {
  if (source == nullptr || status.program_file != source->temporary)
    return;
  status.program_file = source->original;
  status.controller.program_file = source->original;
  status.controller.breakpoint_program = true;
  const bool reached = status.controller.interp == InterpState::paused && status.paused &&
                       source->marker_seen.load() && status.current_line <= source->line &&
                       !status.controller.requested_tool;
  status.breakpoint = ProgramBreakpoint{source->line, reached};
  if (reached)
    status.current_line = source->line;
  else if (status.current_line > source->line)
    --status.current_line;
  if (status.motion_line > source->line)
    --status.motion_line;
}
namespace {

constexpr double seconds_per_minute = 60;

auto xyz(const EmcPose& pose) -> std::array<double, 3> {
  return {pose.tran.x, pose.tran.y, pose.tran.z};
}

} // namespace

// Text of a fixed-size, NUL-terminated NML string field, without trailing white space.
auto text_of(std::span<const char> field) -> std::string {
  std::string text(field.begin(), std::ranges::find(field, '\0'));
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
    text.pop_back();
  return text;
}

auto to_status(const EMC_STAT& s, const MachineConfig& config, const HalShared* hal, Link link)
    -> NmlStatus {
  NmlStatus out;
  const double mm = config.millimetres_per_unit;
  const auto& task = s.task;
  const auto& motion = s.motion;
  auto& c = out.controller;
  c.link = link;
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
  c.tool_in_spindle = s.io.tool.toolInSpindle;
  // As the control thread read the pins last; nothing of it while that thread has stalled.
  const auto view = hal != nullptr ? hal->view(HalShared::Clock::now()) : std::nullopt;
  if (view) {
    if (view->tool_change_request) {
      c.requested_tool = view->tool_change_request->tool;
      c.tool_request = view->tool_change_request->generation;
    }
    out.spindle_actual_rpm = view->spindle_rpm;
    out.spindle_at_speed = view->spindle_at_speed;
    out.probe_input = view->probe_input;
  }
  c.program_units = task.programUnits == CANON_UNITS_INCHES ? LengthUnit::inch : LengthUnit::mm;

  out.echo_serial = s.echo_serial_number;
  out.command_done = s.status == RCS_DONE;
  out.task_executing = task.execState != EMC_TASK_EXEC_DONE || s.status == RCS_EXEC;
  out.task_failed = task.execState == EMC_TASK_EXEC_ERROR || s.status == RCS_ERROR;
  out.teleop = motion.traj.mode == EMC_TRAJ_MODE_TELEOP;
  out.commanded_position = xyz(motion.traj.position);
  for (auto& coordinate : out.commanded_position)
    coordinate *= mm;
  out.wcs = task.g5x_index;
  out.rotation_xy = task.rotation_xy;
  out.velocity = motion.traj.current_vel * mm * seconds_per_minute;
  out.feed_override = motion.traj.scale;
  out.rapid_override = motion.traj.rapid_scale;
  const auto& spindle = motion.spindle[0];
  c.spindle_active = spindle.enabled != 0 || spindle.direction != 0 ||
                     (out.spindle_actual_rpm && std::abs(*out.spindle_actual_rpm) >= 1.0);
  out.spindle_direction = spindle.direction > 0   ? SpindleDirection::forward
                          : spindle.direction < 0 ? SpindleDirection::reverse
                                                  : SpindleDirection::stopped;
  out.spindle_rpm = std::fabs(spindle.speed);
  out.spindle_override = spindle.spindle_scale;
  out.flood = s.io.coolant.flood != 0;
  out.mist = s.io.coolant.mist != 0;
  if (s.io.tool.pocketPrepped >= 0)
    out.prepared_pocket = s.io.tool.pocketPrepped;
  out.program_file = text_of(task.file);
  c.program_file = out.program_file;
  out.current_line = task.currentLine;
  out.motion_line = task.motionLine;
  out.optional_stop = task.optional_stop_state;
  out.block_delete = task.block_delete_state;
  out.paused = task.task_paused != 0;
  if (const auto& limits = config.paused_z) {
    // A feed-held path can retain queued motion and clear traj.inpos even at rest.
    constexpr double rest_mm_s = 0.0001;
    out.paused_z_still = std::abs(motion.traj.current_vel * mm) < rest_mm_s;
    for (std::size_t i = 0; i < c.axes.size(); ++i)
      out.paused_z_still = out.paused_z_still &&
                           std::abs(motion.joint[i].velocity * mm) < rest_mm_s &&
                           std::abs(motion.axis[i].velocity * mm) < rest_mm_s;
    // LinuxCNC 2.9's NML motion status does not serialize the external-offset pose.
    // Read applied offset from HAL; the NML axis position independently checks the move.
    if (view && view->paused_z) {
      const double offset = view->paused_z->offset * mm;
      if (std::isfinite(offset))
        out.paused_z_feedback =
            PausedZInputs{offset, view->paused_z->active, view->paused_z->limited};
    }
    const auto& feedback = out.paused_z_feedback;
    const double z_mm = feedback ? feedback->offset : 0;
    const double tolerance = limits->count_mm / 2;
    // The planner can finish within its servo-cycle tolerance of zero.
    const bool displaced = feedback && (feedback->active || std::abs(z_mm) > tolerance);
    const auto blocked = [&]() -> std::string {
      if (link != Link::live || c.machine != MachineState::on || !motion.traj.enabled ||
          c.mode != TaskMode::automatic || c.interp != InterpState::paused || !out.paused)
        return "Pause a running program before jogging Z.";
      if (std::ranges::any_of(c.axes,
                              [](const AxisState& axis) { return !axis.homed || axis.homing; }))
        return "Home all axes before jogging Z while paused.";
      if (c.requested_tool)
        return "Finish the tool change before jogging Z.";
      if (!view || !feedback || !view->watchdog_ok || view->jog_stop_high ||
          view->probe_cycle_active || (view->probe_input && *view->probe_input))
        return "The paused Z motion interlock is open.";
      if (!out.spindle_actual_rpm || c.spindle_active || task.activeMCodes[2] != 5)
        return "Paused Z jog requires a program spindle stop (M5) and zero spindle speed.";
      if (feedback->limited || motion.on_soft_limit ||
          std::ranges::any_of(c.axes, [](const AxisState& axis) { return axis.on_hard_limit; }))
        return "A travel limit blocks paused Z motion.";
      if (z_mm < -tolerance || z_mm > limits->lift_mm + tolerance)
        return "The active external offset is outside the paused Z range.";
      return {};
    };
    out.paused_z_interlock = blocked();
    c.paused_z = PausedZStatus{z_mm, limits->lift_mm, displaced, feedback.has_value(),
                               out.paused_z_interlock};
    if (c.paused_z->reason.empty() && !out.paused_z_still)
      c.paused_z->reason = "Wait for the axes to stop.";
  }
  out.dwell_seconds_left = task.delayLeft;
  out.queued_mdi = task.queuedMDIcommands;
  // Element 0 of each array is the line number the codes were read at; -1 marks an empty
  // group. G codes are stored times ten: 540 is G54, 591 is G59.1.
  for (std::size_t i = 1; i < std::size(task.activeGCodes); ++i) {
    // Index 2 is the last nonmodal word. Index 16 is LinuxCNC's G92 bookkeeping,
    // which can change its reported word without changing the effective offset.
    if (i == 2 || i == 16)
      continue;
    const int code = task.activeGCodes[i];
    if (code < 0)
      continue;
    out.active_codes.push_back(code % 10 == 0 ? std::format("G{}", code / 10)
                                              : std::format("G{}.{}", code / 10, code % 10));
    if (code == 930)
      out.feed_mode = FeedMode::inverse_time;
    else if (code == 950)
      out.feed_mode = FeedMode::per_revolution;
  }
  for (std::size_t i = 2; i < std::size(task.activeMCodes); ++i) {
    if (i == 3)
      continue; // A tool change is nonmodal. Tool and pending request are reported separately.
    if (const int code = task.activeMCodes[i]; code >= 0)
      out.active_codes.push_back(std::format("M{}", code));
  }
  // The interpreter keeps F in the program's unit; an inverse time feed has no length in it.
  const double feed_scale =
      out.feed_mode == FeedMode::inverse_time || c.program_units == LengthUnit::mm ? 1.0 : 25.4;
  out.programmed_feed = task.activeSettings[1] * feed_scale;
  return out;
}

auto nml_gate() -> std::mutex& {
  static std::mutex gate;
  return gate;
}

StatusChannel::StatusChannel(const std::string& nml_file)
    : channel_(emcFormat, "emcStatus", "xemc", nml_file.c_str()) {}

auto StatusChannel::valid() -> bool {
  return channel_.call([](RCS_STAT_CHANNEL& channel) -> int { return channel.valid(); }) != 0;
}

auto StatusChannel::stat() -> const EMC_STAT* {
  // One call: peek and the write counter it left in this channel's header belong together.
  std::int64_t writes = 0;
  const NMLTYPE type = channel_.call([&](RCS_STAT_CHANNEL& channel) -> NMLTYPE {
    const NMLTYPE read = channel.peek();
    writes = channel.cms->header.write_id;
    return read;
  });
  if (type == EMC_STAT_TYPE)
    have_status_ = true;
  else if (type != 0)
    return nullptr; // a channel error
  if (!have_status_)
    return nullptr;
  // Every reading feeds the heartbeat, so the waits of an action keep it current.
  link_ = heartbeat_.observe(writes, TaskHeartbeat::Clock::now());
  // NML hands out the base class; the buffer was seen to hold a task status. It is this
  // channel's own copy and stays as it is until the next peek.
  return static_cast<const EMC_STAT*>(
      channel_.call([](RCS_STAT_CHANNEL& channel) -> NMLmsg* { return channel.get_address(); }));
}

struct NmlStatusReader::Impl {
  MachineConfig config;
  const HalShared* hal{};
  NmlStatusMailbox* mailbox{};
  StatusChannel channel;

  explicit Impl(MachineConfig machine) : config(std::move(machine)), channel(config.nml_file) {}
};

void NmlStatusMailbox::put(const NmlStatus& status,
                           std::chrono::steady_clock::time_point read_at) {
  const std::scoped_lock lock{mutex_};
  latest_ = TimedStatus{status, read_at};
}

auto NmlStatusMailbox::latest() const -> std::optional<TimedStatus> {
  const std::scoped_lock lock{mutex_};
  return latest_;
}

auto NmlStatusReader::connect(const MachineConfig& config, const HalShared* hal,
                              NmlStatusMailbox* mailbox)
    -> std::expected<std::unique_ptr<NmlStatusReader>, std::string> {
  auto impl = std::make_unique<Impl>(config);
  impl->hal = hal;
  impl->mailbox = mailbox;
  if (!impl->channel.valid())
    return std::unexpected(
        std::format("Cannot open LinuxCNC's status channel from {}.", impl->config.nml_file));
  // The first reading of the write counter: the next can tell whether task is running.
  const auto deadline = TaskHeartbeat::Clock::now() + NmlPort::echo_timeout;
  while (impl->channel.stat() == nullptr && TaskHeartbeat::Clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  return std::unique_ptr<NmlStatusReader>(new NmlStatusReader(std::move(impl)));
}

NmlStatusReader::NmlStatusReader(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
NmlStatusReader::~NmlStatusReader() = default;

auto NmlStatusReader::read() -> ControllerState { return status().controller; }

auto NmlStatusReader::status() -> NmlStatus {
  // Dated before it is acquired: see TimedStatus.
  const auto begun = std::chrono::steady_clock::now();
  const EMC_STAT* s = impl_->channel.stat();
  // Without a status the link is down.
  auto status =
      s == nullptr ? NmlStatus{}
                   : to_status(*s, impl_->config, impl_->hal, impl_->channel.link());
  if (impl_->mailbox != nullptr)
    impl_->mailbox->put(status, begun);
  return status;
}

} // namespace coffeecnc
