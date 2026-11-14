#include <coffeecnc/linuxcnc/hal_component.hpp>

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
};

auto HalComponent::create(const std::string& name) -> std::expected<HalComponent, std::string> {
  const int id = hal_init(name.c_str());
  if (id < 0)
    return std::unexpected(std::format("hal_init(\"{}\") failed: {}", name, id));
  HalComponent component{id, static_cast<Pins*>(hal_malloc(sizeof(Pins)))};
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
      !bit("tool-changed", HAL_OUT, &pins->tool_changed))
    return std::unexpected(std::format("Creating the HAL pins of {} failed", name));
  if (const int result = hal_ready(id); result != 0)
    return std::unexpected(std::format("hal_ready failed: {}", result));
  return component;
}

HalComponent::HalComponent(int id, Pins* pins) : id_(id), pins_(pins) {}

HalComponent::HalComponent(HalComponent&& other) noexcept
    : id_(std::exchange(other.id_, -1)), pins_(std::exchange(other.pins_, nullptr)),
      stop_wanted_(other.stop_wanted_), stop_changed_(other.stop_changed_),
      enable_changed_(other.enable_changed_) {}

auto HalComponent::operator=(HalComponent&& other) noexcept -> HalComponent& {
  if (this != &other) {
    if (id_ >= 0)
      hal_exit(id_);
    id_ = std::exchange(other.id_, -1);
    pins_ = std::exchange(other.pins_, nullptr);
    stop_wanted_ = other.stop_wanted_;
    stop_changed_ = other.stop_changed_;
    enable_changed_ = other.enable_changed_;
  }
  return *this;
}

HalComponent::~HalComponent() {
  if (id_ >= 0)
    hal_exit(id_);
}

void HalComponent::request_jog_stop() {
  const auto now = Clock::now();
  if (!*pins_->jog_stop && now - stop_changed_ >= level_time) {
    *pins_->jog_stop = true;
    stop_changed_ = now;
    stop_wanted_ = false;
  } else {
    // A jog may have started since the pulse in progress rose: it needs an edge of its own.
    stop_wanted_ = true;
  }
}

auto HalComponent::jog_stop_busy() const -> bool { return stop_wanted_ || *pins_->jog_stop; }

void HalComponent::beat() {
  const auto now = Clock::now();
  *pins_->heartbeat = !*pins_->heartbeat;
  if (now - stop_changed_ >= level_time) {
    if (*pins_->jog_stop) {
      *pins_->jog_stop = false;
      stop_changed_ = now;
    } else if (stop_wanted_) {
      *pins_->jog_stop = true;
      stop_changed_ = now;
      stop_wanted_ = false;
    }
  }
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

} // namespace coffeecnc
