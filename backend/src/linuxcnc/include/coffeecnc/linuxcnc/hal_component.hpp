#ifndef COFFEECNC_LINUXCNC_HAL_COMPONENT_HPP
#define COFFEECNC_LINUXCNC_HAL_COMPONENT_HPP

#include <array>
#include <chrono>
#include <cstdint>
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
  bool paused_z{};         // [COFFEECNC] PAUSED_Z_MAX_MM
  bool reference_marks{};  // [COFFEECNC] REFERENCE_MARKS
};

struct PausedZInputs {
  double offset{}; // LinuxCNC machine units
  bool active{};
  bool limited{};
};

struct PausedZOutputs {
  bool enable{};
  std::int32_t counts{};
  bool clear{};
};

struct ProbeCycleOutputs {
  bool active{};
  bool lease{};
  // Route the setter to the probe input. False for the machine's own touch probe tool, which
  // triggers on its own input when it touches the setter.
  bool setter{true};
};

// The executable's own HAL component: the path to LinuxCNC that needs no NML command channel.
//
//   <name>.jog-stop         bit out  a level; drives motion.jog-stop and motion.jog-inhibit
//                                    (share/hal/coffeecnc_postgui.hal)
//   <name>.heartbeat        bit out  toggled by beat(); input of a HAL `watchdog`
//   <name>.watchdog-enable  bit out  arms that watchdog, and re-arms it after it has tripped
//   <name>.watchdog-ok      bit in   the watchdog's ok-out
//   <name>.tool-change      bit in   iocontrol.0.tool-change
//   <name>.tool-number      s32 in   iocontrol.0.tool-prep-number
//   <name>.tool-changed     bit out  iocontrol.0.tool-changed
//   <name>.spindle-rpm      float in measured spindle speed, rpm, either sign (optional)
//   <name>.spindle-at-speed bit in   the signal that drives spindle.0.at-speed (optional)
//   <name>.probe-input      bit in   the signal that drives motion.probe-input (optional)
//   <name>.reference-mark-x, -y, -z  bit in  the axis's reference mark has been found, so
//                                    homing can go to it (optional, all three together)
//   <name>.at-reference-mark-x, -y, -z  bit in  the axis stands at its reference mark now
//                                    (optional, with the three above)
//   <name>.probe-setter-select   bit out  request the fixed setter input during a probe cycle
//   <name>.probe-setter-selected bit in   actual selector state, true only for the fixed setter
//   <name>.probe-cycle-active    bit out  keep the probe interlock engaged until motion stops
//   <name>.probe-cycle-lease     bit out  worker still owns the active probe cycle
//   <name>.probe-motion-permitted bit in readback of the realtime permit driving motion.enable
//
// motion.jog-stop is level-sensitive (measured on LinuxCNC 2.9.4, and emc/motion/control.c:542):
// in every servo period in which it is high, every jog in motion is aborted, so a jog that
// starts while it is high is aborted in its first period. motion.jog-inhibit refuses a start
// outright and does not stop a running jog. A stop is therefore a level that is held, never a
// pulse: a jog start delayed in LinuxCNC's task could run after a pulse has passed.
//
// Runtime members only read or write pin memory and never block. NOT thread-safe: exactly one
// thread, the control thread, calls runtime members, and other threads get what they need from
// HalShared (hal_control.hpp), which that thread keeps current. The pin memory itself belongs to
// the HAL signal and is shared with LinuxCNC's realtime thread and with every other component on
// that signal: each pin access here is a single atomic load or store.
class HalComponent {
public:
  using Clock = std::chrono::steady_clock;
  // Shortest time jog-stop stays high and watchdog-enable keeps a level, so that the servo thread
  // (1 ms on a usual configuration) cannot miss it.
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

  // Startup only, after the machine's post-GUI HAL. Do not replace an existing signal.
  [[nodiscard]] auto connect_manual_tool_change() -> std::expected<void, std::string>;

  // The jog-stop level. `true` drives the pin high at once: every jog in motion stops, whatever
  // the NML command channel is doing, and none starts while it stays high. `false` lowers it,
  // at once or, when it rose less than level_time ago, in the beat() that follows that time.
  void hold_jog_stop(bool on);
  // The pin is driven high at this moment.
  [[nodiscard]] auto jog_stop_high() const -> bool;
  [[nodiscard]] auto jog_contact() const -> bool;

  // Call once per control loop cycle, well inside the watchdog's timeout. Toggles heartbeat,
  // lowers a released jog-stop level once it has lasted level_time, arms the watchdog and takes
  // tool-changed back once LinuxCNC has dropped its request.
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
  // For X, Y, Z: the machine's HAL has found the axis's reference mark.
  [[nodiscard]] auto reference_marks() const -> std::optional<std::array<bool, 3>>;
  // For X, Y, Z: the axis stands at its reference mark now.
  [[nodiscard]] auto at_reference_marks() const -> std::optional<std::array<bool, 3>>;
  [[nodiscard]] auto probe_setter_selected() const -> bool;
  [[nodiscard]] auto probe_motion_permitted() const -> bool;
  [[nodiscard]] auto probe_cycle_active() const -> bool;
  void set_probe_cycle(ProbeCycleOutputs outputs);
  [[nodiscard]] auto paused_z() const -> std::optional<PausedZInputs>;
  void set_paused_z(PausedZOutputs outputs);

private:
  struct Pins;
  HalComponent(int id, Pins* pins, const HalSignals& wired, std::string name);
  void disconnect_manual_tool_change() noexcept;

  int id_{-1};
  Pins* pins_{};
  HalSignals wired_{};
  std::string name_;
  std::array<std::string, 3> manual_change_signals_{};
  bool hold_{};
  Clock::time_point stop_raised_{};
  Clock::time_point enable_changed_{};
};

} // namespace coffeecnc

#endif // COFFEECNC_LINUXCNC_HAL_COMPONENT_HPP
