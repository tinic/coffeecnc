#include <coffeecnc/linuxcnc/hal_component.hpp>

#include <cmath>
#include <format>
#include <utility>

#include <hal.h>

namespace coffeecnc {

// Where HAL keeps each pin's value. Lives in HAL shared memory.
struct HalComponent::Pins {
  hal_bit_t* jog_stop;
  hal_bit_t* heartbeat;
  hal_bit_t* watchdog_enable;
  hal_bit_t* watchdog_ok;
  hal_bit_t* tool_change;
  hal_s32_t* tool_number;
  hal_bit_t* tool_changed;
  hal_float_t* spindle_rpm;
  hal_bit_t* spindle_at_speed;
  hal_bit_t* probe_input;
};

auto HalComponent::create(const std::string& name, const HalSignals& wired)
    -> std::expected<HalComponent, std::string> {
  const int id = hal_init(name.c_str());
  if (id < 0)
    return std::unexpected(std::format("hal_init(\"{}\") failed: {}", name, id));
  HalComponent component{id, static_cast<Pins*>(hal_malloc(sizeof(Pins))), wired};
  auto* pins = component.pins_;
  if (pins == nullptr)
    return std::unexpected(std::string("hal_malloc failed"));
  const auto bit = [&](const char* pin, hal_pin_dir_t direction, hal_bit_t** slot) -> bool {
    return hal_pin_bit_new((name + '.' + pin).c_str(), direction, slot, id) == 0;
  };
  if (!bit("jog-stop", HAL_OUT, &pins->jog_stop) || !bit("heartbeat", HAL_OUT, &pins->heartbeat) ||
      !bit("watchdog-enable", HAL_OUT, &pins->watchdog_enable) ||
      !bit("watchdog-ok", HAL_IN, &pins->watchdog_ok) ||
      !bit("tool-change", HAL_IN, &pins->tool_change) ||
      hal_pin_s32_new((name + ".tool-number").c_str(), HAL_IN, &pins->tool_number, id) != 0 ||
      !bit("tool-changed", HAL_OUT, &pins->tool_changed) ||
      hal_pin_float_new((name + ".spindle-rpm").c_str(), HAL_IN, &pins->spindle_rpm, id) != 0 ||
      !bit("spindle-at-speed", HAL_IN, &pins->spindle_at_speed) ||
      !bit("probe-input", HAL_IN, &pins->probe_input))
    return std::unexpected(std::format("Creating the HAL pins of {} failed", name));
  if (const int result = hal_ready(id); result != 0)
    return std::unexpected(std::format("hal_ready failed: {}", result));
  return component;
}

HalComponent::HalComponent(int id, Pins* pins, const HalSignals& wired)
    : id_(id), pins_(pins), wired_(wired) {}

HalComponent::HalComponent(HalComponent&& other) noexcept
    : id_(std::exchange(other.id_, -1)), pins_(std::exchange(other.pins_, nullptr)),
      wired_(other.wired_), hold_(other.hold_), stop_raised_(other.stop_raised_),
      enable_changed_(other.enable_changed_) {}

auto HalComponent::operator=(HalComponent&& other) noexcept -> HalComponent& {
  if (this != &other) {
    if (id_ >= 0)
      hal_exit(id_);
    id_ = std::exchange(other.id_, -1);
    pins_ = std::exchange(other.pins_, nullptr);
    wired_ = other.wired_;
    hold_ = other.hold_;
    stop_raised_ = other.stop_raised_;
    enable_changed_ = other.enable_changed_;
  }
  return *this;
}

HalComponent::~HalComponent() {
  if (id_ >= 0)
    hal_exit(id_);
}

void HalComponent::hold_jog_stop(bool on) {
  const auto now = Clock::now();
  hold_ = on;
  if (on && !*pins_->jog_stop) {
    *pins_->jog_stop = true;
    stop_raised_ = now;
  } else if (!on && *pins_->jog_stop && now - stop_raised_ >= level_time) {
    *pins_->jog_stop = false;
  }
}

auto HalComponent::jog_stop_high() const -> bool { return *pins_->jog_stop; }

void HalComponent::beat() {
  const auto now = Clock::now();
  *pins_->heartbeat = !*pins_->heartbeat;
  // A level that was released before the servo thread could have seen it.
  if (!hold_ && *pins_->jog_stop && now - stop_raised_ >= level_time)
    *pins_->jog_stop = false;
  // A watchdog that has tripped stays tripped until its enable input falls and rises again.
  if (!*pins_->watchdog_ok && now - enable_changed_ >= level_time) {
    *pins_->watchdog_enable = !*pins_->watchdog_enable;
    enable_changed_ = now;
  }
  if (!*pins_->tool_change)
    *pins_->tool_changed = false;
}

auto HalComponent::watchdog_ok() const -> bool { return *pins_->watchdog_ok; }

auto HalComponent::tool_change_request() const -> std::optional<int> {
  if (!*pins_->tool_change || *pins_->tool_changed)
    return {};
  return *pins_->tool_number;
}

void HalComponent::confirm_tool_changed() {
  if (*pins_->tool_change)
    *pins_->tool_changed = true;
}

auto HalComponent::spindle_rpm() const -> std::optional<double> {
  const double rpm = *pins_->spindle_rpm;
  if (!wired_.spindle_rpm || !std::isfinite(rpm))
    return {};
  return std::fabs(rpm);
}

auto HalComponent::spindle_at_speed() const -> std::optional<bool> {
  if (!wired_.spindle_at_speed)
    return {};
  return *pins_->spindle_at_speed != 0;
}

auto HalComponent::probe_input() const -> std::optional<bool> {
  if (!wired_.probe)
    return {};
  return *pins_->probe_input != 0;
}

} // namespace coffeecnc
