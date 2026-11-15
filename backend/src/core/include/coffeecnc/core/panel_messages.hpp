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

// Sent once after the panel's `ready`: what does not change while the machine runs.
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
  friend auto operator==(const AxisStatus&, const AxisStatus&) -> bool = default;
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

struct ProgramStatus {
  std::optional<std::string> file; // path of the loaded program
  // Changes whenever a program is loaded or reloaded; matches ProgramText::revision.
  std::uint64_t revision{};
  int current_line{}; // 1-based, 0 when none; reads ahead of motion
  int motion_line{};  // 1-based, 0 when none
  bool optional_stop{};
  bool block_delete{};
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

struct ProgramText {
  std::uint64_t revision{};
  std::string file;
  std::vector<std::string> lines;
  // The file is longer than `lines`. Always sent.
  bool truncated{};
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
  friend auto operator==(const PreviewSegment&, const PreviewSegment&) -> bool = default;
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

struct Preview {
  // The Status::preview_context this preview was computed for.
  std::uint64_t context{};
  PreviewState state{PreviewState::unavailable};
  std::optional<PreviewError> error;
  std::vector<PreviewSegment> segments;
  std::optional<Bounds> bounds; // extent of the feed moves; empty without any
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
