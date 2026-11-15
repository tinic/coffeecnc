#include "program_watch.hpp"

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <format>
#include <initializer_list>
#include <string>

namespace coffeecnc {
namespace {

auto digit(char c) -> bool { return c >= '0' && c <= '9'; }

// A comment in parentheses that the interpreter acts on: what it does, or nothing for an
// ordinary one. As Interp::convert_comment recognises them (LinuxCNC 2.9.4,
// interp_convert.cc:1499): white space skipped, any case, the word and its comma first.
auto active_comment(std::string_view comment) -> std::string_view {
  std::string lower;
  bool leading = true;
  for (const char c : comment) {
    if (leading && std::isspace(static_cast<unsigned char>(c)) != 0)
      continue;
    leading = false;
    lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  for (const std::string_view word :
       {"pyrun,", "pyreload", "py,", "logopen,", "logappend,", "logclose", "log,", "probeopen",
        "probeclose", "msg,", "debug,", "print,", "abort,"})
    if (lower.starts_with(word))
      return word;
  return {};
}

} // namespace

auto bare_block(std::string_view line) -> std::string {
  std::string block;
  bool comment = false;
  for (const char c : line) {
    if (comment) {
      comment = c != ')';
      continue;
    }
    if (c == ';')
      break;
    if (c == '(') {
      comment = true;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
      continue;
    block += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return block;
}

auto refused_block(std::string_view line) -> std::optional<std::string> {
  // The comments first: an active one is not an ordinary comment, whatever it holds.
  for (std::size_t i = 0; i < line.size(); ++i) {
    if (line[i] == ';') {
      // The interpreter runs a line that begins `;py,` as Python (interp_read.cc:316).
      std::string rest;
      for (const char c : line.substr(i + 1))
        if (std::isspace(static_cast<unsigned char>(c)) == 0)
          rest += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (rest.starts_with("py"))
        return std::string("a `;py,` comment runs Python in the interpreter");
      break;
    }
    if (line[i] != '(')
      continue;
    const auto end = line.find(')', i);
    if (end == std::string_view::npos)
      break; // not closed: the interpreter's error
    const auto comment = line.substr(i + 1, end - i - 1);
    const auto word = active_comment(comment);
    if (word.starts_with("py"))
      return std::format("a ({}...) comment runs Python in the interpreter", word);
    if (word.starts_with("log") || word.starts_with("probe"))
      return std::format("a ({}...) comment writes a file", word);
    if (!word.empty() && comment.find('#') != std::string_view::npos)
      return std::format("a ({}...) comment with a parameter (#): programs with parameters are "
                         "not previewed",
                         word);
    i = end;
  }
  const auto block = bare_block(line);
  std::string_view text = block;
  if (text.find('#') != std::string_view::npos)
    return std::string("uses a parameter (#): programs with parameters are not previewed");
  if (!text.empty() && text.front() == '/')
    text.remove_prefix(1);
  // A line number, then the O word of a control block; its name is not a word of G-code.
  if (text.size() > 1 && text.front() == 'n' && digit(text[1])) {
    text.remove_prefix(1);
    while (!text.empty() && (digit(text.front()) || text.front() == '.'))
      text.remove_prefix(1);
  }
  if (text.size() > 1 && text.front() == 'o') {
    if (text[1] == '[')
      return std::string("a computed O word: programs with computed names are not previewed");
    text.remove_prefix(1);
    if (text.front() == '<') {
      const auto end = text.find('>');
      text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
    } else {
      while (!text.empty() && digit(text.front()))
        text.remove_prefix(1);
    }
    if (text.starts_with("call") && text.find('[') != std::string_view::npos)
      return std::string("calls a subroutine with arguments: programs with parameters are not "
                         "previewed");
    return {};
  }
  for (std::size_t i = 0; i + 1 < text.size(); ++i) {
    if (text[i] != 'g')
      continue;
    const char next = text[i + 1];
    if (next == '[')
      return std::string("a computed G word: which code runs is not known before it does");
    if (!digit(next) && next != '.')
      continue;
    double code = 0;
    std::size_t end = i + 1;
    while (end < text.size() && (digit(text[end]) || text[end] == '.'))
      ++end;
    std::from_chars(text.data() + i + 1, text.data() + end, code);
    const auto tenths = std::lround(code * 10);
    if (tenths == 280 || tenths == 300)
      return std::format("G{} moves by a position saved in the parameter file, which is not "
                         "the machine's present one for certain",
                         tenths / 10);
    if (tenths == 923)
      return std::string("G92.3 restores an offset saved in the parameter file");
    i = end - 1;
  }
  return {};
}

} // namespace coffeecnc
