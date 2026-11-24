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

// The side effect of one line's comments: Python, or a file written.
auto comment_refusal(std::string_view line) -> std::optional<PreviewStopReason> {
  std::string start;
  for (const char c : line) {
    if (std::isspace(static_cast<unsigned char>(c)) != 0)
      continue;
    start += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (start.size() == 4)
      break;
  }
  if (start == ";py,")
    return PreviewStopReason{KnownPreviewStop::python_line};
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
      return PreviewStopReason{KnownPreviewStop::python_comment, std::string(word)};
    if (word.starts_with("log") || word.starts_with("probe"))
      return PreviewStopReason{KnownPreviewStop::file_writing_comment, std::string(word)};
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
    if (auto reason = comment_refusal(line))
      return Forbidden{number, std::move(*reason)};
    const auto block = bare_block(line);
    if (const auto found = o_word(block); found && found->name.empty())
      return Forbidden{number, {KnownPreviewStop::unsafe_o_word_name}};
    text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
  }
  return {};
}

namespace {

// The words of a block that is not a control (O word) block, outside expressions and named
// parameters: a letter, and its number when one follows it literally. `computed` when the
// value is an expression or a parameter.
struct Word {
  char letter{};
  std::optional<double> value;
  bool computed{};
};

// What follows a block's block-delete mark and line number.
auto after_line_number(std::string_view text) -> std::string_view {
  if (!text.empty() && text.front() == '/')
    text.remove_prefix(1);
  if (text.size() > 1 && text.front() == 'n' && digit(text[1])) {
    text.remove_prefix(1);
    while (!text.empty() && (digit(text.front()) || text.front() == '.'))
      text.remove_prefix(1);
  }
  return text;
}

// The words of a block, or nothing for an O word block, which has none.
auto words_part(std::string_view block) -> std::optional<std::string_view> {
  const auto text = after_line_number(block);
  if (!text.empty() && text.front() == 'o')
    return {};
  return text;
}

auto words_of(std::string_view text) -> std::vector<Word> {
  std::vector<Word> words;
  int depth = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '[') {
      ++depth;
      continue;
    }
    if (c == ']') {
      depth = std::max(depth - 1, 0);
      continue;
    }
    if (depth > 0)
      continue;
    if (c == '#') {
      // A parameter: a number, a named one in <>, or an expression (skipped above).
      if (i + 1 < text.size() && text[i + 1] == '<') {
        const auto end = text.find('>', i);
        i = end == std::string_view::npos ? text.size() : end;
      }
      continue;
    }
    if (c < 'a' || c > 'z')
      continue;
    Word word{c, {}, false};
    std::size_t start = i + 1;
    if (start < text.size() && (text[start] == '+' || text[start] == '-'))
      ++start;
    if (start < text.size() && (text[start] == '[' || text[start] == '#')) {
      word.computed = true;
    } else if (start < text.size() && (digit(text[start]) || text[start] == '.')) {
      double value = 0;
      const auto [parsed, error] = std::from_chars(text.data() + start, text.data() + text.size(),
                                                   value, std::chars_format::fixed);
      if (error == std::errc{}) {
        word.value = text[i + 1] == '-' ? -value : value;
        i = static_cast<std::size_t>(parsed - text.data()) - 1;
      }
    }
    words.push_back(word);
  }
  return words;
}

auto tenths_of(const Word& word) -> std::optional<long> {
  if (!word.value || *word.value < 0 || *word.value > 999)
    return {};
  return std::lround(*word.value * 10);
}

} // namespace

auto first_rigid_tap_line(std::string_view text, const std::function<bool()>& cancelled)
    -> std::optional<int> {
  for (int number = 1; !text.empty(); ++number) {
    if ((number - 1) % 1024 == 0 && cancelled && cancelled())
      return {};
    const auto end = text.find('\n');
    const auto line = text.substr(0, end);
    text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
    if (line.find('3') == std::string_view::npos || line.find_first_of("Gg") == std::string_view::npos)
      continue;
    const auto block = bare_block(line);
    const auto words = words_part(block);
    if (words && std::ranges::any_of(words_of(*words), [](const Word& word) {
          return word.letter == 'g' && tenths_of(word) == 331;
        }))
      return number;
  }
  return {};
}

auto refused_block(std::string_view line) -> std::optional<PreviewStopReason> {
  // The comments first: an active one is not an ordinary comment, whatever it holds.
  if (auto reason = comment_refusal(line))
    return reason;
  const auto block = bare_block(line);
  // The one parameter whose value the preview's interpreter has differently by design.
  if (block.find("#<_task>") != std::string::npos)
    return PreviewStopReason{KnownPreviewStop::task_parameter};
  // The preview's interpreter has no INI or HAL values (INI_VARS and HAL_PIN_VARS are off).
  if (block.find("#<_ini[") != std::string::npos || block.find("#<_hal[") != std::string::npos)
    return PreviewStopReason{KnownPreviewStop::ini_or_hal_parameter};
  // Task keeps global named parameters from earlier programs and MDI blocks; the preview's
  // interpreter starts without them, so EXISTS can answer differently.
  if (block.find("exists[#<_") != std::string::npos)
    return PreviewStopReason{KnownPreviewStop::global_parameter_exists};
  const auto text = words_part(block);
  if (!text) {
    if (after_line_number(block).starts_with("o["))
      return PreviewStopReason{KnownPreviewStop::computed_o_word};
    return {};
  }
  for (const auto& word : words_of(*text)) {
    if (word.letter != 'g')
      continue;
    if (word.computed)
      return PreviewStopReason{KnownPreviewStop::computed_g_word};
    if (tenths_of(word) == 923)
      return PreviewStopReason{KnownPreviewStop::restore_g92};
  }
  return {};
}

auto saved_position_move(std::string_view line) -> int {
  const auto block = bare_block(line);
  const auto text = words_part(block);
  if (!text)
    return 0;
  for (const auto& word : words_of(*text)) {
    if (word.letter != 'g')
      continue;
    if (const auto tenths = tenths_of(word); tenths == 280 || tenths == 300)
      return static_cast<int>(*tenths / 10);
  }
  return 0;
}

auto uses_parameters(std::string_view text) -> bool {
  while (!text.empty()) {
    const auto end = text.find('\n');
    if (bare_block(text.substr(0, end)).find('#') != std::string::npos)
      return true;
    text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
  }
  return false;
}

auto stored_parameters(std::string_view text) -> std::map<int, double> {
  std::map<int, double> parameters;
  while (!text.empty()) {
    const auto end = text.find('\n');
    std::string_view line = text.substr(0, end);
    text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
    const auto skip = [&line] {
      while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
        line.remove_prefix(1);
    };
    skip();
    int number = 0;
    const auto [after_number, number_error] =
        std::from_chars(line.data(), line.data() + line.size(), number);
    if (number_error != std::errc{} || number <= 0)
      continue;
    line.remove_prefix(static_cast<std::size_t>(after_number - line.data()));
    skip();
    double value = 0;
    const auto [after_value, value_error] =
        std::from_chars(line.data(), line.data() + line.size(), value);
    if (value_error == std::errc{} && after_value != line.data())
      parameters[number] = value;
  }
  return parameters;
}

auto parameter_use(std::string_view line) -> ParameterUse {
  ParameterUse use;
  const auto block = bare_block(line);
  const std::string_view text = after_line_number(block);
  use.control = !text.empty() && text.front() == 'o';
  if (use.control) {
    if (const auto found = o_word(block)) {
      use.name = found->name;
      // Longer words first: "elseif" before "else", "endsub" is not "sub".
      for (const std::string_view word :
           {"endrepeat", "endwhile", "continue", "endsub", "elseif", "return", "repeat", "endif",
            "break", "while", "else", "call", "sub", "do", "if"})
        if (found->rest.starts_with(word)) {
          use.word = word;
          break;
        }
    }
  }
  int depth = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '[')
      ++depth;
    else if (c == ']')
      depth = std::max(depth - 1, 0);
    if (c != '#')
      continue;
    const std::size_t at = i + 1;
    if (at < text.size() && text[at] == '<') {
      const auto end = text.find('>', at);
      const auto name = text.substr(at + 1, end == std::string_view::npos ? text.size() : end - at - 1);
      use.returned = use.returned || name == "_value" || name == "_value_returned";
      // `#<_...>` in EXISTS is refused_block's.
      if (use.exists.empty() && i >= 7 && text.substr(i - 7, 7) == "exists[" &&
          !name.starts_with('_'))
        use.exists = std::format("#<{}>", name);
      i = end == std::string_view::npos ? text.size() : end;
      continue;
    }
    std::size_t end = at;
    while (end < text.size() && digit(text[end]))
      ++end;
    int number = 0;
    const auto [parsed, error] = std::from_chars(text.data() + at, text.data() + end, number);
    if (end == at || error != std::errc{} || parsed != text.data() + end ||
        (end < text.size() && text[end] == '.')) {
      use.computed = true;
      continue;
    }
    // `#n=` at the level of the block's words is a setting; an expression has no `=`.
    if (!use.control && depth == 0 && end < text.size() && text[end] == '=')
      use.sets.push_back(number);
    else
      use.reads.push_back(number);
    i = end - 1;
  }
  return use;
}

auto remapped_use(std::string_view line, const std::vector<std::string>& codes)
    -> std::optional<std::string> {
  if (codes.empty())
    return {};
  const auto block = bare_block(line);
  const auto text = words_part(block);
  if (!text)
    return {};
  const auto words = words_of(*text);
  for (const auto& code : codes) {
    if (code.empty())
      continue;
    const char letter = code.front();
    std::optional<double> value;
    if (code.size() > 1) {
      double number = 0;
      const auto [parsed, error] = std::from_chars(code.data() + 1, code.data() + code.size(),
                                                   number, std::chars_format::fixed);
      if (error != std::errc{} || parsed != code.data() + code.size())
        continue;
      value = number;
    }
    const bool used = std::ranges::any_of(words, [&](const Word& word) -> bool {
      if (word.letter != letter)
        return false;
      // A computed value may be the remapped one.
      return !value || word.computed ||
             (word.value && std::lround(*word.value * 10) == std::lround(*value * 10));
    });
    if (used) {
      std::string name = code;
      std::ranges::transform(name, name.begin(), [](char c) {
        return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      });
      return name;
    }
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
