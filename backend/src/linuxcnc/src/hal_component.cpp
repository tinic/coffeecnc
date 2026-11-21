#include <coffeecnc/linuxcnc/hal_component.hpp>

#include <cmath>
#include <format>
#include <utility>

#include <hal.h>

namespace coffeecnc {

namespace {
// Pin memory is shared: LinuxCNC's realtime thread reads and writes it, and so does every other
// HAL component whose pin is on the same signal, in this process or another. Every access is
// therefore one atomic load or store of the whole value; nothing here reads a pin twice and
// expects the same answer.
template <typename Value> auto get(const volatile Value* pin) -> Value {
  Value value;
  __atomic_load(pin, &value, __ATOMIC_RELAXED);
  return value;
}
template <typename Value> void set(volatile Value* pin, Value value) {
  __atomic_store(pin, &value, __ATOMIC_RELAXED);
}
} // namespace

// Where HAL keeps each pin's value. Lives in HAL shared memory.
struct HalComponent::Pins {
  hal_bit_t* jog_stop;
  hal_bit_t* jog_contact;
  hal_bit_t* heartbeat;
  hal_bit_t* watchdog_enable;
  hal_bit_t* watchdog_ok;
  hal_bit_t* tool_change;
  hal_s32_t* tool_number;
  hal_bit_t* tool_changed;
  hal_float_t* spindle_rpm;
  hal_bit_t* spindle_at_speed;
  hal_bit_t* probe_input;
  hal_bit_t* probe_setter_select;
  hal_bit_t* probe_setter_selected;
  hal_bit_t* probe_cycle_active;
  hal_bit_t* probe_cycle_lease;
  hal_bit_t* probe_motion_permitted;
  hal_bit_t* paused_z_enable;
  hal_s32_t* paused_z_counts;
  hal_bit_t* paused_z_clear;
  hal_float_t* paused_z_offset;
  hal_bit_t* paused_z_active;
  hal_bit_t* paused_z_limited;
};

auto HalComponent::create(const std::string& name, const HalSignals& wired)
    -> std::expected<HalComponent, std::string> {
  const int id = hal_init(name.c_str());
  if (id < 0)
    return std::unexpected(std::format("hal_init(\"{}\") failed: {}", name, id));
  HalComponent component{id, static_cast<Pins*>(hal_malloc(sizeof(Pins))), wired, name};
  auto* pins = component.pins_;
  if (pins == nullptr)
    return std::unexpected(std::string("hal_malloc failed"));
  const auto bit = [&](const char* pin, hal_pin_dir_t direction, hal_bit_t** slot) -> bool {
    return hal_pin_bit_new((name + '.' + pin).c_str(), direction, slot, id) == 0;
  };
  if (!bit("jog-stop", HAL_OUT, &pins->jog_stop) || !bit("heartbeat", HAL_OUT, &pins->heartbeat) ||
      !bit("jog-contact", HAL_IN, &pins->jog_contact) ||
      !bit("watchdog-enable", HAL_OUT, &pins->watchdog_enable) ||
      !bit("watchdog-ok", HAL_IN, &pins->watchdog_ok) ||
      !bit("tool-change", HAL_IN, &pins->tool_change) ||
      hal_pin_s32_new((name + ".tool-number").c_str(), HAL_IN, &pins->tool_number, id) != 0 ||
      !bit("tool-changed", HAL_OUT, &pins->tool_changed) ||
      hal_pin_float_new((name + ".spindle-rpm").c_str(), HAL_IN, &pins->spindle_rpm, id) != 0 ||
      !bit("spindle-at-speed", HAL_IN, &pins->spindle_at_speed) ||
      !bit("probe-input", HAL_IN, &pins->probe_input) ||
      !bit("probe-setter-select", HAL_OUT, &pins->probe_setter_select) ||
      !bit("probe-setter-selected", HAL_IN, &pins->probe_setter_selected) ||
      !bit("probe-cycle-active", HAL_OUT, &pins->probe_cycle_active) ||
      !bit("probe-cycle-lease", HAL_OUT, &pins->probe_cycle_lease) ||
      !bit("probe-motion-permitted", HAL_IN, &pins->probe_motion_permitted) ||
      !bit("eoffset-z-enable", HAL_OUT, &pins->paused_z_enable) ||
      hal_pin_s32_new((name + ".eoffset-z-counts").c_str(), HAL_OUT, &pins->paused_z_counts, id) !=
          0 ||
      !bit("eoffset-z-clear", HAL_OUT, &pins->paused_z_clear) ||
      hal_pin_float_new((name + ".eoffset-z").c_str(), HAL_IN, &pins->paused_z_offset, id) != 0 ||
      !bit("eoffset-active", HAL_IN, &pins->paused_z_active) ||
      !bit("eoffset-limited", HAL_IN, &pins->paused_z_limited))
    return std::unexpected(std::format("Creating the HAL pins of {} failed", name));
  if (const int result = hal_ready(id); result != 0)
    return std::unexpected(std::format("hal_ready failed: {}", result));
  return component;
}

HalComponent::HalComponent(int id, Pins* pins, const HalSignals& wired, std::string name)
    : id_(id), pins_(pins), wired_(wired), name_(std::move(name)) {}

HalComponent::HalComponent(HalComponent&& other) noexcept
    : id_(std::exchange(other.id_, -1)), pins_(std::exchange(other.pins_, nullptr)),
      wired_(other.wired_), name_(std::move(other.name_)),
      manual_change_signals_(std::exchange(other.manual_change_signals_, {})),
      hold_(other.hold_), stop_raised_(other.stop_raised_),
      enable_changed_(other.enable_changed_) {}

auto HalComponent::operator=(HalComponent&& other) noexcept -> HalComponent& {
  if (this != &other) {
    disconnect_manual_tool_change();
    if (id_ >= 0)
      hal_exit(id_);
    id_ = std::exchange(other.id_, -1);
    pins_ = std::exchange(other.pins_, nullptr);
    wired_ = other.wired_;
    name_ = std::move(other.name_);
    manual_change_signals_ = std::exchange(other.manual_change_signals_, {});
    hold_ = other.hold_;
    stop_raised_ = other.stop_raised_;
    enable_changed_ = other.enable_changed_;
  }
  return *this;
}

HalComponent::~HalComponent() {
  disconnect_manual_tool_change();
  if (id_ >= 0)
    hal_exit(id_);
}

auto HalComponent::connect_manual_tool_change() -> std::expected<void, std::string> {
  if (!manual_change_signals_.back().empty())
    return {};

  struct Connection {
    const char* panel_pin;
    const char* controller_pin;
    hal_type_t type;
  };
  constexpr std::array connections{
      Connection{"tool-change", "iocontrol.0.tool-change", HAL_BIT},
      Connection{"tool-number", "iocontrol.0.tool-prep-number", HAL_S32},
      Connection{"tool-changed", "iocontrol.0.tool-changed", HAL_BIT},
  };
  for (std::size_t i = 0; i < connections.size(); ++i) {
    const auto& connection = connections[i];
    const auto panel_pin = std::format("{}.{}", name_, connection.panel_pin);
    auto signal = std::format("{}-manual-{}", name_, connection.panel_pin);
    if (hal_signal_new(signal.c_str(), connection.type) != 0) {
      disconnect_manual_tool_change();
      return std::unexpected(std::format("Cannot create HAL signal {}.", signal));
    }
    auto& created = manual_change_signals_[i];
    created = std::move(signal);
    // HAL refuses a pin already linked to another signal. Roll back only signals we created.
    if (hal_link(connection.controller_pin, created.c_str()) != 0 ||
        hal_link(panel_pin.c_str(), created.c_str()) != 0) {
      disconnect_manual_tool_change();
      return std::unexpected(std::format(
          "Cannot wire {} to {}. Leave MANUAL_TOOLCHANGE off for an existing HAL tool changer.",
          connection.controller_pin, panel_pin));
    }
  }
  return {};
}

void HalComponent::disconnect_manual_tool_change() noexcept {
  for (auto& signal : manual_change_signals_) {
    if (!signal.empty()) {
      hal_signal_delete(signal.c_str());
      signal.clear();
    }
  }
}

void HalComponent::hold_jog_stop(bool on) {
  const auto now = Clock::now();
  hold_ = on;
  const bool high = get(pins_->jog_stop);
  if (on && !high) {
    set(pins_->jog_stop, true);
    stop_raised_ = now;
  } else if (!on && high && now - stop_raised_ >= level_time) {
    set(pins_->jog_stop, false);
  }
}

auto HalComponent::jog_stop_high() const -> bool { return get(pins_->jog_stop); }
auto HalComponent::jog_contact() const -> bool { return get(pins_->jog_contact); }

void HalComponent::beat() {
  const auto now = Clock::now();
  set(pins_->heartbeat, !get(pins_->heartbeat));
  // A level that was released before the servo thread could have seen it.
  if (!hold_ && get(pins_->jog_stop) && now - stop_raised_ >= level_time)
    set(pins_->jog_stop, false);
  // A watchdog that has tripped stays tripped until its enable input falls and rises again.
  if (!get(pins_->watchdog_ok) && now - enable_changed_ >= level_time) {
    set(pins_->watchdog_enable, !get(pins_->watchdog_enable));
    enable_changed_ = now;
  }
  if (!get(pins_->tool_change))
    set(pins_->tool_changed, false);
}

auto HalComponent::watchdog_ok() const -> bool { return get(pins_->watchdog_ok); }

auto HalComponent::tool_change_request() const -> std::optional<int> {
  if (!get(pins_->tool_change) || get(pins_->tool_changed))
    return {};
  return get(pins_->tool_number);
}

void HalComponent::confirm_tool_changed() {
  if (get(pins_->tool_change))
    set(pins_->tool_changed, true);
}

auto HalComponent::spindle_rpm() const -> std::optional<double> {
  const double rpm = get(pins_->spindle_rpm);
  if (!wired_.spindle_rpm || !std::isfinite(rpm))
    return {};
  return std::fabs(rpm);
}

auto HalComponent::spindle_at_speed() const -> std::optional<bool> {
  if (!wired_.spindle_at_speed)
    return {};
  return get(pins_->spindle_at_speed);
}

auto HalComponent::probe_input() const -> std::optional<bool> {
  if (!wired_.probe)
    return {};
  return get(pins_->probe_input);
}

auto HalComponent::probe_setter_selected() const -> bool {
  return get(pins_->probe_setter_selected);
}

auto HalComponent::probe_motion_permitted() const -> bool {
  return get(pins_->probe_motion_permitted);
}

auto HalComponent::probe_cycle_active() const -> bool {
  return get(pins_->probe_cycle_active);
}

void HalComponent::set_probe_cycle(ProbeCycleOutputs outputs) {
  // Arm after the lease is high. Disarm before dropping it on normal completion, when the
  // worker has already confirmed the controller is stationary.
  if (!outputs.active)
    set(pins_->probe_cycle_active, false);
  set(pins_->probe_setter_select, outputs.active && outputs.setter);
  set(pins_->probe_cycle_lease, outputs.lease);
  if (outputs.active)
    set(pins_->probe_cycle_active, true);
}

auto HalComponent::paused_z() const -> std::optional<PausedZInputs> {
  if (!wired_.paused_z)
    return {};
  const double offset = get(pins_->paused_z_offset);
  if (!std::isfinite(offset))
    return {};
  return PausedZInputs{offset, get(pins_->paused_z_active), get(pins_->paused_z_limited)};
}

void HalComponent::set_paused_z(PausedZOutputs outputs) {
  if (!outputs.enable)
    set(pins_->paused_z_enable, false);
  set(pins_->paused_z_counts, outputs.counts);
  set(pins_->paused_z_clear, outputs.enable && outputs.clear);
  if (outputs.enable)
    set(pins_->paused_z_enable, true);
}

} // namespace coffeecnc
