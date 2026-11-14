#include <coffeecnc/core/task_heartbeat.hpp>

namespace coffeecnc {

auto TaskHeartbeat::observe(std::int64_t writes, Clock::time_point now) -> Link {
  if (last_ && *last_ != writes && now - read_at_ < stale_after) {
    advanced_ = true;
    advanced_at_ = now;
  }
  last_ = writes;
  read_at_ = now;
  return advanced_ && now - advanced_at_ < stale_after ? Link::live : Link::stale;
}

} // namespace coffeecnc
