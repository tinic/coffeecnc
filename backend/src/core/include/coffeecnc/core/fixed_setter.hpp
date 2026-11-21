#ifndef COFFEECNC_CORE_FIXED_SETTER_HPP
#define COFFEECNC_CORE_FIXED_SETTER_HPP

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace coffeecnc {

struct FixedSetterLimits {
  double travel_mm;
  double feed_mm_min;
  int reference_tool{98};
  double xy_tolerance_mm{};
  double clearance_mm{}; // lift above the reference latch, or zero to use the taught Z
  double latch_feed_mm_min{};
  double latch_backoff_mm{};
  double material_travel_mm{20};
  friend auto operator==(const FixedSetterLimits&, const FixedSetterLimits&) -> bool = default;
};

enum class SetterAction : std::uint8_t {
  status,
  touch_off,
  material,
  corner,
  ball,
  bore,
  boss,
  angle,
  apply_angle
};
enum class ProbeDirection : std::uint8_t { z_top, x_minus, x_plus, y_minus, y_plus };

struct FixedSetterRequest {
  SetterAction action;
  int tool{};
  std::array<double, 3> start{};
  // The travel of a material probe; for a corner, the X travel (x_minus or x_plus).
  ProbeDirection direction{ProbeDirection::z_top};
  // For a corner, the Y travel (y_minus or y_plus).
  ProbeDirection y_direction{ProbeDirection::y_plus};
  // For a ball calibration, the width in X of the gauge the probe starts over; for a boss, its
  // approximate diameter; for an edge angle, how far apart along the edge it probes. Millimetres.
  double gauge_width_mm{};
  std::uint64_t edge_measurement{}; // controller-issued ID for apply_angle
};

struct SetterEpoch {
  std::uint64_t page;
  std::uint64_t datum;
  std::uint64_t tool;
  std::uint64_t stop;
  friend auto operator==(const SetterEpoch&, const SetterEpoch&) -> bool = default;
};

struct SetterMeasurement {
  bool calibration;
  int tool;
  std::array<double, 3> contact;
  double value; // calibration plane or measured tool length in mm
  double previous_length;
  double stopping_distance; // downward travel from the latch to settled machine Z in mm
};

struct SetterState {
  std::optional<std::array<double, 3>> calibration; // XY and effective trigger plane Z in mm
  std::optional<SetterMeasurement> measurement;
  // Tools with a valid touch-off, the reference tool included: against this calibration, or
  // without one the saved marks the tool table still matches.
  std::vector<int> measured_tools;
  struct Material {
    int wcs;
    char axis;
    bool corner{}; // X and Y were both set
  };
  std::optional<Material> material{};
  // After a ball calibration: the effective ball diameter written to the tool table, mm.
  std::optional<double> ball_diameter{};
  // After a bore or boss: its diameter, mm (the chord through the centre).
  std::optional<double> center_diameter{};
  // After an edge angle: how far the edge turns from the machine axis, degrees, counterclockwise
  // seen from above.
  std::optional<double> edge_angle_deg{};
  struct EdgeRotation {
    std::uint64_t id;
    int wcs;
  };
  // Present on a measurement that can be confirmed as a G5x rotation.
  std::optional<EdgeRotation> edge_rotation{};
};

} // namespace coffeecnc

#endif
