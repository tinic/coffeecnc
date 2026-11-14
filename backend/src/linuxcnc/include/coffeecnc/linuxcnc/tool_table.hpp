#ifndef COFFEECNC_LINUXCNC_TOOL_TABLE_HPP
#define COFFEECNC_LINUXCNC_TOOL_TABLE_HPP

#include <coffeecnc/core/command_policy.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace coffeecnc {

// Edits of the text of a LinuxCNC tool table: one tool per line, words `T<number> P<pocket>
// Z<length> D<diameter>` and more, then `;comment`. Lengths in the file are machine units;
// `millimetres_per_unit` converts the tool's millimetres. Pure text, no I/O.

// The table with this tool's row replaced or, when it has none, added at the end. Words of an
// existing row that are not P, Z or D (other offsets, angles, orientation) are kept.
[[nodiscard]] auto with_tool(std::string_view table, const Tool& tool, double millimetres_per_unit)
    -> std::string;

// The table without the row of this tool number; nothing when there is no such row.
[[nodiscard]] auto without_tool(std::string_view table, int number) -> std::optional<std::string>;

// The tools of the table in the order of its rows, lengths in millimetres. A row is a line with
// a T word; a missing P, Z or D word is 0, as LinuxCNC reads it. The comment is the text after
// the first `;`, without the white space around it.
[[nodiscard]] auto tools_of(std::string_view table, double millimetres_per_unit)
    -> std::vector<Tool>;

} // namespace coffeecnc

#endif // COFFEECNC_LINUXCNC_TOOL_TABLE_HPP
