#ifndef COFFEECNC_CORE_STATUS_SOURCE_HPP
#define COFFEECNC_CORE_STATUS_SOURCE_HPP

#include <coffeecnc/core/command_policy.hpp>

namespace coffeecnc {

// A read-only view of the controller that never touches its command channel. The runtime's
// status thread is its only caller and publishes each reading to the session; the control thread
// decides on that copy and never calls this itself: a real implementation takes the status
// buffer's semaphore, which a stopped LinuxCNC task can hold. Implementations need not be
// thread-safe.
class StatusSource {
public:
  StatusSource() = default;
  virtual ~StatusSource() = default;

  // Observed controller state. May block as long as the controller holds its status buffer.
  [[nodiscard]] virtual auto read() -> ControllerState = 0;

protected:
  StatusSource(const StatusSource&) = default;
  StatusSource(StatusSource&&) = default;
  auto operator=(const StatusSource&) -> StatusSource& = default;
  auto operator=(StatusSource&&) -> StatusSource& = default;
};

} // namespace coffeecnc

#endif // COFFEECNC_CORE_STATUS_SOURCE_HPP
