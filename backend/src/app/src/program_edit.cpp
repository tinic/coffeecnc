#include <coffeecnc/app/program_edit.hpp>

#include <coffeecnc/core/command_policy.hpp>

#include <cctype>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace coffeecnc {
namespace {

struct Span {
  std::size_t begin{};
  std::size_t end{};
};

auto digit(char c) -> bool { return c >= '0' && c <= '9'; }

// The numbers of the F and S words outside comments: `(…)` and everything after `;`.
auto feed_and_speed_numbers(std::string_view text) -> std::vector<Span> {
  std::vector<Span> spans;
  bool comment = false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (comment) {
      comment = c != ')';
      continue;
    }
    if (c == '(') {
      comment = true;
      continue;
    }
    if (c == ';')
      break;
    if (c == '<') {
      const auto end = text.find('>', i + 1);
      if (end == std::string_view::npos)
        break;
      i = end;
      continue;
    }
    if (c != 'F' && c != 'f' && c != 'S' && c != 's')
      continue;
    std::size_t begin = i + 1;
    while (begin < text.size() && text[begin] == ' ')
      ++begin;
    std::size_t end = begin;
    while (end < text.size() && (digit(text[end]) || text[end] == '.'))
      ++end;
    if (end > begin)
      spans.push_back({begin, end});
  }
  return spans;
}

// 12, 12., 12.5 and .5: what an operator types for a feed or a speed.
auto plain_decimal(std::string_view text) -> bool {
  std::size_t digits = 0;
  std::size_t points = 0;
  for (const char c : text) {
    if (digit(c))
      ++digits;
    else if (c == '.')
      ++points;
    else
      return false;
  }
  return digits > 0 && points <= 1;
}

auto control(std::string_view text) -> bool {
  for (const char c : text)
    if (std::iscntrl(static_cast<unsigned char>(c)) != 0 && c != '\t')
      return true;
  return false;
}

} // namespace

auto feed_or_speed_edit(std::string_view before, std::string_view after) -> bool {
  if (control(before) || control(after))
    return false;
  for (const auto span : feed_and_speed_numbers(before)) {
    const std::size_t tail = before.size() - span.end;
    if (after.size() < span.begin + tail || after.substr(0, span.begin) != before.substr(0, span.begin) ||
        after.substr(after.size() - tail) != before.substr(span.end))
      continue;
    const auto number = after.substr(span.begin, after.size() - tail - span.begin);
    if (plain_decimal(number) && number != before.substr(span.begin, span.end - span.begin))
      return true;
  }
  return false;
}

ProgramWriter::ProgramWriter(const ProgramStore& programs, ToolModelFiles& files)
    : programs_(programs), files_(files) {}

auto ProgramWriter::replace_line(std::string_view path, int line, std::string_view before,
                                 std::string_view after)
    -> std::expected<std::string, std::string> {
  auto resolved = programs_.resolve_program(path);
  if (!resolved)
    return std::unexpected(resolved.error());
  if (!feed_or_speed_edit(before, after))
    return std::unexpected("Only the number of one F or S word can be changed.");
  const auto content = files_.read(*resolved, ProgramStore::max_program_bytes);
  if (!content)
    return std::unexpected(content.error());
  if (!*content)
    return std::unexpected("The program no longer exists.");
  if ((*content)->more)
    return std::unexpected("The program is too large to edit here.");
  const std::string& bytes = (*content)->bytes;

  // Where line `line` starts and ends, without its line end (\n, \r\n or \r).
  std::size_t start = 0;
  for (int number = 1; number < line; ++number) {
    const auto end = bytes.find_first_of("\r\n", start);
    if (end == std::string::npos)
      return std::unexpected(std::format("The program has no line {}.", line));
    start = end + (bytes.compare(end, 2, "\r\n") == 0 ? 2 : 1);
  }
  if (line < 1 || start >= bytes.size())
    return std::unexpected(std::format("The program has no line {}.", line));
  auto stop = bytes.find_first_of("\r\n", start);
  if (stop == std::string::npos)
    stop = bytes.size();
  if (std::string_view{bytes}.substr(start, stop - start) != before)
    return std::unexpected("The program changed since it was shown. Open it again.");

  const std::string replaced = bytes.substr(0, start) + std::string{after} + bytes.substr(stop);
  const auto slash = resolved->rfind('/');
  const std::string directory = resolved->substr(0, slash);
  const std::string temporary =
      std::format("{}/.{}.coffeecnc-edit", directory, resolved->substr(slash + 1));
  auto step = files_.open_temporary(temporary);
  if (step)
    step = files_.write(replaced);
  if (step)
    step = files_.sync();
  if (step)
    step = files_.rename(temporary, *resolved);
  if (step)
    step = files_.sync_directory(directory.empty() ? "/" : directory);
  if (!step) {
    files_.discard(temporary);
    return std::unexpected("The program could not be saved. " + step.error());
  }
  return std::move(*resolved);
}

auto ProgramWriter::create(std::string_view path, std::string_view text, bool replace)
    -> std::expected<std::string, std::string> {
  auto target = programs_.resolve_new_program(path);
  if (!target)
    return std::unexpected(target.error());
  const auto slash = target->path.rfind('/');
  const std::string name = target->path.substr(slash + 1);
  if (target->exists && !replace)
    return std::unexpected(std::format("A program named {} exists already.", name));
  if (text.size() > max_saved_program_bytes)
    return std::unexpected(std::string("The program text is longer than 512 KiB."));
  const std::string directory = slash == 0 ? "/" : target->path.substr(0, slash);
  const std::string temporary = std::format("{}/.{}.coffeecnc-new", directory, name);
  auto step = files_.open_temporary(temporary);
  if (step)
    step = files_.write(text);
  if (step)
    step = files_.sync();
  if (step)
    step = replace ? files_.rename(temporary, target->path) : files_.link(temporary, target->path);
  if (!step) {
    files_.discard(temporary);
    return std::unexpected("The program could not be saved: " + step.error());
  }
  if (!replace)
    files_.discard(temporary);
  if (auto synced = files_.sync_directory(directory); !synced)
    return std::unexpected("The program could not be saved: " + synced.error());
  return std::move(target->path);
}

} // namespace coffeecnc
