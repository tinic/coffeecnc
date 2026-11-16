#ifndef COFFEECNC_CORE_OPERATOR_SETTINGS_HPP
#define COFFEECNC_CORE_OPERATOR_SETTINGS_HPP

#include <coffeecnc/core/tool_models.hpp>

namespace coffeecnc {

enum class StartupUnits : std::uint8_t { machine, mm, inch };

// Presentation and startup presets only. Never sent to the controller.
struct OperatorPreferences {
  StartupUnits startup_units{StartupUnits::machine};
  std::vector<int> enabled_wcs{1, 2, 3, 4, 5, 6, 7, 8, 9};
  double default_spindle_rpm{};
  double default_jog_velocity{600}; // mm/min
  friend auto operator==(const OperatorPreferences&, const OperatorPreferences&) -> bool = default;
};

struct OperatorSettings {
  std::optional<std::string> machine_id;
  std::uint64_t revision{};
  ToolModelAvailability availability{ToolModelAvailability::session_only};
  std::string reason;
  std::optional<OperatorPreferences> document;
  friend auto operator==(const OperatorSettings&, const OperatorSettings&) -> bool = default;
};

struct SetOperatorSettings {
  std::string machine_id;
  std::uint64_t base_revision{};
  OperatorPreferences document;
};

} // namespace coffeecnc
#endif
