#include <coffeecnc/linuxcnc/tool_table.hpp>

#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <system_error>
#include <utility>
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

auto tools_of(std::string_view table, double millimetres_per_unit) -> std::vector<Tool> {
  // The whole word after its letter as a number; nothing for anything else.
  const auto value = [](std::string_view word) -> std::optional<double> {
    word.remove_prefix(1);
    if (!word.empty() && word.front() == '+')
      word.remove_prefix(1);
    double number{};
    const auto [end, error] = std::from_chars(word.data(), word.data() + word.size(), number);
    if (error != std::errc{} || end != word.data() + word.size() || !std::isfinite(number))
      return {};
    return number;
  };
  std::vector<Tool> tools;
  for (const auto line : lines_of(table)) {
    const auto number = tool_number(line);
    if (!number)
      continue;
    Tool tool{*number, 0, 0, 0, {}};
    for (const auto word : words_of(line)) {
      const char first = letter(word);
      const auto given = value(word);
      if (!given)
        continue;
      if (first == 'P')
        tool.pocket = static_cast<int>(*given);
      else if (first == 'Z')
        tool.length = *given * millimetres_per_unit;
      else if (first == 'D')
        tool.diameter = *given * millimetres_per_unit;
    }
    if (const auto comment = line.find(';'); comment != std::string_view::npos) {
      auto text = line.substr(comment + 1);
      while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0)
        text.remove_prefix(1);
      while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0)
        text.remove_suffix(1);
      tool.comment = std::string(text);
    }
    tools.push_back(std::move(tool));
  }
  return tools;
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

} // namespace coffeecnc
