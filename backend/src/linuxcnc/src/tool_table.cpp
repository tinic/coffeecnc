#include <coffeecnc/linuxcnc/tool_table.hpp>

#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <format>
#include <sstream>
#include <system_error>
#include <vector>

namespace coffeecnc {
namespace {

// Shortest decimal that reads back as the same double, never an exponent.
auto real(double value) -> std::string {
  if (value == 0)
    return "0";
  std::array<char, 512> buffer{};
  const auto [end, error] =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::fixed);
  return error == std::errc{} ? std::string(buffer.data(), end) : std::string("0");
}

auto lines_of(std::string_view table) -> std::vector<std::string_view> {
  std::vector<std::string_view> lines;
  while (!table.empty()) {
    const auto end = table.find('\n');
    lines.push_back(table.substr(0, end));
    if (end == std::string_view::npos)
      break;
    table.remove_prefix(end + 1);
  }
  return lines;
}

// The words of a row before its comment.
auto words_of(std::string_view line) -> std::vector<std::string_view> {
  line = line.substr(0, line.find(';'));
  std::vector<std::string_view> words;
  std::size_t at = 0;
  while (at < line.size()) {
    if (std::isspace(static_cast<unsigned char>(line[at])) != 0) {
      ++at;
      continue;
    }
    std::size_t end = at;
    while (end < line.size() && std::isspace(static_cast<unsigned char>(line[end])) == 0)
      ++end;
    words.push_back(line.substr(at, end - at));
    at = end;
  }
  return words;
}

auto letter(std::string_view word) -> char {
  return word.empty() ? '\0' : static_cast<char>(std::toupper(static_cast<unsigned char>(word[0])));
}

auto tool_number(std::string_view line) -> std::optional<int> {
  for (const auto word : words_of(line)) {
    if (letter(word) != 'T')
      continue;
    int number{};
    const auto digits = word.substr(1);
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), number);
    if (error == std::errc{} && end == digits.data() + digits.size())
      return number;
  }
  return {};
}

auto row(const Tool& tool, double millimetres_per_unit, std::string_view previous) -> std::string {
  std::string text = "T" + std::to_string(tool.number) + " P" + std::to_string(tool.pocket) + " Z" +
                     real(tool.length / millimetres_per_unit) + " D" +
                     real(tool.diameter / millimetres_per_unit);
  for (const auto word : words_of(previous)) {
    const char first = letter(word);
    if (first != 'T' && first != 'P' && first != 'Z' && first != 'D')
      text.append(" ").append(word);
  }
  if (!tool.comment.empty())
    text.append(" ;").append(tool.comment);
  return text;
}

auto joined(const std::vector<std::string>& lines) -> std::string {
  std::string table;
  for (const auto& line : lines)
    table.append(line).append("\n");
  return table;
}

} // namespace

auto with_tool(std::string_view table, const Tool& tool, double millimetres_per_unit)
    -> std::string {
  std::vector<std::string> lines;
  bool replaced = false;
  for (const auto line : lines_of(table)) {
    if (tool_number(line) != tool.number) {
      lines.emplace_back(line);
    } else if (!replaced) {
      lines.push_back(row(tool, millimetres_per_unit, line));
      replaced = true;
    } // A second row for the same tool is dropped.
  }
  if (!replaced)
    lines.push_back(row(tool, millimetres_per_unit, {}));
  return joined(lines);
}

auto comments_of(std::string_view table) -> std::map<int, std::string> {
  std::map<int, std::string> comments;
  for (const auto line : lines_of(table)) {
    const auto number = tool_number(line);
    const auto start = line.find(';');
    if (!number || start == std::string_view::npos)
      continue;
    auto text = line.substr(start + 1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0)
      text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0)
      text.remove_suffix(1);
    if (!text.empty())
      comments.try_emplace(*number, text);
  }
  return comments;
}

auto tool_z(std::string_view table, int tool) -> std::optional<double> {
  std::optional<double> length;
  for (const auto line : lines_of(table)) {
    if (tool_number(line) != tool)
      continue;
    if (length)
      return {}; // Do not guess which duplicate row LinuxCNC loaded.
    length = 0;
    for (auto word : words_of(line)) {
      if (letter(word) != 'Z')
        continue;
      word.remove_prefix(1);
      if (word.starts_with('+'))
        word.remove_prefix(1);
      const auto [end, error] = std::from_chars(word.data(), word.data() + word.size(), *length);
      if (error != std::errc{} || end != word.data() + word.size() || !std::isfinite(*length))
        return {};
    }
  }
  return length;
}

auto without_tool(std::string_view table, int number) -> std::optional<std::string> {
  std::vector<std::string> lines;
  bool found = false;
  for (const auto line : lines_of(table)) {
    if (tool_number(line) == number)
      found = true;
    else
      lines.emplace_back(line);
  }
  if (!found)
    return {};
  return joined(lines);
}

namespace {
constexpr double touched_tolerance = 0.001;
auto unchanged(std::string_view table, int tool, double length) -> bool {
  const auto now = tool_z(table, tool);
  return now && std::abs(*now - length) <= touched_tolerance;
}
} // namespace

auto parse_touched_off(std::string_view text) -> std::optional<TouchedOff> {
  std::istringstream in{std::string(text)};
  TouchedOff touched;
  std::string word;
  if (!(in >> word >> touched.reference) || word != "reference" ||
      !std::isfinite(touched.reference))
    return std::nullopt;
  for (double length{}; in >> word >> length;) {
    int tool{};
    const auto* first = word.data() + 1;
    const auto* last = word.data() + word.size();
    if (word.size() < 2 || word[0] != 'T' || !std::isfinite(length))
      return std::nullopt;
    if (const auto [end, error] = std::from_chars(first, last, tool);
        error != std::errc{} || end != last || tool <= 0)
      return std::nullopt;
    touched.tools[tool] = length;
  }
  if (!in.eof())
    return std::nullopt;
  return touched;
}

auto format_touched_off(const TouchedOff& touched) -> std::string {
  std::string text = std::format("reference {:.6f}\n", touched.reference);
  for (const auto& [tool, length] : touched.tools)
    text += std::format("T{} {:.6f}\n", tool, length);
  return text;
}

auto restored_touched_off(std::string_view table, int reference_tool,
                          const std::optional<TouchedOff>& saved) -> std::map<int, double> {
  std::map<int, double> marks;
  const auto reference = tool_z(table, reference_tool);
  if (!reference)
    return marks;
  marks[reference_tool] = *reference;
  if (!saved || std::abs(saved->reference - *reference) > touched_tolerance)
    return marks;
  for (const auto& [tool, length] : saved->tools)
    if (tool != reference_tool && unchanged(table, tool, length))
      marks[tool] = length;
  return marks;
}

auto still_touched_off(std::string_view table, const std::map<int, double>& measured)
    -> std::vector<int> {
  std::vector<int> tools;
  for (const auto& [tool, length] : measured)
    if (unchanged(table, tool, length))
      tools.push_back(tool);
  return tools;
}

} // namespace coffeecnc
