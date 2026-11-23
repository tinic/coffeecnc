#ifndef COFFEECNC_APP_MACHINE_DATA_HPP
#define COFFEECNC_APP_MACHINE_DATA_HPP

#include <coffeecnc/app/response.hpp>
#include <coffeecnc/app/tool_model_store.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace coffeecnc {

// Backup and restore of one machine's data as a gzip-compressed tar archive, written and read
// by libarchive (backup_archive.hpp). MachineData is in the coffeecnc_backup library; the
// interface, the messages and the names are in the app layer.
//
// The archive holds `coffeecnc-backup.json` (the manifest) and, by role:
//
//   role        archive name            restored to (this machine's configuration now)
//   ini         config/<name>           the INI file LinuxCNC runs with
//   hal         config/<relative path>  that path below the INI file's folder
//   tool_table  tool-table/<name>       [EMCIO] TOOL_TABLE
//   parameters  parameters/<name>       [RS274NGC] PARAMETER_FILE (.var)
//   data        data/<name>             the machine's data folder; only data_file_names
//
// A HAL file is backed up only when it is a file below the INI file's folder; the others
// ([HAL] HALFILE = LIB:..., LinuxCNC's own) are listed in the manifest as not included. A
// configuration file that is a symbolic link is backed up by its content, and the manifest
// says it was a link.
inline constexpr std::string_view backup_manifest_name = "coffeecnc-backup.json";
inline constexpr std::array<std::string_view, 7> data_file_names{
    "tool-models.json", "operator-settings.json", "panel-language.json", "offset-history.json",
    "alarm-log.jsonl",  "alarm-log.1.jsonl",      "alarm-log.2.jsonl"};
// Below the data folder: a restore staged for when LinuxCNC has stopped, and what came of it.
inline constexpr std::string_view restore_directory_name = "restore";
inline constexpr std::string_view restore_plan_name = "plan.json";
inline constexpr std::string_view restore_applying_name = "applying.json";
inline constexpr std::string_view restore_result_name = "result.json";
// The alarm log is in a backup but is never restored: it is a record of what happened on this
// machine since, not a setting, and an old one would replace the newer entries.
[[nodiscard]] auto restored_data_file(std::string_view name) -> bool;
// A plan staged longer than this before the run that staged it ended is not written: its quit
// was not delivered (the page was replaced, the close dropped), and what it was checked against
// is old.
inline constexpr auto restore_plan_limit = std::chrono::minutes{2};

struct MachineDataPaths {
  std::string machine_id;             // empty: none configured
  std::string ini_file;               // absolute
  std::string config_directory;       // of the INI file
  std::vector<std::string> hal_files; // as the INI names them: HALFILE, POSTGUI_HALFILE, SHUTDOWN
  std::string tool_table;             // absolute; empty when not configured
  std::string parameter_file;         // absolute; empty when not configured
  std::string data_directory;         // absolute; empty when the machine has none
};

// The `backup_written` message: where the archive was written, how many files it holds, its
// size, and the HAL files it does not hold, each with the reason.
struct BackupWritten {
  std::string path;
  std::size_t files{};
  std::size_t bytes{};
  std::vector<std::pair<std::string, std::string>> not_included;
};

// The `restore_summary` message: what a restore of the archive at `path` would write.
struct RestoreSummaryFile {
  std::string role;   // ini, hal, tool_table, parameters, data
  std::string name;   // in the archive
  std::string target; // the file it replaces
  // new, changed, same; at_exit for the parameters: LinuxCNC writes its .var file as it stops,
  // and the helper compares it then.
  std::string change;
  std::size_t bytes{};
  // The target is a symbolic link: the restore replaces the link by the file, and does not
  // write where it points.
  bool symlink{};
};
struct RestoreSummary {
  std::string path;
  std::string token; // the `restore` command hands it back: the archive is still the one checked
  std::optional<std::string> machine_id;
  std::int64_t created{}; // milliseconds since the Unix epoch; 0 when the manifest has none
  std::vector<RestoreSummaryFile> files;
};

[[nodiscard]] auto encode_backup_written(const BackupWritten& written) -> std::string;
[[nodiscard]] auto encode_restore_summary(const RestoreSummary& summary) -> std::string;

// What a backup or restore request came to: the messages before its result, the result, and
// whether the application is to quit after it.
struct MachineDataAnswer {
  CommandOutcome outcome{CommandOutcome::accepted};
  std::string reason;
  std::vector<std::string> messages;
  bool quit{};
};

// Where the session sends `backup`, `check_restore` and `restore`, on its worker thread. The
// machine state was checked by the policy before; `restore` asks `recheck` once more just
// before it stages anything (a reason: refused). `page` is the panel page that asked, for the
// plan. A restore that asked to quit and whose quit the session did not deliver (the page ended,
// an abort or an E-stop came first), or whose page is replaced after it (the window then drops
// the close), is abandoned: its plan is removed, and no later exit writes it. abandon_restore is
// called from any thread.
class MachineDataService {
public:
  MachineDataService() = default;
  virtual ~MachineDataService() = default;
  [[nodiscard]] virtual auto backup(const std::string& directory) -> MachineDataAnswer = 0;
  [[nodiscard]] virtual auto check_restore(const std::string& path) -> MachineDataAnswer = 0;
  [[nodiscard]] virtual auto restore(const std::string& path, const std::string& token,
                                     std::uint64_t page,
                                     const std::function<std::string()>& recheck)
      -> MachineDataAnswer = 0;
  virtual void abandon_restore() = 0;

protected:
  MachineDataService(const MachineDataService&) = default;
  MachineDataService(MachineDataService&&) = default;
  auto operator=(const MachineDataService&) -> MachineDataService& = default;
  auto operator=(MachineDataService&&) -> MachineDataService& = default;
};

// Backup: every file of the table that exists is read (at most max_archive_entry_bytes each,
// through a symbolic link) and written as
// `<directory>/coffeecnc-<machine>-<YYYYmmdd-HHMMSS>.tar.gz` (UTC) by a durable replacement
// (temporary file, fsync, rename, fsync of the directory). Answered with `backup_written`.
//
// Check: the archive is read whole (at most max_archive_bytes) and refused unless every entry is a
// regular file named as the table says, listed once in a version 1 manifest of this machine
// (a manifest without a machine is refused when this machine has one), with a name made of
// plain components (no `..`, `.`, empty, absolute, or control characters), whose target is
// configured here and taken by no other entry. A HAL file must be one that this machine's INI
// file or the archive's names ([HAL] HALFILE, POSTGUI_HALFILE, SHUTDOWN), end in .hal or
// .tcl, and be reached beneath the configuration folder with no symbolic link on the way. A
// target that does not exist must be in a folder that does (a restore makes no folder but the
// data folder). The alarm log is not restored (restored_data_file).
// Nothing is written. Answered with `restore_summary`: for each file its target, whether it is
// new, changed or the same (`at_exit` for the .var and the data files, which LinuxCNC and
// CoffeeCNC write as they stop: the helper decides them), whether the target is a symbolic link,
// and the token of this archive at this path.
//
// Restore: the check again, the token must be the same, then `recheck`. The files that change,
// and the .var file and the data files always, are copied to `<data>/restore/`, and a plan is
// written last that names this machine, the time it was staged, the page that asked, and, for
// each target, a hash of what it held. The answer quits the
// application. LinuxCNC writes its .var file when it stops, and it reads the INI, HAL, tool
// table and .var files only when it starts: apply_staged_restore writes them once it has
// stopped.
//
// Start: under the data folder's lock (waited for a little while a helper holds it), what came
// of a restore before this run is read and removed: a result, an interrupted write (the
// applying marker), or a plan the helper never wrote. take_notice() says it once.
class MachineData final : public MachineDataService {
public:
  using Clock = std::function<std::chrono::milliseconds()>;
  using Pause = std::function<void(std::chrono::milliseconds)>;
  // How long the start waits for a helper that holds the data folder's lock.
  static constexpr auto lock_wait = std::chrono::seconds{5};
  static constexpr auto lock_poll = std::chrono::milliseconds{100};
  // The files must outlive this. Reads what a restore before this run came to (notice()).
  // `pause` waits for the lock; a sleep when none is given.
  MachineData(ToolModelFiles& files, MachineDataPaths paths, Clock clock, Pause pause = {});

  [[nodiscard]] auto backup(const std::string& directory) -> MachineDataAnswer override;
  [[nodiscard]] auto check_restore(const std::string& path) -> MachineDataAnswer override;
  [[nodiscard]] auto restore(const std::string& path, const std::string& token, std::uint64_t page,
                             const std::function<std::string()>& recheck)
      -> MachineDataAnswer override;
  void abandon_restore() override;

  // A restore is staged: start the helper when the application ends. Any thread.
  [[nodiscard]] auto staged() const -> bool { return staged_.load(); }
  // What came of a restore before this run, for the operator, once; empty when nothing. Any
  // thread.
  [[nodiscard]] auto take_notice() -> std::optional<std::pair<MessageLevel, std::string>>;

  struct Planned {
    std::string role;
    std::string name;     // in the archive
    std::string root;     // the folder the target is resolved beneath
    std::string relative; // the target beneath it
    std::string bytes;
    std::string change; // new, changed, same, at_exit
    bool symlink{};
    std::optional<std::string> held; // what the target holds now; none: it does not exist
    [[nodiscard]] auto target() const -> std::string { return root + '/' + relative; }
  };

private:
  struct Checked {
    std::vector<Planned> files;
    std::optional<std::string> machine_id;
    std::int64_t created{};
    std::string token;
  };
  [[nodiscard]] auto check(const std::string& path) -> std::expected<Checked, std::string>;
  void report_last_restore(const Pause& pause);

  ToolModelFiles& files_;
  MachineDataPaths paths_;
  Clock clock_;
  void abandon_locked();

  std::atomic<bool> staged_{};
  std::mutex stage_mutex_;                // staging and abandoning
  std::vector<std::string> staged_files_; // the plan's copies, under stage_mutex_
  std::mutex mutex_;
  std::optional<std::pair<MessageLevel, std::string>> notice_;
};

// What the helper asks while it writes; each may be empty.
struct RestoreHooks {
  // When the run that staged the plan ended (the helper's start); none: `now`. A plan staged
  // more than restore_plan_limit before or after it is refused.
  std::optional<std::chrono::milliseconds> ended;
  // Before the applying marker: a reason, and nothing is written.
  std::function<std::optional<std::string>()> may_write;
  // Before each replacement: a reason, and what was written is put back.
  std::function<std::optional<std::string>()> still_may_write;
};

// The helper's work, after LinuxCNC has stopped: reads `<data>/restore/plan.json` and refuses
// it unless it is of this machine, was staged at most restore_plan_limit before the run ended,
// and every target (but the .var file and the data files, which LinuxCNC and CoffeeCNC write as
// they stop) holds what it held when the restore was prepared. A target that now holds the
// backup's file is not written. Then a backup of every target that is written, as it is now,
// to `<data>/pre-restore-<YYYYmmdd-HHMMSS>.tar.gz` (of this machine), then `hooks.may_write` (a
// reason: nothing is written), then the applying marker that names that backup, then each
// target by a durable replacement that keeps its mode and group. When one fails, every target
// already renamed (also one whose directory fsync failed) is put back from that copy (a target
// that was new is removed, one that was a link is made the same link again). The outcome goes
// to `<data>/restore/result.json`, which the next run shows once, and says which files could
// not be put back and which did not keep their group; then the marker, the plan and
// the staged copies are removed. No plan: nothing, and 0.
[[nodiscard]] auto apply_staged_restore(ToolModelFiles& files, const std::string& data_directory,
                                        std::chrono::milliseconds now,
                                        const RestoreHooks& hooks = {})
    -> std::expected<std::size_t, std::string>;

} // namespace coffeecnc

#endif // COFFEECNC_APP_MACHINE_DATA_HPP
