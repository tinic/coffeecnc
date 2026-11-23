#ifndef COFFEECNC_CORE_PANEL_MESSAGES_HPP
#define COFFEECNC_CORE_PANEL_MESSAGES_HPP

#include <coffeecnc/core/command_policy.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace coffeecnc {

// Everything the panel is told, as the controller adapter fills it. Field names are those of
// protocol v1 (`ui/src/protocol.ts`); the wire text is made by the app layer.
//
// Units: lengths in millimetres, feeds in mm/min, spindle speed in rpm, overrides as factors
// (1.0 is 100 %). Every number must be finite: the encoders refuse a message with one that is
// not. Counters (revisions, contexts, sizes) must not exceed 2^53 - 1, the largest integer the
// panel reads exactly.

inline constexpr int protocol_version = 1;

using Vec3 = std::array<double, 3>; // x, y, z

enum class FeedMode : std::uint8_t { per_minute, inverse_time, per_revolution }; // G94, G93, G95

// Which coolant outputs the machine has: iocontrol's coolant pin drives something in HAL.
struct CoolantOutputs {
  bool flood{};
  bool mist{};
  friend auto operator==(const CoolantOutputs&, const CoolantOutputs&) -> bool = default;
};

// Sent once after the panel's `ready`: what does not change while the machine runs.
// A folder the program browser offers as a shortcut.
struct ProgramPlace {
  std::string name;
  std::string path;
  friend auto operator==(const ProgramPlace&, const ProgramPlace&) -> bool = default;
};

// A button that runs one MDI block: [COFFEECNC] MACRO = <label> | <MDI block>.
struct Macro {
  std::string label;
  std::string mdi;
  friend auto operator==(const Macro&, const Macro&) -> bool = default;
};

struct Hello {
  std::string machine_name;
  // No LinuxCNC is behind the session.
  bool simulated{};
  std::vector<Axis> axes;
  LengthUnit machine_units{LengthUnit::mm};
  std::vector<double> jog_increments; // mm
  double max_jog_velocity{};          // mm/min
  // Fastest jog of each axis, mm/min, by axis_index. Required for every axis in `axes`.
  std::array<std::optional<double>, 3> axis_max_jog_velocity{};
  // The machine refuses programs and MDI until its axes are homed.
  bool homing_required{};
  double max_feed_override{};
  double min_spindle_override{};
  double max_spindle_override{};
  // The valid [COFFEECNC] MACHINE_ID; empty when none is configured or it is not valid. The
  // `tool_models` message says which. `tool_model_storage: 1` is always sent with it.
  std::optional<std::string> machine_id;
  // Not known: the panel offers both.
  std::optional<CoolantOutputs> coolant;
  std::optional<double> max_spindle_rpm;
  std::optional<FixedSetterLimits> fixed_setter{};
  // The tool HAL routes to the spindle touch probe, when the machine declares one.
  std::optional<int> touch_probe_tool{};
  // Shortcuts of the program browser, first the program directory.
  std::vector<ProgramPlace> program_places{};
  // The configured macros, in the order of the INI file. Not sent when there are none.
  std::vector<Macro> macros{};
  friend auto operator==(const Hello&, const Hello&) -> bool = default;
};

struct AxisStatus {
  Axis axis{Axis::x};
  bool homed{};
  bool homing{};
  double machine{}; // actual position, machine coordinates
  double work{};    // actual position in the active work system, G92 and tool offset applied
  double dtg{};     // distance to go in the current move
  double min_limit{};
  double max_limit{};
  bool on_hard_limit{};
  // The axis's reference mark has been found, so homing can go to it. Empty when the machine
  // has no such signal; the field is then not sent.
  std::optional<bool> reference_mark{};
  // The axis stands at its reference mark now. Sent together with reference_mark.
  std::optional<bool> at_reference_mark{};
  friend auto operator==(const AxisStatus&, const AxisStatus&) -> bool = default;
};

struct RotaryAxisStatus {
  char axis{'a'};
  bool homed{};
  bool homing{};
  std::optional<double> machine; // degrees
  std::optional<double> work;
  std::optional<double> dtg;
  bool on_hard_limit{};
  friend auto operator==(const RotaryAxisStatus&, const RotaryAxisStatus&) -> bool = default;
};

struct Offsets {
  int wcs{1}; // 1 is G54 ... 9 is G59.3
  Vec3 g5x{};
  Vec3 g92{};
  Vec3 tool{};          // zero unless G43 is active
  double rotation_xy{}; // degrees
  friend auto operator==(const Offsets&, const Offsets&) -> bool = default;
};

struct MotionStatus {
  double velocity{}; // along the path, mm/min
  FeedMode feed_mode{FeedMode::per_minute};
  double programmed_feed{}; // in the unit of feed_mode: mm/min, 1/min or mm/rev
  double feed_override{};
  double rapid_override{};
  bool in_position{true};
  std::optional<double> velocity_limit{};
  friend auto operator==(const MotionStatus&, const MotionStatus&) -> bool = default;
};

struct SpindleStatus {
  SpindleDirection direction{SpindleDirection::stopped};
  double commanded_rpm{}; // the S word in effect, before override
  double override{};
  // Measured magnitude, never negative. Empty when the machine has no speed feedback; never
  // filled from the commanded speed.
  std::optional<double> actual_rpm;
  // Empty when the machine has no at-speed signal.
  std::optional<bool> at_speed;
  friend auto operator==(const SpindleStatus&, const SpindleStatus&) -> bool = default;
};

struct CoolantStatus {
  bool flood{};
  bool mist{};
  friend auto operator==(const CoolantStatus&, const CoolantStatus&) -> bool = default;
};

struct ToolStatus {
  int in_spindle{};            // 0 for none
  std::optional<int> prepared; // selected by a T word and not yet changed to
  friend auto operator==(const ToolStatus&, const ToolStatus&) -> bool = default;
};

struct RestartHint {
  int line{};
  int tool{};
  friend auto operator==(const RestartHint&, const RestartHint&) -> bool = default;
};

struct ProgramBreakpoint {
  int line{};
  bool reached{};
  friend auto operator==(const ProgramBreakpoint&, const ProgramBreakpoint&) -> bool = default;
};

enum class ProgramCycle : std::uint8_t { idle, running, completed };

struct ProgramStatus {
  std::optional<std::string> file; // path of the loaded program
  // Changes whenever a program is loaded or reloaded; matches ProgramText::revision.
  std::uint64_t revision{};
  int current_line{}; // 1-based, 0 when none; reads ahead of motion
  int motion_line{};  // 1-based, 0 when none
  bool optional_stop{};
  bool block_delete{};
  std::optional<RestartHint> restart_hint{};
  std::optional<ProgramBreakpoint> breakpoint{};
  std::optional<ProgramCycle> cycle{};
  friend auto operator==(const ProgramStatus&, const ProgramStatus&) -> bool = default;
};

// Why a running program is not moving.
struct WaitNone {
  friend auto operator==(const WaitNone&, const WaitNone&) -> bool = default;
};
struct WaitPaused {
  friend auto operator==(const WaitPaused&, const WaitPaused&) -> bool = default;
};
struct WaitToolChange {
  int tool{};
  std::optional<std::uint64_t> measure_request{};
  friend auto operator==(const WaitToolChange&, const WaitToolChange&) -> bool = default;
};
struct WaitDwell {
  double seconds_left{};
  friend auto operator==(const WaitDwell&, const WaitDwell&) -> bool = default;
};
using Wait = std::variant<WaitNone, WaitPaused, WaitToolChange, WaitDwell>;

// `seq` is not here: the sender numbers the statuses it sends.
struct Status {
  // With Link::down every other field is a placeholder; it must still be finite.
  Link link{Link::down};
  MachineState machine{MachineState::estop};
  TaskMode mode{TaskMode::manual};
  InterpState interp{InterpState::idle};
  std::vector<AxisStatus> axes;
  std::vector<RotaryAxisStatus> rotary_axes{};
  Offsets offsets;
  MotionStatus motion;
  SpindleStatus spindle;
  CoolantStatus coolant;
  ToolStatus tool;
  ProgramStatus program;
  // Changes whenever anything a preview depends on changes; see PreviewContext.
  std::uint64_t preview_context{};
  Wait wait;
  std::vector<std::string> active_codes; // "G1", "G54", "G90", "M3"
  LengthUnit program_units{LengthUnit::mm};
  // True while the probe is tripped. Empty when the machine has no probe input.
  std::optional<bool> probe_input;
  std::optional<PausedZStatus> paused_z{};
  friend auto operator==(const Status&, const Status&) -> bool = default;
};

struct ToolTable {
  std::uint64_t revision{};
  std::vector<Tool> tools;
  friend auto operator==(const ToolTable&, const ToolTable&) -> bool = default;
};

struct WorkSystem {
  Vec3 offset{};
  double rotation_xy{}; // degrees
  // Whether offset and rotation are known. False: they are placeholders, zero, and not to be
  // shown. Always sent.
  bool available{};
  friend auto operator==(const WorkSystem&, const WorkSystem&) -> bool = default;
};

struct WorkOffsets {
  std::uint64_t revision{};
  // G54 ... G59.3: system `wcs` is element `wcs - 1`.
  std::array<WorkSystem, 9> systems{};
  friend auto operator==(const WorkOffsets&, const WorkOffsets&) -> bool = default;
};

// What made a change of a work offset or of a tool's length or diameter. The panel's own
// commands are named after the command; `mdi` is the panel's MDI line, `program` a program
// that ran, `outside` anything else (another client, a G10 of a remap, the files edited while
// CoffeeCNC did not run).
enum class HistorySource : std::uint8_t {
  touch_off,         // touch-off on the DRO
  zero,              // the DRO's Zero quick action
  half,              // the DRO's Half quick action
  probe,             // a probing cycle
  tool_setter,       // the fixed tool setter
  tool_touch_off,    // the tool length touch-off
  tool_table,        // the tool table editor
  work_offset_table, // the work offset editor
  mdi,
  program,
  restore,           // a Restore of the history itself
  outside,
};
// `number` of an entry: the work system (1 is G54 ... 9 is G59.3), the tool, or 0 for G92/G52.
enum class HistoryTarget : std::uint8_t { wcs, g92, tool };
enum class HistoryField : std::uint8_t { x, y, z, rotation_xy, length, diameter };

// One field of an entry. No `before`: the value was not known (a system not read before, a
// tool added). No `after`: the tool was removed.
struct HistoryChange {
  HistoryField field{HistoryField::x};
  std::optional<double> before;
  std::optional<double> after;
  friend auto operator==(const HistoryChange&, const HistoryChange&) -> bool = default;
};

struct HistoryEntry {
  std::uint64_t id{};  // goes up by one with every entry, across restarts
  std::int64_t time{}; // milliseconds since the Unix epoch
  HistorySource source{HistorySource::outside};
  HistoryTarget target{HistoryTarget::wcs};
  int number{};
  std::vector<HistoryChange> changes;
  // The entry whose earlier values this one wrote back.
  std::optional<std::uint64_t> restores;
  friend auto operator==(const HistoryEntry&, const HistoryEntry&) -> bool = default;
};

// The `offset_history` message. `saved`: kept in the machine's data directory; else for this
// session only, and `reason` says why. `reason` also says when the saved history could not be
// read or written.
struct OffsetHistory {
  std::uint64_t revision{};
  bool saved{};
  std::string reason;
  std::vector<HistoryEntry> entries; // newest first
  friend auto operator==(const OffsetHistory&, const OffsetHistory&) -> bool = default;
};

struct ProgramText {
  std::uint64_t revision{};
  std::string file;
  std::vector<std::string> lines;
  // The file is longer than `lines`. Always sent.
  bool truncated{};
  // Names the exact bytes read (program_stamp): a save over the program is refused when the
  // file no longer has them. Always sent.
  std::string stamp;
  friend auto operator==(const ProgramText&, const ProgramText&) -> bool = default;
};

enum class PreviewState : std::uint8_t {
  pending, // being computed; no segments
  ready,
  failed,      // carries the interpreter's error; segments end where it stopped
  unavailable, // no program loaded, or no preview possible
};
enum class MoveKind : std::uint8_t { rapid, feed };

// One contiguous run of motion from one source line.
struct PreviewSegment {
  int line{};
  MoveKind kind{MoveKind::rapid};
  int tool{};
  // Tool tip positions in machine coordinates, the tool offset applied exactly once.
  std::vector<Vec3> points;
  // Programmed feed of a feed move, mm/min; 0 for a rapid, or when the feed is not per minute.
  double feed{};
  friend auto operator==(const PreviewSegment&, const PreviewSegment&) -> bool = default;
};

// A dwell (G4, or a canned cycle's P) the program makes at `line`.
struct PreviewDwell {
  int line{};
  double seconds{};
  friend auto operator==(const PreviewDwell&, const PreviewDwell&) -> bool = default;
};

struct PreviewError {
  int line{};
  std::string text;
  friend auto operator==(const PreviewError&, const PreviewError&) -> bool = default;
};

struct Bounds {
  Vec3 min{};
  Vec3 max{};
  friend auto operator==(const Bounds&, const Bounds&) -> bool = default;
};

struct ProgramRange {
  double min{};
  double max{};
  friend auto operator==(const ProgramRange&, const ProgramRange&) -> bool = default;
};

enum class ToolLengthCheck : std::uint8_t { missing, table, touched_off };
struct ProgramToolCheck {
  int tool{};
  ToolLengthCheck length{ToolLengthCheck::missing};
  friend auto operator==(const ProgramToolCheck&, const ProgramToolCheck&) -> bool = default;
};

struct ProgramFrameCheck {
  int wcs{};
  bool available{};
  std::optional<Bounds> bounds{};
  friend auto operator==(const ProgramFrameCheck&, const ProgramFrameCheck&) -> bool = default;
};

enum class TravelCheck : std::uint8_t { unknown, fits, outside };
struct ProgramCheck {
  std::vector<ProgramToolCheck> tools;
  std::vector<ProgramFrameCheck> frames;
  std::optional<ProgramRange> feed;    // mm/min
  std::optional<ProgramRange> spindle; // rpm
  std::vector<LengthUnit> units;
  TravelCheck travel{TravelCheck::unknown};
  std::vector<std::string> errors;
  std::vector<std::string> warnings;
  friend auto operator==(const ProgramCheck&, const ProgramCheck&) -> bool = default;
};

struct Preview {
  // The Status::preview_context this preview was computed for.
  std::uint64_t context{};
  PreviewState state{PreviewState::unavailable};
  std::optional<PreviewError> error;
  std::vector<PreviewSegment> segments;
  std::optional<Bounds> bounds; // extent of the feed moves; empty without any
  std::vector<PreviewDwell> dwells;
  std::optional<ProgramCheck> check{};
  friend auto operator==(const Preview&, const Preview&) -> bool = default;
};

enum class EntryKind : std::uint8_t { directory, file };

struct DirectoryEntry {
  std::string name;
  EntryKind kind{EntryKind::file};
  std::uint64_t bytes{};
  std::int64_t modified{}; // milliseconds since the Unix epoch
  friend auto operator==(const DirectoryEntry&, const DirectoryEntry&) -> bool = default;
};

struct Directory {
  std::string path;
  std::optional<std::string> parent; // empty at the top of the program directory
  std::vector<DirectoryEntry> entries;
  // The directory holds more than `entries`. Always sent.
  bool truncated{};
  friend auto operator==(const Directory&, const Directory&) -> bool = default;
};

} // namespace coffeecnc

#endif // COFFEECNC_CORE_PANEL_MESSAGES_HPP
