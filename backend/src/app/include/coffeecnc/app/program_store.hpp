#ifndef COFFEECNC_APP_PROGRAM_STORE_HPP
#define COFFEECNC_APP_PROGRAM_STORE_HPP

#include <coffeecnc/app/file_system.hpp>
#include <coffeecnc/core/panel_messages.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace coffeecnc {

// The programs the panel may see and open: everything below one directory, LinuxCNC's
// [DISPLAY] PROGRAM_PREFIX. Read-only. Every failure is a reason in words for the operator.
//
// A requested path is absolute, or relative to the root. It is resolved (`.`, `..`, symbolic
// links) and the result must be the root or below it; a link inside the root that leads outside
// is refused like any other outside path. What is returned and passed on is always the resolved
// path, so a root that is itself a link is reported by its target. A path that does not exist,
// that holds a NUL byte, or whose resolved form is not UTF-8 (the panel could not name it) is
// refused. With an empty root everything is refused.
//
// The check and the later use are separate system calls: someone who can write below the root
// can swap a directory for a link in between. The root must not be writable by anyone who should
// not choose what the machine runs.
//
// Stateless: the root is resolved again on every call.
class ProgramStore {
public:
  // Entries of one listing. A directory with more is listed truncated.
  static constexpr std::size_t max_directory_entries = 1000;
  // Names read from one directory before sorting. In a directory with more, which entries are
  // listed depends on the order the file system returns them in.
  static constexpr std::size_t max_directory_names = 10000;
  // Program text handed to the panel. A longer file is cut at the last line end before the cap.
  static constexpr std::size_t max_program_bytes = std::size_t{16} * 1024 * 1024;
  // LinuxCNC's LINELEN is 255 including the terminator (EMC_TASK_PLAN_OPEN).
  static constexpr std::size_t max_program_path_bytes = 254;

  // `files` must outlive the store. `root` is absolute; empty when none is configured. A listing
  // without a path shows `start` (the root when empty), which must lie inside the root.
  ProgramStore(const FileSystem& files, std::string root, std::string start = {});

  // The directory at `path`; the root without one. Only directories and regular files (links
  // resolved, and only when they stay inside the root) whose names are UTF-8, do not start with
  // a dot and hold no control character. Directories first, then by name without regard to the
  // case of ASCII letters, then by name.
  [[nodiscard]] auto list(const std::optional<std::string>& path) const
      -> std::expected<Directory, std::string>;

  // The resolved path to hand to LinuxCNC for `open_program`: a regular file inside the root, at
  // most max_program_path_bytes long.
  [[nodiscard]] auto resolve_program(std::string_view path) const
      -> std::expected<std::string, std::string>;

  // The text of a program under the same rules, for the `program` message. Lines end at `\n`,
  // `\r\n` or `\r`; a line end at the end of the file does not start another line. Every byte
  // that is not part of a well-formed UTF-8 sequence becomes one U+FFFD.
  [[nodiscard]] auto read_program(std::string_view path, std::uint64_t revision) const
      -> std::expected<ProgramText, std::string>;

private:
  struct Resolved {
    std::string root;
    std::string path;
  };
  [[nodiscard]] auto resolve(std::string_view path) const -> std::expected<Resolved, std::string>;

  const FileSystem& files_;
  std::string root_;
  std::string start_;
};

} // namespace coffeecnc

#endif // COFFEECNC_APP_PROGRAM_STORE_HPP
