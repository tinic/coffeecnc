#include <coffeecnc/app/file_system.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace coffeecnc {
namespace {
namespace fs = std::filesystem;
} // namespace

auto LocalFileSystem::canonical(const std::string& path) const
    -> std::expected<std::string, std::string> {
  std::error_code error;
  auto resolved = fs::canonical(fs::path{path}, error);
  if (error)
    return std::unexpected(error.message());
  return resolved.string();
}

auto LocalFileSystem::info(const std::string& path) const -> std::expected<FileInfo, std::string> {
  std::error_code error;
  const fs::path file{path};
  const auto status = fs::status(file, error);
  if (error) {
    const auto reason = error.message();
    if (error == std::errc::no_such_file_or_directory && fs::is_symlink(fs::symlink_status(file, error)))
      return FileInfo{FileKind::other};
    return std::unexpected(reason);
  }
  FileInfo found;
  if (fs::is_directory(status)) {
    found.kind = FileKind::directory;
  } else if (fs::is_regular_file(status)) {
    found.kind = FileKind::file;
    found.bytes = fs::file_size(file, error);
    if (error)
      return std::unexpected(error.message());
  }
  const auto written = fs::last_write_time(file, error);
  if (error)
    return std::unexpected(error.message());
  found.modified = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::file_clock::to_sys(written).time_since_epoch())
                       .count();
  return found;
}

auto LocalFileSystem::names(const std::string& directory, std::size_t limit) const
    -> std::expected<Names, std::string> {
  std::error_code error;
  fs::directory_iterator entry{fs::path{directory}, error};
  if (error)
    return std::unexpected(error.message());
  Names found;
  for (; entry != fs::directory_iterator{}; entry.increment(error)) {
    if (found.names.size() == limit) {
      found.more = true;
      break;
    }
    found.names.push_back(entry->path().filename().string());
  }
  if (error)
    return std::unexpected(error.message());
  return found;
}

auto LocalFileSystem::read(const std::string& path, std::size_t limit) const
    -> std::expected<std::string, LocalizedReason> {
  std::ifstream file{path, std::ios::binary};
  if (!file.is_open())
    return std::unexpected(
        LocalizedReason{"The file cannot be opened.", MessageKey{"backend.fileSystem.cannotOpen"}});
  std::string content;
  std::string block(std::size_t{65536}, '\0');
  while (content.size() < limit && file) {
    file.read(block.data(),
              static_cast<std::streamsize>(std::min(block.size(), limit - content.size())));
    content.append(block.data(), static_cast<std::size_t>(file.gcount()));
  }
  if (file.bad())
    return std::unexpected(
        LocalizedReason{"The file cannot be read.", MessageKey{"backend.fileSystem.cannotRead"}});
  return content;
}

} // namespace coffeecnc
