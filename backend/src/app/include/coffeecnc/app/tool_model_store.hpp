#ifndef COFFEECNC_APP_TOOL_MODEL_STORE_HPP
#define COFFEECNC_APP_TOOL_MODEL_STORE_HPP

#include <coffeecnc/app/response.hpp>
#include <coffeecnc/app/session.hpp>
#include <coffeecnc/app/tool_models.hpp>
#include <coffeecnc/core/tool_models.hpp>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace coffeecnc {

// Storage of the tool models on the machine's disk; docs/design/native-tool-model-storage.md.

// What the configuration and the environment say, as written.
struct ToolModelSettings {
  std::optional<std::string> machine_id;     // [COFFEECNC] MACHINE_ID
  std::optional<std::string> data_directory; // [COFFEECNC] DATA_DIRECTORY
  std::optional<std::string> xdg_data_home;  // $XDG_DATA_HOME
  std::optional<std::string> home;           // $HOME
};

// Where one machine's tool models are kept, or why they are not kept.
struct ToolModelLocation {
  // The valid configured ID, also when the directory is not usable.
  std::optional<std::string> machine_id;
  // `available`: `directory` may be opened. Whether it can be is found out by opening it.
  ToolModelAvailability availability{ToolModelAvailability::session_only};
  std::string reason;
  std::string directory; // <data root>/<machine_id>, absolute; empty unless available
  friend auto operator==(const ToolModelLocation&, const ToolModelLocation&) -> bool = default;
};

// No machine ID: session_only. An ID that is not valid (valid_machine_id), a DATA_DIRECTORY
// that is not absolute, or no absolute path to fall back on: unavailable, with the reason, and
// no directory. The data root is DATA_DIRECTORY, else $XDG_DATA_HOME/coffeecnc when that is
// absolute, else $HOME/.local/share/coffeecnc when that is absolute; the machine ID is always
// appended. Nothing is resolved against the working directory. No I/O.
[[nodiscard]] auto resolve_tool_model_location(const ToolModelSettings& settings)
    -> ToolModelLocation;

inline constexpr std::string_view tool_model_file_name = "tool-models.json";
inline constexpr std::string_view tool_model_temporary_name = "tool-models.json.tmp";
inline constexpr std::string_view tool_model_lock_name = "lock";

// The file system as the tool-model writer uses it: one step of a durable replacement per
// call, so that each can fail by itself. One caller, the writer thread. Every failure carries
// a reason in words for the operator.
class ToolModelFiles {
public:
  using Done = std::expected<void, std::string>;
  struct LockError {
    bool in_use{}; // another process holds the lock
    std::string reason;
  };
  struct Content {
    std::string bytes; // at most the limit asked for
    bool more{};       // the file is longer
  };
  struct Directory {
    bool created{};   // by this call
    unsigned mode{};  // its permission bits now
  };

  ToolModelFiles() = default;
  virtual ~ToolModelFiles() = default;

  // Makes this one directory with mode 0700 exactly, whatever the umask, when it is missing;
  // one that exists is left as it is. Fails when the path is not a directory.
  [[nodiscard]] virtual auto make_directory(const std::string& path)
      -> std::expected<Directory, std::string> = 0;
  // Opens `<directory>/lock` (made with mode 0600) and takes an exclusive flock on it without
  // waiting. The lock is held until unlock or destruction.
  [[nodiscard]] virtual auto lock(const std::string& directory)
      -> std::expected<void, LockError> = 0;
  virtual void unlock() = 0;
  // The first `limit` bytes of a regular file. No value: there is no such file.
  [[nodiscard]] virtual auto read(const std::string& path, std::size_t limit)
      -> std::expected<std::optional<Content>, std::string> = 0;
  // A new, empty file with mode 0600, replacing what a crashed write left there.
  [[nodiscard]] virtual auto open_temporary(const std::string& path) -> Done = 0;
  // Appends to the open temporary file.
  [[nodiscard]] virtual auto write(std::string_view bytes) -> Done = 0;
  // fsync of the temporary file, which is then closed.
  [[nodiscard]] virtual auto sync() -> Done = 0;
  [[nodiscard]] virtual auto rename(const std::string& from, const std::string& to) -> Done = 0;
  // fsync of the directory: a rename in it, or an entry made in it, is on the disk.
  [[nodiscard]] virtual auto sync_directory(const std::string& directory) -> Done = 0;
  // Closes the temporary file if it is open and removes it. Failure is not reported.
  virtual void discard(const std::string& path) = 0;

protected:
  ToolModelFiles(const ToolModelFiles&) = default;
  ToolModelFiles(ToolModelFiles&&) = default;
  auto operator=(const ToolModelFiles&) -> ToolModelFiles& = default;
  auto operator=(ToolModelFiles&&) -> ToolModelFiles& = default;
};

// The host's file system through POSIX calls.
class LocalToolModelFiles final : public ToolModelFiles {
public:
  LocalToolModelFiles() = default;
  ~LocalToolModelFiles() override;
  LocalToolModelFiles(const LocalToolModelFiles&) = delete;
  auto operator=(const LocalToolModelFiles&) -> LocalToolModelFiles& = delete;

  [[nodiscard]] auto make_directory(const std::string& path)
      -> std::expected<Directory, std::string> override;
  [[nodiscard]] auto lock(const std::string& directory) -> std::expected<void, LockError> override;
  void unlock() override;
  [[nodiscard]] auto read(const std::string& path, std::size_t limit)
      -> std::expected<std::optional<Content>, std::string> override;
  [[nodiscard]] auto open_temporary(const std::string& path) -> Done override;
  [[nodiscard]] auto write(std::string_view bytes) -> Done override;
  [[nodiscard]] auto sync() -> Done override;
  [[nodiscard]] auto rename(const std::string& from, const std::string& to) -> Done override;
  [[nodiscard]] auto sync_directory(const std::string& directory) -> Done override;
  void discard(const std::string& path) override;

private:
  int lock_{-1};
  int temporary_{-1};
};

// What a command came to: the messages for its page, in the order they are sent.
struct ToolModelReply {
  CommandOutcome outcome{CommandOutcome::accepted};
  std::string reason;
  // The `tool_models` snapshot is sent before the result: after a save, for a stale base
  // revision, and when the store has just become unavailable.
  bool snapshot{};
};

// The saved document of one machine and every step that reads or replaces it. Synchronous and
// not thread-safe: the writer thread's, or a test's.
//
// States, as `models().availability`:
//
//   session_only  the location has no machine ID. Nothing is opened, read or written.
//   unavailable   the location is not usable, the lock is held by another process, the
//                 directory or file cannot be read, the file is not a valid version 1
//                 document of this machine (too large, not JSON, unknown fields, a later
//                 version, another machine's), or the file was changed from outside while
//                 this process runs. The reason names the directory or the file. Nothing is
//                 written, renamed or removed. It ends with the process.
//   available     the lock is held. Without a file: revision 0, factory defaults, no
//                 assignments; no file is made before the first accepted command.
//
// `open` makes every directory of the location that is missing, outermost first, each 0700,
// and fsyncs the parent of each one it made before it goes on: a directory is on the disk
// only once its parent is. A failure of any of these is `unavailable`. Then the lock, then
// the file. A machine directory that existed and that other users can enter (any group or
// other permission bit) is used as it is, and the available snapshot's `reason` says so: the
// operator may have set it up that way, and the files in it are 0600 either way.
//
// `apply`, when available, in this order: the file is read again and must hold exactly what
// this process read or wrote last, revision and content (a file removed, replaced or edited
// from outside makes the store unavailable, and the command is `rejected` with that snapshot
// before it); a base revision other than the held one is `rejected` as stale; the new
// document is checked
// (1001 assignments, 1 MiB as file and as message, revision below 2^53 - 1); then temporary
// file in the same directory, write, fsync, rename, fsync of the directory. A failure up to
// and including the rename is `failed`: the saved file is as it was and the temporary file is
// removed. A failure after the rename is `failed` and says the outcome is uncertain: the file
// is read once more and what it holds is the snapshot sent. Only a save that passed the
// directory fsync is `accepted`, with the revision one higher.
class ToolModelArchive {
public:
  // The files must outlive the archive.
  ToolModelArchive(ToolModelFiles& files, ToolModelLocation location);

  // Makes the directories, takes the lock and reads the file. Once, before anything else.
  void open();
  // Releases the lock.
  void close();
  [[nodiscard]] auto apply(const ToolModelCommand& command) -> ToolModelReply;
  [[nodiscard]] auto models() const -> const ToolModels& { return models_; }
  // The `tool_models` message for `models()`.
  [[nodiscard]] auto snapshot() const -> const std::string& { return snapshot_; }

private:
  [[nodiscard]] auto read() const -> ToolModels;
  [[nodiscard]] auto prepare() -> std::expected<void, std::string>;
  void show(ToolModels models);
  [[nodiscard]] auto unavailable(std::string reason) const -> ToolModels;
  [[nodiscard]] auto path(std::string_view name) const -> std::string;

  ToolModelFiles& files_;
  ToolModelLocation location_;
  ToolModels models_;
  std::string snapshot_;
  // Said with every available snapshot: the machine directory is open to other users.
  std::string note_;
};

// The archive on its writer thread, behind the session.
//
//   thread  period   owns                         may block on
//   writer  on work  ToolModelArchive: the lock,  the file system
//                    the saved file, every read
//                    and write of it
//
// `submit`, `page_ended` and `send_snapshot` return at once, from any thread. `submit` answers
// what needs no file: storage that the configuration rules out (session_only, or unavailable
// with its reason), a command for another machine ID, a command tool_model_command_error
// refuses, and a full queue (max_pending commands waiting, the one being written not counted).
// Everything else is answered by the writer, in the order submitted: one call of `deliver`
// with the snapshot, if the reply has one, and then the CommandResult.
//
// Pages. A command waiting when its page ends is dropped: nothing is written for it and it
// gets no result. A command the writer has begun is finished, whatever becomes of its page;
// what it would have been told is then not delivered, and the next page's snapshot shows what
// was saved. `deliver` carries the page token; the shell compares it once more.
//
// `deliver` is called on the writer thread only; it must not block and must not call the
// store.
class ToolModelStore final : public ToolModelService {
public:
  using Deliver = std::function<void(PageToken, std::vector<std::string>)>;
  static constexpr std::size_t max_pending = 8;
  static constexpr std::string_view busy_reason = "Too many tool model changes are waiting.";

  // The files must outlive the store. Starts the thread, which opens the archive.
  ToolModelStore(ToolModelFiles& files, ToolModelLocation location, Deliver deliver);
  // The command being written is finished; those waiting are dropped; the lock is released.
  ~ToolModelStore() override;
  ToolModelStore(const ToolModelStore&) = delete;
  auto operator=(const ToolModelStore&) -> ToolModelStore& = delete;

  [[nodiscard]] auto submit(PageToken page, RequestId id, ToolModelCommand command)
      -> std::optional<std::string> override;
  void page_ended(PageToken page) override;
  // The snapshot for a page that has just been sent its `hello`. Several calls before the
  // writer gets to them are one snapshot, for the page of the last.
  void send_snapshot(PageToken page);

private:
  struct Pending {
    PageToken page;
    RequestId id{};
    ToolModelCommand command;
  };
  void loop();

  ToolModelLocation location_;
  Deliver deliver_;
  ToolModelArchive archive_; // the writer thread's
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Pending> pending_;
  std::optional<PageToken> snapshot_due_;
  // Every page up to this one has ended.
  std::uint64_t ended_through_{};
  bool stop_{};
  std::thread thread_;
};

} // namespace coffeecnc

#endif // COFFEECNC_APP_TOOL_MODEL_STORE_HPP
