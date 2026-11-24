// LocalToolModelFiles: the files of a machine's configuration, resolved beneath a directory.
// A restore writes them; nothing it writes may be reached through a symbolic link.

#include <coffeecnc/app/tool_model_store.hpp>

#include <cerrno>
#include <climits>
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

auto cannot_open(const std::string& path, int error) -> LocalizedReason {
  const auto detail = said(error);
  return {std::format("{} cannot be opened: {}", path, detail),
          MessageKey{"backend.machineData.configNotOpened", {{"path", path}, {"detail", detail}}}};
}

auto cannot_read(const std::string& path, int error) -> LocalizedReason {
  const auto detail = said(error);
  return {std::format("{} cannot be read: {}", path, detail),
          MessageKey{"backend.machineData.cannotRead", {{"path", path}, {"detail", detail}}}};
}

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
  LocalizedReason reason;
};

auto parent_beneath(const std::string& root, const std::string& relative)
    -> std::expected<Parent, NoParent> {
  const auto shown = root + '/' + relative;
  const auto fail = [](int error, LocalizedReason reason) -> std::unexpected<NoParent> {
    return std::unexpected(NoParent{error == ENOENT, std::move(reason)});
  };
  if (!plain(relative))
    return fail(0, {std::format("{} is not a plain path beneath {}", relative, root),
                    MessageKey{"backend.machineData.configNotRelative",
                               {{"path", relative}, {"root", root}}}});
  // The root is the configuration's own folder, as it is configured: it may be a link.
  const Descriptor top{::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
  if (!top.valid()) {
    const int error = errno;
    return fail(error, cannot_open(root, error));
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
        return fail(error, {std::format("{} is reached through a symbolic link or is not "
                                        "beneath {}",
                                        shown, root),
                            MessageKey{"backend.machineData.configParentOutside",
                                       {{"path", shown}, {"root", root}}}});
      return fail(error, cannot_open(shown, error));
    }
  }
  if (!found.directory.valid()) {
    const int error = errno;
    return fail(error, cannot_open(root, error));
  }
  return found;
}

} // namespace

auto LocalToolModelFiles::read_beneath(const std::string& root, const std::string& relative,
                                       std::size_t limit)
    -> std::expected<std::optional<Found>, LocalizedReason> {
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
    return std::unexpected(cannot_read(shown, errno));
  }
  Found found;
  found.symlink = S_ISLNK(entry.st_mode);
  if (found.symlink) {
    // Where it points, so that a rollback can make the same link again.
    std::string target(PATH_MAX, '\0');
    const auto length =
        ::readlinkat(parent->directory.get(), parent->name.c_str(), target.data(), target.size());
    if (length < 0)
      return std::unexpected(cannot_read(shown, errno));
    if (static_cast<std::size_t>(length) >= target.size())
      return std::unexpected(
          LocalizedReason{std::format("{} cannot be read: link too long", shown),
                          MessageKey{"backend.machineData.configLinkTooLong", {{"path", shown}}}});
    target.resize(static_cast<std::size_t>(length));
    found.link = std::move(target);
  }
  const Descriptor file{::openat(parent->directory.get(), parent->name.c_str(),
                                 O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY)};
  if (!file.valid()) {
    if (errno == ENOENT) // a link to nothing
      return std::optional<Found>{};
    return std::unexpected(cannot_open(shown, errno));
  }
  struct stat info{};
  if (::fstat(file.get(), &info) != 0 || !S_ISREG(info.st_mode))
    return std::unexpected(
        LocalizedReason{std::format("{} is not a regular file", shown),
                        MessageKey{"backend.machineData.configNotRegular", {{"path", shown}}}});
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
      return std::unexpected(cannot_read(shown, errno));
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
  const auto fail = [](LocalizedReason reason, bool renamed = false) -> Replaced {
    return std::unexpected(ReplaceError{renamed, std::move(reason)});
  };
  auto parent = parent_beneath(root, relative);
  if (!parent)
    return fail(std::move(parent.error().reason));
  const auto shown = root + '/' + relative;
  const int directory = parent->directory.get();
  const auto temporary = parent->name + std::string(temporary_suffix);
  static_cast<void>(::unlinkat(directory, temporary.c_str(), 0)); // what a crash left
  Descriptor file{::openat(directory, temporary.c_str(),
                           O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600)};
  if (!file.valid()) {
    const auto detail = said(errno);
    const auto path = shown + std::string(temporary_suffix);
    return fail({std::format("{} cannot be made: {}", path, detail),
                 MessageKey{"backend.machineData.configTempNotMade",
                            {{"path", path}, {"detail", detail}}}});
  }
  Written kept;
  auto written = [&]() -> LocalizedReason {
    while (!bytes.empty()) {
      const auto done = ::write(file.get(), bytes.data(), bytes.size());
      if (done < 0 && errno == EINTR)
        continue;
      if (done < 0) {
        const auto detail = said(errno);
        return {std::format("{} cannot be written: {}", shown, detail),
                MessageKey{"backend.machineData.configNotWritten",
                           {{"path", shown}, {"detail", detail}}}};
      }
      bytes.remove_prefix(static_cast<std::size_t>(done));
    }
    struct stat made{};
    if (::fstat(file.get(), &made) != 0) {
      const auto detail = said(errno);
      return {std::format("{} cannot be read back: {}", shown, detail),
              MessageKey{"backend.machineData.configNotReadBack",
                         {{"path", shown}, {"detail", detail}}}};
    }
    // Only a group that differs is set: a user who is not in the file's group may not set it,
    // even to the group the new file already has. A group the user may not give is not kept.
    if (group && made.st_gid != *group &&
        ::fchown(file.get(), static_cast<uid_t>(-1), static_cast<gid_t>(*group)) != 0) {
      if (errno != EPERM) {
        const auto detail = said(errno);
        const auto id = std::to_string(*group);
        return {std::format("{} cannot keep its group {}: {}", shown, id, detail),
                MessageKey{"backend.machineData.configGroupNotKept",
                           {{"path", shown}, {"group", id}, {"detail", detail}}}};
      }
      kept.group_kept = false;
    }
    if (::fchmod(file.get(), mode & 07777U) != 0) {
      const auto detail = said(errno);
      const auto permission = std::format("{:o}", mode);
      return {std::format("{} cannot keep its mode {}: {}", shown, permission, detail),
              MessageKey{"backend.machineData.configModeNotKept",
                         {{"path", shown}, {"mode", permission}, {"detail", detail}}}};
    }
    if (::fsync(file.get()) != 0) {
      const auto detail = said(errno);
      return {
          std::format("{} cannot be synchronised: {}", shown, detail),
          MessageKey{"backend.machineData.configNotSynced", {{"path", shown}, {"detail", detail}}}};
    }
    if (!file.close()) {
      const auto detail = said(errno);
      return {
          std::format("{} cannot be closed: {}", shown, detail),
          MessageKey{"backend.machineData.configNotClosed", {{"path", shown}, {"detail", detail}}}};
    }
    return {};
  }();
  if (written.empty() &&
      ::renameat(directory, temporary.c_str(), directory, parent->name.c_str()) == 0) {
    if (::fsync(directory) != 0) {
      const auto detail = said(errno);
      return fail(
          {std::format("{} was replaced, but its folder cannot be synchronised: {}", shown, detail),
           MessageKey{"backend.machineData.configFolderNotSynced",
                      {{"path", shown}, {"detail", detail}}}},
          true);
    }
    return kept;
  }
  if (written.empty()) {
    const auto detail = said(errno);
    written = {
        std::format("{} cannot be replaced: {}", shown, detail),
        MessageKey{"backend.machineData.configNotReplaced", {{"path", shown}, {"detail", detail}}}};
  }
  static_cast<void>(::unlinkat(directory, temporary.c_str(), 0));
  return fail(std::move(written));
}

auto LocalToolModelFiles::remove_beneath(const std::string& root, const std::string& relative)
    -> Done {
  auto parent = parent_beneath(root, relative);
  if (!parent) {
    if (parent.error().missing)
      return {};
    return std::unexpected(std::move(parent.error().reason));
  }
  const auto shown = root + '/' + relative;
  if (::unlinkat(parent->directory.get(), parent->name.c_str(), 0) != 0 && errno != ENOENT) {
    const auto error = said(errno);
    return std::unexpected(LocalizedReason{
        std::format("{} cannot be removed: {}", shown, error),
        MessageKey{"backend.machineData.rollbackRemove", {{"path", shown}, {"osError", error}}}});
  }
  if (::fsync(parent->directory.get()) != 0) {
    const auto error = said(errno);
    return std::unexpected(LocalizedReason{
        std::format("{} was removed, but its folder cannot be synchronised: {}", shown, error),
        MessageKey{"backend.machineData.rollbackRemoveSync",
                   {{"path", shown}, {"osError", error}}}});
  }
  return {};
}

auto LocalToolModelFiles::folder_exists_beneath(const std::string& root,
                                                const std::string& relative)
    -> std::expected<bool, LocalizedReason> {
  const auto parent = parent_beneath(root, relative);
  if (parent)
    return true;
  if (parent.error().missing)
    return false;
  return std::unexpected(parent.error().reason);
}

auto LocalToolModelFiles::link_beneath(const std::string& root, const std::string& relative,
                                       const std::string& target) -> Done {
  auto parent = parent_beneath(root, relative);
  if (!parent)
    return std::unexpected(std::move(parent.error().reason));
  const auto shown = root + '/' + relative;
  const int directory = parent->directory.get();
  const auto temporary = parent->name + std::string(temporary_suffix);
  const auto link_failure = [&](int number) {
    const auto error = said(number);
    return std::unexpected(LocalizedReason{
        std::format("{} cannot be made a link again: {}", shown, error),
        MessageKey{"backend.machineData.rollbackRelink", {{"path", shown}, {"osError", error}}}});
  };
  static_cast<void>(::unlinkat(directory, temporary.c_str(), 0));
  if (::symlinkat(target.c_str(), directory, temporary.c_str()) != 0)
    return link_failure(errno);
  if (::renameat(directory, temporary.c_str(), directory, parent->name.c_str()) != 0) {
    auto reason = link_failure(errno);
    static_cast<void>(::unlinkat(directory, temporary.c_str(), 0));
    return reason;
  }
  if (::fsync(directory) != 0) {
    const auto error = said(errno);
    return std::unexpected(LocalizedReason{
        std::format("{} is a link again, but its folder cannot be synchronised: {}", shown, error),
        MessageKey{"backend.machineData.rollbackRelinkSync",
                   {{"path", shown}, {"osError", error}}}});
  }
  return {};
}

} // namespace coffeecnc
