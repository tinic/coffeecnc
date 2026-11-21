#include "program_watch.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <format>
#include <initializer_list>
#include <string>
#include <utility>

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

// The side effect of one line's comments, and (separately) an expanding comment with a
// parameter in it.
auto comment_refusal(std::string_view line, bool side_effects_only) -> std::optional<std::string> {
  std::string start;
  for (const char c : line) {
    if (std::isspace(static_cast<unsigned char>(c)) != 0)
      continue;
    start += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (start.size() == 4)
      break;
  }
  if (start == ";py,")
    return std::string("a `;py,` line runs Python in the interpreter");
  for (std::size_t i = 0; i < line.size(); ++i) {
    if (line[i] == ';')
      break;
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
    if (!side_effects_only && !word.empty() && comment.find('#') != std::string_view::npos)
      return std::format("a ({}...) comment with a parameter (#): programs with parameters are "
                         "not previewed",
                         word);
    i = end;
  }
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

namespace {

// The O word a bare block begins with: its name (empty when it is not plainly one) and the
// rest of the block.
struct OWord {
  std::string name;
  std::string_view rest;
};

auto o_word(std::string_view text) -> std::optional<OWord> {
  if (!text.empty() && text.front() == '/')
    text.remove_prefix(1);
  if (text.size() > 1 && text.front() == 'n' && digit(text[1])) {
    text.remove_prefix(1);
    while (!text.empty() && (digit(text.front()) || text.front() == '.'))
      text.remove_prefix(1);
  }
  if (text.size() < 2 || text.front() != 'o')
    return {};
  text.remove_prefix(1);
  std::string_view name;
  if (digit(text.front())) {
    std::size_t end = 0;
    while (end < text.size() && digit(text[end]))
      ++end;
    name = text.substr(0, end);
    text.remove_prefix(end);
  } else if (const auto end = text.find('>'); text.front() == '<' && end != std::string_view::npos) {
    name = text.substr(1, end - 1);
    text.remove_prefix(end + 1);
  } else {
    return OWord{{}, text}; // computed, or a name that does not end
  }
  // What follows the name is one of the words the interpreter knows after an O word.
  const bool known = std::ranges::any_of(
      std::initializer_list<std::string_view>{"sub", "endsub", "call", "return", "if", "elseif",
                                              "else", "endif", "while", "endwhile", "do",
                                              "repeat", "endrepeat", "break", "continue"},
      [&](std::string_view word) -> bool { return text.starts_with(word); });
  const bool plain = known && !name.empty() && name.find("..") == std::string_view::npos &&
                     std::ranges::all_of(name, [](char c) -> bool {
                       return (c >= 'a' && c <= 'z') || digit(c) || c == '_' || c == '-' ||
                              c == '.';
                     });
  return OWord{plain ? std::string(name) : std::string{}, text};
}

} // namespace

auto o_word_name(std::string_view line) -> std::optional<std::string> {
  const auto block = bare_block(line);
  const auto found = o_word(block);
  if (!found || found->name.empty())
    return {};
  return found->name;
}

auto is_call(std::string_view line) -> bool {
  const auto block = bare_block(line);
  const auto found = o_word(block);
  return found && found->rest.starts_with("call");
}

auto called_names(std::string_view text, const std::set<std::string>& known, std::size_t room,
                  const std::function<bool()>& cancelled) -> CalledNames {
  CalledNames found;
  std::set<std::string> fresh;
  const std::size_t size = text.size();
  for (std::size_t lines = 0; !text.empty(); ++lines) {
    if (lines % 1024 == 0 && cancelled && cancelled()) {
      found.cancelled = true;
      break;
    }
    const auto end = text.find('\n');
    const auto line = text.substr(0, end);
    text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
    if (!is_call(line))
      continue;
    auto name = o_word_name(line);
    if (!name || known.contains(*name) || fresh.contains(*name))
      continue;
    if (found.names.size() >= room) {
      found.too_many = true;
      break;
    }
    fresh.insert(*name);
    found.names.push_back(std::move(*name));
  }
  found.examined = size - text.size();
  return found;
}

auto first_forbidden_line(std::string_view text) -> std::optional<Forbidden> {
  for (int number = 1; !text.empty(); ++number) {
    const auto end = text.find('\n');
    const auto line = text.substr(0, end);
    if (auto what = comment_refusal(line, true))
      return Forbidden{number, std::move(*what)};
    const auto block = bare_block(line);
    if (const auto found = o_word(block); found && found->name.empty())
      return Forbidden{number, "an O word whose name is computed or is not plainly a name "
                               "(letters, digits, `_`, `-`, `.`): the interpreter would look "
                               "for a file by it"};
    text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
  }
  return {};
}

auto refused_block(std::string_view line) -> std::optional<std::string> {
  // The comments first: an active one is not an ordinary comment, whatever it holds.
  if (auto reason = comment_refusal(line, false))
    return reason;
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
    const auto start = i + 1 + static_cast<std::size_t>(text[i + 1] == '+');
    if (start == text.size())
      continue;
    const char next = text[start];
    if (next == '[')
      return std::string("a computed G word: which code runs is not known before it does");
    if (!digit(next) && next != '.')
      continue;
    double code = 0;
    const auto [parsed, error] =
        std::from_chars(text.data() + start, text.data() + text.size(), code, std::chars_format::fixed);
    if (error != std::errc{} || code > 999)
      continue;
    const auto end = static_cast<std::size_t>(parsed - text.data());
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

auto motion_use(std::string_view line) -> MotionUse {
  const auto block = bare_block(line);
  std::string_view text = block;
  if (!text.empty() && text.front() == '/')
    text.remove_prefix(1);
  if (text.size() > 1 && text.front() == 'n' && digit(text[1])) {
    text.remove_prefix(1);
    while (!text.empty() && (digit(text.front()) || text.front() == '.'))
      text.remove_prefix(1);
  }
  if (!text.empty() && text.front() == 'o')
    return MotionUse::none; // a control block has no words
  bool axis = false;
  int depth = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    // Inside an expression letters are functions and operators, not words.
    if (c == '[')
      ++depth;
    else if (c == ']')
      --depth;
    if (depth > 0)
      continue;
    if (c == 'g' && i + 1 < text.size()) {
      const auto start = i + 1 + static_cast<std::size_t>(text[i + 1] == '+');
      double code = 0;
      const auto [parsed, error] =
          std::from_chars(text.data() + start, text.data() + text.size(), code, std::chars_format::fixed);
      if (error != std::errc{} || !(code >= 0 && code <= 999))
        continue;
      const auto end = static_cast<std::size_t>(parsed - text.data());
      const auto tenths = std::lround(code * 10);
      if (tenths == 0 || tenths == 10 || tenths == 20 || tenths == 30 ||
          (tenths >= 50 && tenths <= 53) || tenths == 330 || tenths == 331 ||
          (tenths >= 382 && tenths <= 385) || tenths == 730 || tenths == 760 ||
          (tenths >= 800 && tenths <= 890))
        return MotionUse::sets;
      i = end - 1;
    } else if (std::string_view("xyzabcuvw@^").find(c) != std::string_view::npos) {
      axis = true;
    }
  }
  return axis ? MotionUse::inherits : MotionUse::none;
}

} // namespace coffeecnc
