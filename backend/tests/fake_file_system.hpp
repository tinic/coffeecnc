#ifndef COFFEECNC_TESTS_FAKE_FILE_SYSTEM_HPP
#define COFFEECNC_TESTS_FAKE_FILE_SYSTEM_HPP

#include <coffeecnc/app/file_system.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace coffeecnc {

// A file system in memory. Names are bytes: anything but `/` and NUL, UTF-8 or not.
class FakeFileSystem final : public FileSystem {
public:
  struct Node {
    FileKind kind{FileKind::other};
    std::string content;
    std::int64_t modified{};
    std::string link; // not empty: a symbolic link to this absolute path
  };
  // By absolute path. `/` always exists.
  std::map<std::string, Node> nodes;
  // Directories whose names cannot be read.
  std::vector<std::string> unreadable;
  // Files whose contents cannot be read even when their metadata is available.
  std::vector<std::string> unreadable_files;
  // Every call, as `<operation> <path>`.
  mutable std::vector<std::string> calls;
  // Called once inside the next canonical(): what happens while the file system is slow.
  mutable std::function<void()> during_canonical;

  // Each of these makes the directories above the path.
  void directory(const std::string& path, std::int64_t modified = 0) {
    parents(path);
    nodes[path] = {FileKind::directory, "", modified, ""};
  }
  void file(const std::string& path, std::string content, std::int64_t modified = 0) {
    parents(path);
    nodes[path] = {FileKind::file, std::move(content), modified, ""};
  }
  void link(const std::string& path, const std::string& target) {
    parents(path);
    nodes[path] = {FileKind::other, "", 0, target};
  }
  // A socket, device or pipe.
  void other(const std::string& path) {
    parents(path);
    nodes[path] = {};
  }

  auto canonical(const std::string& path) const
      -> std::expected<std::string, std::string> override {
    calls.push_back("canonical " + path);
    if (during_canonical)
      std::exchange(during_canonical, {})();
    std::string pending = path;
    for (int links = 0; links < 40; ++links) {
      std::string resolved;
      bool restarted = false;
      for (std::size_t start = 0; start < pending.size() && !restarted;) {
        const auto end = std::min(pending.find('/', start), pending.size());
        const auto part = pending.substr(start, end - start);
        start = end + 1;
        if (part.empty() || part == ".")
          continue;
        if (part == "..") {
          resolved.erase(resolved.rfind('/') == std::string::npos ? 0 : resolved.rfind('/'));
          continue;
        }
        resolved += '/' + part;
        const auto node = nodes.find(resolved);
        if (node == nodes.end())
          return std::unexpected(std::string("No such file or directory"));
        if (!node->second.link.empty()) {
          pending = node->second.link + '/' + (start < pending.size() ? pending.substr(start) : "");
          restarted = true;
        } else if (node->second.kind != FileKind::directory && start < pending.size()) {
          return std::unexpected(std::string("Not a directory"));
        }
      }
      if (!restarted)
        return resolved.empty() ? "/" : resolved;
    }
    return std::unexpected(std::string("Too many levels of symbolic links"));
  }

  auto info(const std::string& path) const -> std::expected<FileInfo, std::string> override {
    calls.push_back("info " + path);
    if (path == "/")
      return FileInfo{FileKind::directory, 0, 0};
    const auto node = nodes.find(path);
    if (node == nodes.end())
      return std::unexpected(std::string("No such file or directory"));
    return FileInfo{node->second.kind, node->second.content.size(), node->second.modified};
  }

  auto names(const std::string& folder, std::size_t limit) const
      -> std::expected<Names, std::string> override {
    calls.push_back("names " + folder);
    if (std::ranges::find(unreadable, folder) != unreadable.end())
      return std::unexpected(std::string("Permission denied"));
    const auto prefix = folder == "/" ? folder : folder + '/';
    Names found;
    for (const auto& [path, node] : nodes) {
      const std::string_view name{path};
      if (!name.starts_with(prefix) || name.substr(prefix.size()).contains('/'))
        continue;
      if (found.names.size() == limit) {
        found.more = true;
        break;
      }
      found.names.emplace_back(name.substr(prefix.size()));
    }
    return found;
  }

  auto read(const std::string& path, std::size_t limit) const
      -> std::expected<std::string, LocalizedReason> override {
    calls.push_back("read " + path);
    if (std::ranges::find(unreadable_files, path) != unreadable_files.end())
      return std::unexpected(LocalizedReason{"Permission denied"});
    const auto node = nodes.find(path);
    if (node == nodes.end() || node->second.kind != FileKind::file)
      return std::unexpected(LocalizedReason{"The file cannot be opened.",
                                             MessageKey{"backend.fileSystem.cannotOpen"}});
    return node->second.content.substr(0, limit);
  }

private:
  void parents(const std::string& path) {
    for (auto slash = path.find('/', 1); slash != std::string::npos;
         slash = path.find('/', slash + 1))
      nodes.try_emplace(path.substr(0, slash), Node{FileKind::directory, "", 0, ""});
  }
};

} // namespace coffeecnc

#endif // COFFEECNC_TESTS_FAKE_FILE_SYSTEM_HPP
