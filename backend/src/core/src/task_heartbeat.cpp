#include <coffeecnc/core/task_heartbeat.hpp>

namespace coffeecnc {

auto TaskHeartbeat::observe(std::int64_t writes, Clock::time_point now) -> Link {
  if (last_ && *last_ != writes) {
    advanced_ = true;
    advanced_at_ = now;
  }
  last_ = writes;
  return advanced_ && now - advanced_at_ < stale_after ? Link::live : Link::stale;
}

} // namespace coffeecnc
