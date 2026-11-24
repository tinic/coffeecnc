#include <coffeecnc/linuxcnc/nml_port.hpp>
#include <coffeecnc/core/text_file.hpp>

#include <coffeecnc/core/jog_stop_report.hpp>
#include <coffeecnc/core/preview.hpp>
#include <coffeecnc/core/task_heartbeat.hpp>
#include <coffeecnc/linuxcnc/tool_table.hpp>

#include "durable_file.hpp"
#include "nml_gate.hpp"
#include "nml_status.hpp"
#include "program_watch.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <numbers>
#include <numeric>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <cmd_msg.hh>
#include <cms.hh>
#include <emc.hh>
#include <emc_nml.hh>
#include <nml.hh>
#include <nml_oi.hh>
#include <nml_type.hh>
#include <rcs.hh>
#include <stat_msg.hh>

#include <sys/stat.h>

namespace coffeecnc {

auto file_stamp(const std::string& path) -> std::optional<FileStamp> {
  struct stat info{};
  if (path.empty() || ::stat(path.c_str(), &info) != 0)
    return std::nullopt;
  return FileStamp{.inode = static_cast<std::uint64_t>(info.st_ino),
                   .modified = (std::int64_t{info.st_mtim.tv_sec} * 1'000'000'000) +
                               std::int64_t{info.st_mtim.tv_nsec},
                   .size = std::int64_t{info.st_size}};
}

void WrittenSerials::begin() {
  const std::scoped_lock lock{mutex_};
  writing_ = true;
}

void WrittenSerials::wrote(int serial) {
  const std::scoped_lock lock{mutex_};
  writing_ = false;
  serials_.at(count_++ % kept) = serial;
}

auto WrittenSerials::ours(int echo) const -> bool {
  const std::scoped_lock lock{mutex_};
  return writing_ || std::ranges::contains(std::span(serials_).first(std::min(count_, kept)), echo);
}

namespace {

// How long a synch that failed waits before it is tried again.
constexpr auto synch_retry_period = std::chrono::seconds{1};

// Commands that make the interpreter run G-code, and so may change its parameters.
auto runs_gcode(long type) -> bool {
  return type == EMC_TASK_PLAN_EXECUTE_TYPE || type == EMC_TASK_PLAN_RUN_TYPE ||
         type == EMC_TASK_PLAN_STEP_TYPE || type == EMC_TASK_PLAN_RESUME_TYPE;
}

auto load_touched(const std::string& file) -> std::optional<TouchedOff> {
  std::ifstream in(file);
  std::string text;
  for (std::string line; std::getline(in, line);)
    text += line + '\n';
  return parse_touched_off(text);
}
// The records beside the tool table say what is in the spindle and where a run stopped: on
// disk, whole, before anything relies on them.
auto write_sidecar(const std::filesystem::path& path, std::string_view text) -> bool {
  return replace_synced(path.string(), text).has_value();
}
void save_touched(const std::string& file, const TouchedOff& touched) {
  static_cast<void>(write_sidecar(file, format_touched_off(touched)));
}

auto load_spindle_tool(const std::filesystem::path& path) -> std::optional<int> {
  const auto contents = read_text_file(path.string(), 32);
  if (!contents)
    return std::nullopt;
  std::string_view text = *contents;
  if (text.ends_with('\n'))
    text.remove_suffix(1);
  if (text.empty())
    return std::nullopt;
  int tool{};
  const auto* begin = text.data();
  const auto* end = begin + text.size();
  const auto [parsed, error] = std::from_chars(begin, end, tool);
  return error == std::errc{} && parsed == end && tool >= 0 ? std::optional{tool} : std::nullopt;
}

constexpr std::string_view breakpoint_message = "CoffeeCNC breakpoint ";

auto with_breakpoint(std::string_view program, int target, std::string_view receipt)
    -> std::expected<std::string, std::string> {
  std::size_t insertion = std::string_view::npos;
  auto remaining = program;
  for (int line = 1; !remaining.empty(); ++line) {
    const auto end = remaining.find('\n');
    const auto block = remaining.substr(0, end);
    if (o_word_name(block))
      return std::unexpected("A breakpoint needs a program without subroutines or loops.");
    if (line == target && motion_use(block) != MotionUse::none)
      insertion = program.size() - remaining.size();
    remaining.remove_prefix(end == std::string_view::npos ? remaining.size() : end + 1);
  }
  if (insertion == std::string_view::npos)
    return std::unexpected("Choose a motion block in the loaded program.");
  // The copy preserves the original words, comments and line endings on every source line.
  std::string copied{program};
  // LinuxCNC tags M0 with the previous motion line. This queued message is delivered after
  // prior motion and I/O settle and immediately before that block's unconditional pause.
  copied.insert(insertion, std::format("M0 (MSG,{}{})\n", breakpoint_message, receipt));
  return copied;
}

auto load_breakpoint(const std::filesystem::path& record, const std::filesystem::path& copy)
    -> std::shared_ptr<const BreakpointSource> {
  const auto text = read_text_file(record.string(), 512);
  if (!text)
    return {};
  std::istringstream fields{*text};
  int line{};
  std::string receipt;
  bool received{};
  std::string original;
  if (!(fields >> line >> receipt >> received) || !std::getline(fields >> std::ws, original) ||
      line <= 0 || !original.starts_with('/') || original.size() > 254 ||
      original == copy.string() ||
      !std::ranges::all_of(receipt, [](char c) { return c >= '0' && c <= '9'; }) ||
      std::ranges::any_of(original, [](unsigned char c) { return c < 32 || c == 127; }))
    return {};
  return std::make_shared<const BreakpointSource>(std::move(original), copy.string(), line,
                                                  std::move(receipt), received);
}

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

auto failed(std::string_view reason, std::optional<MessageKey> localization = {}) -> ActionResult {
  return {ActionStatus::failed, std::string(reason), std::move(localization)};
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

// The joint a joint-space command for the axis addresses: its first; on a gantry axis the others
// follow it (homing) or the command is refused (jogging).
auto joint_of(const MachineConfig& config, Axis axis) -> int {
  return config.axis_joints.at(static_cast<std::size_t>(index_of(axis))).front();
}

// A joint jog moves one motor: on an axis with more than one that racks it.
auto joint_jog_refusal(const MachineConfig& config, Axis axis) -> std::optional<std::string> {
  const auto motors = config.axis_joints.at(static_cast<std::size_t>(index_of(axis))).size();
  if (motors < 2)
    return {};
  return std::format("Home the machine before jogging {}: {} motors drive it.",
                     "XYZ"[index_of(axis)], motors == 2 ? "two" : std::to_string(motors));
}

struct ProbeLine {
  std::size_t axis;
  double sign;
  std::array<double, 3> forward; // Signed direction in machine axes.

  auto distance(const std::array<double, 3>& from, const std::array<double, 3>& to) const
      -> double {
    return (to[0] - from[0]) * forward[0] + (to[1] - from[1]) * forward[1] +
           (to[2] - from[2]) * forward[2];
  }

  auto advance(std::array<double, 3> point, double travel) const -> std::array<double, 3> {
    for (std::size_t i = 0; i < point.size(); ++i)
      point[i] += travel * forward[i];
    return point;
  }

  auto word() const -> char { return "XYZ"[axis]; }
};

auto probe_line(ProbeDirection direction, double rotation_xy) -> ProbeLine {
  const double angle = rotation_xy * std::numbers::pi / 180;
  const double sign =
      direction == ProbeDirection::x_plus || direction == ProbeDirection::y_plus ? 1.0 : -1.0;
  switch (direction) {
  case ProbeDirection::x_minus:
  case ProbeDirection::x_plus:
    return {0, sign, {sign * std::cos(angle), sign * std::sin(angle), 0}};
  case ProbeDirection::y_minus:
  case ProbeDirection::y_plus:
    return {1, sign, {-sign * std::sin(angle), sign * std::cos(angle), 0}};
  case ProbeDirection::z_top:
    return {2, -1, {0, 0, -1}};
  }
  std::unreachable();
}

} // namespace

struct NmlPort::Impl {
  struct WorkFrame {
    int wcs;
    std::array<double, 3> g5x;
    std::array<double, 3> g92;
    // Compared by the defaulted equality operator.
    // cppcheck-suppress unusedStructMember
    double rotation;
    friend auto operator==(const WorkFrame&, const WorkFrame&) -> bool = default;
  };
  struct Calibration {
    std::uint64_t datum;
    std::array<double, 3> plane;
    double clearance_z;
    std::map<int, double> measured; // tool -> table length its touch off wrote, table units
    int teacher; // the tool whose length defines the plane
  };

  std::optional<Calibration> setter_calibration;
  struct EdgeMeasurement {
    std::uint64_t id;
    WorkFrame frame;
    SetterEpoch epoch;
    std::array<double, 3> tool_offset;
    double degrees;
  };
  std::optional<EdgeMeasurement> edge_measurement;
  std::uint64_t next_edge_measurement{};

  MachineConfig config;
  HalShared* hal{};
  NmlLoads* loads{};
  Gated<RCS_CMD_CHANNEL> command;
  StatusChannel status;
  Gated<NML> errors;
  // Of the action in progress: asked before every write. Null for an action that only stops
  // something. Set by begin() and used only until that action returns.
  const Wanted* wanted{};
  // The types of the commands this action has written so far.
  std::vector<long> written_types;
  // Every write, for other threads to read.
  std::mutex trace_mutex;
  std::vector<Written> trace;
  std::vector<ControllerMessage> messages;
  // A jog-stop text read in the present collect(), not yet attributed.
  struct JogStopText {
    std::string text;
    Clock::time_point read_at;
  };
  std::vector<JogStopText> jog_stop_texts;
  // The watchdog trip the operator was last told about.
  std::optional<Clock::time_point> told_watchdog;
  std::optional<Clock::time_point> told_contact;
  std::filesystem::path spindle_tool_file;
  std::filesystem::path breakpoint_copy;
  std::string legacy_breakpoint_copy;
  std::filesystem::path breakpoint_record;
  std::shared_ptr<const BreakpointSource> breakpoint;
  Clock::time_point breakpoint_retry{};
  struct SpindleHold {
    std::string file;
    int line;
    int tool;
    SpindleDirection direction;
    double rpm;
    Vec3 base;
    [[nodiscard]] auto matches(const NmlStatus& reading) const -> bool {
      const auto& c = reading.controller;
      return c.link == Link::live && c.machine == MachineState::on &&
             c.mode == TaskMode::automatic && c.interp == InterpState::paused && reading.paused &&
             reading.program_file == file && reading.current_line == line &&
             c.tool_in_spindle == tool && !c.requested_tool;
    }
    [[nodiscard]] auto at_base(const NmlStatus& reading) const -> bool {
      const auto& c = reading.controller;
      return matches(reading) && reading.paused_z_motion_ready && c.paused_z &&
             !c.paused_z->displaced && reading.spindle_actual_rpm && reading.spindle_at_speed &&
             std::ranges::equal(c.axes, base, [](const AxisState& axis, double expected) {
               return std::abs(axis.machine - expected) <= 0.01;
             });
    }
    [[nodiscard]] auto at_speed(const NmlStatus& reading) const -> bool {
      return at_base(reading) && reading.spindle_direction == direction &&
             std::abs(reading.spindle_rpm - rpm) < 0.01 && reading.spindle_at_speed.value_or(false) &&
             reading.spindle_actual_rpm.value_or(0) >= 1.0;
    }
  };
  std::filesystem::path spindle_hold_marker;
  std::optional<SpindleHold> spindle_hold;
  Clock::time_point spindle_hold_retry{};

  int serial{};
  std::chrono::steady_clock::time_point synch_retry{};
  std::optional<int> startup_tool;
  // The operator's modal state a probe cycle changes (distance mode, feed, M50, M52), kept from
  // its start until the cycle restores it. A cycle that stops early drops the machine's enable:
  // idle_maintenance restores the state once the machine is on and idle again.
  struct ProbeModal {
    bool incremental;
    double feed_mm_min;
    bool feed_override;
    bool adaptive_feed;
  };
  std::optional<ProbeModal> probe_modal;
  std::optional<int> observed_tool;
  // The joint or axis and the jjogmode of the jog stop that is the last command written, until
  // task echoes it.
  std::optional<std::pair<int, int>> unanswered_stop;
  // It has answered no: nothing more is written for this action.
  bool cancelled{};
  // A command has been written; `serial` is its number.
  bool written{};
  bool unresolved_legacy_offset{};
  bool breakpoint_reported{};
  bool breakpoint_recovered{};
  // Space of the last jog this port may have submitted for each axis.
  std::array<std::optional<JogSpace>, 3> jog_spaces{};

  explicit Impl(MachineConfig machine)
      : config(std::move(machine)),
        command(emcFormat, "emcCommand", "xemc", config.nml_file.c_str()), status(config.nml_file),
        errors(emcFormat, "emcError", "xemc", config.nml_file.c_str()) {
    const auto runtime = std::filesystem::path(
        config.runtime_directory.empty() ? config.directory : config.runtime_directory);
    if (config.remember_spindle_tool && !config.tool_table.empty()) {
      const auto tools = config.runtime_directory.empty()
                            ? std::filesystem::path(config.tool_table).parent_path() : runtime;
      spindle_tool_file = tools / "coffeecnc-spindle-tool.txt";
      startup_tool = load_spindle_tool(spindle_tool_file);
    }
    breakpoint_copy = runtime / ".coffeecnc-breakpoint.ngc";
    legacy_breakpoint_copy =
        (std::filesystem::path(config.directory) / ".coffeecnc-breakpoint.ngc").string();
    breakpoint_record = runtime / ".coffeecnc-breakpoint.txt";
    breakpoint = load_breakpoint(breakpoint_record, breakpoint_copy);
    breakpoint_recovered = bool(breakpoint);
    spindle_hold_marker = runtime / ".coffeecnc-spindle-hold";
    // An interrupted older build may have left G52 active or suspended.
    // Require recovery with that build before starting a program again.
    const auto legacy_marker = std::filesystem::path(config.directory) / "coffeecnc-air-cut.txt";
    std::error_code error;
    const auto marker = std::filesystem::symlink_status(legacy_marker, error);
    unresolved_legacy_offset = marker.type() != std::filesystem::file_type::not_found;
  }

  void spindle_recovery() {
    spindle_hold.reset();
    hal->paused_spindle.store(PausedSpindle::recover);
  }

  void finish_spindle_hold() {
    std::error_code error;
    std::filesystem::remove(spindle_hold_marker, error);
    if (error) {
      spindle_recovery();
      return;
    }
    spindle_hold.reset();
    hal->paused_spindle.store(PausedSpindle::none);
  }

  void discard_breakpoint() {
    // Keep the last mapping published while the publisher's mailbox catches up to the reopen.
    breakpoint.reset();
    breakpoint_recovered = false;
    breakpoint_reported = false;
    std::error_code unused;
    std::filesystem::remove(breakpoint_record, unused);
    std::filesystem::remove(breakpoint_copy, unused);
  }

  [[nodiscard]] auto save_breakpoint() const -> bool {
    return write_sidecar(breakpoint_record,
                         std::format("{} {} {}\n{}\n", breakpoint->line, breakpoint->receipt,
                                     static_cast<int>(breakpoint->marker_seen.load()),
                                     breakpoint->original));
  }

  [[nodiscard]] auto stat() -> const EMC_STAT* {
    const auto* sample = status.stat();
    if (sample != nullptr && status.link() == Link::live && !spindle_tool_file.empty())
      remember_spindle_tool(sample->io.tool.toolInSpindle);
    return sample;
  }

  void remember_spindle_tool(int tool) {
    if (tool < 0 || observed_tool == tool)
      return;
    const bool initial_empty = !observed_tool && tool == 0 && startup_tool.value_or(0) > 0;
    observed_tool = tool;
    if (initial_empty)
      return;
    startup_tool.reset();
    if (!write_sidecar(spindle_tool_file, std::format("{}\n", tool)))
      messages.push_back({true, "Could not save the spindle tool for the next startup.",
                          MessageKey{"backend.nmlPort.spindleToolNotSaved"}});
  }

  void add(bool error, std::span<const char> field) {
    auto text = text_of(field);
    if (!error && breakpoint && text.starts_with(breakpoint_message) &&
        std::string_view(text).substr(breakpoint_message.size()) == breakpoint->receipt) {
      if (!breakpoint->marker_seen.exchange(true) && !save_breakpoint())
        messages.push_back({true, "Could not save the breakpoint pause for app recovery.",
                            MessageKey{"backend.nmlPort.breakpointPauseNotSaved"}});
      return;
    }
    if (!text.empty())
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
  // log. Watchdog and probe-contact stops get an operator message. Other texts pass through.
  // None of them is ever an action's reason unless it is passed on.
  void attribute_jog_stops() {
    if (jog_stop_texts.empty())
      return;
    const auto last_read = jog_stop_texts.back().read_at;
    auto history = HalShared::JogStopHistory{};
    if (hal != nullptr) {
      // Motion can publish the stop before the control loop samples the contact input.
      history = hal->jog_stop_history();
      if (!history.read_at || *history.read_at < last_read)
        static_cast<void>(until(fresh_pins_time, [&]() -> bool {
          history = hal->jog_stop_history();
          return history.read_at && *history.read_at >= last_read;
        }));
    }
    // One log line per cause and text in this pass, with the count.
    struct Logged {
      JogStopCause cause;
      std::string text;
      int count;
    };
    std::vector<Logged> logged;
    for (auto& seen : jog_stop_texts) {
      const auto cause = classify_jog_stop(seen.text, seen.read_at, history.own_level_high,
                                           history.watchdog_tripped, history.contact_edge);
      if (cause == JogStopCause::unattributed) {
        messages.push_back({true, std::move(seen.text)});
        continue;
      }
      if (cause == JogStopCause::watchdog &&
          (!told_watchdog || *history.watchdog_tripped - *told_watchdog > jog_stop_window)) {
        told_watchdog = history.watchdog_tripped;
        messages.push_back(
            {true, std::string(watchdog_stop_text), MessageKey{"backend.jogStop.watchdog"}});
      }
      if (cause == JogStopCause::probe_contact && told_contact != history.contact_edge) {
        told_contact = history.contact_edge;
        messages.push_back({false, std::string(probe_contact_stop_text),
                            MessageKey{"backend.jogStop.probeContact"}});
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
      const auto cause = line.cause == JogStopCause::own ? "own"
                        : line.cause == JogStopCause::watchdog ? "watchdog" : "probe_contact";
      std::cerr << std::format("coffeecnc: jog_stop_text cause={} count={} own_level_ms_before={} "
                               "watchdog_tripped_ms_before={} contact_ms_before={} text=\"{}\"\n",
                               cause, line.count, since(history.own_level_high, last_read),
                               since(history.watchdog_tripped, last_read),
                               since(history.contact_edge, last_read), line.text)
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
    // Counted before the write: a status that shows the block executing may come before
    // the write returns. Counted also when the write is withdrawn or fails: more is harmless.
    if (loads != nullptr) {
      if (runs_gcode(message.type))
        ++loads->gcode_writes;
      loads->serials.begin();
    }
    const int wrote = command.call(
        [&](RCS_CMD_CHANNEL& channel) -> int { return still() ? channel.write(&message) : 0; });
    if (loads != nullptr)
      loads->serials.wrote(message.serial_number);
    if (cancelled)
      return withdraw(message.type);
    if (wrote != 0)
      return failed("Cannot write to LinuxCNC's command channel.",
                    MessageKey{"backend.nmlPort.cannotWrite"});
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

  // EMC_TASK_PLAN_SYNCH, and the wait until task has done it: task saves its parameter file
  // before it reports the synch done. The wait gives way to every request and stop the session
  // queues meanwhile (still() turns false): those never wait behind a synch. Within an action
  // (begin()).
  enum class Synch : std::uint8_t { done, refused, given_way, unwritten };
  [[nodiscard]] auto plan_synch() -> Synch {
    EMC_TASK_PLAN_SYNCH message;
    if (!accepted(write(message)))
      return still() ? Synch::unwritten : Synch::given_way;
    int rcs = RCS_EXEC;
    const bool ended = until(completion_timeout, [&]() -> bool {
      collect();
      if (!still())
        return true;
      const EMC_STAT* s = stat();
      if (s == nullptr || s->echo_serial_number != serial)
        return false;
      rcs = s->status;
      return rcs != RCS_EXEC;
    });
    if (!still())
      return Synch::given_way;
    return ended && rcs == RCS_DONE ? Synch::done : Synch::refused;
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
    // On disk before LinuxCNC reads it: a power cut leaves the old table or the new one.
    const std::string temporary = config.tool_table + ".coffeecnc";
    if (!write_synced(temporary, table))
      return failed(std::format("Cannot write tool table {}.", temporary));
    if (!still()) {
      std::error_code error;
      std::filesystem::remove(temporary, error);
      return withdrawn();
    }
    if (!rename_synced(temporary, config.tool_table))
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
  if (hal && config.paused_z) {
    std::error_code error;
    if (std::filesystem::exists(impl->spindle_hold_marker, error) || error)
      impl->spindle_recovery();
    else
      hal->paused_spindle.store(PausedSpindle::none);
  }
  impl->loads = loads;
  if (impl->command.call([](RCS_CMD_CHANNEL& channel) -> int { return channel.valid(); }) == 0 ||
      !impl->status.valid() ||
      impl->errors.call([](NML& channel) -> int { return channel.valid(); }) == 0)
    return std::unexpected(
        std::format("Cannot open LinuxCNC's NML channels from {}.", impl->config.nml_file));
  // The first reading of the write counter: the next can tell whether task is running.
  static_cast<void>(until(echo_timeout, [&]() -> bool { return impl->stat() != nullptr; }));
  if (const auto* raw = impl->stat();
      raw && impl->legacy_breakpoint_copy != impl->breakpoint_copy.string() &&
      to_status(*raw, config, hal, impl->status.link()).program_file ==
          impl->legacy_breakpoint_copy) {
    // An application-only restart can leave LinuxCNC paused in the copy made before migration.
    // Reuse its mapping only when the migrated copy still matches the exact execution file.
    constexpr auto max_copy = (std::size_t{16} << 20) + 256;
    const auto active = read_text_file(impl->legacy_breakpoint_copy, max_copy);
    const auto imported = read_text_file(impl->breakpoint_copy.string(), max_copy);
    if (impl->breakpoint && active && active == imported) {
      const auto& record = *impl->breakpoint;
      impl->breakpoint = std::make_shared<const BreakpointSource>(
          record.original, impl->legacy_breakpoint_copy, record.line, record.receipt,
          record.marker_seen.load());
    } else {
      impl->breakpoint.reset();
      impl->breakpoint_recovered = false;
    }
  }
  if (loads)
    loads->breakpoint.store(impl->breakpoint);
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
          EMC_TASK_ABORT_TYPE,
          EMC_TASK_PLAN_SYNCH_TYPE};
}

NmlGateHold::NmlGateHold() : lock_(nml_gate()) {}
NmlGateHold::~NmlGateHold() = default;

auto NmlPort::status() -> NmlStatus {
  const EMC_STAT* s = impl_->stat();
  auto result = s ? to_status(*s, impl_->config, impl_->hal, impl_->status.link()) : NmlStatus{};
  result.controller.unresolved_legacy_offset = impl_->unresolved_legacy_offset;
  result.controller.breakpoint_program = result.program_file == impl_->breakpoint_copy.string() ||
                                         result.program_file == impl_->legacy_breakpoint_copy;
  map_breakpoint(result, impl_->breakpoint.get());
  return result;
}

auto NmlPort::take_messages() -> std::vector<ControllerMessage> {
  // Called every control cycle: also the regular reading the heartbeat needs.
  static_cast<void>(impl_->stat());
  impl_->collect();
  return std::exchange(impl_->messages, {});
}

void NmlPort::synch_parameters(const Wanted& wanted) {
  auto* loads = impl_->loads;
  if (loads == nullptr)
    return;
  const auto done = loads->synched.load();
  const std::uint64_t completed = done ? done->count : 0;
  if (loads->synch_wanted.load() <= completed || Clock::now() < impl_->synch_retry)
    return;
  // Taken before the status is judged: what is counted after this is activity after the synch.
  const SynchPoint point{completed + 1, loads->activity.load(), loads->gcode_writes.load()};
  const auto now = status();
  const auto& state = now.controller;
  // Task refuses the synch, with an operator error, while the machine is on in auto mode.
  if (state.link != Link::live || state.interp != InterpState::idle || now.queued_mdi != 0 ||
      now.task_executing ||
      (state.machine == MachineState::on && state.mode == TaskMode::automatic) || !wanted())
    return;
  static_cast<void>(impl_->begin(&wanted));
  const Impl::Scope scope{*impl_};
  // A synch that gave way is not counted and is asked for again.
  const auto synch = impl_->plan_synch();
  if (synch == Impl::Synch::given_way)
    return;
  if (synch == Impl::Synch::done) {
    auto done_point = point;
    done_point.file = file_stamp(impl_->config.parameter_file);
    loads->synched.store(std::make_shared<const SynchPoint>(done_point));
  }
  else
    impl_->synch_retry = Clock::now() + synch_retry_period;
}

void NmlPort::restore_probe_modal(const Wanted& wanted) {
  if (!impl_->probe_modal)
    return;
  const auto ready = [&](const NmlStatus& status) {
    const auto& state = status.controller;
    return state.link == Link::live && state.machine == MachineState::on &&
           state.mode != TaskMode::automatic && state.interp == InterpState::idle &&
           state.in_position && !status.paused && !status.task_executing &&
           status.queued_mdi == 0 && !state.requested_tool;
  };
  if (impl_->hal != nullptr) {
    // A probe cycle in progress restores the state itself.
    if (const auto pins = impl_->hal->view(Clock::now()); pins && pins->probe_cycle_active)
      return;
  }
  const auto before = status();
  if (!ready(before) || !wanted())
    return;
  const auto modal = *std::exchange(impl_->probe_modal, std::nullopt);
  const auto not_restored = [&] {
    impl_->messages.push_back(
        {true,
         "Could not restore the distance mode and feed controls after the stopped probe cycle. "
         "Check G90 or G91 and the feed override.",
         MessageKey{"backend.nmlPort.probeModalNotRestored"}});
  };
  const auto mode = before.controller.mode;
  if (mode != TaskMode::mdi && !accepted(set_task_mode(TaskMode::mdi, wanted))) {
    not_restored();
    return;
  }
  const auto settled = [&] {
    return until(settle_timeout, [&] {
      impl_->collect();
      const auto current = status();
      return current.controller.link == Link::live && current.controller.interp == InterpState::idle &&
             current.queued_mdi == 0 && !current.task_executing;
    });
  };
  const double scale = before.controller.program_units == LengthUnit::inch ? 25.4 : 1.0;
  std::vector<std::string> blocks{std::format("{} G80 F{:.8f}", modal.incremental ? "G91" : "G90",
                                              modal.feed_mm_min / scale)};
  if (modal.feed_override)
    blocks.emplace_back("M50 P1");
  if (modal.adaptive_feed)
    blocks.emplace_back("M52 P1");
  bool sent = true;
  for (const auto& block : blocks)
    sent = sent && accepted(mdi(block, wanted)) && settled();
  const auto after = status();
  const bool verified =
      sent && std::ranges::contains(after.active_codes, modal.incremental ? "G91" : "G90") &&
      (!modal.feed_override || after.feed_override_enabled) &&
      (!modal.adaptive_feed || after.adaptive_feed_enabled);
  if (mode != TaskMode::mdi && wanted() && after.controller.machine == MachineState::on)
    static_cast<void>(set_task_mode(mode, wanted));
  if (!verified) {
    not_restored();
    return;
  }
  impl_->messages.push_back(
      {false, "Restored the distance mode and feed controls the stopped probe cycle changed.",
       MessageKey{"backend.nmlPort.probeModalRestored"}});
}

void NmlPort::idle_maintenance(const Wanted& wanted) {
  synch_parameters(wanted);
  restore_probe_modal(wanted);
  if (impl_->hal && impl_->hal->paused_spindle.load() != PausedSpindle::none)
    recover_paused_spindle(wanted);
  if (impl_->breakpoint || status().controller.breakpoint_program) {
    finish_breakpoint(wanted);
    if (impl_->breakpoint)
      return;
    // An orphaned wrapper stays blocked for program commands until the operator opens
    // a new file, but does not prevent restoring the already recorded spindle tool.
  }
  if (impl_->startup_tool.value_or(0) <= 0)
    return;
  const auto ready = [&](const NmlStatus& status) {
    const auto& state = status.controller;
    return state.link == Link::live && state.machine == MachineState::on &&
           state.mode != TaskMode::automatic && state.interp == InterpState::idle &&
           state.in_position && state.tool_in_spindle == 0 && !state.requested_tool &&
           !state.spindle_active && !status.paused && !status.task_executing && status.queued_mdi == 0 &&
           !state.any_enabled_axis([&](const auto& axis) {
             return axis.homing || axis.on_hard_limit ||
                    (impl_->config.limits.homing_required && !axis.homed);
           });
  };
  const auto before = status();
  const auto& startup_tool = impl_->startup_tool;
  if (!ready(before) || !wanted() || !startup_tool)
    return;
  const int tool = *startup_tool;
  const auto table = impl_->read_tool_table();
  const auto length = table ? tool_z(*table, tool) : std::nullopt;
  if (!length) {
    impl_->startup_tool.reset();
    impl_->messages.push_back(
        {true, std::format("Could not restore T{} from the tool table.", tool),
         MessageKey{"backend.nmlPort.restoreFromTable", {{"tool", std::to_string(tool)}}}});
    return;
  }
  const auto mode = before.controller.mode;
  if (mode != TaskMode::mdi) {
    const auto changed = set_task_mode(TaskMode::mdi, wanted);
    if (!accepted(changed)) {
      if (wanted()) {
        impl_->startup_tool.reset();
        impl_->messages.push_back(
            {true, std::format("Could not enter MDI to restore T{}.", tool),
             MessageKey{"backend.nmlPort.restoreNoMdi", {{"tool", std::to_string(tool)}}}});
      }
      return;
    }
  }
  if (!ready(status()) || !wanted())
    return;

  impl_->startup_tool.reset();
  const auto restore = mdi(std::format("M61 Q{} G43 H{}", tool, tool), wanted);
  const bool verified = accepted(restore) && until(settle_timeout, [&] {
    impl_->collect();
    const auto current = status();
    return current.controller.link == Link::live && current.controller.tool_in_spindle == tool &&
           current.controller.interp == InterpState::idle &&
           std::ranges::contains(current.active_codes, "G43") &&
           std::abs(current.tool_offset[2] - *length * impl_->config.millimetres_per_unit) < 0.001;
  });
  if (mode != TaskMode::mdi && wanted() && status().controller.machine == MachineState::on)
    static_cast<void>(set_task_mode(mode, wanted));
  impl_->messages.push_back({!verified, verified
      ? std::format("T{} restored in the spindle.", tool)
      : std::format("Could not verify restoring T{} in the spindle. Check the active tool.", tool)});
}

auto NmlPort::set_machine_state(MachineState state, const Wanted& wanted) -> ActionResult {
  // Asserting E-stop is never withdrawn. Protocol off can also reset E-stop.
  const auto from = impl_->begin(state == MachineState::estop ? nullptr : &wanted);
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
    // A step that timed out took nothing out of it: its reason is this port's own words, and the
    // result below says what became of the request.
    if (sent.status == ActionStatus::failed && !sent.reason.empty())
      impl_->messages.push_back({true, sent.reason, sent.localization});
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
  if (!axis && !impl_->config.home_all_configured) {
    if (std::ranges::any_of(impl_->config.rotary_joints,
                            [](const auto& joints) { return !joints.empty(); }))
      return failed("Configure [JOINT_n] HOME_SEQUENCE for every joint to use Home all.",
                    MessageKey{"backend.nmlPort.homeAllSequence"});
    return failed("Homing all axes at once is not configured ([JOINT_n] HOME_SEQUENCE): home "
                  "each axis.",
                  MessageKey{"backend.nmlPort.homeAllNotConfigured"});
  }
  // A request for one joint of a synchronized group (a shared negative HOME_SEQUENCE) homes the
  // whole group, each joint to its own switch (seen on 2.9.4). Without one it homes that joint.
  if (axis && !impl_->config.home_axis_configured.at(static_cast<std::size_t>(index_of(*axis)))) {
    const auto& joints = impl_->config.axis_joints.at(static_cast<std::size_t>(index_of(*axis)));
    std::string sections;
    for (const int joint : joints)
      sections += std::format("{}[JOINT_{}]", sections.empty() ? "" : " and ", joint);
    return failed(std::format(
        "Homing {} alone would home one of its motors and rack it: give {} the same negative "
        "HOME_SEQUENCE (for example -1), or use Home all.",
        "XYZ"[index_of(*axis)], sections));
  }
  const auto from = impl_->begin(&wanted);
  const Impl::Scope scope{*impl_};
  if (!impl_->still())
    return withdrawn();
  // Task homes joints, and only in joint mode; a homed machine is in world mode.
  if (auto joint_mode = impl_->teleop(false, from); !accepted(joint_mode))
    return joint_mode;
  EMC_JOINT_HOME message;
  // -1 homes all joints in their configured sequence.
  message.joint = axis ? joint_of(impl_->config, *axis) : -1;
  return impl_->pass(message, from);
}

auto NmlPort::start_jog(Axis axis, int direction, double velocity, JogSpace space,
                        const Wanted& wanted) -> ActionResult {
  if (auto refusal = joint_jog_refusal(impl_->config, axis); space == JogSpace::joint && refusal)
    return failed(*refusal);
  const auto from = impl_->begin(&wanted);
  const Impl::Scope scope{*impl_};
  if (!impl_->still())
    return withdrawn();
  if (auto mode = impl_->teleop(space == JogSpace::axis, from); !accepted(mode))
    return mode;
  impl_->jog_spaces.at(static_cast<std::size_t>(index_of(axis))) = space;
  EMC_JOG_CONT message;
  // World mode jogs the axis, joint mode its joint.
  message.joint_or_axis = space == JogSpace::axis ? index_of(axis) : joint_of(impl_->config, axis);
  message.vel = impl_->units_per_second(direction < 0 ? -velocity : velocity);
  message.jjogmode = space == JogSpace::joint ? 1 : 0;
  return impl_->pass(message, from);
}

auto NmlPort::stop_jog(Axis axis) -> ActionResult {
  const auto from = impl_->begin(nullptr);
  const Impl::Scope scope{*impl_};
  const EMC_STAT* s = impl_->stat();
  const bool answered = impl_->answered(s);
  auto& previous = impl_->jog_spaces.at(static_cast<std::size_t>(index_of(axis)));
  const bool live = s != nullptr && impl_->status.link() == Link::live;
  // After task has taken up our commands, a live mode discloses where motion can be jogging.
  // If status is missing or an earlier command could still be queued, retain its known space.
  if (live && answered &&
      (s->task.state != EMC_TASK_STATE_ON || s->task.mode != EMC_TASK_MODE_MANUAL)) {
    previous.reset();
    return {};
  }
  const auto reported = s != nullptr && s->motion.traj.mode == EMC_TRAJ_MODE_TELEOP
                            ? JogSpace::axis : JogSpace::joint;
  const auto space = live && answered ? reported : previous.value_or(reported);
  EMC_JOG_STOP message;
  message.jjogmode = space == JogSpace::axis ? 0 : 1;
  message.joint_or_axis = space == JogSpace::axis ? index_of(axis) : joint_of(impl_->config, axis);
  // Retry the stop already in the queue without filling it with duplicate commands.
  const std::pair stop{message.joint_or_axis, message.jjogmode};
  if (answered || impl_->unanswered_stop != stop) {
    if (auto wrote = impl_->write(message); !accepted(wrote))
      return wrote;
    impl_->unanswered_stop = stop;
  }
  auto stopped = impl_->finished(from);
  if (accepted(stopped))
    previous.reset();
  return stopped;
}

auto NmlPort::jog_increment(Axis axis, double distance, double velocity, JogSpace space,
                            const Wanted& wanted) -> ActionResult {
  if (auto refusal = joint_jog_refusal(impl_->config, axis); space == JogSpace::joint && refusal)
    return failed(*refusal);
  const auto from = impl_->begin(&wanted);
  const Impl::Scope scope{*impl_};
  if (!impl_->still())
    return withdrawn();
  if (auto mode = impl_->teleop(space == JogSpace::axis, from); !accepted(mode))
    return mode;
  impl_->jog_spaces.at(static_cast<std::size_t>(index_of(axis))) = space;
  EMC_JOG_INCR message;
  message.joint_or_axis = space == JogSpace::axis ? index_of(axis) : joint_of(impl_->config, axis);
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
    return failed("The MDI block is too long for LinuxCNC.",
                  MessageKey{"backend.nmlPort.mdiTooLong"});
  auto started = impl_->start(message, from);
  if (accepted(started) && impl_->loads != nullptr)
    ++impl_->loads->mdi_blocks;
  return started;
}

auto NmlPort::probe_fixed_setter(const FixedSetterMove& move, const Wanted& wanted)
    -> std::expected<FixedSetterContact, std::string> {
  using Contact = std::expected<FixedSetterContact, std::string>;
  // Position checks: a hobby mill with encoder feedback, backlash and step resolution.
  constexpr double tolerance_mm = 0.1;
  const auto limits = impl_->config.fixed_setter;
  auto* hal = impl_->hal;
  if (!limits || hal == nullptr)
    return std::unexpected("The fixed setter is not configured.");
  const bool material = move.material.has_value();
  const double max_travel = material ? limits->material_travel_mm : limits->travel_mm;
  if (move.tool <= 0 || !std::isfinite(move.travel_mm) || move.travel_mm <= 0 ||
      move.travel_mm > max_travel || !std::isfinite(move.feed_mm_min) || move.feed_mm_min <= 0 ||
      move.feed_mm_min > limits->feed_mm_min ||
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
           !c.any_enabled_axis([](const auto& axis) { return axis.homing || axis.on_hard_limit; });
  };
  const auto near = [](double a, double b) {
    return std::isfinite(a) && std::abs(a - b) <= tolerance_mm;
  };
  const auto& c = before.controller;
  const auto line = probe_line(move.material.value_or(ProbeDirection::z_top), before.rotation_xy);
  const bool rotated = material && line.axis != 2 && before.rotation_xy != 0;
  auto search_start = move.approach.value_or(move.start);
  if (rotated && !move.approach) {
    // Hold the commanded transverse position, as the trajectory planner will.
    search_start[0] = before.commanded_position[0];
    search_start[1] = before.commanded_position[1];
  }
  const auto endpoint = line.advance(search_start, move.travel_mm);
  const auto coordinate = [&](const auto& point) { return line.distance(search_start, point); };
  const auto in_travel = [&](const std::array<double, 3>& point) {
    for (std::size_t i = 0; i < point.size(); ++i) {
      const auto& travel = impl_->config.travel[i];
      if (!std::isfinite(point[i]) || point[i] < travel.min || point[i] > travel.max)
        return false;
    }
    return true;
  };
  // Homing is not required: the probe runs in the session's machine frame.
  if (!normal_mode(before) || !raw->motion.traj.enabled || raw->motion.external_offsets_applied ||
      c.interp != InterpState::idle || !c.in_position || before.queued_mdi != 0 ||
      c.tool_in_spindle != move.tool)
    return std::unexpected("The probe requires a stopped spindle, stopped axes and idle MDI.");
  if (!std::ranges::contains(before.active_codes, "G40"))
    return std::unexpected("The probe requires cutter compensation off (G40).");
  if (before.feed_mode != FeedMode::per_minute || !std::isfinite(before.programmed_feed) ||
      !std::isfinite(before.rotation_xy))
    return std::unexpected("The probe requires feed per minute (G94).");
  for (std::size_t axis = 0; axis < move.start.size(); ++axis)
    if (!near(c.axes[axis].machine, move.start[axis]))
      return std::unexpected("The machine moved after the probe was confirmed.");
  // The height the approach traverses at: never below the start or the search start.
  const double lift_z = move.clearance_z.value_or(search_start[2]);
  const bool descends = move.approach && move.clearance_z && search_start[2] < lift_z;
  if (!in_travel(search_start) || !in_travel(endpoint) ||
      (!move.clearance_z && search_start[2] < move.start[2]) ||
      (move.clearance_z && (!move.approach || !std::isfinite(lift_z) || lift_z < move.start[2] ||
                            lift_z < search_start[2] ||
                            !in_travel({move.start[0], move.start[1], lift_z}) ||
                            !in_travel({search_start[0], search_start[1], lift_z}))))
    return std::unexpected("The probe move is outside the configured travel.");

  // What the cycle restores at its end, or idle_maintenance after a stop: the state before this
  // cycle, or before an earlier one that stopped and is not restored yet.
  const auto modal = impl_->probe_modal.value_or(
      Impl::ProbeModal{std::ranges::contains(before.active_codes, "G91"), before.programmed_feed,
                       raw->motion.traj.feed_override_enabled,
                       raw->motion.traj.adaptive_feed_enabled});
  const bool restore_feed_override = modal.feed_override;
  const bool restore_adaptive = modal.adaptive_feed;
  const bool restore_incremental = modal.incremental;
  const double program_scale = c.program_units == LengthUnit::inch ? 25.4 : 1.0;
  const double machine_scale = impl_->config.millimetres_per_unit;
  auto line_origin = move.approach ? move.start : search_start;
  const auto on_line = [&](const std::array<double, 3>& point,
                           const std::array<double, 3>& origin) {
    // A linear encoder can lag on reversal. The machine defines the acceptable deviation.
    const double off_line_allowance = impl_->config.motion_tolerances.probe_off_line_mm;
    const auto projected = line.advance(origin, line.distance(origin, point));
    for (std::size_t i = 0; i < point.size(); ++i)
      if (!(std::abs(point[i] - projected[i]) <= off_line_allowance))
        return false;
    return true;
  };
  const auto machine_position = [&](const auto& pose) -> std::array<double, 3> {
    return {pose.tran.x * machine_scale, pose.tran.y * machine_scale, pose.tran.z * machine_scale};
  };
  // After reversal the Z encoder can lag the command (0.125 mm on the G0759).
  // Require the commanded endpoint and bound the encoder difference along the probe line.
  const auto reached_backoff = [&](const auto& trajectory, double target) {
    return near(coordinate(machine_position(trajectory.position)), target) &&
           std::abs(coordinate(machine_position(trajectory.actualPosition)) - target) <=
               impl_->config.motion_tolerances.probe_backlash_mm;
  };
  const auto machine_move = [&](double position, const std::string& feed) {
    const auto target = line.advance(search_start, position);
    if (line.axis == 2)
      return std::format("G90 G53 G1 Z{:.8f}{}", target[2] / program_scale, feed);
    return std::format("G90 G53 G1 X{:.8f} Y{:.8f}{}", target[0] / program_scale,
                       target[1] / program_scale, feed);
  };
  bool positioning = false;
  bool traversing_xy = false;
  std::optional<std::array<double, 3>> vertical_at;
  bool lifting = false;
  const auto steady_context = [&](const EMC_STAT& current) -> bool {
    const auto s = to_status(current, impl_->config, hal, Link::live);
    return normal_mode(s) && std::ranges::contains(s.active_codes, "G40") &&
           current.motion.traj.enabled && !current.motion.traj.paused &&
           !current.motion.external_offsets_applied && !current.motion.on_soft_limit &&
           s.controller.tool_in_spindle == move.tool &&
           s.controller.program_units == c.program_units && s.feed_mode == before.feed_mode &&
           s.wcs == before.wcs && s.g5x == before.g5x && s.g92 == before.g92 &&
           s.tool_offset == before.tool_offset && s.rotation_xy == before.rotation_xy &&
           (traversing_xy ? near(s.controller.axes[2].machine, lift_z)
            : lifting     ? near(s.controller.axes[0].machine, move.start[0]) &&
                               near(s.controller.axes[1].machine, move.start[1])
            : vertical_at ? near(s.controller.axes[0].machine, (*vertical_at)[0]) &&
                                near(s.controller.axes[1].machine, (*vertical_at)[1])
                          : on_line({s.controller.axes[0].machine, s.controller.axes[1].machine,
                                     s.controller.axes[2].machine},
                                    line_origin));
  };

  // The machine's touch probe trips on its own input before the setter's plunger moves (G0759:
  // 2 mm of stylus deflection before the setter switched), so it touches off on its own input.
  const bool own_probe = impl_->config.touch_probe_tool == move.tool;
  if ((material && !own_probe) || !hal->begin_probe_cycle(Clock::now(), !own_probe))
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
    // The cycle's own reason comes first: dropping the permit makes LinuxCNC report "motion
    // stopped by enable input", which says nothing about why the cycle stopped.
    auto errors = impl_->take_errors(from);
    auto message = errors.empty() ? std::string(reason) : std::format("{} ({})", reason, errors);
    std::cerr << "coffeecnc: fixed setter interrupted: " << message << '\n';
    return std::unexpected(std::move(message));
  };
  std::string cycle_failure; // why live_cycle() last refused, for the operator
  const auto live_cycle = [&]() -> const EMC_STAT* {
    const auto* s = impl_->stat();
    cycle_failure = !impl_->still()                          ? "the request was withdrawn"
                    : s == nullptr || impl_->status.link() != Link::live ? "controller status lost"
                    : !steady_context(*s) ? "machine state or position left the probe line"
                    : !hal->renew_probe_cycle(Clock::now()) ? "probe lease could not be renewed"
                                                            : "";
    return cycle_failure.empty() ? s : nullptr;
  };
  const auto interlock = [&]() -> std::optional<HalView> {
    const auto view = hal->view(Clock::now());
    return view && view->probe_cycle_active && view->probe_setter_selected == !own_probe &&
                   view->probe_motion_permitted && view->probe_input
               ? view
               : std::nullopt;
  };
  const auto armed_at = Clock::now() + settle_timeout;
  while (live_cycle() != nullptr && !interlock() && Clock::now() < armed_at)
    std::this_thread::sleep_for(poll_interval);
  const auto selected = interlock();
  raw = live_cycle();
  if (raw == nullptr || !selected || selected->probe_input.value_or(true) || raw->motion.traj.probeval != 0 ||
      !near(line.distance(move.start, machine_position(raw->motion.traj.actualPosition)), 0))
    return interrupt("The fixed setter is tripped or its interlock is unavailable.");

  bool saw_probe = false;
  std::string block_failure; // why the last block() did not complete, for the operator
  const auto block = [&](std::string_view code, std::chrono::milliseconds timeout,
                         bool watch_probe = false, bool moves = false) -> bool {
    if (live_cycle() == nullptr) {
      block_failure = cycle_failure;
      return false;
    }
    if (!interlock()) {
      block_failure = "probe interlock lost";
      return false;
    }
    EMC_TASK_PLAN_EXECUTE command;
    if (!put(command.command, code) || !accepted(impl_->write(command))) {
      block_failure = "LinuxCNC did not take the block";
      return false;
    }
    if (impl_->loads != nullptr)
      ++impl_->loads->mdi_blocks;
    bool done = false;
    block_failure = "timed out";
    std::optional<Clock::time_point> done_since;
    static_cast<void>(until(timeout, [&]() {
      impl_->collect();
      const auto* s = live_cycle();
      const auto pins = interlock();
      if (s == nullptr || !pins || (positioning && *pins->probe_input) ||
          impl_->error_since(from) || s->echo_serial_number > impl_->serial) {
        block_failure = s == nullptr                           ? cycle_failure
                        : !pins                                ? "probe interlock lost"
                        : positioning && *pins->probe_input    ? "probe touched while positioning"
                        : s->echo_serial_number > impl_->serial ? "another command took over"
                                                                : "LinuxCNC reported an error";
        return true;
      }
      if (s->echo_serial_number != impl_->serial)
        return false;
      const auto& traj = s->motion.traj;
      if (moves && (traj.feed_override_enabled || traj.adaptive_feed_enabled)) {
        block_failure = "feed override was enabled";
        return true;
      }
      if (watch_probe && traj.probing && !traj.probe_tripped)
        saw_probe = true;
      // RCS_DONE with the axes still can come before motion has taken the move: a move is done
      // only once motion's queue is empty as well.
      const bool still = s->status == RCS_DONE && s->task.interpState == EMC_TASK_INTERP_IDLE &&
                         s->task.queuedMDIcommands == 0 && traj.inpos && traj.current_vel == 0 &&
                         (!moves || (traj.queue == 0 && traj.activeQueue == 0));
      // Task can report a move done before motion has started it: a move is done once that
      // state has held for 150 ms.
      if (!still)
        done_since.reset();
      else if (!done_since)
        done_since = Clock::now();
      done = still && (!moves || Clock::now() - *done_since >= std::chrono::milliseconds{150});
      if (s->status == RCS_ERROR)
        block_failure = "LinuxCNC rejected the move";
      return done || s->status == RCS_ERROR;
    }));
    return done;
  };

  // From here the cycle changes the operator's modal state: keep it for a restore.
  impl_->probe_modal = modal;
  if (!block("M50 P0", completion_timeout) || !block("M52 P0", completion_timeout))
    return interrupt(std::format("Could not prepare the probing feed ({}).", block_failure));
  raw = live_cycle();
  const auto probe_pin = interlock();
  if (raw == nullptr || !probe_pin || probe_pin->probe_input.value_or(true) || raw->motion.traj.probeval != 0 ||
      raw->motion.traj.feed_override_enabled || raw->motion.traj.adaptive_feed_enabled ||
      !near(line.distance(move.start, machine_position(raw->motion.traj.actualPosition)), 0) ||
      !near(line.distance(move.start, machine_position(raw->motion.traj.position)), 0))
    return interrupt("Probe input, position or feed controls changed before descent.");

  // Back-offs and the final retract move away from the setter: twice the search feed (turo).
  const auto feed_word = std::format(" F{:.8f}", 2 * move.feed_mm_min / program_scale);
  if (move.approach) {
    constexpr auto positioning_timeout = std::chrono::seconds{30};
    positioning = true;
    // Straight up: off the search line of an edge, so only X and Y are held.
    lifting = true;
    if (lift_z > move.start[2] &&
        !block(std::format("G90 G53 G0 Z{:.8f}", lift_z / program_scale), positioning_timeout,
               false, true))
      return interrupt("Could not raise Z to setter clearance.");
    raw = live_cycle();
    if (raw == nullptr || !near(raw->motion.traj.position.tran.z * machine_scale, lift_z) ||
        !(std::abs(raw->motion.traj.actualPosition.tran.z * machine_scale - lift_z) <=
          impl_->config.motion_tolerances.probe_backlash_mm))
      return interrupt(
          raw == nullptr
              ? std::format("The machine did not reach the clearance height ({}).", cycle_failure)
              : std::format("The machine did not reach the clearance height (commanded Z {:.3f}, "
                            "measured {:.3f}, wanted {:.3f} mm).",
                            raw->motion.traj.position.tran.z * machine_scale,
                            raw->motion.traj.actualPosition.tran.z * machine_scale, lift_z));
    traversing_xy = true;
    lifting = false;
    if (!block(std::format("G90 G53 G0 X{:.8f} Y{:.8f}", search_start[0] / program_scale,
                           search_start[1] / program_scale),
               positioning_timeout, false, true))
      return interrupt(std::format("Could not position XY for {} ({}).",
                                   material ? "the probe" : "the setter", block_failure));
    traversing_xy = false;
    if (descends) {
      // Feed down beside the work: a contact on the way stops the motion (probe permit).
      vertical_at = search_start;
      if (!block(std::format("G90 G53 G1 Z{:.8f}{}", search_start[2] / program_scale, feed_word),
                 positioning_timeout, false, true))
        return interrupt("Could not lower the probe to the search height; check that the way down "
                         "is clear.");
      vertical_at.reset();
    }
    line_origin = search_start;
    positioning = false;
    raw = live_cycle();
    const auto positioned = interlock();
    if (raw == nullptr || !positioned || positioned->probe_input.value_or(true) ||
        raw->motion.traj.probeval != 0 ||
        !(std::abs(raw->motion.traj.actualPosition.tran.z * machine_scale - search_start[2]) <=
          impl_->config.motion_tolerances.probe_backlash_mm) ||
        !near(raw->motion.traj.position.tran.z * machine_scale, search_start[2]) ||
        std::hypot(raw->motion.traj.actualPosition.tran.x * machine_scale - search_start[0],
                   raw->motion.traj.actualPosition.tran.y * machine_scale - search_start[1]) >
            limits->xy_tolerance_mm)
      return interrupt(material ? "The probe did not reach a clear search position."
                                : "The tool must be centered above a clear setter before probing.");
  }

  bool missed = false;
  const auto search = [&](double begin_at, double travel, double feed) -> Contact {
    saw_probe = false;
    const auto timeout = std::chrono::milliseconds{
        static_cast<int>((travel / feed * seconds_per_minute + 3) * 1000)};
    std::string code;
    if (rotated) {
      // Supply both coordinates in the rotated frame from one commanded machine target.
      const auto target = work_position(line.advance(search_start, begin_at + travel), before.g5x,
                                        before.g92, before.tool_offset, before.rotation_xy);
      code = std::format("G90 G38.3 X{:.8f} Y{:.8f} F{:.8f}", target[0] / program_scale,
                         target[1] / program_scale, feed / program_scale);
    } else {
      code = std::format("G91 {} {}{:.8f} F{:.8f}", material ? "G38.3" : "G38.2", line.word(),
                         line.sign * travel / program_scale, feed / program_scale);
    }
    if (!block(code, timeout, true, true))
      return interrupt(std::format("The probe search did not complete ({}, {}).", block_failure,
                                   code));
    const auto* sample = live_cycle();
    const auto at_contact = interlock();
    if (sample == nullptr || !at_contact || !saw_probe || sample->motion.traj.probing ||
        sample->echo_serial_number != impl_->serial || !sample->motion.traj.inpos ||
        sample->motion.traj.current_vel != 0)
      return interrupt("The fixed setter did not report a fresh contact.");
    const auto actual = machine_position(sample->motion.traj.actualPosition);
    const auto& trajectory = sample->motion.traj;
    if (material && !trajectory.probe_tripped && !*at_contact->probe_input &&
        trajectory.probeval == 0 && near(coordinate(actual), begin_at + travel)) {
      missed = true;
      return FixedSetterContact{actual, 0};
    }
    if (!*at_contact->probe_input || !trajectory.probe_tripped || trajectory.probeval == 0)
      return interrupt("The fixed setter did not report a fresh contact.");
    const auto position = machine_position(trajectory.probedPosition);
    const double travel_after_contact = line.distance(position, actual);
    if (!std::isfinite(travel_after_contact))
      return interrupt("The fixed setter did not report a settled probe position.");
    const double latched = coordinate(position);
    if (!on_line(position, search_start) || !std::isfinite(latched) || latched <= begin_at ||
        latched > begin_at + travel + tolerance_mm)
      return interrupt("The setter contact lies outside the confirmed search move.");
    return FixedSetterContact{position, std::max(0.0, travel_after_contact)};
  };
  auto contact = search(0, move.travel_mm, move.feed_mm_min);
  if (!contact)
    return contact;
  std::optional<double> unstable_spread;
  if (!missed && limits->latch_feed_mm_min > 0) {
    const auto search_stop = contact->stopping_distance;
    std::array<double, 3> latches{};
    for (double& latch : latches) {
      const double backoff = coordinate(contact->machine) - limits->latch_backoff_mm;
      const double retreat = material ? std::max(0.0, backoff) : backoff;
      if (!in_travel(line.advance(search_start, retreat)) ||
          !block(machine_move(retreat, feed_word), move_timeout, false, true))
        return interrupt("The fixed setter latch backoff did not complete.");
      const auto released_at_backoff = [&](const EMC_STAT& sample, const HalView& pins) {
        const auto& trajectory = sample.motion.traj;
        return !*pins.probe_input && trajectory.probeval == 0 && trajectory.inpos &&
               trajectory.current_vel == 0 && reached_backoff(trajectory, retreat);
      };
      static_cast<void>(until(std::chrono::milliseconds{2000}, [&]() {
        const auto* s = live_cycle();
        const auto pins = interlock();
        return s == nullptr || !pins || released_at_backoff(*s, *pins);
      }));
      raw = live_cycle();
      const auto released = interlock();
      if (raw == nullptr || !released || !released_at_backoff(*raw, *released)) {
        if (material && backoff < 0 && raw != nullptr && released && released->probe_input.value_or(true))
          return interrupt("The probe did not release at the confirmed start. Start farther from "
                           "the workpiece before trying again.");
        return interrupt("The setter did not release before the latch pass.");
      }

      const double latch_travel = std::min(2 * limits->latch_backoff_mm, move.travel_mm - retreat);
      contact = search(retreat, latch_travel, limits->latch_feed_mm_min);
      if (!contact)
        return contact;
      if (missed)
        break;
      latch = coordinate(contact->machine);
    }
    if (!missed) {
      const auto [low, high] = std::ranges::minmax(latches);
      constexpr double max_spread_mm = 0.01;
      if (high - low > max_spread_mm)
        unstable_spread = high - low;
      else {
        const double average = std::accumulate(latches.begin(), latches.end(), 0.0) /
                               static_cast<double>(latches.size());
        contact->machine = line.advance(contact->machine, average - coordinate(contact->machine));
      }
      contact->stopping_distance = search_stop;
    }
  }
  // A material probe always retracts along the search line to its confirmed start.
  const double retract = material ? 0.0
                         : move.retract_above_contact_mm > 0 && !unstable_spread
                             ? coordinate(contact->machine) - move.retract_above_contact_mm
                             : search_start[2] - move.retract_z.value_or(search_start[2]);
  raw = live_cycle();
  if (raw == nullptr || !in_travel(line.advance(search_start, retract)) ||
      !(retract < coordinate(machine_position(raw->motion.traj.actualPosition))) ||
      !(retract < coordinate(contact->machine)))
    return interrupt("The retract position is outside safe travel for this contact.");
  if (!block(machine_move(retract, feed_word), move_timeout, false, true))
    return interrupt("The fixed setter retract did not complete.");
  // The stylus or plunger can spring back after the retract move ends (the ruby probe did on
  // the mill after an edge probe): give it the same 2 s as the latch back-off.
  const auto released_at_retract = [&](const EMC_STAT& sample, const HalView& pins) {
    const auto& t = sample.motion.traj;
    return !*pins.probe_input && t.probeval == 0 && reached_backoff(t, retract);
  };
  static_cast<void>(until(std::chrono::milliseconds{2000}, [&]() {
    const auto* s = live_cycle();
    const auto pins = interlock();
    return s == nullptr || !pins || released_at_retract(*s, *pins);
  }));
  raw = live_cycle();
  const auto after_retract = interlock();
  if (raw == nullptr || !after_retract || !released_at_retract(*raw, *after_retract)) {
    const auto why =
        raw == nullptr || !after_retract
            ? std::string{" (no controller status)"}
            : std::format(" (probe {}, latched {}, commanded {:+.3f} mm, measured {:+.3f} mm "
                          "from the retract)",
                          after_retract->probe_input.value_or(true) ? "touching" : "clear",
                          raw->motion.traj.probeval,
                          coordinate(machine_position(raw->motion.traj.position)) - retract,
                          coordinate(machine_position(raw->motion.traj.actualPosition)) - retract);
    return interrupt((material ? "The probe did not release at the retract position"
                               : "The fixed setter did not release at the retract position") +
                     why + ".");
  }

  if (move.return_to_clearance && descends) {
    positioning = true;
    // Hold the verified encoder position across the lift. Retraction can leave the measured
    // axis one backlash allowance from its commanded location.
    vertical_at = machine_position(raw->motion.traj.actualPosition);
    if (!block(std::format("G90 G53 G0 Z{:.8f}", lift_z / program_scale),
               std::chrono::seconds{30}, false, true))
      return interrupt(std::format("Could not lift the probe to the clearance height ({}).", block_failure));
    raw = live_cycle();
    if (raw == nullptr || !near(raw->motion.traj.position.tran.z * machine_scale, lift_z) ||
        !(std::abs(raw->motion.traj.actualPosition.tran.z * machine_scale - lift_z) <=
          impl_->config.motion_tolerances.probe_backlash_mm))
      return interrupt("The probe did not reach the clearance height.");
    line_origin[2] = lift_z;
    vertical_at.reset();
    positioning = false;
  }

  if (!block(std::format("{} G80 F{:.8f}", restore_incremental ? "G91" : "G90",
                         modal.feed_mm_min / program_scale),
             completion_timeout) ||
      (restore_feed_override && !block("M50 P1", completion_timeout)) ||
      (restore_adaptive && !block("M52 P1", completion_timeout)))
    return interrupt("The probe completed, but LinuxCNC did not restore its feed settings.");
  raw = live_cycle();
  if (raw == nullptr || raw->motion.traj.feed_override_enabled != restore_feed_override ||
      raw->motion.traj.adaptive_feed_enabled != restore_adaptive)
    return interrupt("The probe completed, but LinuxCNC did not confirm its feed controls.");
  impl_->probe_modal.reset();
  hal->finish_probe_cycle();
  // A subsequent cycle must see its own permit. Wait for the control thread to publish and
  // sample the low level before the next request can mistake the previous cycle for its own.
  if (!until(settle_timeout, [&] {
        const auto pins = hal->view(Clock::now());
        return pins && !pins->probe_cycle_active;
      }))
    return std::unexpected("The probe retracted, but the interlock did not return to idle.");
  if (missed)
    return std::unexpected(move.return_to_clearance && descends
                               ? "No probe contact within the search travel. Retracted and lifted clear."
                               : "No probe contact within the search travel. Returned to the start.");
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

  const bool apply_angle = request.action == SetterAction::apply_angle;
  if (!apply_angle && request.action != SetterAction::status)
    impl_->edge_measurement.reset();
  auto& calibration = impl_->setter_calibration;
  if (calibration && calibration->datum != epoch.datum)
    calibration.reset();
  // Saved marks hold without a taught setter while the table still matches the file: tool
  // lengths are relative to the reference, not to the setter (turo: no T98 after a restart).
  const auto saved_marks = [&](std::string_view table) -> std::map<int, double> {
    const auto saved = load_touched(impl_->config.touched_off_file());
    const auto reference = tool_z(table, limits->reference_tool);
    if (!saved || !reference || std::abs(saved->reference - *reference) > 0.001)
      return {};
    return restored_touched_off(table, limits->reference_tool, saved);
  };
  const auto marks = [&](std::string_view table) -> std::vector<int> {
    return still_touched_off(table, calibration ? calibration->measured : saved_marks(table));
  };
  const auto table = impl_->read_tool_table();
  auto current_marks = table ? marks(*table) : std::vector<int>{};
  // The reference's table length is part of the taught plane. Losing its mark
  // invalidates every tool measured against that plane, including saved marks.
  if (calibration && table && !std::ranges::contains(current_marks, limits->reference_tool)) {
    calibration.reset();
    current_marks.clear();
    std::error_code error;
    std::filesystem::remove(impl_->config.touched_off_file(), error);
  } else if (calibration && table && !std::ranges::contains(current_marks, calibration->teacher)) {
    // The plane came from the teacher's length: it is gone, the saved marks still hold.
    calibration.reset();
    current_marks = marks(*table);
  }
  if (request.action == SetterAction::status)
    return SetterState{calibration ? std::optional{calibration->plane} : std::nullopt, std::nullopt,
                       std::move(current_marks)};

  const auto remember = [&]() {
    save_touched(impl_->config.touched_off_file(),
                 {calibration->measured.at(limits->reference_tool), calibration->measured});
  };

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
           !c.any_enabled_axis([](const auto& axis) { return axis.homing || axis.on_hard_limit; });
  };
  // Homing is not required: machine coordinates are continuous within a session, and a homing
  // or a restart forgets the taught setter (Session::observe).
  if (!ready(*raw, before))
    return std::unexpected("The fixed setter requires stationary axes and a stopped spindle.");
  const int tool = before.controller.tool_in_spindle;
  const bool checking = request.action == SetterAction::check;
  const bool corner = request.action == SetterAction::corner;
  const bool inside_corner = request.action == SetterAction::inside_corner;
  const bool pocket = request.action == SetterAction::pocket;
  const bool slot = request.action == SetterAction::slot;
  const bool ridge = request.action == SetterAction::ridge;
  const bool ball = request.action == SetterAction::ball;
  const bool bore = request.action == SetterAction::bore;
  const bool boss = request.action == SetterAction::boss;
  const bool angle = request.action == SetterAction::angle;
  const bool material = is_material_probe(request.action);
  if (material) {
    const auto& probe_tool = impl_->config.touch_probe_tool;
    if (!probe_tool || limits->latch_feed_mm_min <= 0 || limits->latch_backoff_mm <= 0)
      return std::unexpected("Material touch-off requires a configured touch probe and slow latch.");
    if (tool != *probe_tool)
      return std::unexpected(std::format("Load T{} to probe material.", *probe_tool));
  }
  if (tool <= 0 || request.tool != tool)
    return std::unexpected("The confirmed tool must be in the spindle.");
  if (checking && tool == impl_->config.touch_probe_tool)
    return std::unexpected("Load a cutting tool before checking its length.");

  const double position_tolerance = impl_->config.motion_tolerances.probe_position_mm;
  const auto near = [position_tolerance](double actual, double expected) {
    return std::abs(actual - expected) <= position_tolerance;
  };
  const auto at_start = [&](const NmlStatus& s, const std::array<double, 3>& start) {
    return near(s.controller.axes[0].machine, start[0]) &&
           near(s.controller.axes[1].machine, start[1]) &&
           near(s.controller.axes[2].machine, start[2]);
  };
  // After a probe the Z encoder can lag the command by the backlash (0.125 mm on the mill).
  const auto back_at = [&](const NmlStatus& s, const std::array<double, 3>& start) {
    for (std::size_t i = 0; i < start.size(); ++i)
      if (!(std::abs(s.controller.axes[i].machine - start[i]) <= impl_->config.motion_tolerances.probe_backlash_mm))
        return false;
    return true;
  };
  if (apply_angle) {
    const auto& saved = impl_->edge_measurement;
    if (!saved || saved->id != request.edge_measurement)
      return std::unexpected("Measure the edge again before applying rotation.");
    const auto measurement = *std::exchange(impl_->edge_measurement, std::nullopt);
    if (before.controller.mode != TaskMode::mdi || measurement.epoch != epoch ||
        measurement.frame != frame || measurement.tool_offset != before.tool_offset)
      return std::unexpected("The work frame or tool changed. Measure the edge again.");
    const auto from = impl_->begin(&wanted);
    const Impl::Scope scope{*impl_};
    const auto* current = impl_->stat();
    if (!current || impl_->status.link() != Link::live || !impl_->still())
      return std::unexpected("Rotation was cancelled before writing the work system.");
    const auto applying = to_status(*current, impl_->config, impl_->hal, Link::live);
    if (!ready(*current, applying) || applying.controller.mode != TaskMode::mdi ||
        applying.controller.tool_in_spindle != tool || frame_of(applying) != frame ||
        applying.tool_offset != measurement.tool_offset)
      return std::unexpected("The work frame or tool changed. Measure the edge again.");
    EMC_TASK_PLAN_EXECUTE rotate;
    if (!put(rotate.command, std::format("G10 L2 P{} R{:.8f}", frame.wcs, measurement.degrees)))
      return std::unexpected("Could not build the work rotation command.");
    if (auto done = impl_->run(rotate, from); !accepted(done))
      return std::unexpected(done.reason);
    if (impl_->loads)
      ++impl_->loads->mdi_blocks;
    const auto verified = until(settle_timeout, [&] {
      const auto* after_raw = impl_->stat();
      if (!after_raw || impl_->status.link() != Link::live)
        return false;
      const auto after = to_status(*after_raw, impl_->config, impl_->hal, Link::live);
      const auto same_offset = [](const auto& a, const auto& b) {
        return std::ranges::equal(a, b, [](double x, double y) { return std::abs(x - y) < 1e-6; });
      };
      return ready(*after_raw, after) && after.wcs == frame.wcs &&
             same_offset(after.g5x, frame.g5x) && same_offset(after.g92, frame.g92) &&
             same_offset(after.tool_offset, measurement.tool_offset) &&
             std::abs(after.rotation_xy - measurement.degrees) < 1e-6 &&
             std::ranges::equal(
                 after.controller.axes, before.controller.axes,
                 [](const AxisState& axis, const AxisState& original) {
                   return std::abs(axis.machine - original.machine) < 0.01;
                 });
    });
    if (!verified)
      return std::unexpected(
          "Could not verify the applied rotation. Check the active work system.");
    SetterState state{calibration ? std::optional{calibration->plane} : std::nullopt, std::nullopt,
                      std::move(current_marks)};
    state.edge_angle_deg = measurement.degrees;
    return state;
  }
  if (before.controller.mode != TaskMode::mdi || !at_start(before, request.start))
    return std::unexpected("Wait for idle MDI at the confirmed starting position.");
  // Before the setter is taught, any tool with a valid saved length teaches it like the
  // reference (turo: T99 can act like T98).
  const bool teaching = !checking && (tool == limits->reference_tool ||
                        (!calibration && std::ranges::contains(current_marks, tool)));
  if (!material && !teaching && !calibration)
    return std::unexpected(std::format("Teach the setter with T{} or a touched-off tool first.",
                                       limits->reference_tool));

  const double machine_scale = impl_->config.millimetres_per_unit;
  const auto previous = table ? tool_z(*table, tool) : std::nullopt;
  if (!previous || !std::isfinite(*previous * machine_scale))
    return std::unexpected("Could not read the loaded tool's length from the tool table.");
  const double previous_length = *previous * machine_scale;
  if (checking && previous_length == 0 && !std::ranges::contains(current_marks, tool))
    return std::unexpected("Set the loaded tool's length before checking it.");
  // Feature probes run along the active work system's XY axes, turned by its rotation.
  // `to_frame` turns a machine point into those axes about the machine origin and `to_machine`
  // turns it back. A distance in this frame is the same distance in work coordinates, so faces,
  // centres and widths follow from frame values. With no rotation both are the identity.
  const bool rotated = before.rotation_xy != 0;
  const double turn = before.rotation_xy * std::numbers::pi / 180;
  const double cos_turn = std::cos(turn);
  const double sin_turn = std::sin(turn);
  const auto to_frame = [&](const std::array<double, 3>& p) -> std::array<double, 3> {
    if (!rotated)
      return p;
    return {p[0] * cos_turn + p[1] * sin_turn, -p[0] * sin_turn + p[1] * cos_turn, p[2]};
  };
  const auto to_machine = [&](const std::array<double, 3>& p) -> std::array<double, 3> {
    if (!rotated)
      return p;
    return {p[0] * cos_turn - p[1] * sin_turn, p[0] * sin_turn + p[1] * cos_turn, p[2]};
  };
  const auto start_frame = to_frame(request.start);
  // Sets the measured axes of the active work system at `origin`, in the rotated frame, and
  // verifies the command against the encoder lag. The probe cycle has already returned to
  // clearance.
  const auto set_xy_origin = [&](std::array<double, 2> origin,
                                 std::array<bool, 2> axes = {true, true})
      -> std::expected<void, std::string> {
      const auto from = impl_->begin(&wanted);
      const Impl::Scope scope{*impl_};
      const auto* current = impl_->stat();
      if (current == nullptr || impl_->status.link() != Link::live || !impl_->still())
        return std::unexpected<std::string>("Probing was cancelled before setting the work origin.");
      const auto after = to_status(*current, impl_->config, impl_->hal, Link::live);
      if (!ready(*current, after) || after.controller.mode != TaskMode::mdi ||
          after.controller.tool_in_spindle != tool || frame_of(after) != frame ||
          after.tool_offset != before.tool_offset ||
          after.controller.program_units != before.controller.program_units ||
          impl_->read_tool_table() != table)
        return std::unexpected<std::string>("Machine or tool table changed before setting the work origin.");
      // G10 L20 applies at the commanded position (see the single faces).
      const auto& commanded = current->motion.traj.position;
      const std::array<double, 3> machine_at{commanded.tran.x * machine_scale,
                                             commanded.tran.y * machine_scale,
                                             commanded.tran.z * machine_scale};
      const auto at = to_frame(machine_at);
      std::array<double, 2> expected{at[0] - origin[0], at[1] - origin[1]};
      // In a rotated system one work axis moves the stored origin along both machine axes, as
      // for a single face: write both, the axis not measured at its commanded work value.
      const auto written = rotated ? std::array{true, true} : axes;
      const auto commanded_work = work_position(machine_at, after.g5x, after.g92,
                                                after.tool_offset, after.rotation_xy);
      for (std::size_t axis = 0; axis < axes.size(); ++axis)
        if (!axes[axis])
          expected[axis] = commanded_work[axis];
      const double scale = after.controller.program_units == LengthUnit::inch ? 25.4 : 1.0;
      EMC_TASK_PLAN_EXECUTE command;
      std::string code{"G10 L20 P0"};
      for (std::size_t axis = 0; axis < written.size(); ++axis)
        if (written[axis])
          code += std::format(" {}{:.8f}", "XY"[axis], expected[axis] / scale);
      if (!put(command.command, code))
        return std::unexpected<std::string>("Could not format the work origin.");
      const auto result = impl_->run(command, from);
      if (!accepted(result))
        return std::unexpected<std::string>(result.reason.empty() ? "LinuxCNC did not set the work origin."
                                                     : result.reason);
      if (impl_->loads != nullptr)
        ++impl_->loads->mdi_blocks;
      const bool verified = until(settle_timeout, [&] {
        const auto* applied = impl_->stat();
        if (applied == nullptr || applied->echo_serial_number != impl_->serial ||
            applied->status != RCS_DONE || impl_->status.link() != Link::live)
          return false;
        const auto s = to_status(*applied, impl_->config, impl_->hal, Link::live);
        const auto& traj = applied->motion.traj;
        // The encoder lag along the work axes.
        const auto lag = to_frame({(traj.actualPosition.tran.x - traj.position.tran.x) * machine_scale,
                                   (traj.actualPosition.tran.y - traj.position.tran.y) * machine_scale,
                                   0});
        for (std::size_t i = 0; i < written.size(); ++i)
          if (written[i] ? (!std::isfinite(s.work[i]) || std::abs(s.work[i] - lag[i] - expected[i]) > 0.01)
                         : !(std::abs(s.g5x[i] - frame.g5x[i]) <= 1e-6))
            return false;
        return s.wcs == before.wcs && s.g92 == before.g92 && s.rotation_xy == before.rotation_xy &&
               s.tool_offset == before.tool_offset && std::abs(s.g5x[2] - frame.g5x[2]) <= 1e-6;
      });
      if (!verified)
        return std::unexpected<std::string>("Could not verify the work origin. Check the active WCS.");
    return {};
  };
  const auto encloses_start = [&](std::size_t axis, double low, double high) {
    return low < start_frame[axis] && start_frame[axis] < high;
  };
  if (inside_corner || pocket || slot || ridge) {
    if (frame.wcs < 1 || frame.wcs > 9)
      return std::unexpected("The active work coordinate system is unavailable.");
    const double radius = tool_diameter(*table, tool).value_or(0) * machine_scale / 2;
    if (!std::isfinite(radius) || radius <= 0)
      return std::unexpected("Set the probe ball diameter in the tool table first.");
    const auto selected = probe_line(request.direction, 0).axis;
    if ((inside_corner && (selected != 0 || probe_line(request.y_direction, 0).axis != 1)) ||
        ((slot || ridge) && selected > 1))
      return std::unexpected("Choose the feature's X or Y probing axis.");

    std::optional<double> clearance;
    double depth{};
    double step_off{};
    if (ridge) {
      if (!(request.gauge_width_mm >= 2 && request.gauge_width_mm <= 200))
        return std::unexpected("Enter the approximate ridge width, 2 to 200 mm.");
      if (!std::ranges::contains(current_marks, tool) ||
          std::abs(before.tool_offset[2] - previous_length) > 0.001 ||
          !std::ranges::contains(before.active_codes, "G43"))
        return std::unexpected("Touch off the probe and apply its length first. Ridge probing uses Z0 as the top.");
      const auto& geometry = limits->material_geometry;
      const double top = before.controller.axes[2].machine - before.work[2];
      if (!std::isfinite(top) || request.start[2] < top + radius)
        return std::unexpected("Set Z0 on the ridge top and start with the probe above it.");
      clearance = std::max(request.start[2], top + geometry.clearance_mm);
      depth = top - radius - geometry.depth_mm;
      step_off = request.gauge_width_mm / 2 + radius + geometry.margin_mm + geometry.reach_mm;
      if (limits->material_travel_mm < geometry.margin_mm + geometry.reach_mm)
        return std::unexpected("The material probe travel is too short for the ridge approach.");
    }

    const auto face = [&](ProbeDirection direction) -> std::expected<double, std::string> {
      const auto line = probe_line(direction, before.rotation_xy);
      const auto* current = impl_->stat();
      if (!current || impl_->status.link() != Link::live)
        return std::unexpected("Controller status was lost between the feature faces.");
      const auto status = to_status(*current, impl_->config, impl_->hal, Link::live);
      if (frame_of(status) != frame || status.tool_offset != before.tool_offset ||
          status.controller.program_units != before.controller.program_units)
        return std::unexpected("The work frame changed between the feature faces.");
      const std::array from{status.controller.axes[0].machine, status.controller.axes[1].machine,
                            status.controller.axes[2].machine};
      FixedSetterMove move{.tool = tool, .start = from, .travel_mm = limits->material_travel_mm,
                           .feed_mm_min = limits->feed_mm_min, .material = direction};
      if (clearance) {
        move.approach = line.advance(request.start, -step_off);
        (*move.approach)[2] = depth;
        move.clearance_z = clearance;
        move.return_to_clearance = true;
      } else if (!back_at(status, request.start)) {
        return std::unexpected("The probe did not return to the confirmed point inside the feature.");
      }
      const auto contact = probe_fixed_setter(move, wanted);
      // The physical face is beyond the centre of the ball in the direction of travel.
      if (!contact)
        return std::unexpected(std::format("{}{} face: {}", line.sign > 0 ? "+" : "−",
                                           line.word(), contact.error()));
      return to_frame(contact->machine)[line.axis] + line.sign * radius;
    };

    const std::array<bool, 2> axes{inside_corner || pocket || selected == 0,
                                  inside_corner || pocket || selected == 1};
    std::array<double, 2> origin{};
    SetterState state{calibration ? std::optional{calibration->plane} : std::nullopt, std::nullopt,
                      std::move(current_marks),
                      SetterState::Material{frame.wcs, axes[0] ? 'X' : 'Y', axes[0] && axes[1]}};
    for (std::size_t axis = 0; axis < axes.size(); ++axis) {
      if (!axes[axis])
        continue;
      if (inside_corner) {
        const auto wall = face(axis == 0 ? request.direction : request.y_direction);
        if (!wall)
          return std::unexpected(wall.error());
        origin[axis] = *wall;
        continue;
      }
      const auto negative = axis == 0 ? ProbeDirection::x_minus : ProbeDirection::y_minus;
      const auto positive = axis == 0 ? ProbeDirection::x_plus : ProbeDirection::y_plus;
      const auto low = face(ridge ? positive : negative);
      if (!low)
        return std::unexpected(low.error());
      const auto high = face(ridge ? negative : positive);
      if (!high)
        return std::unexpected(high.error());
      if (!encloses_start(axis, *low, *high))
        return std::unexpected("The probed faces do not enclose the confirmed starting point.");
      origin[axis] = std::midpoint(*low, *high);
      state.feature_size[axis] = *high - *low;
    }
    if (auto set = set_xy_origin(origin, axes); !set)
      return std::unexpected(set.error());
    return state;
  }
  if (angle) {
    // An edge angle: the edge from beside the work, as a single face, then again `spacing`
    // further along the edge. Only measured: nothing is written.
    // In a rotated work system both points lie along the rotated work axis, and the angle is the
    // edge's turn from that axis. Applying it adds that turn to the present rotation.
    const auto line = probe_line(request.direction, before.rotation_xy);
    if (line.axis == 2)
      return std::unexpected("An edge angle needs an X or Y edge.");
    if (frame.wcs < 1 || frame.wcs > 9)
      return std::unexpected("The active work coordinate system is unavailable.");
    const double spacing = request.gauge_width_mm;
    if (!(std::abs(spacing) >= 5 && std::abs(spacing) <= 300))
      return std::unexpected("Enter how far apart to probe along the edge, 5 to 300 mm.");
    const auto contact = [&](const std::array<double, 3>& from,
                             std::optional<std::array<double, 3>> approach)
        -> std::expected<std::array<double, 3>, std::string> {
      const auto found = probe_fixed_setter({.tool = tool,
                                             .start = from,
                                             .travel_mm = limits->material_travel_mm,
                                             .feed_mm_min = limits->feed_mm_min,
                                             .approach = approach,
                                             .material = request.direction},
                                            wanted);
      if (!found)
        return std::unexpected(found.error());
      return found->machine;
    };
    const auto first = contact(request.start, std::nullopt);
    if (!first)
      return std::unexpected("First point: " + first.error());
    // Along the edge: Y for an X face, X for a Y face, at the start height.
    const std::size_t along = line.axis == 0 ? 1 : 0;
    const auto* now = impl_->stat();
    if (now == nullptr || impl_->status.link() != Link::live)
      return std::unexpected("Controller status was lost between the edge points.");
    const auto between = to_status(*now, impl_->config, impl_->hal, Link::live);
    const std::array here{between.controller.axes[0].machine, between.controller.axes[1].machine,
                          between.controller.axes[2].machine};
    auto next = start_frame;
    next[along] += spacing;
    const auto second = contact(here, to_machine(next));
    if (!second)
      return std::unexpected("Second point: " + second.error());
    // The face moves across by `across` while the probe moves `spacing` along it.
    const auto first_at = to_frame(*first);
    const auto second_at = to_frame(*second);
    const double across = second_at[line.axis] - first_at[line.axis];
    const double measured_spacing = second_at[along] - first_at[along];
    if (!std::isfinite(measured_spacing) || std::abs(measured_spacing - spacing) > position_tolerance)
      return std::unexpected("The two contact points did not reach the requested spacing.");
    // An edge is a line, so measuring it in reverse gives the same orientation.
    const double degrees =
        (line.axis == 0 ? -1.0 : 1.0) * std::atan(across / measured_spacing) * 180 / std::numbers::pi;
    if (!std::isfinite(degrees) || std::abs(degrees) > 10)
      return std::unexpected(std::format(
          "The edge turns {:.2f}° over {:.1f} mm, more than 10°. Check that both points touched "
          "the same straight edge.",
          degrees, measured_spacing));
    SetterState state{calibration ? std::optional{calibration->plane} : std::nullopt, std::nullopt,
                      std::move(current_marks)};
    const auto* current = impl_->stat();
    if (!current || impl_->status.link() != Link::live || (wanted && !wanted()))
      return std::unexpected("The machine changed before the measurement could be saved.");
    const auto after = to_status(*current, impl_->config, impl_->hal, Link::live);
    if (!ready(*current, after) || frame_of(after) != frame ||
        after.controller.tool_in_spindle != tool || after.tool_offset != before.tool_offset)
      return std::unexpected("The work frame or tool changed. Measure the edge again.");
    const auto id = ++impl_->next_edge_measurement;
    const double rotation = wrapped_rotation(before.rotation_xy + degrees);
    impl_->edge_measurement = Impl::EdgeMeasurement{id, frame, epoch, before.tool_offset, rotation};
    state.edge_angle_deg = degrees;
    state.edge_rotation = SetterState::EdgeRotation{id, frame.wcs, rotation};
    return state;
  }
  if (bore) {
    // A bore: the ball starts inside it, below its top. Both X walls from there; the midpoint of
    // any chord lies on the centre line, so the centre X follows. Then both Y walls from the
    // centre X: a chord through the centre, which also gives the diameter.
    if (before.wcs < 1 || before.wcs > 9)
      return std::unexpected("The active work coordinate system is unavailable.");
    const double radius = tool_diameter(*table, tool).value_or(0) * machine_scale / 2;
    if (!std::isfinite(radius) || radius <= 0)
      return std::unexpected("Set the probe ball diameter in the tool table first.");
    const auto here = [&]() -> std::optional<std::array<double, 3>> {
      const auto* s = impl_->stat();
      if (s == nullptr || impl_->status.link() != Link::live)
        return std::nullopt;
      const auto status = to_status(*s, impl_->config, impl_->hal, Link::live);
      return std::array{status.controller.axes[0].machine, status.controller.axes[1].machine,
                        status.controller.axes[2].machine};
    };
    // The wall's ball-centre contact along the line, from `from`, after an optional traverse
    // at the start height to `approach`.
    const auto wall = [&](ProbeDirection direction, const std::array<double, 3>& from,
                          std::optional<std::array<double, 3>> approach)
        -> std::expected<double, std::string> {
      const auto line = probe_line(direction, before.rotation_xy);
      const auto contact = probe_fixed_setter({.tool = tool,
                                               .start = from,
                                               .travel_mm = limits->material_travel_mm,
                                               .feed_mm_min = limits->feed_mm_min,
                                               .approach = approach,
                                               .material = direction},
                                              wanted);
      if (!contact)
        return std::unexpected(contact.error());
      return to_frame(contact->machine)[line.axis];
    };
    const auto x_plus = wall(ProbeDirection::x_plus, request.start, std::nullopt);
    if (!x_plus)
      return std::unexpected("+X wall: " + x_plus.error());
    auto at = here();
    if (!at)
      return std::unexpected("Controller status was lost inside the bore.");
    const auto x_minus = wall(ProbeDirection::x_minus, *at, std::nullopt);
    if (!x_minus)
      return std::unexpected("−X wall: " + x_minus.error());
    if (!encloses_start(0, *x_minus, *x_plus))
      return std::unexpected("The probed X faces do not enclose the confirmed bore starting point.");
    const double centre_x = (*x_plus + *x_minus) / 2;
    at = here();
    if (!at)
      return std::unexpected("Controller status was lost inside the bore.");
    const auto y_plus =
        wall(ProbeDirection::y_plus, *at, to_machine({centre_x, start_frame[1], (*at)[2]}));
    if (!y_plus)
      return std::unexpected("+Y wall: " + y_plus.error());
    at = here();
    if (!at)
      return std::unexpected("Controller status was lost inside the bore.");
    const auto y_minus = wall(ProbeDirection::y_minus, *at, std::nullopt);
    if (!y_minus)
      return std::unexpected("−Y wall: " + y_minus.error());
    if (!encloses_start(1, *y_minus, *y_plus))
      return std::unexpected("The probed Y faces do not enclose the confirmed bore starting point.");
    const double centre_y = (*y_plus + *y_minus) / 2;
    const double diameter = *y_plus - *y_minus + 2 * radius;
    if (auto set = set_xy_origin({centre_x, centre_y}); !set)
      return std::unexpected(set.error());
    SetterState state{calibration ? std::optional{calibration->plane} : std::nullopt, std::nullopt,
                      std::move(current_marks), SetterState::Material{before.wcs, 'X', true}};
    state.center_diameter = diameter;
    return state;
  }
  if (corner || ball || boss) {
    // A corner: the X face, then the Y face, each probed from beside the work below its top.
    // A ball calibration: both X faces of a gauge of known width, the same way.
    // The operator jogs the probe over the work within the configured reach of both faces; Z0 of the
    // active work system is the top (probe Z top first). Nothing is written unless both touch.
    const auto& geometry = limits->material_geometry;
    const auto x_line =
        probe_line(ball || boss ? ProbeDirection::x_plus : request.direction, before.rotation_xy);
    const auto y_line = probe_line(request.y_direction, before.rotation_xy);
    if (corner && (x_line.axis != 0 || y_line.axis != 1))
      return std::unexpected("A corner needs one X and one Y probe direction.");
    const double half_gauge = request.gauge_width_mm / 2;
    if (ball && !(request.gauge_width_mm >= 2 && request.gauge_width_mm <= 200))
      return std::unexpected("Enter the gauge width, 2 to 200 mm.");
    if (boss && !(request.gauge_width_mm >= 2 && request.gauge_width_mm <= 200))
      return std::unexpected("Enter the approximate boss diameter, 2 to 200 mm.");
    if (ball && rotated)
      return std::unexpected("Ball calibration needs a work system without rotation.");
    if (before.wcs < 1 || before.wcs > 9)
      return std::unexpected("The active work coordinate system is unavailable.");
    if (!std::ranges::contains(current_marks, tool) ||
        std::abs(before.tool_offset[2] - previous_length) > 0.001 ||
        !std::ranges::contains(before.active_codes, "G43"))
      return std::unexpected(std::format(
          "Touch off T{} and apply its length first: probing beside the work uses Z0 as the top.",
          tool));
    const double radius = tool_diameter(*table, tool).value_or(0) * machine_scale / 2;
    if (!std::isfinite(radius) || radius <= 0)
      return std::unexpected("Set the probe ball diameter in the tool table first.");
    // Machine Z of the probe tip where the active work system reads Z0.
    const double top = before.controller.axes[2].machine - before.work[2];
    const double clearance = std::max(request.start[2], top + geometry.clearance_mm);
    const double depth = top - radius - geometry.depth_mm;
    const double outside = geometry.reach_mm + radius + geometry.margin_mm;
    if (!std::isfinite(top) || request.start[2] < top + radius)
      return std::unexpected("Start with the probe above the top of the work.");
    if (limits->material_travel_mm < outside + radius)
      return std::unexpected("The material probe travel is too short to probe beside the work.");

    const auto here = [&]() -> std::optional<std::array<double, 3>> {
      const auto* s = impl_->stat();
      if (s == nullptr || impl_->status.link() != Link::live)
        return std::nullopt;
      const auto status = to_status(*s, impl_->config, impl_->hal, Link::live);
      return std::array{status.controller.axes[0].machine, status.controller.axes[1].machine,
                        status.controller.axes[2].machine};
    };
    // `base` is in the rotated frame: the confirmed start unless given.
    const auto face = [&](const ProbeLine& line, ProbeDirection direction,
                          const std::array<double, 3>& from, double offset,
                          std::optional<std::array<double, 3>> base = std::nullopt)
        -> std::expected<double, std::string> {
      auto approach = base.value_or(start_frame);
      approach[line.axis] -= line.sign * offset;
      approach[2] = depth;
      const auto contact = probe_fixed_setter({.tool = tool,
                                               .start = from,
                                               .travel_mm = limits->material_travel_mm,
                                               .feed_mm_min = limits->feed_mm_min,
                                               .approach = to_machine(approach),
                                               .clearance_z = clearance,
                                               .material = direction,
                                               .return_to_clearance = true},
                                              wanted);
      if (!contact)
        return std::unexpected(contact.error());
      // The ball's centre touched: the face is one radius further along the travel.
      return to_frame(contact->machine)[line.axis] + line.sign * radius;
    };
    if (boss) {
      // A boss: both X faces from outside, then both Y faces through the centre X. Faces are
      // where the surface is, so their midpoints are the centre and their distance the diameter.
      const auto left = face(x_line, ProbeDirection::x_plus, request.start, half_gauge + outside);
      if (!left)
        return std::unexpected("−X face: " + left.error());
      auto at = here();
      if (!at)
        return std::unexpected("Controller status was lost between the boss faces.");
      const auto right = face(probe_line(ProbeDirection::x_minus, before.rotation_xy),
                              ProbeDirection::x_minus, *at, half_gauge + outside);
      if (!right)
        return std::unexpected("+X face: " + right.error());
      if (!encloses_start(0, *left, *right))
        return std::unexpected("The probed X faces do not enclose the confirmed boss starting point.");
      const double centre_x = (*left + *right) / 2;
      const std::array through{centre_x, start_frame[1], request.start[2]};
      at = here();
      if (!at)
        return std::unexpected("Controller status was lost between the boss faces.");
      const auto front = face(probe_line(ProbeDirection::y_plus, before.rotation_xy),
                              ProbeDirection::y_plus, *at, half_gauge + outside, through);
      if (!front)
        return std::unexpected("−Y face: " + front.error());
      at = here();
      if (!at)
        return std::unexpected("Controller status was lost between the boss faces.");
      const auto back = face(probe_line(ProbeDirection::y_minus, before.rotation_xy),
                             ProbeDirection::y_minus, *at, half_gauge + outside, through);
      if (!back)
        return std::unexpected("+Y face: " + back.error());
      if (!encloses_start(1, *front, *back))
        return std::unexpected("The probed Y faces do not enclose the confirmed boss starting point.");
      const double centre_y = (*front + *back) / 2;
      if (auto set = set_xy_origin({centre_x, centre_y}); !set)
        return std::unexpected(set.error());
      SetterState state{calibration ? std::optional{calibration->plane} : std::nullopt,
                        std::nullopt, std::move(current_marks),
                        SetterState::Material{before.wcs, 'X', true}};
      state.center_diameter = *back - *front;
      return state;
    }
    if (ball) {
      // Both faces of the gauge from outside: the ball centres touch one effective diameter
      // further apart than the gauge is wide.
      const auto left = face(x_line, ProbeDirection::x_plus, request.start, half_gauge + outside);
      if (!left)
        return std::unexpected("Left face: " + left.error());
      const auto between = here();
      if (!between)
        return std::unexpected("Controller status was lost between the gauge faces.");
      const auto right = face(probe_line(ProbeDirection::x_minus, 0), ProbeDirection::x_minus,
                              *between, half_gauge + outside);
      if (!right)
        return std::unexpected("Right face: " + right.error());
      const double centres = (*right + radius) - (*left - radius);
      const double diameter = centres - request.gauge_width_mm;
      if (!std::isfinite(diameter) || diameter <= 0 || std::abs(diameter - 2 * radius) > 0.5)
        return std::unexpected(std::format(
            "Measured ball diameter {:.3f} mm is more than 0.5 mm from the table's {:.3f} mm. "
            "Check the gauge width and that the probe started over the gauge.",
            diameter, 2 * radius));
      const auto from = impl_->begin(&wanted);
      const Impl::Scope scope{*impl_};
      const auto* current = impl_->stat();
      if (current == nullptr || impl_->status.link() != Link::live || !impl_->still())
        return std::unexpected("Ball calibration was cancelled before writing the diameter.");
      const auto after = to_status(*current, impl_->config, impl_->hal, Link::live);
      if (!ready(*current, after) || after.controller.mode != TaskMode::mdi ||
          after.controller.tool_in_spindle != tool || impl_->read_tool_table() != table)
        return std::unexpected("Machine or tool table changed before writing the diameter.");
      const double scale = after.controller.program_units == LengthUnit::inch ? 25.4 : 1.0;
      EMC_TASK_PLAN_EXECUTE write;
      if (!put(write.command, std::format("G10 L1 P{} R{:.6f}", tool, diameter / 2 / scale)))
        return std::unexpected("Could not format the ball diameter.");
      if (const auto result = impl_->run(write, from); !accepted(result))
        return std::unexpected(result.reason.empty() ? "LinuxCNC did not write the ball diameter."
                                                     : result.reason);
      if (impl_->loads != nullptr)
        ++impl_->loads->mdi_blocks;
      const bool written = until(settle_timeout, [&] {
        const auto written_table = impl_->read_tool_table();
        const auto value = written_table ? tool_diameter(*written_table, tool) : std::nullopt;
        return value && std::abs(*value * machine_scale - diameter) <= 0.002;
      });
      if (!written)
        return std::unexpected("Could not verify the ball diameter in the tool table.");
      SetterState state{calibration ? std::optional{calibration->plane} : std::nullopt,
                        std::nullopt, std::move(current_marks)};
      state.ball_diameter = diameter;
      return state;
    }
    const auto x_face = face(x_line, request.direction, request.start, outside);
    if (!x_face)
      return std::unexpected("X face: " + x_face.error());
    const auto between = here();
    if (!between)
      return std::unexpected("Controller status was lost between the corner faces.");
    const auto y_face = face(y_line, request.y_direction, *between, outside);
    if (!y_face)
      return std::unexpected("Y face: " + y_face.error());

    if (auto set = set_xy_origin({*x_face, *y_face}); !set)
      return std::unexpected(set.error());
    return SetterState{calibration ? std::optional{calibration->plane} : std::nullopt, std::nullopt,
                       std::move(current_marks), SetterState::Material{before.wcs, 'X', true}};
  }
  if (material) {
    const auto line = probe_line(request.direction, before.rotation_xy);
    if (before.wcs < 1 || before.wcs > 9)
      return std::unexpected("The active work coordinate system is unavailable.");
    double radius = 0;
    if (line.axis == 2) {
      if (!std::ranges::contains(current_marks, tool))
        return std::unexpected(
            std::format("Touch off T{} at the tool setter before probing Z.", tool));
      if (std::abs(before.tool_offset[2] - previous_length) > 0.001 ||
          !std::ranges::contains(before.active_codes, "G43"))
        return std::unexpected(
            std::format("Apply the measured T{} length before probing Z.", tool));
    } else {
      radius = tool_diameter(*table, tool).value_or(0) * machine_scale / 2;
      if (!std::isfinite(radius) || radius <= 0)
        return std::unexpected(
            "Set the probe ball diameter in the tool table before probing an edge.");
    }
    const auto contact = probe_fixed_setter({.tool = tool,
                                             .start = request.start,
                                             .travel_mm = limits->material_travel_mm,
                                             .feed_mm_min = limits->feed_mm_min,
                                             .material = request.direction},
                                            wanted);
    if (!contact)
      return std::unexpected(contact.error());

    const auto from = impl_->begin(&wanted);
    const Impl::Scope scope{*impl_};
    const auto* current = impl_->stat();
    if (current == nullptr || impl_->status.link() != Link::live || !impl_->still())
      return std::unexpected("Material touch-off was cancelled before setting the work origin.");
    const auto after = to_status(*current, impl_->config, impl_->hal, Link::live);
    if (!ready(*current, after) || after.controller.mode != TaskMode::mdi ||
        after.controller.tool_in_spindle != tool || !back_at(after, request.start) ||
        frame_of(after) != frame || after.tool_offset != before.tool_offset ||
        after.controller.program_units != before.controller.program_units ||
        impl_->read_tool_table() != table)
      return std::unexpected("Machine or tool table changed before setting the work origin.");

    // G10 L20 applies at the commanded position, so measure from it, not from the Z encoder
    // reading, which lags the command by the backlash after the retract.
    const auto& commanded = current->motion.traj.position;
    const std::array at{commanded.tran.x * machine_scale, commanded.tran.y * machine_scale,
                        commanded.tran.z * machine_scale};
    // G10 L20 sets the coordinate at the current position. Use the distance back
    // from the measured face, including ball radius only for side contact.
    auto expected = after.work;
    expected[line.axis] = line.sign * (line.distance(contact->machine, at) - radius);
    const double scale = after.controller.program_units == LengthUnit::inch ? 25.4 : 1.0;
    const auto code =
        line.axis != 2 && after.rotation_xy != 0
            ? std::format("G10 L20 P0 X{:.8f} Y{:.8f}", expected[0] / scale, expected[1] / scale)
            : std::format("G10 L20 P0 {}{:.8f}", line.word(), expected[line.axis] / scale);
    EMC_TASK_PLAN_EXECUTE command;
    if (!put(command.command, code))
      return std::unexpected("Could not format the material work origin.");
    const auto result = impl_->run(command, from);
    if (!accepted(result))
      return std::unexpected(result.reason.empty() ? "LinuxCNC did not set the work origin."
                                                   : result.reason);
    if (impl_->loads != nullptr)
      ++impl_->loads->mdi_blocks;
    const bool verified = until(settle_timeout, [&] {
      const auto* applied = impl_->stat();
      if (applied == nullptr || applied->echo_serial_number != impl_->serial ||
          applied->status != RCS_DONE || impl_->status.link() != Link::live)
        return false;
      const auto s = to_status(*applied, impl_->config, impl_->hal, Link::live);
      const auto& traj = applied->motion.traj;
      const std::array lag{(traj.actualPosition.tran.x - traj.position.tran.x) * machine_scale,
                           (traj.actualPosition.tran.y - traj.position.tran.y) * machine_scale,
                           (traj.actualPosition.tran.z - traj.position.tran.z) * machine_scale};
      for (std::size_t i = 0; i < expected.size(); ++i)
        // s.work follows the encoder: compare its commanded value. 0.01 mm like the latch
        // spread, since the encoder reading moves by more than 0.002 mm at rest.
        if (!std::isfinite(s.work[i]) || std::abs(s.work[i] - lag[i] - expected[i]) > 0.01)
          return false;
      return s.wcs == before.wcs && s.g92 == before.g92 && s.rotation_xy == before.rotation_xy &&
             s.tool_offset == before.tool_offset;
    });
    if (!verified)
      return std::unexpected(
          "Could not verify the work origin after probing. Check the active WCS.");
    return SetterState{calibration ? std::optional{calibration->plane} : std::nullopt, std::nullopt,
                       std::move(current_marks), SetterState::Material{before.wcs, line.word()}};
  }
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
    return std::unexpected(checking ? std::format("T{} check failed. {}", tool, contact.error())
                                    : contact.error());
  if (wanted && !wanted())
    return std::unexpected("The fixed setter result was cancelled.");
  const double value = contact->machine[2] - (teaching ? previous_length : calibration->plane[2]);
  if (!std::isfinite(value))
    return std::unexpected("The fixed setter result is invalid.");
  const auto current_table = impl_->read_tool_table();
  if (!current_table || *current_table != *table)
    return std::unexpected("The tool table changed during the setter cycle.");

  const SetterMeasurement measurement{
      teaching, tool, contact->machine, value, previous_length, contact->stopping_distance,
      checking ? std::optional{limits->check_tolerance_mm} : std::nullopt};
  if (checking) {
    const double difference = value - previous_length;
    if (std::abs(difference) > limits->check_tolerance_mm)
      return std::unexpected(std::format(
          "T{} check failed. Tool is {:.4f} mm {} (measured {:.4f} mm, table {:.4f} mm, "
          "tolerance {} mm). Inspect the tool.",
          tool, std::abs(difference), difference < 0 ? "shorter" : "longer", value,
          previous_length, limits->check_tolerance_mm));
    return SetterState{calibration->plane, measurement, std::move(current_marks)};
  }
  if (teaching) {
    calibration = Impl::Calibration{
        epoch.datum,
        {request.start[0], request.start[1], value},
        limits->clearance_mm > 0 ? contact->machine[2] + limits->clearance_mm : request.start[2],
        restored_touched_off(*table, limits->reference_tool, load_touched(impl_->config.touched_off_file())),
        tool};
    remember();
    return SetterState{calibration->plane, measurement, marks(*table)};
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
  calibration->measured[tool] = *saved;
  remember();
  return SetterState{calibration->plane, measurement, marks(*changed)};
}

auto NmlPort::open_program(std::string_view path, const Wanted& wanted) -> ActionResult {
  if (path == impl_->breakpoint_copy.string() || path == impl_->legacy_breakpoint_copy)
    return failed("Open the original program instead of its temporary breakpoint copy.",
                  MessageKey{"backend.nmlPort.openOriginal"});
  return open_program_file(path, wanted, true);
}

auto NmlPort::open_program_file(std::string_view path, const Wanted& wanted, bool publish_load)
    -> ActionResult {
  const auto from = impl_->begin(&wanted);
  const Impl::Scope scope{*impl_};
  if (!impl_->still())
    return withdrawn();
  EMC_TASK_PLAN_OPEN message;
  if (!put(message.file, path))
    return failed("The program path is too long for LinuxCNC.",
                  MessageKey{"backend.nmlPort.pathTooLong"});
  // The interpreter keeps a program open until it has run to its end or was aborted, and
  // refuses another while it does ("A file is already open"): close first. Closing with none
  // open does nothing.
  EMC_TASK_PLAN_CLOSE close;
  if (auto closed = impl_->run(close, from); !accepted(closed))
    return closed;
  if (auto done = impl_->run(message, from); !accepted(done))
    return done;
  if (publish_load) {
    // A newly opened file is a load, even when the operator opens the same path again.
    if (impl_->loads)
      ++impl_->loads->programs;
    if (impl_->breakpoint)
      impl_->discard_breakpoint();
  }
  return impl_->settled(
      from, "LinuxCNC did not open the program.",
      [&](const EMC_STAT& now) -> bool { return text_of(now.task.file) == path; });
}

auto NmlPort::run(const Wanted& wanted) -> ActionResult {
  if (status().controller.breakpoint_program)
    return failed("Wait for the original program to reopen after the breakpoint run.",
                  MessageKey{"messages.policy.waitBreakpointReopen"});
  const auto from = impl_->begin(&wanted);
  const Impl::Scope scope{*impl_};
  if (!impl_->still())
    return withdrawn();
  EMC_TASK_PLAN_RUN message;
  message.line = 0; // from the start
  return impl_->start(message, from);
}

auto NmlPort::run_checked(const PreviewInputs& inputs, const Wanted& current, const Wanted& wanted)
    -> ActionResult {
  return start_checked(inputs, current, wanted);
}

auto NmlPort::start_checked(const PreviewInputs& inputs, const Wanted& current,
                            const Wanted& wanted, std::optional<std::string_view> execution_path)
    -> ActionResult {
  const auto from = impl_->begin(&wanted);
  const Impl::Scope scope{*impl_};
  if (!impl_->still())
    return withdrawn();
  constexpr auto stale = "The program or tool data changed. Review the new program check.";
  if (!current())
    return failed(stale);
  if (const auto reason = validate_program_inputs(inputs); !reason.empty())
    return failed(reason);
  EMC_TASK_PLAN_OPEN open;
  if (!inputs.program_file || !put(open.file, execution_path.value_or(*inputs.program_file)))
    return failed("The program path is too long for LinuxCNC.",
                  MessageKey{"backend.nmlPort.pathTooLong"});
  // Refresh the interpreter's descriptor for this path without publishing a new load.
  EMC_TASK_PLAN_CLOSE close;
  if (auto closed = impl_->run(close, from); !accepted(closed))
    return closed;
  if (auto opened = impl_->run(open, from); !accepted(opened))
    return opened;
  // The public status maps the temporary wrapper to its original. Check the exact file
  // reported by Task before revalidating the inputs or sending PLAN_RUN.
  if (auto settled = impl_->settled(from, "LinuxCNC did not open the checked program.",
                                   [&](const EMC_STAT& now) -> bool {
                                     return text_of(now.task.file) == open.file;
                                   });
      !accepted(settled))
    return settled;
  if (!current())
    return failed(stale);
  if (const auto reason = validate_program_inputs(inputs); !reason.empty())
    return failed(reason);
  EMC_TASK_PLAN_RUN message;
  message.line = 0;
  return impl_->start(message, from);
}

auto NmlPort::run_to_breakpoint(const PreviewInputs& inputs, std::string_view program, int line,
                                const Wanted& current, const Wanted& wanted) -> ActionResult {
  if (!inputs.program_file || impl_->breakpoint || status().controller.breakpoint_program)
    return failed("The breakpoint needs the original program.",
                  MessageKey{"backend.nmlPort.breakpointOriginal"});
  const auto receipt = std::to_string(Clock::now().time_since_epoch().count());
  const auto copy = with_breakpoint(program, line, receipt);
  if (!copy)
    return failed(copy.error());
  if (!current())
    return failed("The program or tool data changed. Review the new program check.",
                  MessageKey{"messages.session.programOrToolsChanged"});
  if (const auto reason = validate_program_inputs(inputs); !reason.empty())
    return failed(reason);
  impl_->breakpoint = std::make_shared<const BreakpointSource>(
      *inputs.program_file, impl_->breakpoint_copy.string(), line, receipt);
  if (!impl_->save_breakpoint() || !write_sidecar(impl_->breakpoint_copy, *copy)) {
    impl_->discard_breakpoint();
    return failed("Could not save the temporary breakpoint program.",
                  MessageKey{"backend.nmlPort.breakpointCopyNotSaved"});
  }
  if (impl_->loads)
    impl_->loads->breakpoint.store(impl_->breakpoint);
  auto started = start_checked(inputs, current, wanted, impl_->breakpoint->temporary);
  if (!accepted(started))
    finish_breakpoint(wanted);
  return started;
}

void NmlPort::finish_breakpoint(const Wanted& wanted) {
  const auto observed = status();
  const auto& state = observed.controller;
  if (state.link != Link::live || state.interp != InterpState::idle || !state.in_position ||
      state.requested_tool || observed.paused || observed.task_executing || !wanted() ||
      Clock::now() < impl_->breakpoint_retry)
    return;
  impl_->breakpoint_retry = Clock::now() + std::chrono::seconds{1};
  if (!state.breakpoint_program) {
    impl_->discard_breakpoint();
    return;
  }
  if (impl_->breakpoint) {
    const auto reopened = open_program_file(impl_->breakpoint->original, wanted);
    if (accepted(reopened)) {
      impl_->discard_breakpoint();
      return;
    }
  }
  if (wanted() && !std::exchange(impl_->breakpoint_reported, true))
    impl_->messages.push_back({true,
                               "The original program could not be reopened after the "
                               "breakpoint. Open a program to continue.",
                               MessageKey{"backend.nmlPort.originalNotReopened"}});
}

namespace {

constexpr double restart_command_tolerance = 0.001; // mm

auto restart_near(const Vec3& a, const Vec3& b, double tolerance = 1e-6) -> bool {
  return std::ranges::equal(a, b, [=](double x, double y) { return std::abs(x - y) <= tolerance; });
}

auto restart_frame(const NmlStatus& s, const RestartPoint& p, const PreviewInputs& inputs)
    -> bool {
  return s.controller.link == Link::live && s.controller.machine == MachineState::on &&
         s.controller.program_file == inputs.program_file && s.block_delete == inputs.block_delete &&
         s.wcs == p.wcs && restart_near(s.g5x, p.g5x) && restart_near(s.g92, p.g92) &&
         std::abs(s.rotation_xy - p.rotation) <= 1e-6;
}

auto restart_position(const ControllerState& s) -> Vec3 {
  return {s.axes[0].machine, s.axes[1].machine, s.axes[2].machine};
}

auto restart_at(const NmlStatus& s, const Vec3& target, double tolerance) -> bool {
  return restart_near(s.commanded_position, target, restart_command_tolerance) &&
         restart_near(restart_position(s.controller), target, tolerance);
}

auto restart_at_z(const NmlStatus& s, double z, double tolerance) -> bool {
  return std::abs(s.commanded_position[2] - z) <= restart_command_tolerance &&
         std::abs(s.controller.axes[2].machine - z) <= tolerance;
}

auto restart_clearance(const RestartPoint& point) -> double {
  return point.clearance_z + point.g92[2] + point.g5x[2] + point.tool_offset[2];
}

auto restart_travel(const Vec3& position, const MachineConfig& config) -> bool {
  for (std::size_t i = 0; i < position.size(); ++i)
    if (!(position[i] >= config.travel[i].min && position[i] <= config.travel[i].max))
      return false;
  return true;
}

auto restart_idle(const NmlStatus& status, const MachineConfig& config) -> bool {
  const auto& s = status.controller;
  return s.interp == InterpState::idle && s.in_position && !s.requested_tool &&
         !status.paused && !status.task_executing && status.queued_mdi == 0 &&
         std::ranges::all_of(s.axes, &AxisState::enabled) &&
         !s.any_enabled_axis([&](const auto& a) {
           return a.homing || a.on_hard_limit || (config.limits.homing_required && !a.homed);
         });
}

auto check_restart_approach(RestartPoint& point, const NmlStatus& status,
                            const MachineConfig& config, std::optional<double> tool_length)
    -> std::string {
  if (point.tool == config.touch_probe_tool)
    return "Remove the touch probe before running a program from a line.";
  if (point.spindle_direction != SpindleDirection::stopped &&
      point.spindle_rpm > config.limits.max_spindle_rpm)
    return "The restart spindle speed exceeds the machine limit.";
  if (point.h > 0 && (!tool_length ||
      std::abs(*tool_length * config.millimetres_per_unit - point.tool_offset[2]) > 0.001))
    return "The required tool length changed in the tool table.";
  // Raise the controlled point enough for both tools before a manual exchange.
  const double original_clearance = restart_clearance(point);
  const double old_tool_difference = std::max(0.0, status.tool_offset[2] - point.tool_offset[2]);
  const auto& position = status.commanded_position;
  const double minimum = std::max(position[2], original_clearance + old_tool_difference);
  point.clearance_z += minimum - original_clearance;
  point.approach_feed = std::min(
      {100.0, config.limits.max_axis_jog_velocity[2], point.feed > 0 ? point.feed : 100.0});
  const auto end = restart_machine_position(point);
  const double clearance = restart_clearance(point);
  if (!restart_travel(end, config) || !restart_travel({end[0], end[1], clearance}, config) ||
      !restart_travel({position[0], position[1], clearance}, config))
    return "The restart approach is outside the configured machine travel.";
  return {};
}

} // namespace

auto NmlPort::validate_program_inputs(const PreviewInputs& inputs) -> std::string {
  const auto s = status();
  const auto& start = inputs.start;
  if (s.controller.link == Link::live && s.active_codes != start.active_codes)
    return "The interpreter modes changed. Review a fresh program check.";
  if (start.wcs < 1 || start.wcs > 9 || s.controller.link != Link::live ||
      !restart_idle(s, impl_->config) || s.controller.program_file != inputs.program_file ||
      s.wcs != start.wcs || s.block_delete != inputs.block_delete ||
      s.controller.tool_in_spindle != start.tool_in_spindle ||
      s.controller.program_units != start.program_units ||
      s.spindle_direction != start.spindle_direction ||
      std::abs(s.spindle_rpm - start.spindle_speed) > 1e-6 ||
      std::abs(s.programmed_feed - start.feed) > 1e-6 || !restart_at(s, start.position, impl_->config.motion_tolerances.restart_position_mm) ||
      !restart_near(s.tool_offset, start.tool_offset) || !restart_near(s.g92, inputs.g92))
    return "The machine state changed. Review a fresh program check.";
  const auto& system = inputs.systems.at(static_cast<std::size_t>(start.wcs - 1));
  if (!system.available || !restart_near(s.g5x, system.offset) ||
      std::abs(s.rotation_xy - system.rotation_xy) > 1e-6)
    return "The work origin changed. Review a fresh program check.";
  return {};
}

auto NmlPort::prepare_tool_measurement(int tool, std::uint64_t request, int line,
                                      const RestartPoint& point, const PreviewInputs& inputs,
                                      const Wanted& wanted)
    -> ActionResult {
  const auto from = impl_->begin(&wanted);
  const Impl::Scope scope{*impl_};
  const auto& config = impl_->config;
  const auto safe = [&](const EMC_STAT& raw, const NmlStatus& status) {
    const auto& state = status.controller;
    const auto hal = impl_->hal != nullptr ? impl_->hal->view(HalShared::Clock::now()) : std::nullopt;
    return state.link == Link::live && state.machine == MachineState::on && state.in_position &&
           hal && hal->watchdog_ok && !hal->jog_stop_high && !hal->probe_cycle_active &&
           state.program_file == inputs.program_file && status.block_delete == inputs.block_delete &&
           !state.breakpoint_program && !state.spindle_active &&
           status.spindle_actual_rpm && std::abs(*status.spindle_actual_rpm) < 1.0 &&
           raw.motion.traj.enabled && raw.motion.traj.current_vel == 0 &&
           !raw.motion.external_offsets_applied && !raw.motion.on_soft_limit &&
           std::ranges::all_of(state.axes, &AxisState::enabled) &&
           !state.any_enabled_axis([&](const auto& a) {
             return a.homing || a.on_hard_limit || (config.limits.homing_required && !a.homed);
           });
  };
  const auto reading = [&](const EMC_STAT& raw) {
    return to_status(raw, config, impl_->hal, impl_->status.link());
  };
  if (!impl_->still())
    return withdrawn();
  const auto table = impl_->read_tool_table();
  const auto length = table ? tool_z(*table, tool) : std::nullopt;
  const auto& limits = config.fixed_setter;
  const auto& calibration = impl_->setter_calibration;
  if (tool <= 0 || !limits || tool == limits->reference_tool ||
      tool == config.touch_probe_tool || !length || !calibration)
    return failed("A cutting tool in the tool table and a fixed setter are required.");
  const auto setter_plane = calibration->plane;
  const double setter_clearance = calibration->clearance_z;
  const bool retract_to_setter_clearance = limits->clearance_mm > 0;
  const auto* raw = impl_->stat();
  if (raw == nullptr)
    return failed("The controller is unavailable.");
  const auto before = reading(*raw);
  const auto& state = before.controller;
  if (!safe(*raw, before) || state.mode != TaskMode::automatic || state.interp == InterpState::idle ||
      state.requested_tool != tool || state.tool_request != request || state.tool_change_line != line ||
      point.tool != tool || point.h != tool || !restart_frame(before, point, inputs) ||
      !restart_at(before, restart_machine_position(point), config.motion_tolerances.restart_position_mm))
    return failed("The program tool change or its stationary position could not be verified.");
  // Use the table length for the approach estimate while M6 is pending. The measured length
  // and actual position are checked again before the operator is offered a restart.
  auto nominal = point;
  auto at_setter = before;
  at_setter.commanded_position = {
      setter_plane[0], setter_plane[1],
      retract_to_setter_clearance ? setter_clearance
                                  : std::max(setter_clearance, before.commanded_position[2])};
  at_setter.tool_offset = point.tool_offset;
  if (auto reason = check_restart_approach(nominal, at_setter, config, length); !reason.empty())
    return failed(std::move(reason));
  if (!impl_->still())
    return withdrawn();

  if (impl_->loads)
    ++impl_->loads->aborts;
  EMC_TASK_ABORT stop;
  if (auto stopped = impl_->run(stop, from); !accepted(stopped))
    return stopped;
  const auto stationary = [&](const EMC_STAT& current) {
    const auto now = reading(current);
    return safe(current, now) && restart_idle(now, config) &&
           restart_at(now, before.commanded_position, config.motion_tolerances.restart_position_mm) &&
           now.wcs == before.wcs && now.g5x == before.g5x && now.g92 == before.g92 &&
           now.rotation_xy == before.rotation_xy;
  };
  if (auto stopped = impl_->settled(from, "LinuxCNC did not clear the pending tool change.", stationary);
      !accepted(stopped))
    return stopped;
  EMC_TASK_SET_MODE mode;
  mode.mode = EMC_TASK_MODE_MDI;
  if (auto sent = impl_->send(mode); !accepted(sent))
    return sent;
  if (auto switched = impl_->settled(
          from, "LinuxCNC did not enter idle MDI for tool measurement.",
          [&](const EMC_STAT& current) { return current.task.mode == mode.mode && stationary(current); });
      !accepted(switched))
    return switched;
  if (impl_->read_tool_table() != table)
    return failed("The tool table changed before registering the installed tool.");
  raw = impl_->stat();
  if (raw == nullptr || raw->task.mode != mode.mode || !stationary(*raw))
    return failed("The mill changed state before registering the installed tool.");
  EMC_TASK_PLAN_EXECUTE register_tool;
  if (!put(register_tool.command, std::format("M61 Q{}", tool)))
    return failed("Could not register the installed tool.");
  if (auto registered = impl_->run(register_tool, from); !accepted(registered))
    return registered;
  if (impl_->loads)
    ++impl_->loads->mdi_blocks;
  return impl_->settled(from, "LinuxCNC did not confirm the installed tool.",
                       [&](const EMC_STAT& current) {
                         return current.task.mode == mode.mode && stationary(current) &&
                                current.io.tool.toolInSpindle == tool;
                       });
}

auto NmlPort::validate_restart(RestartPoint& point, const PreviewInputs& inputs) -> std::string {
  const auto s = status();
  const auto& config = impl_->config;
  // Task skips the blocks before the line without their effect on motion, the selection of a
  // work system among them: the line's system must be the active one already.
  if (s.controller.link == Link::live && point.wcs >= 1 && point.wcs <= 9 && s.wcs != point.wcs) {
    static constexpr std::array names{"G54", "G55", "G56", "G57", "G58",
                                      "G59", "G59.1", "G59.2", "G59.3"};
    const auto* name = names.at(static_cast<std::size_t>(point.wcs - 1));
    return std::format("This line runs in {}. Select {}, then choose the line again.", name, name);
  }
  if (!restart_frame(s, point, inputs))
    return "Machine power, the program or the work coordinate system changed.";
  if (!restart_idle(s, config) || s.controller.tool_in_spindle != inputs.start.tool_in_spindle ||
      !restart_near(s.tool_offset, inputs.start.tool_offset) ||
      !restart_at(s, inputs.start.position, config.motion_tolerances.restart_position_mm))
    return "The machine must remain stopped at the position shown in the preview.";
  std::optional<double> length;
  if (point.h > 0) {
    const auto table = impl_->read_tool_table();
    if (table)
      length = tool_z(*table, point.h);
  }
  return check_restart_approach(point, s, config, length);
}

auto NmlPort::start_restart(const RestartPoint& point, const PreviewInputs& inputs,
                            bool after_change, const Wanted& files_current, const Wanted& wanted)
    -> ActionResult {
  const auto execute = [&]() -> ActionResult {
    const auto from = impl_->begin(&wanted);
    const Impl::Scope scope{*impl_};
    const auto& config = impl_->config;
    const auto endpoint = restart_machine_position(point);
    const double clearance = restart_clearance(point);
    const auto read = [&]() { return status(); };
    const auto idle = [&](const NmlStatus& s) {
      return restart_frame(s, point, inputs) && restart_idle(s, config);
    };
    const auto in_context = [&](const EMC_STAT& raw) {
      return restart_frame(to_status(raw, config, impl_->hal, impl_->status.link()), point, inputs) &&
             raw.motion.traj.enabled && !raw.motion.external_offsets_applied &&
             !raw.motion.on_soft_limit;
    };
    const auto wait = [&](std::chrono::milliseconds timeout, const auto& ready,
                          std::string_view reason) -> ActionResult {
      bool done = false;
      static_cast<void>(until(timeout, [&] {
        impl_->collect();
        const auto* raw = impl_->stat();
        if (!impl_->still() || raw == nullptr || !in_context(*raw) || impl_->error_since(from))
          return true;
        done = ready(*raw);
        return done;
      }));
      if (!impl_->still())
        return impl_->withdraw();
      return done ? ActionResult{} : impl_->failure(from, reason);
    };
    const auto mode = [&](auto target) -> ActionResult {
      EMC_TASK_SET_MODE message;
      message.mode = target;
      if (auto sent = impl_->send(message); !accepted(sent))
        return sent;
      return wait(settle_timeout, [&](const EMC_STAT& s) { return s.task.mode == target; },
                  "LinuxCNC did not change mode for the restart.");
    };
    const auto send_mdi = [&](std::string_view block) -> ActionResult {
      EMC_TASK_PLAN_EXECUTE message;
      if (!put(message.command, block))
        return failed("The restart block is too long for LinuxCNC.",
                      MessageKey{"backend.nmlPort.restartBlockTooLong"});
      auto sent = impl_->send(message);
      if (accepted(sent) && impl_->loads)
        ++impl_->loads->mdi_blocks;
      return sent;
    };
    const auto complete = [](const EMC_STAT& s) {
      return s.status == RCS_DONE && s.motion.traj.inpos && s.task.interpState == EMC_TASK_INTERP_IDLE;
    };
    const auto block = [&](std::string_view text, std::chrono::milliseconds timeout)
        -> ActionResult {
      if (auto sent = send_mdi(text); !accepted(sent))
        return sent;
      return wait(timeout, [&](const EMC_STAT& s) {
        return s.echo_serial_number == impl_->serial && complete(s);
      }, "LinuxCNC did not complete the restart approach.");
    };
    const auto current_tool = [&] {
      const auto table = impl_->read_tool_table();
      const auto length = table ? tool_z(*table, point.h) : std::nullopt;
      return point.h == 0 || (length &&
          std::abs(*length * config.millimetres_per_unit - point.tool_offset[2]) <= 0.001);
    };
    // Task writes its parameter file when it synchronises its interpreter (Interp::synch). A
    // G28.1, G10 or `#n=` given in MDI is in the file only from the next synchronisation, and
    // a restart in MDI mode does not switch modes. So task is made to synchronise, and the
    // files are compared again, before the restart moves anything. The synch is the one of
    // idle maintenance (Impl::plan_synch): it gives way to Stop and E-stop.
    const auto synched = [&]() -> ActionResult {
      const auto synch = impl_->plan_synch();
      if (synch == Impl::Synch::given_way)
        return impl_->withdraw();
      if (synch != Impl::Synch::done)
        return impl_->failure(from, "LinuxCNC did not save its parameters for the restart.");
      if (!files_current())
        return failed("The machine's saved parameters changed after the restart was reviewed. "
                      "Choose the line again.",
                      MessageKey{"backend.nmlPort.restartParametersChanged"});
      return {};
    };
    if (!files_current() || !current_tool())
      return failed("The restart program or tool length changed.",
                    MessageKey{"backend.nmlPort.restartChanged"});
    if (after_change) {
      if (auto done = wait(completion_timeout, [&](const EMC_STAT& s) {
            return complete(s) && s.io.tool.toolInSpindle == point.tool;
          }, "LinuxCNC did not finish loading the restart tool."); !accepted(done))
        return done;
      if (auto current = synched(); !accepted(current))
        return current;
    } else {
      const auto before = read();
      if (!idle(before) || before.controller.tool_in_spindle != inputs.start.tool_in_spindle ||
          !restart_at(before, inputs.start.position, config.motion_tolerances.restart_position_mm) ||
          before.commanded_position[2] > clearance + restart_command_tolerance)
        return failed("The machine moved after the restart was reviewed.",
                      MessageKey{"backend.nmlPort.movedAfterReview"});
      if (before.controller.mode != TaskMode::mdi)
        if (auto switched = mode(EMC_TASK_MODE_MDI); !accepted(switched))
          return switched;
      if (auto current = synched(); !accepted(current))
        return current;
      if (auto done = block("M5 M9", completion_timeout); !accepted(done))
        return done;
      if (config.signals.spindle_rpm) {
        if (auto stopped = wait(
                std::chrono::seconds{15},
                [&](const EMC_STAT& raw) {
                  const auto s = to_status(raw, config, impl_->hal, impl_->status.link());
                  return s.spindle_actual_rpm && *s.spindle_actual_rpm < 1.0;
                },
                "The spindle did not stop before the restart approach.");
            !accepted(stopped))
          return stopped;
      }
      if (auto done = block("G21 G90 G94 G97 G40 G80 G61.1", completion_timeout); !accepted(done))
        return done;
      const auto now = read();
      if (!idle(now) || now.controller.spindle_active ||
          !restart_at(now, inputs.start.position, config.motion_tolerances.restart_position_mm) ||
          now.commanded_position[2] > clearance + restart_command_tolerance)
        return failed("The spindle or approach position changed before retracting.",
                      MessageKey{"backend.nmlPort.changedBeforeRetract"});
      if (auto done = block(std::format("G53 G0 Z{:.8f}", clearance),
                            std::chrono::seconds{45}); !accepted(done))
        return done;
      // The trajectory reports completion before the individual joints can report in-position.
      if (auto lifted = wait(settle_timeout, [&](const EMC_STAT& raw) {
            const auto settled = to_status(raw, config, impl_->hal, impl_->status.link());
            return idle(settled) &&
                   restart_at_z(settled, clearance, config.motion_tolerances.restart_position_mm);
          }, "The machine did not reach the confirmed Z clearance.");
          !accepted(lifted))
        return lifted;
      if (point.tool != inputs.start.tool_in_spindle) {
        if (auto sent = send_mdi(std::format("T{} M6", point.tool)); !accepted(sent))
          return sent;
        return wait(completion_timeout, [&](const EMC_STAT& raw) {
          const auto s = to_status(raw, config, impl_->hal, impl_->status.link());
          return s.controller.requested_tool == point.tool;
        }, "LinuxCNC did not request the required tool change.");
      }
    }
    const auto loaded = read();
    if (!idle(loaded) || loaded.controller.mode != TaskMode::mdi ||
        loaded.controller.tool_in_spindle != point.tool ||
        !restart_at_z(loaded, clearance, config.motion_tolerances.restart_position_mm))
      return failed("The tool or clearance changed before the restart approach.",
                    MessageKey{"backend.nmlPort.changedBeforeApproach"});
    if (auto done = block(point.h == 0 ? "G49" : std::format("G43 H{}", point.h),
                          completion_timeout); !accepted(done))
      return done;
    const auto compensated = read();
    if (!idle(compensated) || !restart_at_z(compensated, clearance, config.motion_tolerances.restart_position_mm) ||
        !restart_near(compensated.tool_offset, point.tool_offset) ||
        compensated.work[2] < point.clearance_z - config.motion_tolerances.restart_position_mm)
      return failed("LinuxCNC did not confirm the tool offset and Z clearance.",
                    MessageKey{"backend.nmlPort.offsetUnconfirmed"});
    if (auto done = block(std::format("G53 G0 X{:.8f} Y{:.8f}", endpoint[0], endpoint[1]),
                          std::chrono::seconds{45}); !accepted(done))
      return done;
    const auto spindle = point.spindle_direction == SpindleDirection::forward   ? "M3"
                         : point.spindle_direction == SpindleDirection::reverse ? "M4"
                                                                               : "M5";
    if (auto done = block(std::format("S{:.8f} {}{}{}", point.spindle_rpm, spindle,
                                      point.mist ? " M7" : "", point.flood ? " M8" : ""),
                          completion_timeout); !accepted(done))
      return done;
    const auto at_clearance = read();
    const Vec3 above{endpoint[0], endpoint[1], clearance};
    if (!idle(at_clearance) || !restart_at(at_clearance, above, config.motion_tolerances.restart_position_mm) ||
        at_clearance.spindle_direction != point.spindle_direction ||
        at_clearance.flood != point.flood || at_clearance.mist != point.mist)
      return failed("LinuxCNC did not confirm the restart position, spindle or coolant.",
                    MessageKey{"backend.nmlPort.restartPositionUnconfirmed"});
    if (clearance > endpoint[2] + 1e-6) {
      const double feed = point.approach_feed;
      const auto bound = std::clamp((clearance - endpoint[2]) / feed * 120000.0 + 5000.0,
                                    5000.0, 180000.0);
      if (auto done = block(std::format("G53 G1 Z{:.8f} F{:.8f}", endpoint[2], feed),
                            std::chrono::milliseconds{static_cast<int>(bound)}); !accepted(done))
        return done;
    }
    const bool mm = point.units == LengthUnit::mm;
    const double unit = mm ? 1.0 : 25.4;
    const auto path = point.path_control == 610 ? "G61"
                      : point.path_control == 611
                          ? "G61.1"
                          : std::format("G64 P{:.8f} Q{:.8f}", point.path_tolerance / unit,
                                        point.naivecam_tolerance / unit);
    if (auto done = block(std::format("G{} G{} G{} G{} {}", mm ? 21 : 20, point.plane,
                                      point.incremental ? 91 : 90,
                                      point.absolute_arc ? "90.1" : "91.1", path),
                          completion_timeout);
        !accepted(done))
      return done;
    const auto motion = point.motion == 0                      ? "G0"
                        : point.motion == 10 && point.feed > 0 ? "G1"
                                                               : "G80";
    if (auto done = block(std::format("{} F{:.8f}", motion, point.feed / unit), completion_timeout);
        !accepted(done))
      return done;
    if (!files_current() || !current_tool())
      return failed("The restart program or tool length changed during the approach.",
                    MessageKey{"backend.nmlPort.changedDuringApproach"});
    const auto at_start = read();
    if (!idle(at_start) ||
        !restart_at(at_start, endpoint, config.motion_tolerances.restart_position_mm) ||
        !restart_near(at_start.tool_offset, point.tool_offset))
      return failed("The machine did not reach the verified restart point.",
                    MessageKey{"backend.nmlPort.restartPointNotReached"});
    if (auto switched = mode(EMC_TASK_MODE_AUTO); !accepted(switched))
      return switched;
    EMC_TASK_PLAN_CLOSE close;
    if (auto closed = impl_->run(close, from); !accepted(closed))
      return closed;
    EMC_TASK_PLAN_OPEN open;
    if (!put(open.file, *inputs.program_file))
      return failed("The program path is too long for LinuxCNC.",
                    MessageKey{"backend.nmlPort.pathTooLong"});
    if (auto opened = impl_->run(open, from); !accepted(opened))
      return opened;
    if (!files_current())
      return failed("The program changed before LinuxCNC could restart it.",
                    MessageKey{"backend.nmlPort.changedBeforeRestart"});
    EMC_TASK_PLAN_RUN run;
    run.line = point.line;
    return impl_->start(run, from);
  };
  auto result = execute();
  if (!accepted(result) && (after_change || !impl_->written_types.empty())) {
    static_cast<void>(abort());
    static_cast<void>(set_spindle(SpindleDirection::stopped, 0, {}));
    static_cast<void>(set_coolant(false, false, {}));
  }
  return result;
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

auto NmlPort::jog_paused_z(double distance, bool stop_spindle, const Wanted& wanted)
    -> ActionResult {
  return move_paused_z(distance, false, wanted, stop_spindle);
}

auto NmlPort::move_paused_z(double distance, bool returning, const Wanted& wanted,
                            bool stop_spindle) -> ActionResult {
  const auto& limits = impl_->config.paused_z;
  auto* hal = impl_->hal;
  const auto start = status();
  const auto& z = start.controller.paused_z;
  if (!wanted())
    return withdrawn();
  if (!limits || !hal || !z || !start.paused_z_feedback)
    return failed("Paused Z feedback is unavailable.",
                  MessageKey{"backend.nmlPort.pausedZFeedback"});
  if (stop_spindle && (returning || distance <= 0 || z->spindle != PausedSpindle::can_stop))
    return failed("The paused spindle state changed. Review the controls and try again.",
                  MessageKey{"messages.policy.pausedSpindleChanged"});
  if (!stop_spindle && !z->reason.empty())
    return failed(z->reason);
  if (!std::isfinite(distance) ||
      (!returning && (distance == 0 || std::abs(distance) > limits->lift_mm)))
    return failed("The paused Z jog distance is out of range.",
                  MessageKey{"messages.policy.pausedZDistance"});

  const double count = limits->count_mm;
  const double tolerance = count / 2;
  const auto delta = returning ? 0 : static_cast<std::int32_t>(std::round(distance / count));
  const double target = returning ? 0 : z->offset + delta * count;
  if (target < -tolerance || target > limits->lift_mm + 1e-9 || (!returning && delta == 0))
    return failed("Z must stay between the paused position and the configured lift limit.",
                  MessageKey{"backend.nmlPort.pausedZBetween"});
  const auto& axes = start.controller.axes;
  const double base_z = axes[2].machine - z->offset;
  const bool clear = std::abs(target) <= tolerance;
  const double end_z = base_z + (clear ? 0 : target);
  if (!std::isfinite(end_z) || !std::isfinite(axes[2].min_limit) ||
      !std::isfinite(axes[2].max_limit) || end_z < axes[2].min_limit || end_z > axes[2].max_limit)
    return failed("The requested lift would cross a Z travel limit.",
                  MessageKey{"backend.nmlPort.liftCrossesLimit"});
  if (stop_spindle) {
    auto held = hold_paused_spindle(start, wanted);
    if (!accepted(held))
      return held;
  }
  if (!hal->begin_paused_z(Clock::now()))
    return failed("The paused Z motion interlock is open.",
                  MessageKey{"backend.nmlStatus.interlockOpen"});
  const struct Permit {
    HalShared& hal;
    ~Permit() { hal.stop_paused_z(); }
  } permit{*hal};

  auto stopped = failed("Paused Z motion stopped. Check the Z position before continuing.",
                        MessageKey{"backend.nmlPort.pausedZStopped"});
  const auto safe = [&](const NmlStatus& now) {
    return wanted() && now.controller.paused_z && now.paused_z_feedback &&
           now.paused_z_interlock.empty() && now.program_file == start.program_file &&
           now.current_line == start.current_line;
  };
  // Let the realtime gate open before changing the counts. LinuxCNC samples counts even
  // with the gate closed, so changing the enable and counts in the same cycle can lose a step.
  constexpr auto gate_settle = std::chrono::milliseconds{40};
  const auto enable_at = Clock::now();
  bool interrupted = false;
  until(gate_settle, [&] {
    interrupted = !safe(status()) || !hal->renew_paused_z(Clock::now());
    return interrupted || Clock::now() - enable_at >= gate_settle;
  });
  if (interrupted || !hal->move_paused_z(Clock::now(), delta, clear))
    return stopped;

  // Include acceleration and allow for the slower tail at either end of the move.
  using Seconds = std::chrono::duration<double>;
  const auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(
      Seconds{std::min(120.0, std::abs(end_z - axes[2].machine) / limits->velocity_mm_s +
                                  limits->velocity_mm_s / limits->acceleration_mm_s2 + 2.0)});
  const auto arrived = [&](const NmlStatus& now) {
    const auto& current = now.controller.axes;
    return now.paused_z_still &&
           std::abs(now.paused_z_feedback->offset - (clear ? 0 : target)) <= tolerance &&
           std::abs(current[0].machine - axes[0].machine) <= 0.01 &&
           std::abs(current[1].machine - axes[1].machine) <= 0.01 &&
           std::abs(current[2].machine - end_z) <= 0.01 &&
           (!clear || !now.controller.paused_z->displaced);
  };
  const bool finished = until(timeout, [&] {
    const auto now = status();
    interrupted = !safe(now) || !hal->renew_paused_z(Clock::now());
    return interrupted || arrived(now);
  });
  hal->stop_paused_z();
  if (interrupted)
    return stopped;
  if (!finished)
    return failed(
        "LinuxCNC did not confirm the requested Z position. Check the axis and HAL wiring.",
        MessageKey{"backend.nmlPort.zUnconfirmed"});
  std::this_thread::sleep_for(gate_settle);
  const auto now = status();
  if (!safe(now) || !arrived(now))
    return stopped;
  if (clear) {
    // LinuxCNC samples count deltas with the gate closed but does not apply them. Leave the
    // gate closed for a full control interval on either side of resetting the baseline.
    if (!hal->rebase_paused_z_counts())
      return stopped;
    std::this_thread::sleep_for(gate_settle);
    const auto rebased = status();
    if (!safe(rebased) || !arrived(rebased))
      return stopped;
  }
  return {};
}

auto NmlPort::hold_paused_spindle(const NmlStatus& start, const Wanted& wanted) -> ActionResult {
  auto hold = Impl::SpindleHold{
      start.program_file,      start.current_line, start.controller.tool_in_spindle,
      start.spindle_direction, start.spindle_rpm,  restart_position(start.controller)};
  if (!wanted() || hold.file.empty() || !hold.matches(status()))
    return failed("The paused program changed before stopping the spindle.",
                  MessageKey{"backend.nmlPort.changedBeforeSpindleStop"});
  // After a restart there is no trustworthy in-memory pause identity. Keep only a recovery
  // marker on disk, written before changing the spindle.
  if (!write_sidecar(impl_->spindle_hold_marker, "CoffeeCNC interrupted spindle hold\n"))
    return failed("Could not save spindle hold recovery state.",
                  MessageKey{"backend.nmlPort.spindleHoldNotSaved"});
  impl_->hal->paused_spindle.store(PausedSpindle::stopping);
  auto stopped = spindle_command(SpindleDirection::stopped, 0, wanted, true);
  if (!accepted(stopped)) {
    impl_->spindle_recovery();
    return stopped;
  }
  bool interrupted = false;
  const bool verified = until(std::chrono::seconds{15}, [&] {
    const auto now = status();
    interrupted = !wanted() || !hold.matches(now);
    return interrupted || (now.spindle_actual_rpm && !now.controller.spindle_active);
  });
  if (interrupted || !verified) {
    impl_->spindle_recovery();
    return failed("Spindle stop was not verified. Press Stop before continuing.",
                  MessageKey{"backend.nmlPort.spindleStopUnverified"});
  }
  impl_->spindle_hold = std::move(hold);
  impl_->hal->paused_spindle.store(PausedSpindle::restore);
  return {};
}

auto NmlPort::restore_paused_spindle(const Wanted& wanted) -> ActionResult {
  const auto& hold = impl_->spindle_hold;
  const auto before = status();
  if (!hold || !wanted() || !hold->at_base(before) || before.controller.spindle_active)
    return failed("Return Z and verify the stopped spindle before continuing.",
                  MessageKey{"backend.nmlPort.returnZ"});
  auto started = spindle_command(hold->direction, hold->rpm, wanted, true);
  bool interrupted = false;
  const bool at_speed =
      accepted(started) && until(std::chrono::seconds{15}, [&] {
        const auto now = status();
        interrupted = !wanted() || !hold->at_base(now);
        return interrupted || hold->at_speed(now);
      });
  if (at_speed && !interrupted)
    return {};
  impl_->spindle_recovery();
  static_cast<void>(spindle_command(SpindleDirection::stopped, 0, {}, true));
  return failed("Spindle restart was not verified. Press Stop before continuing.",
                MessageKey{"backend.nmlPort.spindleRestartUnverified"});
}

void NmlPort::recover_paused_spindle(const Wanted& wanted) {
  const auto now = status();
  if (now.controller.link != Link::live || !now.spindle_actual_rpm || !wanted())
    return;
  if (now.controller.interp == InterpState::idle && !now.controller.spindle_active) {
    impl_->finish_spindle_hold();
    return;
  }
  if (const auto& hold = impl_->spindle_hold; hold && hold->matches(now))
    return;
  impl_->spindle_recovery();
  if (now.controller.spindle_active && Clock::now() >= impl_->spindle_hold_retry) {
    impl_->spindle_hold_retry = Clock::now() + std::chrono::seconds{1};
    // A crash can happen just after PLAN_RESUME. Abort the program before removing torque.
    if (now.controller.interp != InterpState::idle && !now.paused)
      static_cast<void>(abort());
    else
      static_cast<void>(spindle_command(SpindleDirection::stopped, 0, {}, true));
  }
}

auto NmlPort::resume(const Wanted& wanted) -> ActionResult {
  if (!impl_->breakpoint && status().controller.breakpoint_program)
    return failed("The breakpoint source is unavailable. Stop and reopen the original program.",
                  MessageKey{"backend.nmlPort.breakpointSource"});
  if (impl_->breakpoint_recovered && impl_->breakpoint && status().controller.breakpoint_program) {
    // After an app restart the panel loaded the source again. It must still agree with the
    // execution copy before the operator can resume a recovered pause.
    constexpr std::size_t limit = (std::size_t{16} << 20) + 256;
    const auto original = read_text_file(impl_->breakpoint->original, limit);
    const auto saved = read_text_file(impl_->breakpoint->temporary, limit);
    const auto matches = [&] {
      if (!original || !saved)
        return false;
      const auto expected =
          with_breakpoint(*original, impl_->breakpoint->line, impl_->breakpoint->receipt);
      return expected && *expected == *saved;
    };
    if (!matches())
      return failed("The original program changed. Stop and reopen it before running again.",
                    MessageKey{"backend.nmlPort.originalChanged"});
    impl_->breakpoint_recovered = false;
  }
  const auto before = status();
  if (impl_->config.paused_z && (before.controller.link != Link::live || !before.paused_z_feedback))
    return failed("Paused Z feedback is unavailable. Resume could not check the Z position.",
                  MessageKey{"backend.nmlPort.resumeNoFeedback"});
  if (before.controller.paused_z && before.controller.paused_z->spindle == PausedSpindle::recover)
    return failed("Press Stop before continuing after an interrupted spindle hold.",
                  MessageKey{"messages.policy.spindleHoldInterrupted"});
  if (before.controller.paused_z && before.controller.paused_z->displaced) {
    auto returned = move_paused_z(0, true, wanted);
    if (!accepted(returned))
      return returned;
  }
  const bool restoring = impl_->spindle_hold.has_value();
  if (restoring) {
    auto at_speed = restore_paused_spindle(wanted);
    if (!accepted(at_speed))
      return at_speed;
  }
  const bool at_breakpoint = impl_->breakpoint && impl_->breakpoint->marker_seen.load();
  const auto resume_program = [&]() -> ActionResult {
    if (restoring && !impl_->spindle_hold->at_speed(status()))
      return failed("Spindle speed or the paused position changed before Resume.",
                    MessageKey{"backend.nmlPort.changedBeforeResume"});
    const auto from = impl_->begin(&wanted);
    const Impl::Scope scope{*impl_};
    if (!impl_->still())
      return withdrawn();
    EMC_TASK_PLAN_RESUME message;
    if (auto sent = impl_->send(message); !accepted(sent))
      return sent;
    return impl_->settled(from, "LinuxCNC did not resume.", [&](const EMC_STAT& now) {
      return now.task.task_paused == 0 ||
             (!at_breakpoint && impl_->breakpoint && impl_->breakpoint->marker_seen.load());
    });
  };
  auto resumed = resume_program();
  if (restoring) {
    if (accepted(resumed)) {
      impl_->finish_spindle_hold();
    } else {
      impl_->spindle_recovery();
      static_cast<void>(abort());
    }
  }
  if (accepted(resumed) && at_breakpoint && impl_->breakpoint->marker_seen.exchange(false) &&
      !impl_->save_breakpoint())
    impl_->messages.push_back({true, "Could not save that the breakpoint has resumed.",
                               MessageKey{"backend.nmlPort.breakpointResumeNotSaved"}});
  return resumed;
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
  if (impl_->loads)
    ++impl_->loads->aborts;
  EMC_TASK_ABORT message;
  return impl_->run(message, from);
}

// Task applies an override as soon as it reads it, but while a program runs its status is the
// program's (RCS_EXEC), so waiting for the command to finish timed out after 3 s and dropped every
// slider move meanwhile (turo: "feed override does not work", dragging to 200% errored). Like a
// jog, it is only passed on: echoed, and refused only by an operator error.
auto NmlPort::set_override(Override which, double value, const Wanted& wanted) -> ActionResult {
  const auto from = impl_->begin(&wanted);
  const Impl::Scope scope{*impl_};
  if (!impl_->still())
    return withdrawn();
  switch (which) {
  case Override::feed: {
    EMC_TRAJ_SET_SCALE message;
    message.scale = value;
    return impl_->pass(message, from);
  }
  case Override::rapid: {
    EMC_TRAJ_SET_RAPID_SCALE message;
    message.scale = value;
    return impl_->pass(message, from);
  }
  case Override::velocity_limit: {
    const auto maximum = impl_->config.limits.max_trajectory_velocity;
    if (!maximum)
      return failed("Maximum velocity control is unavailable on this machine.",
                    MessageKey{"messages.policy.velocityLimitUnavailable"});
    if (!std::isfinite(value) || value < 0 || value > 1)
      return failed("Maximum velocity limit is out of range.",
                    MessageKey{"messages.policy.velocityLimitRange"});
    EMC_TRAJ_SET_MAX_VELOCITY message;
    message.velocity = value * *maximum / 60 / impl_->config.millimetres_per_unit;
    return impl_->pass(message, from);
  }
  case Override::spindle:
    break;
  }
  EMC_TRAJ_SET_SPINDLE_SCALE message;
  message.spindle = 0;
  message.scale = value;
  return impl_->pass(message, from);
}

auto NmlPort::set_spindle(SpindleDirection direction, double rpm, const Wanted& wanted)
    -> ActionResult {
  return spindle_command(direction, rpm, wanted, false);
}

auto NmlPort::spindle_command(SpindleDirection direction, double rpm, const Wanted& wanted,
                              bool during_pause) -> ActionResult {
  // A stop already sent is never withdrawn. A confirmed stop-and-lift can be cancelled before
  // the spindle command is sent, just like the lift itself.
  const auto from = impl_->begin(
      wanted && (during_pause || direction != SpindleDirection::stopped) ? &wanted : nullptr);
  const Impl::Scope scope{*impl_};
  if (!impl_->still())
    return withdrawn();
  if (direction == SpindleDirection::stopped) {
    EMC_SPINDLE_OFF message;
    message.spindle = 0;
    return during_pause ? impl_->pass(message, from) : impl_->run(message, from);
  }
  EMC_SPINDLE_ON message;
  message.spindle = 0;
  // Reverse is a negative speed.
  message.speed = direction == SpindleDirection::reverse ? -rpm : rpm;
  message.factor = 0;
  message.xoffset = 0;
  message.wait_for_spindle_at_speed = 1;
  return during_pause ? impl_->pass(message, from) : impl_->run(message, from);
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

auto NmlPort::tool_length_in_effect() -> std::expected<ToolLengthInEffect, std::string> {
  const auto current = status();
  if (current.controller.link != Link::live)
    return std::unexpected(std::string("The tool length offset in effect cannot be read."));
  ToolLengthInEffect effect;
  effect.offset = current.tool_offset;
  const int tool = current.controller.tool_in_spindle;
  if (tool <= 0)
    return effect;
  const auto table = impl_->read_tool_table();
  if (!table)
    return std::unexpected(table.error());
  if (const auto length = tool_z(*table, tool))
    effect.loaded_length = *length * impl_->config.millimetres_per_unit;
  return effect;
}

auto NmlPort::set_tool(const Tool& tool, const Wanted& wanted) -> ActionResult {
  static_cast<void>(impl_->begin(&wanted));
  const Impl::Scope scope{*impl_};
  if (!impl_->still())
    return withdrawn();
  const auto table = impl_->read_tool_table();
  if (!table)
    return failed(table.error());
  const auto edited = with_tool(*table, tool, impl_->config.millimetres_per_unit);
  return edited ? impl_->load_tool_table(*edited) : failed(edited.error());
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
