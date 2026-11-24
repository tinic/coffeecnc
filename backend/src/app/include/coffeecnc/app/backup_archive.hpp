#ifndef COFFEECNC_APP_BACKUP_ARCHIVE_HPP
#define COFFEECNC_APP_BACKUP_ARCHIVE_HPP

#include <coffeecnc/core/message_key.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace coffeecnc {

// Backup archives through libarchive: POSIX tar (pax restricted: ustar headers unless a name
// needs more), compressed with gzip. Built only where libarchive is (not in the msan preset).
struct ArchiveEntry {
  std::string name; // relative, `/` separated
  std::string bytes;
  std::int64_t modified{}; // seconds since the Unix epoch
  friend auto operator==(const ArchiveEntry&, const ArchiveEntry&) -> bool = default;
};

inline constexpr std::size_t max_archive_entry_bytes = std::size_t{16} * 1024 * 1024;
// Of the archive as stored, and of all its entries unpacked.
inline constexpr std::size_t max_archive_bytes = std::size_t{64} * 1024 * 1024;

// The archive of the entries, in their order: regular files with mode 0644, owner 0.
[[nodiscard]] auto write_archive(const std::vector<ArchiveEntry>& entries)
    -> std::expected<std::string, LocalizedReason>;

// The regular files of a tar archive, plain or gzip-compressed, in its order; directory entries
// are skipped. Refused: anything libarchive cannot read, every other entry type (symbolic and
// hard links, devices), an entry over max_archive_entry_bytes, more than max_archive_bytes in
// all. Nothing is extracted: the names come back as written, and what they may be is the
// caller's to check against its own list of targets.
[[nodiscard]] auto read_archive(std::string_view archive)
    -> std::expected<std::vector<ArchiveEntry>, LocalizedReason>;

} // namespace coffeecnc

#endif // COFFEECNC_APP_BACKUP_ARCHIVE_HPP
