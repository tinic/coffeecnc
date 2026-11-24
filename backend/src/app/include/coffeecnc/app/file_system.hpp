#ifndef COFFEECNC_APP_FILE_SYSTEM_HPP
#define COFFEECNC_APP_FILE_SYSTEM_HPP

#include <coffeecnc/core/message_key.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace coffeecnc {

enum class FileKind : std::uint8_t { directory, file, other };

struct FileInfo {
  FileKind kind{FileKind::other};
  std::uint64_t bytes{};   // 0 for anything but a regular file
  std::int64_t modified{}; // milliseconds since the Unix epoch
};

struct Names {
  std::vector<std::string> names; // without `.` and `..`, in no particular order
  bool more{};                    // the directory holds names beyond the limit
};

// The file system as ProgramStore reads it. Paths are absolute byte strings with `/` separators.
// Every failure carries a reason in words for the operator. Nothing here writes.
class FileSystem {
public:
  FileSystem() = default;
  virtual ~FileSystem() = default;

  // The path with every `.`, `..` and symbolic link resolved. Fails when it does not exist.
  [[nodiscard]] virtual auto canonical(const std::string& path) const
      -> std::expected<std::string, std::string> = 0;
  // Of what the path refers to after following symbolic links.
  [[nodiscard]] virtual auto info(const std::string& path) const
      -> std::expected<FileInfo, std::string> = 0;
  // At most `limit` names of the directory.
  [[nodiscard]] virtual auto names(const std::string& directory, std::size_t limit) const
      -> std::expected<Names, std::string> = 0;
  // The first `limit` bytes of the file, or all of it when it is shorter.
  [[nodiscard]] virtual auto read(const std::string& path, std::size_t limit) const
      -> std::expected<std::string, LocalizedReason> = 0;

protected:
  FileSystem(const FileSystem&) = default;
  FileSystem(FileSystem&&) = default;
  auto operator=(const FileSystem&) -> FileSystem& = default;
  auto operator=(FileSystem&&) -> FileSystem& = default;
};

// The host's file system through std::filesystem.
class LocalFileSystem final : public FileSystem {
public:
  [[nodiscard]] auto canonical(const std::string& path) const
      -> std::expected<std::string, std::string> override;
  [[nodiscard]] auto info(const std::string& path) const
      -> std::expected<FileInfo, std::string> override;
  [[nodiscard]] auto names(const std::string& directory, std::size_t limit) const
      -> std::expected<Names, std::string> override;
  [[nodiscard]] auto read(const std::string& path, std::size_t limit) const
      -> std::expected<std::string, LocalizedReason> override;
};

} // namespace coffeecnc

#endif // COFFEECNC_APP_FILE_SYSTEM_HPP
