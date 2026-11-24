#ifndef COFFEECNC_APP_ALARM_LOG_HPP
#define COFFEECNC_APP_ALARM_LOG_HPP

#include <coffeecnc/app/response.hpp>
#include <coffeecnc/app/session.hpp>
#include <coffeecnc/app/tool_model_store.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace coffeecnc {

// One thing the operator was told: an operator message, or a command result other than
// `accepted` (a refusal or an error).
struct AlarmRecord {
  // The run of CoffeeCNC it was told in: 1 for the first run of a log, one higher for each
  // run after it.
  std::uint64_t session{};
  // Its place in the run, from 1; 0 when it is not known. A line written twice (a retry after
  // an append that was cut short) is read once.
  std::uint64_t number{};
  std::int64_t time{}; // milliseconds since the Unix epoch
  MessageLevel level{MessageLevel::error};
  std::string text;
  // A result's outcome (`rejected`, `failed`, `timed_out`); empty for an operator message.
  std::string outcome;
  // A result's command as the panel sent it, without its id, as JSON text; `{"type":...}`
  // only when it is longer than max_command_bytes; empty when it is not known.
  std::string command;
  std::optional<MessageKey> localization{};
  friend auto operator==(const AlarmRecord&, const AlarmRecord&) -> bool = default;
};

// What a page is sent once after its `hello`: what was told before the page was loaded, of
// earlier runs and of earlier pages of this run, oldest first.
struct AlarmLogView {
  bool saved{};       // kept on the disk
  std::string reason; // why not, or why not now; empty when it is
  std::uint64_t session{};
  std::uint64_t skipped{}; // lines of the files that could not be read
  std::vector<AlarmRecord> entries;
  std::optional<MessageKey> localization{}; // of reason
};

// Wire text of the `alarm_log` message. Text that is not UTF-8 is replaced.
[[nodiscard]] auto encode_alarm_log(const AlarmLogView& view) -> std::string;

// The persistent alarm log: every operator message and every result other than `accepted`
// that is posted to a page, appended to `<directory>/alarm-log.jsonl`, one JSON object per
// line.
//
//   thread  period   owns                         may block on
//   writer  on work  the files of the log         the file system
//
// `request` (shell thread), `posted` (any thread that posts to the page) and `send_snapshot`
// (publisher thread) take the mutex briefly and return at once: no thread that posts a message
// waits for the disk. The writer appends what waits, in the order it was posted.
//
// The lock. The log writes only while this process holds the data directory's lock
// (`<directory>/lock`, the one the tool models take): `lock` says whether it does, or without
// one the writer takes it itself. Without the lock the log is kept for this run only, with the
// reason, and nothing in the directory is written or renamed.
//
// Rotation. Before an append that would take the file over max_file_bytes, the file becomes
// `alarm-log.1.jsonl` and that one `alarm-log.2.jsonl` (the older one is removed): at most
// three files of about max_file_bytes each. Only the bytes of appends that worked count: a
// file that is empty, or that only failed appends made larger, is never rotated, so a full
// disk does not push the older files out.
//
// Reading. At start the writer reads the three files, oldest first, each line by itself: a line
// that is not a version 1 record (a write torn by a crash, an edit, another program's text) is
// skipped and counted, and a record whose session and number were read before is dropped. The
// order of the log is the order of the lines, whatever the clock said. This run's session is one
// higher than the highest one read. A file that does not end with a line break gets one before the
// next record.
//
// A failed append (a full disk, a removed directory) keeps the records in memory, up to
// max_pending (older ones are then lost and counted); the next record or retry_period later
// tries again, after a line break that ends whatever part was written. The reason is in the
// view and on stderr, once per reason.
//
// Without a directory the log is kept for this run only, with the reason.
class AlarmLog {
public:
  using Deliver = std::function<void(PageToken, std::vector<std::string>)>;
  // Waits until the lock is decided: no value when this process holds it, else why not.
  using Lock = std::function<std::optional<LocalizedReason>()>;
  using Clock = std::chrono::steady_clock;
  static constexpr std::size_t max_file_bytes = std::size_t{512} * 1024;
  static constexpr std::size_t max_entries = 500; // in a view
  static constexpr std::size_t max_pending = 1000;
  static constexpr std::size_t max_text_bytes = 2000;
  static constexpr std::size_t max_command_bytes = 512;
  static constexpr std::size_t max_requests = 256;
  static constexpr auto retry_period = std::chrono::seconds{5};
  static constexpr std::string_view file_name = "alarm-log.jsonl";
  static constexpr std::string_view older_name = "alarm-log.1.jsonl";
  static constexpr std::string_view oldest_name = "alarm-log.2.jsonl";

  // An empty directory: this run only, `unsaved_reason` says why. The files, `deliver` and
  // `lock` must outlive the log. Starts the writer, which finds the lock (with `lock`, else
  // through `files`) and then reads the files.
  AlarmLog(ToolModelFiles& files, std::string directory, LocalizedReason unsaved_reason,
           Deliver deliver, Lock lock = {});
  // Appends what waits, once, and joins the writer.
  ~AlarmLog();
  AlarmLog(const AlarmLog&) = delete;
  auto operator=(const AlarmLog&) -> AlarmLog& = delete;

  // A request of the page: its command, for the result it gets.
  void request(PageToken page, std::string_view json);
  // Messages posted to the page: operator messages and results other than `accepted` are
  // recorded, stamped with `unix_time` when they carry no time of their own.
  void posted(PageToken page, std::span<const std::string> messages,
              std::chrono::milliseconds unix_time);
  // The page has been sent its `hello`: it gets the view of what was recorded before now, from
  // the writer thread.
  void send_snapshot(PageToken page);

  // Tests: waits until the writer has nothing to do that it can do now.
  void settle();
  [[nodiscard]] auto view() const -> AlarmLogView;

private:
  struct Recorded {
    std::uint64_t order{};
    AlarmRecord record;
  };
  void loop();
  // Takes or checks the lock; false: this run only.
  auto locked() -> bool;
  void load();
  // Writes `batch` with the files as they are; false when it failed.
  auto append(const std::vector<AlarmRecord>& batch) -> bool;
  void add(AlarmRecord record);
  [[nodiscard]] auto view_before(std::uint64_t order) const -> AlarmLogView;
  [[nodiscard]] auto path(std::string_view name) const -> std::string;

  ToolModelFiles& files_;
  std::string directory_; // empty once the lock is not held
  Deliver deliver_;
  Lock lock_;
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable idle_;
  bool loaded_{};
  bool busy_{};
  bool stop_{};
  std::uint64_t session_{};
  std::uint64_t skipped_{};
  std::uint64_t lost_{};
  std::uint64_t next_order_{1};
  std::deque<Recorded> kept_;       // what a view shows, oldest first
  std::deque<AlarmRecord> pending_; // not on the disk yet
  std::map<std::pair<std::uint64_t, std::uint64_t>, std::string> commands_; // by page, id
  std::optional<std::pair<PageToken, std::uint64_t>> snapshot_due_;         // page, order
  std::optional<Clock::time_point> retry_at_;
  LocalizedReason reason_;
  // Writer only.
  bool own_lock_{}; // taken through files_
  // Bytes of the current file that appends which worked wrote (or that were read at start);
  // what failed appends left in it is not counted.
  std::size_t file_bytes_{};
  bool needs_line_break_{};
  std::string said_;
  std::thread thread_;
};

} // namespace coffeecnc

#endif // COFFEECNC_APP_ALARM_LOG_HPP
