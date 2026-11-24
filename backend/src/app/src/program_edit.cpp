#include <coffeecnc/app/program_edit.hpp>

#include <coffeecnc/core/command_policy.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace coffeecnc {
namespace {

constexpr std::size_t max_copy_bytes = 32uz * 1024 * 1024;

auto changed_program() -> LocalizedReason {
  return {"The program file changed since it was opened. Open it again to edit it.",
          MessageKey{"backend.programEdit.changedSinceOpened"}};
}

auto incoming_temporary_name() -> std::string {
  static std::atomic<unsigned long> sequence{};
  return std::format(".coffeecnc-import-{}-{}", ::getpid(), sequence.fetch_add(1));
}

auto rename_file(const std::string& from, const std::string& to) -> ToolModelFiles::Done {
#ifdef __APPLE__
  const int result = ::renamex_np(from.c_str(), to.c_str(), RENAME_EXCL);
#else
  const int result = ::renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE);
#endif
  if (result != 0)
    return std::unexpected(
        LocalizedReason{std::error_code(errno, std::generic_category()).message()});
  return {};
}

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
  return std::ranges::any_of(text, [](char c) {
    return std::iscntrl(static_cast<unsigned char>(c)) != 0 && c != '\t';
  });
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
    -> std::expected<std::string, LocalizedReason> {
  auto resolved = programs_.resolve_program(path);
  if (!resolved)
    return std::unexpected(resolved.error());
  if (!feed_or_speed_edit(before, after))
    return std::unexpected(LocalizedReason{"Only the number of one F or S word can be changed.",
                                           MessageKey{"backend.programEdit.oneWord"}});
  const auto content = files_.read(*resolved, ProgramStore::max_program_bytes);
  if (!content)
    return std::unexpected(content.error());
  const auto& file = *content;
  if (!file)
    return std::unexpected(LocalizedReason{"The program no longer exists.",
                                           MessageKey{"backend.programEdit.noLongerExists"}});
  if (file->more)
    return std::unexpected(LocalizedReason{"The program is too large to edit here.",
                                           MessageKey{"backend.programEdit.tooLarge"}});
  const std::string& bytes = file->bytes;

  // Where line `line` starts and ends, without its line end (\n, \r\n or \r).
  std::size_t start = 0;
  for (int number = 1; number < line; ++number) {
    const auto end = bytes.find_first_of("\r\n", start);
    if (end == std::string::npos)
      return std::unexpected(LocalizedReason{
          std::format("The program has no line {}.", line),
          MessageKey{"backend.programEdit.noLine", {{"line", std::to_string(line)}}}});
    start = end + (bytes.compare(end, 2, "\r\n") == 0 ? 2 : 1);
  }
  if (line < 1 || start >= bytes.size())
    return std::unexpected(LocalizedReason{
        std::format("The program has no line {}.", line),
        MessageKey{"backend.programEdit.noLine", {{"line", std::to_string(line)}}}});
  auto stop = bytes.find_first_of("\r\n", start);
  if (stop == std::string::npos)
    stop = bytes.size();
  if (std::string_view{bytes}.substr(start, stop - start) != before)
    return std::unexpected(LocalizedReason{"The program changed since it was shown. Open it again.",
                                           MessageKey{"backend.programEdit.changedSinceShown"}});

  const std::string replaced = bytes.substr(0, start) + std::string{after} + bytes.substr(stop);
  if (auto written = write_over(*resolved, replaced, ".coffeecnc-edit"); !written)
    return std::unexpected(written.error());
  return std::move(*resolved);
}

auto ProgramWriter::write_over(const std::string& resolved, std::string_view bytes,
                               std::string_view suffix, std::optional<std::string_view> previous,
                               const std::function<LocalizedReason()>& check_controller)
    -> std::expected<void, LocalizedReason> {
  const auto slash = resolved.rfind('/');
  const std::string directory = resolved.substr(0, slash);
  const std::string temporary =
      std::format("{}/.{}{}", directory, resolved.substr(slash + 1), suffix);
  auto step = files_.open_temporary(temporary);
  if (step)
    step = files_.write(bytes);
  if (step)
    step = files_.sync();
  if (step && check_controller) {
    if (auto reason = check_controller(); !reason.empty()) {
      files_.discard(temporary);
      return std::unexpected(std::move(reason));
    }
  }
  if (step && previous) {
    const auto current = files_.read(resolved, previous->size() + 1);
    if (!current) {
      files_.discard(temporary);
      return std::unexpected(current.error());
    }
    const auto& file = *current;
    if (!file || file->more || file->bytes != *previous) {
      files_.discard(temporary);
      return std::unexpected(changed_program());
    }
  }
  if (step)
    step = files_.rename(temporary, resolved);
  if (!step) {
    files_.discard(temporary);
    return std::unexpected(
        LocalizedReason{std::format("The program could not be saved. {}", step.error().text),
                        MessageKey{"backend.programEdit.notSavedDetail",
                                   {{"detail", step.error().text}},
                                   parameter_localization("detail", step.error().localization)}});
  }
  if (auto durable = files_.sync_directory(directory.empty() ? "/" : directory); !durable)
    return std::unexpected(LocalizedReason{
        std::format("The program was replaced, but its directory could not be synchronized. {}",
                    durable.error().text),
        MessageKey{"backend.programEdit.notSynchronized",
                   {{"detail", durable.error().text}},
                   parameter_localization("detail", durable.error().localization)}});
  return {};
}

auto ProgramWriter::replace(std::string_view path, std::string_view text, std::string_view stamp,
                            const std::function<LocalizedReason()>& check_controller)
    -> std::expected<std::string, LocalizedReason> {
  auto resolved = programs_.resolve_program(path);
  if (!resolved)
    return std::unexpected(resolved.error());
  if (text.size() > max_saved_program_bytes)
    return std::unexpected(LocalizedReason{"The program text is longer than 512 KiB.",
                                           MessageKey{"backend.programEdit.textTooLong"}});
  // As read_program reads it, so that the stamps compare the same bytes.
  const auto content = files_.read(*resolved, ProgramStore::max_program_bytes + 1);
  if (!content)
    return std::unexpected(content.error());
  const auto& file = *content;
  if (!file)
    return std::unexpected(LocalizedReason{"The program no longer exists.",
                                           MessageKey{"backend.programEdit.noLongerExists"}});
  const std::string& bytes = file->bytes;
  if (file->more || program_stamp(bytes) != stamp)
    return std::unexpected(changed_program());
  const auto end = bytes.find_first_of("\r\n");
  const bool windows = end != std::string::npos && bytes.compare(end, 2, "\r\n") == 0;
  std::string converted;
  if (windows) {
    converted.reserve(text.size() + (text.size() / 16));
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r'))
        converted += '\r';
      converted += text[i];
    }
  }
  const std::string_view saved = windows ? std::string_view{converted} : text;
  if (saved.size() > max_saved_program_bytes)
    return std::unexpected(
        LocalizedReason{"The program text with its line endings is longer than 512 KiB.",
                        MessageKey{"backend.programEdit.textWithEndingsTooLong"}});
  if (auto written = write_over(*resolved, saved, ".coffeecnc-edit", bytes, check_controller);
      !written)
    return std::unexpected(written.error());
  return std::move(*resolved);
}

auto ProgramWriter::create(std::string_view path, std::string_view text, bool replace)
    -> std::expected<std::string, LocalizedReason> {
  auto target = programs_.resolve_new_program(path);
  if (!target)
    return std::unexpected(target.error());
  const auto slash = target->path.rfind('/');
  const std::string name = target->path.substr(slash + 1);
  if (target->exists && !replace)
    return std::unexpected(
        LocalizedReason{std::format("A program named {} exists already.", name),
                        MessageKey{"backend.programEdit.exists", {{"name", name}}}});
  if (text.size() > max_saved_program_bytes)
    return std::unexpected(LocalizedReason{"The program text is longer than 512 KiB.",
                                           MessageKey{"backend.programEdit.textTooLong"}});
  return write_new(std::move(*target), text, replace, std::format(".{}.coffeecnc-new", name), {});
}

auto ProgramWriter::receive(std::string_view path, std::string_view bytes, bool replace,
                            const Publish& publish) -> std::expected<std::string, LocalizedReason> {
  if (bytes.size() > ProgramStore::max_program_bytes)
    return std::unexpected(LocalizedReason{"The program is longer than 16 MiB.",
                                           MessageKey{"backend.programFiles.size"}});
  auto target = programs_.resolve_new_program(path);
  if (!target)
    return std::unexpected(target.error());
  return write_new(std::move(*target), bytes, replace, incoming_temporary_name(), publish);
}

auto ProgramWriter::write_new(ProgramStore::NewProgram target, std::string_view bytes, bool replace,
                              std::string temporary_name, const Publish& publish)
    -> std::expected<std::string, LocalizedReason> {
  const auto slash = target.path.rfind('/');
  const std::string name = target.path.substr(slash + 1);
  if (target.exists && !replace)
    return std::unexpected(
        LocalizedReason{std::format("A program named {} exists already.", name),
                        MessageKey{"backend.programEdit.exists", {{"name", name}}}});
  const std::string directory = slash == 0 ? "/" : target.path.substr(0, slash);
  const std::string temporary = std::format("{}/{}", directory, temporary_name);
  bool committed = false;
  const auto commit = [&]() -> ToolModelFiles::Done {
    auto stored = replace ? files_.rename(temporary, target.path) : files_.link(temporary, target.path);
    committed = stored.has_value();
    return stored;
  };
  auto step = files_.open_temporary(temporary);
  if (step)
    step = files_.write(bytes);
  if (step)
    step = files_.sync();
  auto written = std::move(step);
  if (written)
    written = publish ? publish(target.path,
                                [&] {
                                  return commit().transform_error([](LocalizedReason reason) {
                                    return std::move(reason.text);
                                  });
                                })
                      : commit();
  if (!written && !committed) {
    files_.discard(temporary);
    return std::unexpected(LocalizedReason{
        std::format("The program could not be saved: {}", written.error().text),
        MessageKey{"backend.programEdit.notSaved",
                   {{"detail", written.error().text}},
                   parameter_localization("detail", written.error().localization)}});
  }
  if (!replace)
    files_.discard(temporary);
  auto synced = files_.sync_directory(directory);
  if (!written)
    return std::unexpected(std::move(written.error()));
  if (!synced)
    return std::unexpected(LocalizedReason{
        std::format("Saved on disk. Directory synchronization failed. {}", synced.error().text),
        MessageKey{"backend.programFiles.directorySync",
                   {{"detail", synced.error().text}},
                   parameter_localization("detail", synced.error().localization)}});
  return std::move(target.path);
}

auto ProgramWriter::manage(const ManageProgramFile& command, std::string_view loaded)
    -> std::expected<void, LocalizedReason> {
  if (!programs_.has_program_directory())
    return std::unexpected(LocalizedReason{"No program directory is configured.",
                                           MessageKey{"backend.programStore.noDirectory"}});
  if (command.operation == ProgramFileOperation::copy) {
    const auto source = programs_.resolve_program(command.path);
    if (!source)
      return std::unexpected(source.error());
    const auto name = std::string_view{command.path}.substr(command.path.rfind('/') + 1);
    const auto target = programs_.resolve_new_program(name);
    if (!target)
      return std::unexpected(target.error());
    const auto content = files_.read(*source, max_copy_bytes);
    if (!content)
      return std::unexpected(content.error());
    const auto& file = *content;
    if (!file)
      return std::unexpected(LocalizedReason{"The file or folder no longer exists.",
                                             MessageKey{"backend.programFiles.missing"}});
    if (file->more)
      return std::unexpected(LocalizedReason{"The file is longer than 32 MiB.",
                                             MessageKey{"backend.programFiles.copySize"}});
    const auto copied = write_new(*target, file->bytes, false, incoming_temporary_name(), {});
    if (!copied)
      return std::unexpected(copied.error());
    return {};
  }

  const auto entry = programs_.resolve_new_program(command.path);
  if (!entry)
    return std::unexpected(entry.error());
  const auto slash = entry->path.rfind('/');
  const auto directory = slash == 0 ? "/" : entry->path.substr(0, slash);
  if (command.operation == ProgramFileOperation::create_folder) {
    const auto folder = files_.make_directory(entry->path);
    if (!folder)
      return std::unexpected(folder.error());
    if (!folder->created)
      return std::unexpected(LocalizedReason{"A file or folder already has that name.",
                                             MessageKey{"backend.programFiles.nameTaken"}});
    return files_.sync_directory(directory);
  }
  if (!entry->exists)
    return std::unexpected(LocalizedReason{"The file or folder no longer exists.",
                                           MessageKey{"backend.programFiles.missing"}});
  const auto contains = [&](std::string_view path) -> bool {
    return path == entry->path || path.starts_with(entry->path + "/");
  };
  if (contains(loaded) || contains(programs_.resolve_program(loaded).value_or("")))
    return std::unexpected(
        LocalizedReason{"Open a different program before moving or deleting this item.",
                        MessageKey{"backend.programFiles.loaded"}});
  if (command.operation == ProgramFileOperation::remove) {
    if (::remove(entry->path.c_str()) != 0)
      return std::unexpected(
          LocalizedReason{std::error_code(errno, std::generic_category()).message()});
  } else {
    if (command.name.find_first_of("/\\") != std::string::npos)
      return std::unexpected(LocalizedReason{"Use a single file or folder name.",
                                             MessageKey{"backend.programFiles.oneName"}});
    const auto target = programs_.resolve_new_program(entry->path.substr(0, slash + 1) + command.name);
    if (!target)
      return std::unexpected(target.error());
    if (auto moved = rename_file(entry->path, target->path); !moved)
      return std::unexpected(moved.error());
  }
  return files_.sync_directory(directory);
}

} // namespace coffeecnc
