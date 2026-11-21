#ifndef COFFEECNC_LINUXCNC_TOOL_TABLE_HPP
#define COFFEECNC_LINUXCNC_TOOL_TABLE_HPP

#include <cstddef>
#include <coffeecnc/core/command_policy.hpp>

#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace coffeecnc {

// The most of a tool table file that is read. LinuxCNC holds 1001 entries; a line of the file
// is at most 255 characters.
inline constexpr std::size_t max_tool_table_bytes = std::size_t{4} << 20;

// Edits of the text of a LinuxCNC tool table: one tool per line, words `T<number> P<pocket>
// Z<length> D<diameter>` and more, then `;comment`. Lengths in the file are machine units;
// `millimetres_per_unit` converts the tool's millimetres. Pure text, no I/O.

// The table with this tool's row replaced or, when it has none, added at the end. Words of an
// existing row that are not P, Z or D (other offsets, angles, orientation) are kept.
[[nodiscard]] auto with_tool(std::string_view table, const Tool& tool, double millimetres_per_unit)
    -> std::expected<std::string, std::string>;

// The table without the row of this tool number; nothing when there is no such row.
[[nodiscard]] auto without_tool(std::string_view table, int number) -> std::optional<std::string>;

// The comment of each tool row of the table, by tool number: the text after the first `;`,
// without the white space around it. LinuxCNC keeps comments only in the file, not with the
// table it has loaded. Rows without a comment and lines without a T word are left out; of two
// rows for one tool the first counts.
[[nodiscard]] auto comments_of(std::string_view table) -> std::map<int, std::string>;
[[nodiscard]] auto tool_z(std::string_view table, int tool) -> std::optional<double>;
[[nodiscard]] auto tool_diameter(std::string_view table, int tool) -> std::optional<double>;

// Tools touched off at the fixed setter, kept beside the tool table so their marks survive a
// restart. Text: a line `reference <length>`, the reference tool's table length then, and a line
// `T<n> <length>` per tool. Lengths in tool table units. A mark stands while the table holds
// both lengths (0.001 table units, the file's rounding).
struct TouchedOff {
  double reference{};
  std::map<int, double> tools;
  friend auto operator==(const TouchedOff&, const TouchedOff&) -> bool = default;
};
[[nodiscard]] auto parse_touched_off(std::string_view text) -> std::optional<TouchedOff>;
[[nodiscard]] auto format_touched_off(const TouchedOff& touched) -> std::string;
// After a teach with the reference tool: the reference tool with its length, and each saved
// tool whose length and the reference tool's are unchanged since. Lengths are relative to the
// reference tool, so a new teach does not void them.
[[nodiscard]] auto restored_touched_off(std::string_view table, int reference_tool,
                                        const std::optional<TouchedOff>& saved)
    -> std::map<int, double>;
// The tools whose table length still equals the one their touch off wrote.
[[nodiscard]] auto still_touched_off(std::string_view table, const std::map<int, double>& measured)
    -> std::vector<int>;

} // namespace coffeecnc

#endif // COFFEECNC_LINUXCNC_TOOL_TABLE_HPP
