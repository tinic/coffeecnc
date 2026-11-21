#ifndef COFFEECNC_TESTS_FAKE_TOOL_MODEL_FILES_HPP
#define COFFEECNC_TESTS_FAKE_TOOL_MODEL_FILES_HPP

#include <coffeecnc/app/tool_model_store.hpp>

#include <algorithm>
#include <cstddef>
#include <expected>
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
      -> std::expected<Directory, std::string> override {
    if (begin("make_directory", path))
      return std::unexpected("the directory " + path + " cannot be made: Permission denied");
    const bool created = disk.directories.insert(path).second;
    const auto mode = disk.modes.find(path);
    return Directory{created, mode == disk.modes.end() ? 0700U : mode->second};
  }
  auto lock(const std::string& directory) -> std::expected<void, LockError> override {
    if (begin("lock", directory))
      return std::unexpected(LockError{false, "the lock file cannot be opened: Permission denied"});
    disk.files.try_emplace(directory + "/lock", "");
    if (!disk.locked.insert(directory).second)
      return std::unexpected(LockError{true, "the lock cannot be taken: Resource temporarily "
                                             "unavailable"});
    held_ = directory;
    return {};
  }
  void unlock() override {
    calls.emplace_back("unlock");
    release();
  }
  auto read(const std::string& path, std::size_t limit)
      -> std::expected<std::optional<Content>, std::string> override {
    if (begin("read", path))
      return std::unexpected(std::string("it cannot be read: Input/output error"));
    const auto file = disk.files.find(path);
    if (file == disk.files.end())
      return std::optional<Content>{};
    return Content{file->second.substr(0, limit), file->second.size() > limit};
  }
  auto open_temporary(const std::string& path) -> Done override {
    if (begin("open_temporary", path))
      return std::unexpected(std::string("the temporary file cannot be made: Permission denied"));
    disk.files[path].clear();
    temporary_ = path;
    return {};
  }
  auto write(std::string_view bytes) -> Done override {
    if (begin("write", temporary_)) {
      // Half of it reached the disk.
      disk.files[temporary_] += bytes.substr(0, bytes.size() / 2);
      return std::unexpected(
          std::string("the temporary file cannot be written: No space left on device"));
    }
    disk.files[temporary_] += bytes;
    return {};
  }
  auto sync() -> Done override {
    if (begin("sync", temporary_))
      return std::unexpected(
          std::string("the temporary file cannot be synchronised: Input/output error"));
    return {};
  }
  auto rename(const std::string& from, const std::string& to) -> Done override {
    if (begin("rename", from + " " + to))
      return std::unexpected(std::string("the file cannot be replaced: Read-only file system"));
    auto node = disk.files.extract(from);
    disk.files.insert_or_assign(to, std::move(node.mapped()));
    return {};
  }
  auto link(const std::string& from, const std::string& to) -> Done override {
    if (begin("link", from + " " + to))
      return std::unexpected(std::string("the file cannot be made: Read-only file system"));
    if (disk.files.contains(to) || disk.directories.contains(to))
      return std::unexpected(std::string("the file cannot be made: File exists"));
    disk.files[to] = disk.files[from];
    return {};
  }
  auto sync_directory(const std::string& directory) -> Done override {
    if (begin("sync_directory", directory))
      return std::unexpected(
          std::string("the directory cannot be synchronised: Input/output error"));
    return {};
  }
  void discard(const std::string& path) override {
    calls.push_back("discard " + path);
    disk.files.erase(path);
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
