#ifndef COFFEECNC_CORE_TOOL_MODELS_HPP
#define COFFEECNC_CORE_TOOL_MODELS_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace coffeecnc {

// The visual tool models the panel draws: seven family defaults and the models the operator
// assigned to rows of the tool table. Lengths are millimetres, the angle is degrees, whatever
// unit is displayed. None of it reaches LinuxCNC. Field names on the wire are those of
// `docs/design/native-tool-model-storage.md`.

// In wire order: end-mill, ball-mill, drill, spot-drill, chamfer, face-mill, generic.
enum class ToolFamily : std::uint8_t {
  end_mill,
  ball_mill,
  drill,
  spot_drill,
  chamfer,
  face_mill,
  generic
};
inline constexpr std::size_t tool_family_count = 7;
inline constexpr std::array<ToolFamily, tool_family_count> tool_families{
    ToolFamily::end_mill,  ToolFamily::ball_mill, ToolFamily::drill,  ToolFamily::spot_drill,
    ToolFamily::chamfer,   ToolFamily::face_mill, ToolFamily::generic};

struct ToolDimensions {
  double diameter{};
  double cutting_length{}; // cuttingLength
  double shank_diameter{}; // shankDiameter
  double overall_length{}; // overallLength
  double stickout{};       // tool tip to holder face
  double tip_angle{};      // tipAngle, included angle
  friend auto operator==(const ToolDimensions&, const ToolDimensions&) -> bool = default;
};

// By family, in the order of `tool_families`.
using ToolDefaults = std::array<ToolDimensions, tool_family_count>;

// The dimensions an assignment sets itself; the others come from its family. The cutting
// diameter is the tool table's and cannot be overridden.
struct ToolOverrides {
  std::optional<double> cutting_length;
  std::optional<double> shank_diameter;
  std::optional<double> overall_length;
  std::optional<double> stickout;
  std::optional<double> tip_angle;
  [[nodiscard]] auto empty() const -> bool {
    return !cutting_length && !shank_diameter && !overall_length && !stickout && !tip_angle;
  }
  friend auto operator==(const ToolOverrides&, const ToolOverrides&) -> bool = default;
};

// The tool table row a model was assigned to, as `tool_table` reported it: the diameter in
// millimetres, the very number that was sent. The Z offset is not part of it.
struct ToolBinding {
  int number{};
  int pocket{};
  double diameter{};
  std::string comment;
  friend auto operator==(const ToolBinding&, const ToolBinding&) -> bool = default;
};

struct ToolModelAssignment {
  ToolBinding tool;
  ToolFamily family{ToolFamily::generic};
  ToolOverrides overrides;
  friend auto operator==(const ToolModelAssignment&, const ToolModelAssignment&) -> bool = default;
};

// Document version 1.
struct ToolModelDocument {
  ToolDefaults defaults{};
  // Unique tool numbers, in rising order.
  std::vector<ToolModelAssignment> assignments;
  friend auto operator==(const ToolModelDocument&, const ToolModelDocument&) -> bool = default;
};

enum class ToolModelAvailability : std::uint8_t {
  available,    // saved for this machine
  session_only, // no machine identity is configured: nothing is saved
  unavailable,  // configured and not usable; `reason` says why
};

// The `tool_models` message: everything saved for the machine, or why there is nothing.
// Available: a machine ID and a document. Otherwise revision 0 and no document.
struct ToolModels {
  std::optional<std::string> machine_id;
  std::uint64_t revision{};
  ToolModelAvailability availability{ToolModelAvailability::session_only};
  std::string reason;
  std::optional<ToolModelDocument> document;
  friend auto operator==(const ToolModels&, const ToolModels&) -> bool = default;
};

// Replaces all seven family defaults; assignments stay.
struct SetToolModelDefaults {
  std::string machine_id;
  std::uint64_t base_revision{};
  ToolDefaults defaults{};
};
// Replaces the assignment of tool `tool.number`, or adds it. Without a family it removes it.
struct SetToolModelAssignment {
  std::string machine_id;
  std::uint64_t base_revision{};
  ToolBinding tool;
  std::optional<ToolFamily> family;
  ToolOverrides overrides;
};
using ToolModelCommand = std::variant<SetToolModelDefaults, SetToolModelAssignment>;

// [COFFEECNC] MACHINE_ID: `[A-Za-z0-9][A-Za-z0-9_-]{0,63}`, compared byte for byte.
[[nodiscard]] constexpr auto valid_machine_id(std::string_view id) -> bool {
  constexpr std::size_t max_bytes = 64;
  const auto alphanumeric = [](char c) -> bool {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
  };
  return !id.empty() && id.size() <= max_bytes && alphanumeric(id.front()) &&
         std::ranges::all_of(id, [&](char c) -> bool {
           return alphanumeric(c) || c == '_' || c == '-';
         });
}

} // namespace coffeecnc

#endif // COFFEECNC_CORE_TOOL_MODELS_HPP
