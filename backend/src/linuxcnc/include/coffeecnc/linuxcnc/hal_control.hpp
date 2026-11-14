#ifndef COFFEECNC_LINUXCNC_HAL_CONTROL_HPP
#define COFFEECNC_LINUXCNC_HAL_CONTROL_HPP

#include <coffeecnc/core/safety_outputs.hpp>
#include <coffeecnc/linuxcnc/hal_component.hpp>

#include <mutex>
#include <optional>

namespace coffeecnc {

// What the HAL component's input pins read in one control period.
struct HalInputs {
  // The tool LinuxCNC is waiting for the operator to load, until it is confirmed.
  std::optional<int> tool_change_request;
  // Nothing where the machine does not connect the pin.
  std::optional<double> spindle_rpm; // magnitude
  std::optional<bool> spindle_at_speed;
  std::optional<bool> probe_input;
  bool watchdog_ok{};
  bool jog_stop_high{};
};

// What crosses between the control thread, which alone touches the HAL component, and every
// other thread: a copy of the inputs one way, the tool change confirmation the other. Thread-safe;
// its mutex is held for a copy only.
class HalShared {
public:
  // Any thread: the inputs as the control thread read them last, at most one control period old.
  [[nodiscard]] auto inputs() const -> HalInputs;
  // Any thread: the operator has loaded the requested tool. The control thread raises
  // tool-changed in its next period; the request is gone from inputs() at once. Ignored while
  // no change is requested.
  void confirm_tool_changed();

  // Control thread.
  void publish(const HalInputs& inputs);
  [[nodiscard]] auto take_confirmation() -> bool;

private:
  mutable std::mutex mutex_;
  HalInputs inputs_;
  bool confirmed_{};
};

// The control thread's use of the HAL component, as the runtime sees it. Every member is called
// by that thread only.
class HalControl final : public SafetyOutputs {
public:
  // Both must outlive this.
  HalControl(HalComponent& hal, HalShared& shared);

  // Raises tool-changed when another thread has confirmed the change, then HalComponent::beat,
  // then publishes the inputs. In that order: a confirmation is on the pin before beat() looks
  // at the handshake, and beat() takes it back only once LinuxCNC has dropped tool-change.
  void beat() override;
  void hold_jog_stop(bool on) override;
  [[nodiscard]] auto jog_stop_high() const -> bool override;
  [[nodiscard]] auto watchdog_ok() const -> bool override;

private:
  HalComponent& hal_;
  HalShared& shared_;
};

} // namespace coffeecnc

#endif // COFFEECNC_LINUXCNC_HAL_CONTROL_HPP
