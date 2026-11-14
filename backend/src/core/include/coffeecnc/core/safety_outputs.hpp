#ifndef COFFEECNC_CORE_SAFETY_OUTPUTS_HPP
#define COFFEECNC_CORE_SAFETY_OUTPUTS_HPP

namespace coffeecnc {

// The path to the machine that needs no command channel: the heartbeat a hardware watchdog
// listens to and the jog-stop level. One thread, the runtime's control thread, calls every
// member, once per control period. No member may block.
class SafetyOutputs {
public:
  SafetyOutputs() = default;
  virtual ~SafetyOutputs() = default;

  // Proves the control thread is running. The watchdog stops and inhibits jogs without it.
  virtual void beat() = 0;
  // The jog-stop level. While it is high every jog in motion is stopped and none can start. It
  // rises at once and stays high until `false` is asked for; it may then stay high a little
  // longer so that the machine cannot miss a short level.
  virtual void hold_jog_stop(bool on) = 0;
  // The level as it is driven at this moment.
  [[nodiscard]] virtual auto jog_stop_high() const -> bool = 0;
  // False once the watchdog has stopped jogs on its own: this process stalled.
  [[nodiscard]] virtual auto watchdog_ok() const -> bool = 0;

protected:
  SafetyOutputs(const SafetyOutputs&) = default;
  SafetyOutputs(SafetyOutputs&&) = default;
  auto operator=(const SafetyOutputs&) -> SafetyOutputs& = default;
  auto operator=(SafetyOutputs&&) -> SafetyOutputs& = default;
};

} // namespace coffeecnc

#endif // COFFEECNC_CORE_SAFETY_OUTPUTS_HPP
