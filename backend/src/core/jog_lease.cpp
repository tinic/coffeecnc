#include <coffeecnc/core/jog_lease.hpp>

namespace coffeecnc {

void JogLease::refresh(Axis axis, Time now) {
  if (const auto i = axis_index(axis)) refreshed_[*i] = now;
}
void JogLease::release(Axis axis) {
  if (const auto i = axis_index(axis)) refreshed_[*i].reset();
}
JogLease::Stops JogLease::expired(Time now) {
  Stops stops{};
  for (std::size_t i = 0; i < refreshed_.size(); ++i) {
    if (refreshed_[i] && now - *refreshed_[i] >= timeout) {
      stops[i] = true;
      refreshed_[i].reset();
    }
  }
  return stops;
}
JogLease::Stops JogLease::cancel_all() {
  Stops stops{};
  for (std::size_t i = 0; i < refreshed_.size(); ++i) {
    stops[i] = refreshed_[i].has_value();
    refreshed_[i].reset();
  }
  return stops;
}
bool JogLease::active(Axis axis) const {
  const auto i = axis_index(axis);
  return i && refreshed_[*i].has_value();
}

} // namespace coffeecnc
