#ifndef COFFEECNC_LINUXCNC_DURABLE_FILE_HPP
#define COFFEECNC_LINUXCNC_DURABLE_FILE_HPP

#include <expected>
#include <string>
#include <string_view>

namespace coffeecnc {

// Files the adapter replaces (the tool table, its sidecar records) survive a power cut as the
// old file or the new one, whole: as LocalToolModelFiles does it in the app layer.

// Writes `bytes` to `path`, a new file or one truncated; never through a symbolic link
// (O_NOFOLLOW). Mode 0666 less the umask, as std::ofstream made it. On disk when it returns:
// written, synchronised and closed. The error names what failed.
[[nodiscard]] auto write_synced(const std::string& path, std::string_view bytes)
    -> std::expected<void, std::string>;

// Renames `from` over `to`, then synchronises the directory of `to` so that the rename is on
// disk too. Both in one directory.
[[nodiscard]] auto rename_synced(const std::string& from, const std::string& to)
    -> std::expected<void, std::string>;

// Both: `bytes` to `<path>.new`, then that over `path`. What a failure leaves is the old file,
// and perhaps `<path>.new`.
[[nodiscard]] auto replace_synced(const std::string& path, std::string_view bytes)
    -> std::expected<void, std::string>;

} // namespace coffeecnc

#endif // COFFEECNC_LINUXCNC_DURABLE_FILE_HPP
