#ifndef COFFEECNC_APP_OFFSET_HISTORY_HPP
#define COFFEECNC_APP_OFFSET_HISTORY_HPP

#include <coffeecnc/app/tool_model_store.hpp>
#include <coffeecnc/core/panel_messages.hpp>
#include <coffeecnc/core/panel_source.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace coffeecnc {

// The history of changes to the work offsets (the active G54 ... G59.3 system with its XY
// rotation, G92/G52) and to the length and diameter of every tool, as the readings of the
// publisher thread show them: a change is seen whoever made it. Bounded to max_entries, newest
// first; kept in `<directory>/offset-history.json` when a directory is given.
//
//   thread     calls                         what
//   worker     begin, end, written, entry    a panel command that can change offsets
//   publisher  observe                       every reading; the only writer of the file
//
// Attribution. The worker marks a panel command from just before it is sent (begin) until
// `grace` after it returned (end); while the controller still executes MDI the grace starts
// again with every reading. A change seen while a mark is open or in its grace is that
// command's; else one seen while a program runs, or within `grace` after, is the program's;
// else it is `outside`. A change made by something else inside a mark is attributed to the
// mark: the readings cannot tell them apart.
//
// What is seen. LinuxCNC's status carries the active work system only. An inactive system is
// known from the last time it was active, or field by field from what a panel command wrote
// into it (`written`: an entry whose `before` is empty where the field was not known); a change
// made to it from outside is seen when it is next active, as one entry.
// What was last seen is kept in the file too: a change made while CoffeeCNC did not run is an
// `outside` entry at the first reading. The first value of a system or a tool table is no
// entry; a tool added has no `before`, a tool removed no `after`.
//
// The file. Written whole by a durable replacement (temporary file, sync, rename, sync of the
// directory): after an entry at once, after a change of what was seen alone at most once per
// write_period. A file that cannot be read as version 1 is renamed to
// `offset-history.json.corrupt` (replacing an older one); the history starts empty and says so,
// in `reason` and once as an operator message. A failed write is said once per reason.
//
// Thread-safe: one mutex, never held across a file operation's caller.
class OffsetHistoryStore {
public:
  using Clock = std::chrono::steady_clock;
  static constexpr std::size_t max_entries = 200;
  static constexpr auto grace = std::chrono::seconds{2};
  static constexpr auto write_period = std::chrono::seconds{1};
  // Smaller differences are the same value: millimetres, degrees.
  static constexpr double tolerance = 1e-6;
  static constexpr std::size_t max_file_bytes = std::size_t{1024} * 1024;
  static constexpr std::string_view file_name = "offset-history.json";
  static constexpr std::string_view corrupt_name = "offset-history.json.corrupt";

  // An empty directory: the history is kept for this session only, and `unsaved_reason` says
  // why. Reads the file. The files must outlive the store.
  OffsetHistoryStore(ToolModelFiles& files, std::string directory,
                     std::string unsaved_reason = {});

  // Worker. A panel command that can change offsets begins: the mark it ends with.
  [[nodiscard]] auto begin(HistorySource source, std::optional<std::uint64_t> restores = {})
      -> std::uint64_t;
  // Worker. The command has returned.
  void end(std::uint64_t mark, Clock::time_point now);
  // Worker. The command of `mark` wrote these values into work system `wcs` (x, y, z,
  // rotation; none: unchanged), also when it is not the active one: an entry unless the
  // readings already showed them.
  void written(std::uint64_t mark, int wcs, const std::array<std::optional<double>, 4>& values,
               std::chrono::milliseconds unix_time);
  // Worker. The entry with this id, while it is in the history.
  [[nodiscard]] auto entry(std::uint64_t id) const -> std::optional<HistoryEntry>;

  // Publisher. Compares the reading with what was seen last, records what changed, writes the
  // file when due, and sets reading.offset_history; operator messages go to reading.messages.
  void observe(PanelReading& reading, std::chrono::milliseconds unix_time, Clock::time_point now);

  // The history as it is now.
  [[nodiscard]] auto snapshot() const -> std::shared_ptr<const OffsetHistory>;

private:
  struct Mark {
    std::uint64_t id{};
    HistorySource source{HistorySource::outside};
    std::optional<std::uint64_t> restores;
    bool open{true};
    Clock::time_point until{};
  };
  // X, Y, Z and the XY rotation of one work system, each as far as it is known.
  using System = std::array<std::optional<double>, 4>;
  struct ToolValue {
    double length{};
    double diameter{};
  };
  // What was seen last.
  struct Seen {
    std::array<System, 9> systems{};
    std::optional<Vec3> g92;
    std::optional<std::map<int, ToolValue>> tools;
  };

  void load();
  void save(PanelReading& reading);
  [[nodiscard]] auto source(Clock::time_point now) const
      -> std::pair<HistorySource, std::optional<std::uint64_t>>;
  void add(HistoryEntry entry);
  void publish();
  // `values`: what is written or read; a field without a value is left as it is. A field not
  // known before is learnt without an entry unless `record_unknown`.
  void change_system(int wcs, const System& values, bool record_unknown,
                     std::chrono::milliseconds unix_time,
                     std::pair<HistorySource, std::optional<std::uint64_t>> by);

  ToolModelFiles& files_;
  std::string directory_;
  mutable std::mutex mutex_;
  std::vector<HistoryEntry> entries_; // newest first
  std::uint64_t next_id_{1};
  std::uint64_t revision_{1};
  std::uint64_t next_mark_{1};
  std::vector<Mark> marks_;
  Seen seen_;
  Clock::time_point program_until_{};
  // The work system the last live reading showed active; 0 before one.
  int active_wcs_{};
  Clock::time_point write_after_{};
  std::string reason_;
  std::string load_notice_;
  std::string save_error_;
  std::shared_ptr<const OffsetHistory> snapshot_;
  bool saved_{};
  // An entry is not on the disk yet: written at the next reading.
  bool entries_due_{};
  // What was seen changed without an entry.
  bool seen_due_{};
};

} // namespace coffeecnc

#endif // COFFEECNC_APP_OFFSET_HISTORY_HPP
