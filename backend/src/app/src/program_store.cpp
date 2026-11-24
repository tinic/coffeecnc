#include <coffeecnc/app/program_store.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <ranges>
#include <tuple>
#include <utility>
#include <vector>

namespace coffeecnc {
namespace {

auto byte(std::string_view text, std::size_t index) -> unsigned {
  return index < text.size() ? static_cast<unsigned char>(text[index]) : 0x100U;
}

// Length of the well-formed UTF-8 sequence that starts at `index` (Unicode table 3-7: no
// overlong forms, no surrogates, nothing above U+10FFFF), or 0.
auto sequence(std::string_view text, std::size_t index) -> std::size_t {
  const auto in = [&text, index](std::size_t offset, unsigned low, unsigned high) -> bool {
    const auto value = byte(text, index + offset);
    return value >= low && value <= high;
  };
  const auto tail = [&in](std::size_t offset) -> bool { return in(offset, 0x80, 0xBF); };
  if (in(0, 0x00, 0x7F))
    return 1;
  if (in(0, 0xC2, 0xDF) && tail(1))
    return 2;
  if (((in(0, 0xE0, 0xE0) && in(1, 0xA0, 0xBF)) || (in(0, 0xE1, 0xEC) && tail(1)) ||
       (in(0, 0xED, 0xED) && in(1, 0x80, 0x9F)) || (in(0, 0xEE, 0xEF) && tail(1))) &&
      tail(2))
    return 3;
  if (((in(0, 0xF0, 0xF0) && in(1, 0x90, 0xBF)) || (in(0, 0xF1, 0xF3) && tail(1)) ||
       (in(0, 0xF4, 0xF4) && in(1, 0x80, 0x8F))) &&
      tail(2) && tail(3))
    return 4;
  return 0;
}

auto utf8(std::string_view text) -> bool {
  for (std::size_t i = 0; i < text.size();) {
    const auto length = sequence(text, i);
    if (length == 0)
      return false;
    i += length;
  }
  return true;
}

// The text with one U+FFFD for every byte outside a well-formed sequence.
auto as_utf8(std::string_view text) -> std::string {
  std::string clean;
  clean.reserve(text.size());
  for (std::size_t i = 0; i < text.size();) {
    const auto length = sequence(text, i);
    if (length == 0) {
      clean += "\xEF\xBF\xBD";
      ++i;
    } else {
      clean.append(text.substr(i, length));
      i += length;
    }
  }
  return clean;
}

auto lines(std::string_view text) -> std::vector<std::string> {
  std::vector<std::string> found;
  std::size_t start = 0;
  while (start < text.size()) {
    const auto end = std::min(text.find_first_of("\r\n", start), text.size());
    found.push_back(as_utf8(text.substr(start, end - start)));
    start = end + 1;
    if (byte(text, end) == '\r' && byte(text, end + 1) == '\n')
      ++start;
  }
  return found;
}

auto inside(std::string_view root, std::string_view path) -> bool {
  if (path == root || root == "/")
    return path.starts_with('/');
  return path.starts_with(root) && path.substr(root.size()).starts_with('/');
}

auto child(std::string_view directory, std::string_view name) -> std::string {
  auto path = std::string(directory);
  if (!path.ends_with('/'))
    path += '/';
  return path += name;
}

auto listable(std::string_view name) -> bool {
  return !name.empty() && name.front() != '.' && utf8(name) &&
         std::ranges::none_of(name, [](char c) -> bool {
           const auto value = static_cast<unsigned char>(c);
           return value < 0x20 || value == 0x7F;
         });
}

auto folded(std::string_view name) -> std::string {
  std::string lower{name};
  std::ranges::transform(lower, lower.begin(), [](char c) -> char {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
  });
  return lower;
}

} // namespace

auto program_stamp(std::string_view bytes) -> std::string {
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  for (const char c : bytes) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 0x100000001b3ULL;
  }
  constexpr std::array<char, 16> digits{'0', '1', '2', '3', '4', '5', '6', '7',
                                        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  std::string stamp(16, '0');
  for (auto& digit : stamp | std::views::reverse) {
    digit = digits.at(hash & 0xFU);
    hash >>= 4U;
  }
  return stamp;
}

ProgramStore::ProgramStore(const FileSystem& files, std::string root, std::string start)
    : files_(files), root_(std::move(root)), start_(std::move(start)) {}

auto ProgramStore::has_program_directory() const noexcept -> bool {
  return !start_.empty() || (!root_.empty() && root_ != "/");
}

auto ProgramStore::resolve(std::string_view path) const
    -> std::expected<Resolved, LocalizedReason> {
  if (root_.empty())
    return std::unexpected(LocalizedReason{"No program directory is configured.",
                                           MessageKey{"backend.programStore.noDirectory"}});
  if (path.empty())
    return std::unexpected(
        LocalizedReason{"Path is empty.", MessageKey{"backend.programStore.pathEmpty"}});
  if (path.find('\0') != std::string_view::npos)
    return std::unexpected(
        LocalizedReason{"Path contains a NUL byte.", MessageKey{"backend.programStore.pathNul"}});
  auto root = files_.canonical(root_);
  if (!root)
    return std::unexpected(LocalizedReason{
        std::format("The program directory is not available: {}", root.error()),
        MessageKey{"backend.programStore.directoryUnavailable", {{"detail", root.error()}}}});
  auto resolved = files_.canonical(path.front() == '/' ? std::string(path) : child(*root, path));
  if (!resolved)
    return std::unexpected(LocalizedReason{
        std::format("Path is not available: {}", resolved.error()),
        MessageKey{"backend.programStore.pathUnavailable", {{"detail", resolved.error()}}}});
  if (!inside(*root, *resolved))
    return std::unexpected(LocalizedReason{"Path is outside the program directory.",
                                           MessageKey{"backend.programStore.outsideDirectory"}});
  if (!utf8(*resolved))
    return std::unexpected(
        LocalizedReason{"Path is not UTF-8 text.", MessageKey{"backend.programStore.notUtf8"}});
  return Resolved{std::move(*root), std::move(*resolved)};
}

auto ProgramStore::list(const std::optional<std::string>& path) const
    -> std::expected<Directory, LocalizedReason> {
  const auto where = resolve(path ? std::string_view{*path}
                                  : std::string_view{start_.empty() ? root_ : start_});
  if (!where)
    return std::unexpected(where.error());
  const auto info = files_.info(where->path);
  if (!info)
    return std::unexpected(LocalizedReason{
        std::format("Directory is not available: {}", info.error()),
        MessageKey{"backend.programStore.folderUnavailable", {{"detail", info.error()}}}});
  if (info->kind != FileKind::directory)
    return std::unexpected(LocalizedReason{"Path is not a directory.",
                                           MessageKey{"backend.programStore.notDirectory"}});
  const auto names = files_.names(where->path, max_directory_names);
  if (!names)
    return std::unexpected(LocalizedReason{
        std::format("Directory cannot be read: {}", names.error()),
        MessageKey{"backend.programStore.folderUnreadable", {{"detail", names.error()}}}});

  Directory directory;
  directory.path = where->path;
  const auto programs = has_program_directory() ? files_.canonical(start_.empty() ? root_ : start_)
                                                 : std::unexpected(std::string{});
  directory.writable = programs && inside(*programs, where->path);
  if (where->path != where->root) {
    const auto slash = where->path.rfind('/');
    directory.parent = slash == 0 ? "/" : where->path.substr(0, slash);
  }
  for (const auto& name : names->names) {
    if (!listable(name))
      continue;
    // Through a link, the entry is what the link leads to, and only when that is inside the root.
    const auto target = files_.canonical(child(where->path, name));
    if (!target || !inside(where->root, *target))
      continue;
    const auto entry = files_.info(*target);
    if (!entry || entry->kind == FileKind::other)
      continue;
    const bool is_directory = entry->kind == FileKind::directory;
    directory.entries.push_back({name, is_directory ? EntryKind::directory : EntryKind::file,
                                 is_directory ? 0 : entry->bytes, entry->modified});
  }
  std::ranges::sort(
      directory.entries, [](const DirectoryEntry& a, const DirectoryEntry& b) -> bool {
        const auto a_folded = folded(a.name);
        const auto b_folded = folded(b.name);
        return std::tie(a.kind, a_folded, a.name) < std::tie(b.kind, b_folded, b.name);
      });
  directory.truncated = names->more || directory.entries.size() > max_directory_entries;
  if (directory.entries.size() > max_directory_entries)
    directory.entries.resize(max_directory_entries);
  return directory;
}

auto ProgramStore::resolve_program(std::string_view path) const
    -> std::expected<std::string, LocalizedReason> {
  auto where = resolve(path);
  if (!where)
    return std::unexpected(where.error());
  const auto info = files_.info(where->path);
  if (!info)
    return std::unexpected(LocalizedReason{
        std::format("Program is not available: {}", info.error()),
        MessageKey{"backend.programStore.programUnavailable", {{"detail", info.error()}}}});
  if (info->kind != FileKind::file)
    return std::unexpected(LocalizedReason{"Path is not a regular file.",
                                           MessageKey{"backend.programStore.notRegularFile"}});
  if (where->path.size() > max_program_path_bytes)
    return std::unexpected(LocalizedReason{"Program path is longer than 254 bytes.",
                                           MessageKey{"backend.programStore.pathTooLong"}});
  return std::move(where->path);
}

auto ProgramStore::resolve_new_program(std::string_view path) const
    -> std::expected<NewProgram, LocalizedReason> {
  if (path.empty() || path.ends_with('/'))
    return std::unexpected(
        LocalizedReason{"Name the program file.", MessageKey{"backend.programStore.nameFile"}});
  if (path.find('\0') != std::string_view::npos)
    return std::unexpected(
        LocalizedReason{"Path contains a NUL byte.", MessageKey{"backend.programStore.pathNul"}});
  const auto programs = resolve(start_.empty() ? root_ : start_);
  if (!programs)
    return std::unexpected(programs.error());
  const auto absolute = path.front() == '/' ? std::string(path) : child(programs->path, path);
  const auto slash = absolute.rfind('/');
  const auto name = std::string_view{absolute}.substr(slash + 1);
  if (!listable(name) || name == "..")
    return std::unexpected(LocalizedReason{
        "Use a file name without control characters that does not start with a dot.",
        MessageKey{"backend.programStore.fileName"}});
  const auto directory = resolve(slash == 0 ? std::string("/") : absolute.substr(0, slash));
  if (!directory)
    return std::unexpected(directory.error());
  if (!inside(programs->path, directory->path))
    return std::unexpected(LocalizedReason{"New programs go in the program directory.",
                                           MessageKey{"backend.programStore.newInDirectory"}});
  const auto info = files_.info(directory->path);
  if (!info || info->kind != FileKind::directory)
    return std::unexpected(LocalizedReason{"The folder of the new program is not a directory.",
                                           MessageKey{"backend.programStore.folderNotDirectory"}});
  NewProgram created{child(directory->path, name), false};
  if (created.path.size() > max_program_path_bytes)
    return std::unexpected(LocalizedReason{"Program path is longer than 254 bytes.",
                                           MessageKey{"backend.programStore.pathTooLong"}});
  // A link of that name counts, also one that leads nowhere: it is never written through.
  created.exists = files_.info(created.path).has_value() || files_.canonical(created.path).has_value();
  return created;
}

auto ProgramStore::read_program(std::string_view path, std::uint64_t revision) const
    -> std::expected<ProgramText, LocalizedReason> {
  auto file = resolve_program(path);
  if (!file)
    return std::unexpected(std::move(file.error()));
  const auto content = files_.read(*file, max_program_bytes + 1);
  if (!content)
    return std::unexpected(LocalizedReason{
        std::format("Program cannot be read: {}", content.error()),
        MessageKey{"backend.programStore.cannotRead", {{"detail", content.error()}}}});
  ProgramText program;
  program.revision = revision;
  program.file = std::move(*file);
  program.stamp = program_stamp(*content);
  std::string_view text{*content};
  if (text.size() > max_program_bytes) {
    program.truncated = true;
    text = text.substr(0, max_program_bytes);
    // Whole lines only: a block cut short must never read as a source line. When even the
    // first line is longer than the cap there is no text at all.
    const auto end = text.find_last_of("\r\n");
    text = end == std::string_view::npos ? std::string_view{} : text.substr(0, end + 1);
  }
  program.lines = lines(text);
  return program;
}

} // namespace coffeecnc
