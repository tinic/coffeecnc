#ifndef COFFEECNC_CORE_TASK_HEARTBEAT_HPP
#define COFFEECNC_CORE_TASK_HEARTBEAT_HPP

#include <coffeecnc/core/command_policy.hpp>

#include <chrono>
#include <cstdint>
#include <optional>

namespace coffeecnc {

// Tells a running LinuxCNC task from the last status it left behind. Task writes its status every
// cycle and each write advances the status buffer's write counter; after task has died the last
// status stays readable for ever. EMC_STAT's own task.heartbeat counts on LinuxCNC 2.9.4 but was
// seen stuck at 0 on another 2.9 build, so the write counter is what is used.
class TaskHeartbeat {
public:
  using Clock = std::chrono::steady_clock;
  // Task cycles every few milliseconds; a second without a write means it is gone or hung.
  static constexpr auto stale_after = std::chrono::milliseconds{1000};

  // Records one reading of the write counter. `live` only once the counter has been seen to
  // advance, and only while its last advance is less than `stale_after` old; otherwise `stale`.
  // The first reading proves nothing, and neither does an advance since a reading `stale_after`
  // or more ago (task may have died right after it): read at least a few times per second.
  // Never `down`: that is the absence of any status.
  [[nodiscard]] auto observe(std::int64_t writes, Clock::time_point now) -> Link;

private:
  std::optional<std::int64_t> last_;
  Clock::time_point read_at_{};
  Clock::time_point advanced_at_{};
  bool advanced_{};
};

} // namespace coffeecnc

#endif // COFFEECNC_CORE_TASK_HEARTBEAT_HPP
