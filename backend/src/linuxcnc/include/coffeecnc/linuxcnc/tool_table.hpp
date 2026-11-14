#ifndef COFFEECNC_LINUXCNC_TOOL_TABLE_HPP
#define COFFEECNC_LINUXCNC_TOOL_TABLE_HPP

#include <coffeecnc/core/command_policy.hpp>

#include <map>
#include <optional>
#include <string>
#include <string_view>

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

// The comment of each tool row of the table, by tool number: the text after the first `;`,
// without the white space around it. LinuxCNC keeps comments only in the file, not with the
// table it has loaded. Rows without a comment and lines without a T word are left out; of two
// rows for one tool the first counts.
[[nodiscard]] auto comments_of(std::string_view table) -> std::map<int, std::string>;

} // namespace coffeecnc

#endif // COFFEECNC_LINUXCNC_TOOL_TABLE_HPP
