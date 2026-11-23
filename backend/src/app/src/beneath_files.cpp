// LocalToolModelFiles: the files of a machine's configuration, resolved beneath a directory.
// A restore writes them; nothing it writes may be reached through a symbolic link.

#include <coffeecnc/app/tool_model_store.hpp>

#include <cerrno>
#include <format>
#include <string_view>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__linux__) && __has_include(<linux/openat2.h>)
#include <linux/openat2.h>
#include <sys/syscall.h>
#define COFFEECNC_HAVE_OPENAT2 1
#endif

namespace coffeecnc {
namespace {

constexpr std::string_view temporary_suffix = ".coffeecnc-tmp";

auto said(int error) -> std::string { return std::generic_category().message(error); }

// A descriptor that is closed with its scope.
class Descriptor {
public:
  explicit Descriptor(int descriptor = -1) : descriptor_(descriptor) {}
  ~Descriptor() {
    if (descriptor_ >= 0)
      static_cast<void>(::close(descriptor_));
  }
  Descriptor(const Descriptor&) = delete;
  auto operator=(const Descriptor&) -> Descriptor& = delete;
  Descriptor(Descriptor&& other) noexcept : descriptor_(std::exchange(other.descriptor_, -1)) {}
  auto operator=(Descriptor&& other) noexcept -> Descriptor& {
    std::swap(descriptor_, other.descriptor_);
    return *this;
  }
  [[nodiscard]] auto get() const -> int { return descriptor_; }
  [[nodiscard]] auto valid() const -> bool { return descriptor_ >= 0; }
  // Closes it now: false when close failed.
  auto close() -> bool { return descriptor_ < 0 || ::close(std::exchange(descriptor_, -1)) == 0; }

private:
  int descriptor_;
};

// A path of plain components: none empty, `.` or `..`.
auto plain(std::string_view relative) -> bool {
  if (relative.empty() || relative.front() == '/' || relative.back() == '/')
    return false;
  while (!relative.empty()) {
    const auto slash = relative.find('/');
    const auto part = relative.substr(0, slash);
    if (part.empty() || part == "." || part == "..")
      return false;
    relative.remove_prefix(slash == std::string_view::npos ? relative.size() : slash + 1);
  }
  return true;
}

// The directory `relative` beneath the open directory `root`, no component a symbolic link:
// openat2 with RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS where the kernel has it, else one
// component at a time with O_NOFOLLOW | O_DIRECTORY. Not valid, with errno, when it fails.
auto open_directory_beneath(int root, const std::string& relative) -> Descriptor {
#ifdef COFFEECNC_HAVE_OPENAT2
  open_how how{};
  how.flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
  how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;
  const auto opened = ::syscall(SYS_openat2, root, relative.c_str(), &how, sizeof(how));
  if (opened >= 0)
    return Descriptor{static_cast<int>(opened)};
  // An old kernel, or a sandbox that refuses the call: the walk below does the same.
  if (errno != ENOSYS && errno != EPERM)
    return Descriptor{};
#endif
  Descriptor current{::fcntl(root, F_DUPFD_CLOEXEC, 0)};
  std::string_view rest{relative};
  while (current.valid() && !rest.empty()) {
    const auto slash = rest.find('/');
    const std::string part{rest.substr(0, slash)};
    rest.remove_prefix(slash == std::string_view::npos ? rest.size() : slash + 1);
    current = Descriptor{
        ::openat(current.get(), part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
  }
  return current;
}

// The directory that holds `relative` beneath `root`, opened, and the name in it.
struct Parent {
  Descriptor directory;
  std::string name;
};

// Why the parent could not be opened; `missing`: a folder on the way does not exist.
struct NoParent {
  bool missing{};
  std::string reason;
};

auto parent_beneath(const std::string& root, const std::string& relative)
    -> std::expected<Parent, NoParent> {
  const auto shown = root + '/' + relative;
  const auto fail = [](int error, std::string reason) -> std::unexpected<NoParent> {
    return std::unexpected(NoParent{error == ENOENT, std::move(reason)});
  };
  if (!plain(relative))
    return fail(0, std::format("{} is not a plain path beneath {}", relative, root));
  // The root is the configuration's own folder, as it is configured: it may be a link.
  const Descriptor top{::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
  if (!top.valid()) {
    const int error = errno;
    return fail(error, std::format("{} cannot be opened: {}", root, said(error)));
  }
  const auto slash = relative.rfind('/');
  Parent found{Descriptor{}, slash == std::string::npos ? relative : relative.substr(slash + 1)};
  if (slash == std::string::npos) {
    found.directory = Descriptor{::fcntl(top.get(), F_DUPFD_CLOEXEC, 0)};
  } else {
    found.directory = open_directory_beneath(top.get(), relative.substr(0, slash));
    if (!found.directory.valid()) {
      const int error = errno;
      if (error == ELOOP || error == EXDEV || error == ENOTDIR)
        return fail(error, std::format("{} is reached through a symbolic link or is not "
                                       "beneath {}",
                                       shown, root));
      return fail(error, std::format("{} cannot be opened: {}", shown, said(error)));
    }
  }
  if (!found.directory.valid()) {
    const int error = errno;
    return fail(error, std::format("{} cannot be opened: {}", root, said(error)));
  }
  return found;
}

} // namespace

auto LocalToolModelFiles::read_beneath(const std::string& root, const std::string& relative,
                                       std::size_t limit)
    -> std::expected<std::optional<Found>, std::string> {
  auto parent = parent_beneath(root, relative);
  if (!parent) {
    // A missing folder is a missing file.
    if (parent.error().missing)
      return std::optional<Found>{};
    return std::unexpected(parent.error().reason);
  }
  const auto shown = root + '/' + relative;
  struct stat entry{};
  if (::fstatat(parent->directory.get(), parent->name.c_str(), &entry, AT_SYMLINK_NOFOLLOW) != 0) {
    if (errno == ENOENT)
      return std::optional<Found>{};
    return std::unexpected(std::format("{} cannot be read: {}", shown, said(errno)));
  }
  Found found;
  found.symlink = S_ISLNK(entry.st_mode);
  const Descriptor file{::openat(parent->directory.get(), parent->name.c_str(),
                                 O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY)};
  if (!file.valid()) {
    if (errno == ENOENT) // a link to nothing
      return std::optional<Found>{};
    return std::unexpected(std::format("{} cannot be opened: {}", shown, said(errno)));
  }
  struct stat info{};
  if (::fstat(file.get(), &info) != 0 || !S_ISREG(info.st_mode))
    return std::unexpected(std::format("{} is not a regular file", shown));
  found.mode = info.st_mode & 07777U;
  found.group = info.st_gid;
  auto& bytes = found.content.bytes;
  bytes.resize(limit + 1);
  std::size_t size = 0;
  while (size < bytes.size()) {
    const auto got = ::read(file.get(), bytes.data() + size, bytes.size() - size);
    if (got < 0 && errno == EINTR)
      continue;
    if (got < 0)
      return std::unexpected(std::format("{} cannot be read: {}", shown, said(errno)));
    if (got == 0)
      break;
    size += static_cast<std::size_t>(got);
  }
  found.content.more = size > limit;
  bytes.resize(std::min(size, limit));
  return found;
}

auto LocalToolModelFiles::replace_beneath(const std::string& root, const std::string& relative,
                                          std::string_view bytes, unsigned mode,
                                          std::optional<unsigned> group) -> Replaced {
  const auto fail = [&](std::string reason, bool renamed = false) -> Replaced {
    return std::unexpected(ReplaceError{renamed, std::move(reason)});
  };
  auto parent = parent_beneath(root, relative);
  if (!parent)
    return fail(parent.error().reason);
  const auto shown = root + '/' + relative;
  const int directory = parent->directory.get();
  const auto temporary = parent->name + std::string(temporary_suffix);
  static_cast<void>(::unlinkat(directory, temporary.c_str(), 0)); // what a crash left
  Descriptor file{::openat(directory, temporary.c_str(),
                           O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600)};
  if (!file.valid())
    return fail(
        std::format("{} cannot be made: {}", shown + std::string(temporary_suffix), said(errno)));
  const auto written = [&]() -> std::string {
    while (!bytes.empty()) {
      const auto done = ::write(file.get(), bytes.data(), bytes.size());
      if (done < 0 && errno == EINTR)
        continue;
      if (done < 0)
        return std::format("{} cannot be written: {}", shown, said(errno));
      bytes.remove_prefix(static_cast<std::size_t>(done));
    }
    if (group && ::fchown(file.get(), static_cast<uid_t>(-1), *group) != 0)
      return std::format("{} cannot keep its group {}: {}", shown, *group, said(errno));
    if (::fchmod(file.get(), mode & 07777U) != 0)
      return std::format("{} cannot keep its mode {:o}: {}", shown, mode, said(errno));
    if (::fsync(file.get()) != 0)
      return std::format("{} cannot be synchronised: {}", shown, said(errno));
    if (!file.close())
      return std::format("{} cannot be closed: {}", shown, said(errno));
    return {};
  }();
  if (written.empty() &&
      ::renameat(directory, temporary.c_str(), directory, parent->name.c_str()) == 0) {
    if (::fsync(directory) != 0)
      return fail(std::format("{} was replaced, but its folder cannot be synchronised: {}", shown,
                              said(errno)),
                  true);
    return {};
  }
  const auto reason =
      written.empty() ? std::format("{} cannot be replaced: {}", shown, said(errno)) : written;
  static_cast<void>(::unlinkat(directory, temporary.c_str(), 0));
  return fail(reason);
}

auto LocalToolModelFiles::remove_beneath(const std::string& root, const std::string& relative)
    -> Done {
  auto parent = parent_beneath(root, relative);
  if (!parent) {
    if (parent.error().missing)
      return {};
    return std::unexpected(parent.error().reason);
  }
  const auto shown = root + '/' + relative;
  if (::unlinkat(parent->directory.get(), parent->name.c_str(), 0) != 0 && errno != ENOENT)
    return std::unexpected(std::format("{} cannot be removed: {}", shown, said(errno)));
  if (::fsync(parent->directory.get()) != 0)
    return std::unexpected(std::format("{} was removed, but its folder cannot be synchronised: {}",
                                       shown, said(errno)));
  return {};
}

} // namespace coffeecnc
