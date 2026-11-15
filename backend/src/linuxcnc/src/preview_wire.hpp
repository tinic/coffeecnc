#ifndef COFFEECNC_LINUXCNC_PREVIEW_WIRE_HPP
#define COFFEECNC_LINUXCNC_PREVIEW_WIRE_HPP

#include <coffeecnc/core/panel_messages.hpp>

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
  std::vector<HelperTool> tools;
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
  // The last program line a complete record named; 0 for none.
  int last_line{};
};

// Never fails: a line that is not complete (no newline) or not understood ends the reading, and
// what was read before it stands with `end` missing.
[[nodiscard]] auto parse_output(std::string_view text) -> HelperOutput;

} // namespace coffeecnc

#endif // COFFEECNC_LINUXCNC_PREVIEW_WIRE_HPP
