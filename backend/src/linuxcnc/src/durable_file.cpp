#include "durable_file.hpp"

#include <cerrno>
#include <cstring>
#include <format>

#include <fcntl.h>
#include <unistd.h>

namespace coffeecnc {
namespace {

auto failure(std::string_view what, const std::string& path) -> std::unexpected<std::string> {
  return std::unexpected(std::format("{} {}: {}", what, path, std::strerror(errno)));
}

// Closes and reports whether the close failed for another reason than a signal.
auto close_file(int file) -> bool { return ::close(file) == 0 || errno == EINTR; }

auto directory_of(const std::string& path) -> std::string {
  const auto slash = path.rfind('/');
  if (slash == std::string::npos)
    return ".";
  return slash == 0 ? std::string("/") : path.substr(0, slash);
}

} // namespace

auto write_synced(const std::string& path, std::string_view bytes)
    -> std::expected<void, std::string> {
  constexpr mode_t mode = 0666;
  const int file = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, mode);
  if (file < 0)
    return failure("Cannot make", path);
  while (!bytes.empty()) {
    const auto written = ::write(file, bytes.data(), bytes.size());
    if (written < 0 && errno == EINTR)
      continue;
    if (written < 0) {
      auto failed = failure("Cannot write", path);
      static_cast<void>(close_file(file));
      return failed;
    }
    bytes.remove_prefix(static_cast<std::size_t>(written));
  }
  if (::fsync(file) != 0) {
    auto failed = failure("Cannot synchronise", path);
    static_cast<void>(close_file(file));
    return failed;
  }
  if (!close_file(file))
    return failure("Cannot close", path);
  return {};
}

auto rename_synced(const std::string& from, const std::string& to)
    -> std::expected<void, std::string> {
  if (::rename(from.c_str(), to.c_str()) != 0)
    return failure("Cannot replace", to);
  const auto directory = directory_of(to);
  const int handle = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (handle < 0)
    return failure("Cannot open the directory", directory);
  const bool synced = ::fsync(handle) == 0;
  auto failed = synced ? std::expected<void, std::string>{}
                       : failure("Cannot synchronise the directory", directory);
  static_cast<void>(close_file(handle));
  return failed;
}

auto replace_synced(const std::string& path, std::string_view bytes)
    -> std::expected<void, std::string> {
  const auto temporary = path + ".new";
  if (auto written = write_synced(temporary, bytes); !written)
    return written;
  return rename_synced(temporary, path);
}

} // namespace coffeecnc
