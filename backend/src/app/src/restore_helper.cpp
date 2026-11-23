#include <coffeecnc/app/restore_helper.hpp>

#include <coffeecnc/app/machine_data.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <filesystem>
#include <format>
#include <fstream>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace coffeecnc {
namespace {

constexpr std::string_view linuxcnc_lock = "/tmp/linuxcnc.lock";
// LinuxCNC's own processes: the task controller and the realtime host (uspace).
constexpr std::array<std::string_view, 2> linuxcnc_processes{"milltask", "rtapi_app"};

auto digits(std::string_view text) -> bool {
  return !text.empty() && std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; });
}

} // namespace

LocalRestoreHost::LocalRestoreHost() : started_(std::chrono::steady_clock::now()) {}

auto LocalRestoreHost::alive(long pid) -> bool {
  if (pid <= 0 || (::kill(static_cast<pid_t>(pid), 0) != 0 && errno != EPERM))
    return false;
  std::ifstream stat{std::format("/proc/{}/stat", pid)};
  std::string line;
  std::getline(stat, line);
  const auto close = line.rfind(')');
  return close == std::string::npos || close + 2 >= line.size() || line[close + 2] != 'Z';
}

auto LocalRestoreHost::linuxcnc_running() -> std::optional<std::string> {
  std::error_code error;
  if (std::filesystem::exists(linuxcnc_lock, error))
    return std::format("{} exists", linuxcnc_lock);
  for (std::filesystem::directory_iterator entries{"/proc", error}, end; !error && entries != end;
       entries.increment(error)) {
    const auto pid = entries->path().filename().string();
    if (!digits(pid))
      continue;
    std::ifstream comm{entries->path() / "comm"};
    std::string name;
    std::getline(comm, name);
    if (std::ranges::find(linuxcnc_processes, name) != linuxcnc_processes.end())
      return std::format("{} (pid {}) runs", name, pid);
  }
  return std::nullopt;
}

auto LocalRestoreHost::take_linuxcnc_lock() -> std::optional<std::string> {
  const std::string path{linuxcnc_lock};
  const int made = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
  if (made < 0)
    return std::format("{} cannot be made: {}", path, std::generic_category().message(errno));
  struct stat info{};
  const bool known = ::fstat(made, &info) == 0;
  static_cast<void>(::close(made));
  if (!known) {
    static_cast<void>(::unlink(path.c_str()));
    return std::format("{} cannot be read back", path);
  }
  lock_ = {static_cast<unsigned long>(info.st_dev), static_cast<unsigned long>(info.st_ino)};
  return std::nullopt;
}

auto LocalRestoreHost::holds_linuxcnc_lock() -> bool {
  struct stat info{};
  const std::string path{linuxcnc_lock};
  return lock_ && ::lstat(path.c_str(), &info) == 0 &&
         static_cast<unsigned long>(info.st_dev) == lock_->first &&
         static_cast<unsigned long>(info.st_ino) == lock_->second;
}

void LocalRestoreHost::release_linuxcnc_lock() {
  if (holds_linuxcnc_lock())
    static_cast<void>(::unlink(std::string(linuxcnc_lock).c_str()));
  lock_.reset();
}

void LocalRestoreHost::pause(std::chrono::milliseconds period) {
  std::this_thread::sleep_for(period);
}

auto LocalRestoreHost::elapsed() -> std::chrono::milliseconds {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                               started_);
}

auto LocalRestoreHost::now() -> std::chrono::milliseconds {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch());
}

auto run_restore_helper(ToolModelFiles& files, const std::string& directory, long parent,
                        RestoreHost& host) -> RestoreHelperOutcome {
  // The run that staged the restore ends now, or has ended: a plan much older was not this run's
  // to write.
  const auto started = host.now();
  if (parent <= 1)
    return {1, std::format("restore=not_written reason=the parent process is {}, not the "
                           "linuxcnc script: when LinuxCNC stops cannot be told",
                           parent)};
  for (;;) {
    const bool waiting = host.alive(parent);
    const auto running = waiting ? std::nullopt : host.linuxcnc_running();
    if (!waiting && !running)
      break;
    if (host.elapsed() > restore_helper_wait)
      return {1, std::format("restore=not_written reason=LinuxCNC did not stop ({})",
                             waiting ? std::format("pid {} runs", parent) : *running)};
    host.pause(restore_helper_poll);
  }
  // A CoffeeCNC started again at once holds the lock: wait for it a little, then leave the plan
  // for that run to report.
  for (int tries = 0;; ++tries) {
    const auto locked = files.lock(directory);
    if (locked)
      break;
    if (!locked.error().in_use || tries > 50)
      return {1, std::format("restore=not_written reason={}", locked.error().reason)};
    host.pause(restore_helper_poll);
  }
  RestoreHooks hooks;
  hooks.ended = started;
  bool locked_linuxcnc = false;
  hooks.may_write = [&]() -> std::optional<std::string> {
    if (auto running = host.linuxcnc_running())
      return std::format("LinuxCNC runs again ({}): nothing was written.", *running);
    if (auto refused = host.take_linuxcnc_lock())
      return std::format("LinuxCNC runs again ({}): nothing was written.", *refused);
    locked_linuxcnc = true;
    return std::nullopt;
  };
  hooks.still_may_write = [&host]() -> std::optional<std::string> {
    if (!host.holds_linuxcnc_lock())
      return std::format("LinuxCNC was started while the files were written ({} was removed).",
                         linuxcnc_lock);
    return std::nullopt;
  };
  const auto applied = apply_staged_restore(files, directory, host.now(), hooks);
  if (locked_linuxcnc)
    host.release_linuxcnc_lock();
  files.unlock();
  if (!applied)
    return {1, std::format("restore=failed reason={}", applied.error())};
  return {0, std::format("restore=written files={}", *applied)};
}

auto spawn_detached(const std::vector<std::string>& arguments) -> std::expected<long, std::string> {
  if (arguments.empty())
    return std::unexpected(std::string("nothing to start"));
  std::vector<std::string> copies{arguments};
  std::vector<char*> argv(copies.size() + 1, nullptr);
  // posix_spawn takes char*, which only a string that is not const gives.
  std::ranges::transform(copies, argv.begin(),
                         // cppcheck-suppress constParameterReference
                         [](std::string& argument) -> char* { return argument.data(); });
  posix_spawn_file_actions_t actions;
  if (const int failed = posix_spawn_file_actions_init(&actions); failed != 0)
    return std::unexpected(std::generic_category().message(failed));
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
  int closing = posix_spawn_file_actions_addclosefrom_np(&actions, 3);
#else
  // Every descriptor this process has open above 2, listed before the first is closed.
  int closing = 0;
  std::vector<int> open;
  std::error_code listed;
  for (std::filesystem::directory_iterator entries{"/proc/self/fd", listed}, end;
       !listed && entries != end; entries.increment(listed))
    if (const auto name = entries->path().filename().string(); digits(name))
      open.push_back(std::stoi(name));
  for (const int descriptor : open)
    if (descriptor > 2 && ::fcntl(descriptor, F_GETFD) != -1 && closing == 0)
      closing = posix_spawn_file_actions_addclose(&actions, descriptor);
#endif
  posix_spawnattr_t attributes;
  if (const int failed = posix_spawnattr_init(&attributes); failed != 0) {
    posix_spawn_file_actions_destroy(&actions);
    return std::unexpected(std::generic_category().message(failed));
  }
  sigset_t none;
  sigemptyset(&none);
  int flags = POSIX_SPAWN_SETSIGMASK;
#ifdef POSIX_SPAWN_SETSID
  flags |= POSIX_SPAWN_SETSID;
#endif
  int failed = closing;
  if (failed == 0)
    failed = posix_spawnattr_setsigmask(&attributes, &none);
  if (failed == 0)
    failed = posix_spawnattr_setflags(&attributes, static_cast<short>(flags));
  pid_t child{};
  if (failed == 0)
    failed = posix_spawn(&child, argv.front(), &actions, &attributes, argv.data(), environ);
  posix_spawnattr_destroy(&attributes);
  posix_spawn_file_actions_destroy(&actions);
  if (failed != 0)
    return std::unexpected(std::generic_category().message(failed));
  return static_cast<long>(child);
}

} // namespace coffeecnc
