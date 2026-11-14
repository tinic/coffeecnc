#include <coffeecnc/core/jog_lease.hpp>

namespace coffeecnc {

void JogLease::refresh(Axis axis, Time now) {
  if (const auto i = axis_index(axis))
    refreshed_[*i] = now;
}
void JogLease::release(Axis axis) {
  if (const auto i = axis_index(axis))
    refreshed_[*i].reset();
}
auto JogLease::expired(Time now) -> JogLease::Stops {
  Stops stops{};
  for (std::size_t i = 0; i < refreshed_.size(); ++i) {
    auto& refreshed = refreshed_[i];
    if (refreshed && now - *refreshed >= timeout) {
      stops[i] = true;
      refreshed.reset();
    }
  }
  return stops;
}
auto JogLease::cancel_all() -> JogLease::Stops {
  Stops stops{};
  for (std::size_t i = 0; i < refreshed_.size(); ++i) {
    stops[i] = refreshed_[i].has_value();
    refreshed_[i].reset();
  }
  return stops;
}
auto JogLease::active(Axis axis) const -> bool {
  const auto i = axis_index(axis);
  return i && refreshed_[*i].has_value();
}
auto JogLease::refreshed(Axis axis) const -> std::optional<Time> {
  const auto i = axis_index(axis);
  return i ? refreshed_[*i] : std::nullopt;
}

} // namespace coffeecnc
