#ifndef COFFEECNC_LINUXCNC_HAL_COMPONENT_HPP
#define COFFEECNC_LINUXCNC_HAL_COMPONENT_HPP

#include <chrono>
#include <expected>
#include <optional>
#include <string>

namespace coffeecnc {

// Which of the component's optional input pins the machine's HAL files connect. An input pin
// reads 0 while nothing drives it, so only a pin declared here is ever reported as a reading.
struct HalSignals {
  bool spindle_rpm{};      // [COFFEECNC] SPINDLE_RPM_SIGNAL
  bool spindle_at_speed{}; // [COFFEECNC] SPINDLE_AT_SPEED_SIGNAL
  bool probe{};            // [COFFEECNC] PROBE_SIGNAL
};

// The executable's own HAL component: the path to LinuxCNC that needs no NML command channel.
//
//   <name>.jog-stop         bit out  pulsed; net to motion.jog-stop (acts on its rising edge)
//   <name>.heartbeat        bit out  toggled by beat(); input of a HAL `watchdog`
//   <name>.watchdog-enable  bit out  arms that watchdog, and re-arms it after it has tripped
//   <name>.watchdog-ok      bit in   the watchdog's ok-out
//   <name>.tool-change      bit in   iocontrol.0.tool-change
//   <name>.tool-number      s32 in   iocontrol.0.tool-prep-number
//   <name>.tool-changed     bit out  iocontrol.0.tool-changed
//   <name>.spindle-rpm      float in measured spindle speed, rpm, either sign (optional)
//   <name>.spindle-at-speed bit in   the signal that drives spindle.0.at-speed (optional)
//   <name>.probe-input      bit in   the signal that drives motion.probe-input (optional)
//
// Every member only reads or writes pin memory and never blocks. NOT thread-safe: one thread,
// the control loop, calls all of them.
class HalComponent {
public:
  using Clock = std::chrono::steady_clock;
  // Shortest time jog-stop and watchdog-enable keep a level, so that the servo thread (1 ms on a
  // usual configuration) cannot miss an edge.
  static constexpr auto level_time = std::chrono::milliseconds{5};

  // Creates the component with its pins and marks it ready. One per process and name. Every pin
  // exists whatever `wired` says; `wired` decides which optional inputs are reported.
  [[nodiscard]] static auto create(const std::string& name = "coffeecnc",
                                   const HalSignals& wired = {})
      -> std::expected<HalComponent, std::string>;

  HalComponent(HalComponent&& other) noexcept;
  auto operator=(HalComponent&& other) noexcept -> HalComponent&;
  HalComponent(const HalComponent&) = delete;
  auto operator=(const HalComponent&) -> HalComponent& = delete;
  ~HalComponent();

  // Stops every jog in motion, whatever the NML command channel is doing. Raises jog-stop at
  // once, or as soon as beat() has finished the previous pulse; beat() lowers it again.
  void request_jog_stop();
  // A requested stop has not been raised yet, or its pulse is not finished.
  [[nodiscard]] auto jog_stop_busy() const -> bool;

  // Call once per control loop cycle, well inside the watchdog's timeout. Toggles heartbeat,
  // finishes jog-stop pulses, arms the watchdog and takes tool-changed back once LinuxCNC has
  // seen it.
  void beat();
  [[nodiscard]] auto watchdog_ok() const -> bool;

  // The tool LinuxCNC is waiting for the operator to load, until it is confirmed.
  [[nodiscard]] auto tool_change_request() const -> std::optional<int>;
  // The operator has loaded the requested tool. Ignored while no change is requested.
  void confirm_tool_changed();

  // Optional signals. Nothing when the machine does not declare the pin as connected, and for a
  // speed that is not a finite number.
  // Measured spindle speed as a magnitude, rpm.
  [[nodiscard]] auto spindle_rpm() const -> std::optional<double>;
  [[nodiscard]] auto spindle_at_speed() const -> std::optional<bool>;
  [[nodiscard]] auto probe_input() const -> std::optional<bool>;

private:
  struct Pins;
  HalComponent(int id, Pins* pins, const HalSignals& wired);

  int id_{-1};
  Pins* pins_{};
  HalSignals wired_{};
  bool stop_wanted_{};
  Clock::time_point stop_changed_{};
  Clock::time_point enable_changed_{};
};

} // namespace coffeecnc

#endif // COFFEECNC_LINUXCNC_HAL_COMPONENT_HPP
