#ifndef COFFEECNC_LINUXCNC_PREVIEW_WIRE_HPP
#define COFFEECNC_LINUXCNC_PREVIEW_WIRE_HPP

#include <coffeecnc/core/preview.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace coffeecnc {

// What passes between the executable and its preview helper process (coffeecnc-preview): the
// request file the helper is started with and the text it writes to its standard output. Pure:
// no LinuxCNC, no I/O. Both ends are built from this file.

// One entry of the loaded tool table as LinuxCNC holds it, in machine units. Index 0 is the
// spindle.
struct HelperTool {
  int index{};
  int number{};
  int pocket{};
  Vec3 offset{};
  double diameter{};
  double front_angle{};
  double back_angle{};
  int orientation{};
  friend auto operator==(const HelperTool&, const HelperTool&) -> bool = default;
};

// Request, one `key value...` line each; lengths in millimetres unless said otherwise.
struct HelperRequest {
  std::string program;    // the private copy to interpret
  // The directory of the private subroutine copies: the helper scans every file in it
  // before its interpreter reads anything. Empty when there is none.
  std::string subroutines;
  std::string parameters; // the private parameter file; the interpreter writes it at M2
  double units_per_mm{1}; // machine units per millimetre: 1, or 1/25.4
  // Where the first move starts: the controlled point in machine coordinates.
  Vec3 position{};
  int tool_in_spindle{};
  int tool_prepared{-1}; // tool number selected by a T word and not changed to; -1 for none
  Vec3 tool_offset{};    // in effect at the start
  double feed{};         // mm/min
  bool block_delete{};
  int wcs{1}; // the active system
  // The work systems whose offsets the parameter file holds, bit `wcs - 1` each; the active
  // one is always known. A move in another one ends the preview.
  unsigned known_systems{};
  // Whether the parameter file holds the G28 and the G30 position; without one, a G28 or G30
  // ends the preview.
  bool saved_g28{};
  bool saved_g30{};
  // [EMCIO] TOOL_CHANGE_AT_G30: a tool change then moves to a saved position.
  bool tool_change_at_g30{};
  // The codes [RS274NGC] REMAP names, lower case ("m6", "t"): a block that uses one ends the
  // preview.
  std::vector<std::string> remapped;
  // [EMCIO] TOOL_CHANGE_POSITION: task moves there before a tool change. Machine coordinates.
  std::optional<Vec3> tool_change_position;
  std::size_t max_points{400000};
  double chord_tolerance{0.01};
  std::optional<RigidTapLimits> rigid_tap{};
  // Blocks that put the interpreter in task's modal state, in this order: "G21", "G91", "G1".
  std::vector<std::string> codes;
  // The motion mode task is in when it is one that cannot be put in place by a block ("G38.2",
  // "G2", "G81"); empty otherwise. The helper's interpreter is then in G80, and a block that
  // would move by the mode it inherits ends the preview.
  std::string inherited_motion;
  std::vector<HelperTool> tools;
  std::optional<int> restart_line;
  friend auto operator==(const HelperRequest&, const HelperRequest&) -> bool = default;
};

[[nodiscard]] auto write_request(const HelperRequest& request) -> std::string;
[[nodiscard]] auto parse_request(std::string_view text) -> std::expected<HelperRequest, std::string>;

// Output: text lines, each complete in itself, so that what arrived before the helper was
// ended can be used.
//
//   seg <line> r|f <tool> <x> <y> <z> <x> <y> <z> ...   one run of motion: tool tip, machine
//                                                       coordinates, mm
//   error <line> <text>     the interpreter's error; nothing follows but `end`
//   stop <line> <text>      the path after this line depends on the machine
//   stop_known <line> <id> [argument] [source-line file]
//                          a stop code, with a number (v2), source (v3), or token (v4 onward)
//   cut <line> <text>       a limit was reached
//   restart_refused <id>   a CoffeeCNC restart refusal; old helpers send restart_blocked <text>
//   end ok|error|stop|cut   the last line of a helper that ended by itself
//
// `line` is the line of the program (not of a subroutine file) that was being executed.
enum class HelperEnd : std::uint8_t {
  missing, // the helper was ended or died before it wrote `end`
  ok,
  error,
  stop,
  cut,
};

// The numbers are written by the helper. Keep assigned values stable.
enum class RestartRefusal : std::uint8_t {
  none = 0,
  cutter_compensation = 1,
  canned_cycle = 2,
  feed_spindle_mode = 3,
  no_motion = 4,
  move_kind = 5,
  prior_motion = 6,
  explicit_arc = 7,
  skipped_effects = 8,
  frame_or_tool = 9,
  incomplete_state = 10,
  length_inactive = 11,
  length_mismatch = 12,
  subroutines = 13,
  tool_preselection = 14,
  rigid_tap = 15,
  not_executed = 16,
};

// The caller opts the helper into refusal codes. Older callers expect the text record.
inline constexpr char preview_restart_refusals_env[] = "COFFEECNC_PREVIEW_RESTART_REFUSALS";

// Stable reason IDs sent by the preview helper on request.
enum class KnownPreviewStop : std::uint8_t {
  nurbs = 1,
  probe_move = 2,
  wait_input = 3,
  read_input = 4,
  tool_change_g30 = 5,
  saved_g28 = 6,
  saved_g30 = 7,
  unknown_work_system = 8,
  machine_m_code = 9,
  computed_parameter = 10,
  prior_return_value = 11,
  undefined_parameter = 12,
  parameter_exists = 13,
  inherited_motion = 14,
  remapped_code = 15,
  cross_scope_o_word = 16,
  task_parameter = 17,
  ini_or_hal_parameter = 18,
  global_parameter_exists = 19,
  computed_o_word = 20,
  computed_g_word = 21,
  restore_g92 = 22,
  python_line = 23,
  python_comment = 24,
  file_writing_comment = 25,
  unsafe_o_word_name = 26,
};
using PreviewStopArgument = std::variant<std::monostate, int, std::string>;

struct PreviewStopReason {
  KnownPreviewStop reason;
  PreviewStopArgument argument{};
  friend auto operator==(const PreviewStopReason&, const PreviewStopReason&) -> bool = default;
};

[[nodiscard]] auto preview_stop_message(KnownPreviewStop reason,
                                        const PreviewStopArgument& argument) -> LocalizedReason;
// Opt in to the stop formats by version. Callers without the relevant version receive text.
inline constexpr char preview_stop_codes_env[] = "COFFEECNC_PREVIEW_STOP_CODES";
inline constexpr std::string_view preview_stop_codes_version = "5";

struct PreviewStopSource {
  std::string file;
  int line{};
  friend auto operator==(const PreviewStopSource&, const PreviewStopSource&) -> bool = default;
};

// No source means the main program. An empty file is a subroutine whose location is unknown.
[[nodiscard]] auto preview_stop_prefix(const std::optional<PreviewStopSource>& source)
    -> std::string;
[[nodiscard]] auto write_preview_stop(int line, KnownPreviewStop reason,
                                      const std::optional<PreviewStopSource>& source,
                                      std::string_view version,
                                      const PreviewStopArgument& argument = {}) -> std::string;

struct HelperOutput {
  std::vector<PreviewSegment> segments;
  std::optional<PreviewError> problem; // of error, stop or cut
  std::optional<PreviewStopSource> stop_source; // present for a coded stop in a subroutine
  HelperEnd end{HelperEnd::missing};
  // Why the reading ended before an `end` that fits the records: the text is not what a
  // helper writes. Empty for a result that is whole, and for one that only stops short (a
  // helper that was ended): then `end` is missing and nothing more.
  LocalizedReason defect;
  std::optional<RestartPoint> restart;
  std::string restart_blocked;
  std::optional<MessageKey> restart_blocked_localization{};
  std::vector<PreviewDwell> dwells;
  // Recorded by canonical callbacks, with controlled-point travel and tool-tip work bounds.
  struct ProgramFacts {
    std::vector<int> tools;
    std::vector<ProgramFrameCheck> frames;
    std::optional<Bounds> machine;
    std::optional<ProgramRange> feed;
    std::optional<ProgramRange> spindle;
    bool mm{};
    bool inch{};
    bool variable_spindle{};
    bool feed_per_revolution{};
  };
  std::optional<ProgramFacts> check{};
};

[[nodiscard]] auto write_restart(const RestartPoint& point) -> std::string;
[[nodiscard]] auto write_restart_refusal(RestartRefusal reason, bool coded) -> std::string;
[[nodiscard]] auto write_check(const HelperOutput::ProgramFacts& check) -> std::string;

// What a result may hold at most.
struct OutputLimits {
  std::size_t max_points{400000}; // in all segments together
  int max_line{2000000000};       // the last line of the program
  static constexpr int max_tool{99999};
};

// Reads a result as text of unknown origin: the helper runs a program's G-code, and its
// result is trusted only as far as every record is well formed. Never fails and always ends:
// every step consumes input. Kept are the complete, valid records before the first that is
// not; that first one sets `defect` and ends the reading with `end` missing:
//
//   a line without its newline at the end of the text (cut off): no defect, `end` missing
//   a record of an unknown kind, with a number that is not a finite number, a line outside
//   0..max_line, a tool outside 0..max_tool, a segment of fewer than two points or with a
//   coordinate missing, more points than max_points in all
//   a second error, stop or cut; a segment after one of them
//   an `end` whose reason does not fit the records (`ok` after a problem, `error` without
//   one), and anything at all after `end`
[[nodiscard]] auto parse_output(std::string_view text, const OutputLimits& limits = {})
    -> HelperOutput;

} // namespace coffeecnc

#endif // COFFEECNC_LINUXCNC_PREVIEW_WIRE_HPP
