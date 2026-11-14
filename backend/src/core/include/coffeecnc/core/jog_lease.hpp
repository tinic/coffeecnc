#ifndef COFFEECNC_CORE_JOG_LEASE_HPP
#define COFFEECNC_CORE_JOG_LEASE_HPP

#include <coffeecnc/core/command_policy.hpp>

#include <array>
#include <chrono>
#include <optional>

namespace coffeecnc {

// Owned and polled by the native control thread, never a GTK/GLib UI callback.
// Tracks only jogs started by this panel. A live adapter still needs an independently
// scheduled HAL watchdog: this class cannot run when the executable dies or stalls.
class JogLease {
public:
  using Clock = std::chrono::steady_clock;
  using Time = Clock::time_point;
  static constexpr auto timeout = std::chrono::milliseconds{150};
  using Stops = std::array<bool, 3>;

  // Call after a held jog is actually submitted, not after merely validating a request.
  void refresh(Axis, Time now);
  // A release must explicitly stop the adapter even if no lease was recorded.
  void release(Axis);
  [[nodiscard]] auto expired(Time now) -> Stops;
  // On panel navigation/disconnect, controller link loss, mode change, abort or shutdown.
  [[nodiscard]] auto cancel_all() -> Stops;
  [[nodiscard]] auto active(Axis) const -> bool;

private:
  std::array<std::optional<Time>, 3> refreshed_{};
};

} // namespace coffeecnc

#endif // COFFEECNC_CORE_JOG_LEASE_HPP
