#ifndef COFFEECNC_CORE_DIAGNOSTICS_HPP
#define COFFEECNC_CORE_DIAGNOSTICS_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace coffeecnc {

// The HAL tools the diagnostics view starts, each as a program of its own.
enum class HalTool : std::uint8_t { halshow, halmeter, halscope };
inline constexpr std::array hal_tools{HalTool::halshow, HalTool::halmeter, HalTool::halscope};
// The program name, which is also the wire name.
[[nodiscard]] constexpr auto hal_tool_name(HalTool tool) -> std::string_view {
  switch (tool) {
  case HalTool::halshow:
    return "halshow";
  case HalTool::halmeter:
    return "halmeter";
  case HalTool::halscope:
    return "halscope";
  }
  return "halshow";
}

// What one row of the diagnostics view is. The standard rows read LinuxCNC's own pins; a
// `configured` row reads what [COFFEECNC] DIAGNOSTIC names.
enum class DiagnosticRole : std::uint8_t {
  min_limit,         // joint.N.neg-lim-sw-in
  max_limit,         // joint.N.pos-lim-sw-in
  home_switch,       // joint.N.home-sw-in
  amp_fault,         // joint.N.amp-fault-in
  estop_chain,       // iocontrol.0.emc-enable-in: true while the external chain is closed
  estop_request,     // iocontrol.0.user-enable-out: true while LinuxCNC is out of E-stop
  probe,             // motion.probe-input
  spindle_on,        // spindle.0.on
  spindle_at_speed,  // spindle.0.at-speed
  spindle_speed_out, // spindle.0.speed-out, rpm
  spindle_speed_in,  // spindle.0.speed-in, revolutions per second
  spindle_fault,     // spindle.0.amp-fault-in
  configured,
};

// [COFFEECNC] DIAGNOSTIC = <label> | <HAL signal or pin>, each occurrence in order.
struct DiagnosticSignal {
  std::string label;
  std::string name;
  friend auto operator==(const DiagnosticSignal&, const DiagnosticSignal&) -> bool = default;
};

// Limits of [COFFEECNC] DIAGNOSTIC. A label is counted in characters (UTF-8 code points); a
// name is a HAL name (HAL_NAME_LEN).
inline constexpr std::size_t max_diagnostics = 16;
inline constexpr std::size_t max_diagnostic_label = 32;
inline constexpr std::size_t max_hal_name = 47;

// One row: what it shows and where it is read.
struct DiagnosticInput {
  DiagnosticRole role{DiagnosticRole::configured};
  std::optional<int> joint{}; // the joint rows
  std::string label{};        // configured rows
  std::string name;           // a HAL pin, or for a configured row a signal or pin
  friend auto operator==(const DiagnosticInput&, const DiagnosticInput&) -> bool = default;
};

enum class DiagnosticState : std::uint8_t {
  missing,  // HAL has no pin or signal of that name
  unwired,  // a pin linked to no signal: it reads its own value, which `setp` sets
  undriven, // a signal, or a pin's signal, that nothing writes: it reads what `sets` set
  live,     // something writes it
};

// A bit, or a number (float, s32 or u32); nothing while missing.
using DiagnosticValue = std::variant<std::monostate, bool, double>;

struct DiagnosticReading {
  DiagnosticInput input;
  DiagnosticState state{DiagnosticState::missing};
  DiagnosticValue value{};
  bool signal{}; // `input.name` is a signal, not a pin
  friend auto operator==(const DiagnosticReading&, const DiagnosticReading&) -> bool = default;
};

struct HalToolState {
  HalTool tool{HalTool::halshow};
  bool available{}; // the program is installed
  bool open{};      // CoffeeCNC started it and it has not ended
  friend auto operator==(const HalToolState&, const HalToolState&) -> bool = default;
};

// The protocol `diagnostics` message: every row in order and the HAL tools.
struct Diagnostics {
  std::vector<DiagnosticReading> inputs;
  std::vector<HalToolState> tools;
  friend auto operator==(const Diagnostics&, const Diagnostics&) -> bool = default;
};

} // namespace coffeecnc

#endif // COFFEECNC_CORE_DIAGNOSTICS_HPP
