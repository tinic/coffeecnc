#ifndef COFFEECNC_LINUXCNC_PREVIEW_WIRE_HPP
#define COFFEECNC_LINUXCNC_PREVIEW_WIRE_HPP

#include <coffeecnc/core/preview.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
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
  int wcs{1}; // the active system; the only one whose offsets are known
  // [EMCIO] TOOL_CHANGE_AT_G30: a tool change then moves to a saved position.
  bool tool_change_at_g30{};
  // [EMCIO] TOOL_CHANGE_POSITION: task moves there before a tool change. Machine coordinates.
  std::optional<Vec3> tool_change_position;
  std::size_t max_points{400000};
  double chord_tolerance{0.01};
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
//   cut <line> <text>       a limit was reached
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

struct HelperOutput {
  std::vector<PreviewSegment> segments;
  std::optional<PreviewError> problem; // of error, stop or cut
  HelperEnd end{HelperEnd::missing};
  // Why the reading ended before an `end` that fits the records: the text is not what a
  // helper writes. Empty for a result that is whole, and for one that only stops short (a
  // helper that was ended): then `end` is missing and nothing more.
  std::string defect;
  std::optional<RestartPoint> restart;
  std::string restart_blocked;
  std::vector<PreviewDwell> dwells;
};

[[nodiscard]] auto write_restart(const RestartPoint& point) -> std::string;

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
