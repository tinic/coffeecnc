#ifndef COFFEECNC_TESTS_FAKE_TOOL_MODEL_FILES_HPP
#define COFFEECNC_TESTS_FAKE_TOOL_MODEL_FILES_HPP

#include <coffeecnc/app/tool_model_store.hpp>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace coffeecnc {

// What several FakeToolModelFiles share, as processes share a disk.
struct FakeDisk {
  std::map<std::string, std::string> files; // by path
  std::set<std::string> directories{"/"};
  std::map<std::string, unsigned> modes; // of directories; 0700 where none is given
  std::set<std::string> locked;          // directories whose lock is held
  // Symbolic links: path, the absolute path it points to. A link to a directory makes every
  // path below it a path below the target.
  std::map<std::string, std::string> links;
  std::map<std::string, unsigned> file_modes; // 0644 where none is given
  std::map<std::string, unsigned> groups;     // 1000 where none is given
  // Groups the user is not in: a file is not given one of them (EPERM), and keeps 1000.
  std::set<unsigned> foreign_groups;
};

// The tool-model writer's file system in memory. Every step can be made to fail, and a hook
// runs inside each: what happens while the disk is slow. Not thread-safe: read it only while
// the writer is at rest or from the hook.
class FakeToolModelFiles final : public ToolModelFiles {
public:
  explicit FakeToolModelFiles(FakeDisk& shared) : disk(shared) {}
  ~FakeToolModelFiles() override { release(); }
  FakeToolModelFiles(const FakeToolModelFiles&) = delete;
  auto operator=(const FakeToolModelFiles&) -> FakeToolModelFiles& = delete;

  FakeDisk& disk;
  // Steps that fail, each as often as its count: make_directory, lock, read, open_temporary,
  // write, sync, rename, link, sync_directory; or one call of a step, as `<step> <path>`.
  std::map<std::string, int> failures;
  // Every call, as `<step> <path>`.
  std::vector<std::string> calls;
  // Called inside every step, with its name, before it does anything.
  std::function<void(std::string_view)> during;

  auto make_directory(const std::string& path)
      -> std::expected<Directory, LocalizedReason> override {
    if (begin("make_directory", path))
      return std::unexpected(
          LocalizedReason{"the directory " + path + " cannot be made: Permission denied"});
    const bool created = disk.directories.insert(path).second;
    const auto mode = disk.modes.find(path);
    return Directory{created, mode == disk.modes.end() ? 0700U : mode->second};
  }
  auto lock(const std::string& directory) -> std::expected<void, LockError> override {
    if (begin("lock", directory))
      return std::unexpected(
          LockError{false, {"the lock file cannot be opened: Permission denied"}});
    disk.files.try_emplace(directory + "/lock", "");
    if (!disk.locked.insert(directory).second)
      return std::unexpected(
          LockError{true, {"the lock cannot be taken: Resource temporarily unavailable"}});
    held_ = directory;
    return {};
  }
  void unlock() override {
    calls.emplace_back("unlock");
    release();
  }
  auto read(const std::string& path, std::size_t limit)
      -> std::expected<std::optional<Content>, LocalizedReason> override {
    if (begin("read", path))
      return std::unexpected(LocalizedReason{"it cannot be read: Input/output error"});
    const auto file = disk.files.find(path);
    if (file == disk.files.end())
      return std::optional<Content>{};
    return Content{file->second.substr(0, limit), file->second.size() > limit};
  }
  auto open_temporary(const std::string& path) -> Done override {
    if (begin("open_temporary", path))
      return std::unexpected(
          LocalizedReason{"the temporary file cannot be made: Permission denied"});
    disk.files[path].clear();
    temporary_ = path;
    return {};
  }
  auto write(std::string_view bytes) -> Done override {
    if (begin("write", temporary_)) {
      // Half of it reached the disk.
      disk.files[temporary_] += bytes.substr(0, bytes.size() / 2);
      return std::unexpected(
          LocalizedReason{"the temporary file cannot be written: No space left on device"});
    }
    disk.files[temporary_] += bytes;
    return {};
  }
  auto sync() -> Done override {
    if (begin("sync", temporary_))
      return std::unexpected(
          LocalizedReason{"the temporary file cannot be synchronised: Input/output error"});
    return {};
  }
  auto rename(const std::string& from, const std::string& to) -> Done override {
    if (begin("rename", from + " " + to))
      return std::unexpected(LocalizedReason{"the file cannot be replaced: Read-only file system"});
    auto node = disk.files.extract(from);
    disk.files.insert_or_assign(to, std::move(node.mapped()));
    return {};
  }
  auto link(const std::string& from, const std::string& to) -> Done override {
    if (begin("link", from + " " + to))
      return std::unexpected(LocalizedReason{"the file cannot be made: Read-only file system"});
    if (disk.files.contains(to) || disk.directories.contains(to))
      return std::unexpected(LocalizedReason{"the file cannot be made: File exists"});
    disk.files[to] = disk.files[from];
    return {};
  }
  auto sync_directory(const std::string& directory) -> Done override {
    if (begin("sync_directory", directory))
      return std::unexpected(
          LocalizedReason{"the directory cannot be synchronised: Input/output error"});
    return {};
  }
  void discard(const std::string& path) override {
    calls.push_back("discard " + path);
    disk.files.erase(path);
  }
  // A failing append writes half of the bytes, as a full disk does.
  auto append(const std::string& path, std::string_view bytes) -> Done override {
    if (begin("append", path)) {
      disk.files[path] += bytes.substr(0, bytes.size() / 2);
      return std::unexpected(
          LocalizedReason{"the file cannot be written: No space left on device"});
    }
    disk.files[path] += bytes;
    return {};
  }

  // Beneath `root`: a link on the way is refused, the last component is followed for reading.
  // Steps: read_beneath, replace (before the rename), replace_sync (after it), remove,
  // folder_exists, relink.
  auto read_beneath(const std::string& root, const std::string& relative, std::size_t limit)
      -> std::expected<std::optional<Found>, LocalizedReason> override {
    const auto path = root + '/' + relative;
    if (begin("read_beneath", path))
      return std::unexpected(
          LocalizedReason{path + " cannot be read: Input/output error",
                          MessageKey{"backend.machineData.cannotRead",
                                     {{"path", path}, {"detail", "Input/output error"}}}});
    if (auto linked = through_link(root, relative))
      return std::unexpected(LocalizedReason{*linked});
    const auto linked = disk.links.find(path);
    const auto actual = linked == disk.links.end() ? path : linked->second;
    const auto file = disk.files.find(actual);
    if (file == disk.files.end())
      return std::optional<Found>{};
    const auto mode = disk.file_modes.find(actual);
    const auto group = disk.groups.find(actual);
    return Found{{file->second.substr(0, limit), file->second.size() > limit},
                 linked != disk.links.end(),
                 linked == disk.links.end() ? std::string{} : linked->second,
                 mode == disk.file_modes.end() ? 0644U : mode->second,
                 group == disk.groups.end() ? 1000U : group->second};
  }
  auto replace_beneath(const std::string& root, const std::string& relative,
                       std::string_view bytes, unsigned mode, std::optional<unsigned> group)
      -> Replaced override {
    const auto path = root + '/' + relative;
    if (begin("replace", path))
      return std::unexpected(
          ReplaceError{false,
                       {path + " cannot be replaced: No space left on device",
                        MessageKey{"backend.machineData.configNotReplaced",
                                   {{"path", path}, {"detail", "No space left on device"}}}}});
    if (auto linked = through_link(root, relative))
      return std::unexpected(ReplaceError{false, {*linked}});
    disk.links.erase(path); // the link is replaced, not followed
    disk.files[path] = std::string(bytes);
    disk.file_modes[path] = mode;
    Written kept;
    if (group && disk.foreign_groups.contains(*group))
      kept.group_kept = false;
    if (group && kept.group_kept)
      disk.groups[path] = *group;
    else
      disk.groups.erase(path);
    if (begin("replace_sync", path))
      return std::unexpected(ReplaceError{
          true,
          {path + " was replaced, but its folder cannot be synchronised: Input/output error",
           MessageKey{"backend.machineData.configFolderNotSynced",
                      {{"path", path}, {"detail", "Input/output error"}}}}});
    return kept;
  }
  // A folder exists when it was made or holds a file or a link.
  auto folder_exists_beneath(const std::string& root, const std::string& relative)
      -> std::expected<bool, LocalizedReason> override {
    const auto path = root + '/' + relative;
    if (begin("folder_exists", path))
      return std::unexpected(
          LocalizedReason{path + " cannot be read: Input/output error",
                          MessageKey{"backend.machineData.cannotRead",
                                     {{"path", path}, {"detail", "Input/output error"}}}});
    if (auto linked = through_link(root, relative))
      return std::unexpected(LocalizedReason{*linked});
    const auto folder = path.substr(0, path.rfind('/'));
    const auto below = [&](const auto& entries) {
      return std::ranges::any_of(
          entries, [&](const auto& entry) { return entry.first.starts_with(folder + '/'); });
    };
    return disk.directories.contains(folder) || below(disk.files) || below(disk.links);
  }
  auto link_beneath(const std::string& root, const std::string& relative, const std::string& target)
      -> std::expected<void, std::string> override {
    const auto path = root + '/' + relative;
    if (begin("relink", path))
      return std::unexpected(path + " cannot be made a link again: Read-only file system");
    if (auto linked = through_link(root, relative))
      return std::unexpected(*linked);
    disk.files.erase(path);
    disk.file_modes.erase(path);
    disk.groups.erase(path);
    disk.links[path] = target;
    return {};
  }
  auto remove_beneath(const std::string& root, const std::string& relative)
      -> std::expected<void, std::string> override {
    const auto path = root + '/' + relative;
    if (begin("remove", path))
      return std::unexpected(path + " cannot be removed: Read-only file system");
    if (auto linked = through_link(root, relative))
      return std::unexpected(*linked);
    disk.links.erase(path);
    disk.files.erase(path);
    return {};
  }

  // Calls of this step so far.
  [[nodiscard]] auto count(std::string_view step) const -> std::size_t {
    return static_cast<std::size_t>(std::ranges::count_if(calls, [&](const auto& call) -> bool {
      return call == step || call.starts_with(std::string(step) + ' ');
    }));
  }

private:
  // Records the call, runs the hook; true when the step is to fail.
  auto begin(std::string_view step, const std::string& path) -> bool {
    calls.push_back(std::string(step) + ' ' + path);
    if (during)
      during(step);
    for (const auto& key : {std::string(step), std::string(step) + ' ' + path}) {
      const auto failing = failures.find(key);
      if (failing != failures.end() && failing->second > 0) {
        --failing->second;
        return true;
      }
    }
    return false;
  }
  // Why `relative` is not beneath `root`: a component on the way is a link, or not plain.
  [[nodiscard]] auto through_link(const std::string& root, const std::string& relative) const
      -> std::optional<std::string> {
    std::string at = root;
    std::string_view rest{relative};
    while (!rest.empty()) {
      const auto slash = rest.find('/');
      const auto part = rest.substr(0, slash);
      if (part.empty() || part == "." || part == "..")
        return std::format("{}/{} is not a plain path", root, relative);
      rest.remove_prefix(slash == std::string_view::npos ? rest.size() : slash + 1);
      at += '/';
      at += part;
      if (slash != std::string_view::npos && disk.links.contains(at))
        return std::format("{}/{} is reached through a symbolic link or is not beneath {}", root,
                           relative, root);
    }
    return std::nullopt;
  }
  void release() {
    if (!held_.empty())
      disk.locked.erase(held_);
    held_.clear();
  }

  std::string held_;
  std::string temporary_;
};

} // namespace coffeecnc

#endif // COFFEECNC_TESTS_FAKE_TOOL_MODEL_FILES_HPP
