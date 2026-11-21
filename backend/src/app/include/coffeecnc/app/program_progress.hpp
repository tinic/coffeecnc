#ifndef COFFEECNC_APP_PROGRAM_PROGRESS_HPP
#define COFFEECNC_APP_PROGRAM_PROGRESS_HPP

#include <coffeecnc/app/tool_model_store.hpp>
#include <coffeecnc/core/panel_source.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace coffeecnc {

// One publisher thread tracks motion blocks. Advancing to another block establishes that the
// previous block has completed. The ordinary restart planner replays the most recent block.
// Durable writes are bounded during motion and immediate on a stop.
class ProgramProgress {
public:
  using Clock = std::chrono::steady_clock;

  ProgramProgress(ToolModelFiles& files, std::string directory);
  void observe(PanelReading& reading, Clock::time_point now);

private:
  struct Stamp {
    std::int64_t modified{};
    std::uintmax_t bytes{};
    friend auto operator==(const Stamp&, const Stamp&) -> bool = default;
  };
  struct Record {
    std::string path;
    Stamp stamp;
    RestartHint next;
    int completed_line{};
  };
  struct Run {
    Record progress;
    std::uint64_t revision{};
    std::uint64_t aborts{};
  };

  [[nodiscard]] static auto stamp(const std::string& path) -> std::optional<Stamp>;
  void load();
  void save(PanelReading& reading);
  void record(const Record& entry);

  ToolModelFiles& files_;
  std::string directory_;
  std::vector<Record> records_;
  std::optional<Run> running_;
  Clock::time_point write_after_{};
  std::string save_error_;
  bool dirty_{};
  bool interrupted_{};
};

} // namespace coffeecnc

#endif // COFFEECNC_APP_PROGRAM_PROGRESS_HPP
