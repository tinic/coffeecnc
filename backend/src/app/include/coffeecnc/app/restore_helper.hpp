#ifndef COFFEECNC_APP_RESTORE_HELPER_HPP
#define COFFEECNC_APP_RESTORE_HELPER_HPP

#include <coffeecnc/app/machine_data.hpp>
#include <coffeecnc/app/tool_model_store.hpp>

#include <chrono>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace coffeecnc {

// `coffeecnc --apply-restore <data directory> --after <pid>`: the helper that a run which staged
// a restore starts as it ends (machine_data.hpp). In the coffeecnc_backup library.

// What the helper asks of the system; LocalRestoreHost asks it, a test answers itself.
class RestoreHost {
public:
  RestoreHost() = default;
  virtual ~RestoreHost() = default;
  // The process exists and has not ended (a zombie has ended).
  [[nodiscard]] virtual auto alive(long pid) -> bool = 0;
  // Why LinuxCNC runs on this host: its lock file, or a milltask or rtapi_app process. None when
  // it does not.
  [[nodiscard]] virtual auto linuxcnc_running() -> std::optional<std::string> = 0;
  // Makes LinuxCNC's lock file (O_EXCL) and writes a token of this helper into it (its process ID
  // and random bytes), so that a `linuxcnc` started while the helper writes finds it: a reason
  // when it cannot (it exists: LinuxCNC runs).
  [[nodiscard]] virtual auto take_linuxcnc_lock() -> std::optional<std::string> = 0;
  // The lock file is still the one take_linuxcnc_lock made: the same device, inode and token. A
  // `linuxcnc` that cleaned up "the other session" removed it; one that made its own lock again
  // may have got the same inode, but not the token.
  [[nodiscard]] virtual auto holds_linuxcnc_lock() -> bool = 0;
  // Removes the lock file, only when it is still the one this helper made.
  virtual void release_linuxcnc_lock() = 0;
  virtual void pause(std::chrono::milliseconds period) = 0;
  // Since the helper started, on a clock that does not jump.
  [[nodiscard]] virtual auto elapsed() -> std::chrono::milliseconds = 0;
  // Milliseconds since the Unix epoch.
  [[nodiscard]] virtual auto now() -> std::chrono::milliseconds = 0;
  // This boot and the time since it (boot_time()): how old a plan is, on a clock that NTP does
  // not step.
  [[nodiscard]] virtual auto boot_time() -> BootTime = 0;

protected:
  RestoreHost(const RestoreHost&) = default;
  RestoreHost(RestoreHost&&) = default;
  auto operator=(const RestoreHost&) -> RestoreHost& = default;
  auto operator=(RestoreHost&&) -> RestoreHost& = default;
};

class LocalRestoreHost final : public RestoreHost {
public:
  // `lock_path`: LinuxCNC's lock file; a test gives one of its own.
  explicit LocalRestoreHost(std::string lock_path = "/tmp/linuxcnc.lock");
  [[nodiscard]] auto alive(long pid) -> bool override;
  [[nodiscard]] auto linuxcnc_running() -> std::optional<std::string> override;
  [[nodiscard]] auto take_linuxcnc_lock() -> std::optional<std::string> override;
  [[nodiscard]] auto holds_linuxcnc_lock() -> bool override;
  void release_linuxcnc_lock() override;
  void pause(std::chrono::milliseconds period) override;
  [[nodiscard]] auto elapsed() -> std::chrono::milliseconds override;
  [[nodiscard]] auto now() -> std::chrono::milliseconds override;
  [[nodiscard]] auto boot_time() -> BootTime override;

private:
  struct Lock {
    unsigned long device{};
    unsigned long inode{};
    std::string token; // what the file holds
  };
  std::chrono::steady_clock::time_point started_;
  std::string lock_path_;
  // The lock file this helper made; none while it holds none.
  std::optional<Lock> lock_;
};

// What the helper came to: its exit code and its one line for stderr, `restore=<what>` and a
// reason, key=value.
struct RestoreHelperOutcome {
  int code{};
  std::string line;
};

inline constexpr auto restore_helper_wait = std::chrono::minutes{5};
inline constexpr auto restore_helper_poll = std::chrono::milliseconds{200};

// Refuses a parent of PID 1 or less: the run that staged the restore was not started by the
// `linuxcnc` script (or that script is gone), and its end tells nothing about LinuxCNC. Then
// waits, at most restore_helper_wait, until process `parent` has ended and no LinuxCNC runs.
// Then takes the data directory's lock (a CoffeeCNC started again at once holds it: waited for a
// little, then the plan is left for that run to report) and writes the staged files
// (apply_staged_restore with the helper's start as the end of the run that staged them),
// refusing once more if LinuxCNC runs by then. From that check to the last write it holds
// /tmp/linuxcnc.lock: the `linuxcnc` script of LinuxCNC 2.9 then asks "LinuxCNC is still
// running. Restart it?" (and, with no terminal and no display, says yes itself), and on yes
// waits 2 s for the lock to go before it removes it and starts. A lock that is gone before a
// write stops the restore and puts back what was written. Code 0: written, or nothing was
// staged. Whatever is not written stays staged and the next start says so.
[[nodiscard]] auto run_restore_helper(ToolModelFiles& files, const std::string& directory,
                                      long parent, RestoreHost& host) -> RestoreHelperOutcome;

// Starts `arguments` (the program first, a path) in a session of its own, so that it outlives
// this process and the script that started it, with the default signal mask and no descriptor
// open but 0, 1 and 2: a socket of this process (the LAN screen's port) is not held by the
// child. The child's process ID.
[[nodiscard]] auto spawn_detached(const std::vector<std::string>& arguments)
    -> std::expected<long, std::string>;

} // namespace coffeecnc

#endif // COFFEECNC_APP_RESTORE_HELPER_HPP
