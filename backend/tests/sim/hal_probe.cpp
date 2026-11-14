#include "hal_probe.hpp"

#include <format>
#include <utility>

#include <hal.h>

namespace coffeecnc {

namespace {
// As the executable's component reads its own: the memory belongs to the signal and is written
// by another thread or process.
template <typename Value> auto get(const volatile Value* pin) -> Value {
  Value value;
  __atomic_load(pin, &value, __ATOMIC_RELAXED);
  return value;
}
} // namespace

struct HalProbe::Pins {
  hal_bit_t* jog_stop;
  hal_bit_t* watchdog_ok;
  hal_bit_t* tool_change;
  hal_bit_t* tool_changed;
  hal_float_t* x;
};

auto HalProbe::create() -> std::expected<HalProbe, std::string> {
  const int id = hal_init("coffeecnc-test");
  if (id < 0)
    return std::unexpected(std::format("hal_init(\"coffeecnc-test\") failed: {}", id));
  HalProbe probe{id, static_cast<Pins*>(hal_malloc(sizeof(Pins)))};
  auto* pins = probe.pins_;
  if (pins == nullptr)
    return std::unexpected(std::string("hal_malloc failed"));
  if (hal_pin_bit_new("coffeecnc-test.jog-stop", HAL_IN, &pins->jog_stop, id) != 0 ||
      hal_pin_bit_new("coffeecnc-test.watchdog-ok", HAL_IN, &pins->watchdog_ok, id) != 0 ||
      hal_pin_bit_new("coffeecnc-test.tool-change", HAL_IN, &pins->tool_change, id) != 0 ||
      hal_pin_bit_new("coffeecnc-test.tool-changed", HAL_IN, &pins->tool_changed, id) != 0 ||
      hal_pin_float_new("coffeecnc-test.x-pos", HAL_IN, &pins->x, id) != 0)
    return std::unexpected(std::string("Creating the pins of coffeecnc-test failed"));
  if (const int result = hal_ready(id); result != 0)
    return std::unexpected(std::format("hal_ready failed: {}", result));
  return probe;
}

HalProbe::HalProbe(int id, Pins* pins) : id_(id), pins_(pins) {}
HalProbe::HalProbe(HalProbe&& other) noexcept
    : id_(std::exchange(other.id_, -1)), pins_(std::exchange(other.pins_, nullptr)) {}
HalProbe::~HalProbe() {
  if (id_ >= 0)
    hal_exit(id_);
}

auto HalProbe::jog_stop() const -> bool { return get(pins_->jog_stop); }
auto HalProbe::watchdog_ok() const -> bool { return get(pins_->watchdog_ok); }
auto HalProbe::tool_change() const -> bool { return get(pins_->tool_change); }
auto HalProbe::tool_changed() const -> bool { return get(pins_->tool_changed); }
auto HalProbe::x() const -> double { return get(pins_->x); }

} // namespace coffeecnc
