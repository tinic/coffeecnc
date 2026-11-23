#ifndef COFFEECNC_APP_RESTORE_HELPER_HPP
#define COFFEECNC_APP_RESTORE_HELPER_HPP

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
  virtual void pause(std::chrono::milliseconds period) = 0;
  // Since the helper started, on a clock that does not jump.
  [[nodiscard]] virtual auto elapsed() -> std::chrono::milliseconds = 0;
  // Milliseconds since the Unix epoch.
  [[nodiscard]] virtual auto now() -> std::chrono::milliseconds = 0;

protected:
  RestoreHost(const RestoreHost&) = default;
  RestoreHost(RestoreHost&&) = default;
  auto operator=(const RestoreHost&) -> RestoreHost& = default;
  auto operator=(RestoreHost&&) -> RestoreHost& = default;
};

class LocalRestoreHost final : public RestoreHost {
public:
  LocalRestoreHost();
  [[nodiscard]] auto alive(long pid) -> bool override;
  [[nodiscard]] auto linuxcnc_running() -> std::optional<std::string> override;
  void pause(std::chrono::milliseconds period) override;
  [[nodiscard]] auto elapsed() -> std::chrono::milliseconds override;
  [[nodiscard]] auto now() -> std::chrono::milliseconds override;

private:
  std::chrono::steady_clock::time_point started_;
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
// (apply_staged_restore), refusing once more if LinuxCNC runs by then. Code 0: written, or
// nothing was staged. Whatever is not written stays staged and the next start says so.
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
