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
// ([HAL] HALFILE = LIB:..., LinuxCNC's own) are listed in the manifest as not included.
inline constexpr std::string_view backup_manifest_name = "coffeecnc-backup.json";
inline constexpr std::array<std::string_view, 7> data_file_names{
    "tool-models.json",  "operator-settings.json", "panel-language.json", "offset-history.json",
    "alarm-log.jsonl",   "alarm-log.1.jsonl",      "alarm-log.2.jsonl"};
// Below the data folder: a restore staged for when LinuxCNC has stopped, and what came of it.
inline constexpr std::string_view restore_directory_name = "restore";

struct MachineDataPaths {
  std::string machine_id;            // empty: none configured
  std::string ini_file;              // absolute
  std::string config_directory;      // of the INI file
  std::vector<std::string> hal_files; // as the INI names them: HALFILE, POSTGUI_HALFILE, SHUTDOWN
  std::string tool_table;            // absolute; empty when not configured
  std::string parameter_file;        // absolute; empty when not configured
  std::string data_directory;        // absolute; empty when the machine has none
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
  std::string change; // new, changed, same
  std::size_t bytes{};
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
// before it stages anything (a reason: refused).
class MachineDataService {
public:
  MachineDataService() = default;
  virtual ~MachineDataService() = default;
  [[nodiscard]] virtual auto backup(const std::string& directory) -> MachineDataAnswer = 0;
  [[nodiscard]] virtual auto check_restore(const std::string& path) -> MachineDataAnswer = 0;
  [[nodiscard]] virtual auto restore(const std::string& path, const std::string& token,
                                     const std::function<std::string()>& recheck)
      -> MachineDataAnswer = 0;

protected:
  MachineDataService(const MachineDataService&) = default;
  MachineDataService(MachineDataService&&) = default;
  auto operator=(const MachineDataService&) -> MachineDataService& = default;
  auto operator=(MachineDataService&&) -> MachineDataService& = default;
};

// Backup: every file of the table that exists is read (at most max_archive_entry_bytes each) and
// written as `<directory>/coffeecnc-<machine>-<YYYYmmdd-HHMMSS>.tar.gz` (UTC) by a durable
// replacement (temporary file, fsync, rename, fsync of the directory). Answered with `backup_written`.
//
// Check: the archive is read whole (at most max_archive_bytes) and refused unless every entry is a
// regular file named as the table says, listed once in a version 1 manifest of this machine
// (or of none), with a name made of plain components (no `..`, `.`, empty, absolute, or
// control characters), whose target is configured here and taken by no other entry. Nothing
// is written. Answered with `restore_summary`: for each file its target and whether it is
// new, changed or the same, and the token of this archive at this path.
//
// Restore: the check again, the token must be the same, then `recheck`. The files that change
// are copied to `<data>/restore/` and a plan is written last; the answer quits the application.
// LinuxCNC writes its .var file when it stops, and it reads the INI, HAL, tool table and .var
// files only when it starts: apply_staged_restore writes them once it has stopped.
class MachineData final : public MachineDataService {
public:
  using Clock = std::function<std::chrono::milliseconds()>;
  // The files must outlive this. Reads what a restore before this run came to (notice()).
  MachineData(ToolModelFiles& files, MachineDataPaths paths, Clock clock);

  [[nodiscard]] auto backup(const std::string& directory) -> MachineDataAnswer override;
  [[nodiscard]] auto check_restore(const std::string& path) -> MachineDataAnswer override;
  [[nodiscard]] auto restore(const std::string& path, const std::string& token,
                             const std::function<std::string()>& recheck)
      -> MachineDataAnswer override;

  // A restore is staged: start the helper when the application ends. Any thread.
  [[nodiscard]] auto staged() const -> bool { return staged_.load(); }
  // What came of a restore before this run, for the operator, once; empty when nothing. Any
  // thread.
  [[nodiscard]] auto take_notice() -> std::optional<std::pair<MessageLevel, std::string>>;

  struct Planned {
    std::string role;
    std::string name;   // in the archive
    std::string target; // absolute
    std::string bytes;
    std::string change; // new, changed, same
  };

private:
  struct Checked {
    std::vector<Planned> files;
    std::optional<std::string> machine_id;
    std::int64_t created{};
    std::string token;
  };
  [[nodiscard]] auto check(const std::string& path) -> std::expected<Checked, std::string>;

  ToolModelFiles& files_;
  MachineDataPaths paths_;
  Clock clock_;
  std::atomic<bool> staged_{};
  std::mutex mutex_;
  std::optional<std::pair<MessageLevel, std::string>> notice_;
};

// The helper's work, after LinuxCNC has stopped: reads `<data>/restore/plan.json`, writes a
// backup of every target as it is now to `<data>/pre-restore-<YYYYmmdd-HHMMSS>.tar.gz`, then
// replaces each target by a durable replacement. When one fails, those already replaced are
// written back from that copy (a target that was new is removed), and nothing is left changed
// that can be put back. The outcome goes to `<data>/restore/result.json`, which the next run
// shows once; the plan and the staged copies are removed. No plan: nothing, and 0.
[[nodiscard]] auto apply_staged_restore(ToolModelFiles& files, const std::string& data_directory,
                                        std::chrono::milliseconds now)
    -> std::expected<std::size_t, std::string>;

} // namespace coffeecnc

#endif // COFFEECNC_APP_MACHINE_DATA_HPP
