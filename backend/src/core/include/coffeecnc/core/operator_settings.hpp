#ifndef COFFEECNC_CORE_OPERATOR_SETTINGS_HPP
#define COFFEECNC_CORE_OPERATOR_SETTINGS_HPP

#include <coffeecnc/core/tool_models.hpp>

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace coffeecnc {

enum class StartupUnits : std::uint8_t { machine, mm, inch };
enum class ProgramLayout : std::uint8_t { path, text, split };
enum class PanelTheme : std::uint8_t { light, dark };

// A machine position the operator saved in Manual; the panel moves there only on request.
struct SavedPosition {
  std::string name; // not blank, at most 64 UTF-8 bytes, no control characters
  std::array<double, 3> position{}; // machine X, Y, Z in mm
  friend auto operator==(const SavedPosition&, const SavedPosition&) -> bool = default;
};

// Presentation and startup presets only. Never sent to the controller.
struct OperatorPreferences {
  StartupUnits startup_units{StartupUnits::machine};
  std::vector<int> enabled_wcs{1, 2, 3, 4, 5, 6, 7, 8, 9};
  double default_spindle_rpm{};
  double default_jog_velocity{600}; // mm/min
  double tool_reference_height{}; // mm above work Z zero; touch-off dialog preset
  ProgramLayout program_layout{ProgramLayout::path};
  PanelTheme theme{PanelTheme::light};
  // Programs opened most recently first, absolute paths on the machine; at most eight.
  std::vector<std::string> recent_programs;
  // At most sixteen, in the order the operator saved them.
  std::vector<SavedPosition> saved_positions;
  // Machine Z in mm that Go to lifts to before X and Y move; unset: Z max less 0.5 mm. The
  // panel keeps it inside the Z soft limits, which this document does not know.
  std::optional<double> go_to_safe_z;
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
