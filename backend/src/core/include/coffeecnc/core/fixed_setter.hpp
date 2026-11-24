#ifndef COFFEECNC_CORE_FIXED_SETTER_HPP
#define COFFEECNC_CORE_FIXED_SETTER_HPP

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <vector>

namespace coffeecnc {

struct MaterialProbeGeometry {
  double reach_mm{10};
  double margin_mm{3};
  double clearance_mm{5};
  double depth_mm{2};
  friend auto operator==(const MaterialProbeGeometry&, const MaterialProbeGeometry&) -> bool = default;
};

struct FixedSetterLimits {
  double travel_mm;
  double feed_mm_min;
  int reference_tool{98};
  double xy_tolerance_mm{};
  double clearance_mm{}; // lift above the reference latch, or zero to use the taught Z
  double latch_feed_mm_min{};
  double latch_backoff_mm{};
  double material_travel_mm{20};
  MaterialProbeGeometry material_geometry{};
  double check_tolerance_mm{0.1};
  friend auto operator==(const FixedSetterLimits&, const FixedSetterLimits&) -> bool = default;
};

enum class SetterAction : std::uint8_t {
  status,
  touch_off,
  check,
  material,
  corner,
  inside_corner,
  pocket,
  slot,
  ridge,
  ball,
  bore,
  boss,
  // A bore or boss from three contacts at chosen angles: also a partial arc (#247).
  bore_arc,
  boss_arc,
  angle,
  apply_angle
};
enum class ProbeDirection : std::uint8_t { z_top, x_minus, x_plus, y_minus, y_plus };

// A G5x rotation in (-180, 180] degrees: the same turn as `degrees`. Applying an edge angle adds
// it to the present rotation, which could otherwise pass the panel's ±360° limit (355 + 8).
inline auto wrapped_rotation(double degrees) -> double {
  const double turn = std::remainder(degrees, 360.0);
  return turn == -180.0 ? 180.0 : turn;
}

// Three probing directions on a circle, degrees counterclockwise from the work +X axis: the
// smallest angle between two of them and the arc they cover (360° less the largest gap).
struct ArcSpread {
  double closest_deg;
  double span_deg;
};
inline auto arc_spread(std::array<double, 3> degrees) -> ArcSpread {
  std::ranges::transform(degrees, degrees.begin(), [](double angle) {
    return std::fmod(std::fmod(angle, 360.0) + 360.0, 360.0);
  });
  std::ranges::sort(degrees);
  const std::array gaps{degrees[1] - degrees[0], degrees[2] - degrees[1],
                        360.0 - degrees[2] + degrees[0]};
  return {std::ranges::min(gaps), 360.0 - std::ranges::max(gaps)};
}
// Fewer degrees than these between two directions, or a smaller arc, give a poor centre.
constexpr double arc_min_separation_deg = 10;
constexpr double arc_min_span_deg = 30;
// The measured contacts must cover at least this arc around the centre found from them; a
// smaller arc means they lie almost on a line.
constexpr double arc_min_measured_span_deg = 20;

struct Circle {
  std::array<double, 2> centre;
  double radius;
};
// The circle through three points; nothing when they lie on a line (or nearly: the circle would
// be more than 10 m across).
inline auto circle_through(const std::array<std::array<double, 2>, 3>& points)
    -> std::optional<Circle> {
  // Relative to the first point, for precision.
  const double bx = points[1][0] - points[0][0];
  const double by = points[1][1] - points[0][1];
  const double cx = points[2][0] - points[0][0];
  const double cy = points[2][1] - points[0][1];
  const double d = 2 * (bx * cy - by * cx);
  if (!std::isfinite(d) || d == 0)
    return std::nullopt;
  const double b2 = bx * bx + by * by;
  const double c2 = cx * cx + cy * cy;
  const double ux = (cy * b2 - by * c2) / d;
  const double uy = (bx * c2 - cx * b2) / d;
  const double radius = std::hypot(ux, uy);
  if (!std::isfinite(radius) || radius > 5000)
    return std::nullopt;
  return Circle{{points[0][0] + ux, points[0][1] + uy}, radius};
}
// The arc the points cover seen from `centre`, degrees.
inline auto measured_span(const std::array<std::array<double, 2>, 3>& points,
                          std::array<double, 2> centre) -> double {
  std::array<double, 3> degrees{};
  std::ranges::transform(points, degrees.begin(), [&](const std::array<double, 2>& point) {
    return std::atan2(point[1] - centre[1], point[0] - centre[0]) * 180 / std::numbers::pi;
  });
  return arc_spread(degrees).span_deg;
}

constexpr auto is_material_probe(SetterAction action) -> bool {
  return action != SetterAction::status && action != SetterAction::touch_off &&
         action != SetterAction::check;
}

struct FixedSetterRequest {
  SetterAction action;
  int tool{};
  std::array<double, 3> start{};
  // The travel of a material probe; for a corner, the X travel (x_minus or x_plus).
  ProbeDirection direction{ProbeDirection::z_top};
  // For a corner, the Y travel (y_minus or y_plus).
  ProbeDirection y_direction{ProbeDirection::y_plus};
  // For a ball calibration, the width in X of the gauge the probe starts over; for a boss, its
  // approximate diameter; for a ridge, the approximate width; for an edge angle, how far apart
  // along the edge it probes. Millimetres.
  double gauge_width_mm{};
  std::uint64_t edge_measurement{}; // controller-issued ID for apply_angle
  // For bore_arc and boss_arc: the three probing directions, degrees counterclockwise from the
  // active work system's +X axis. A bore is probed outward from the start along them; a boss
  // inward from beyond `gauge_width_mm` / 2 along them.
  std::array<double, 3> angles_deg{};
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
  std::optional<double> check_tolerance_mm{}; // present for a successful read-only check
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
  // After a bore or boss: its diameter, mm (the chord through the centre). After a three-point
  // bore or boss: the diameter of the circle through the contacts.
  std::optional<double> center_diameter{};
  // After a three-point bore or boss: the arc the contacts cover around the centre, degrees.
  std::optional<double> arc_span_deg{};
  // After an edge angle: how far the edge turns from the active work system's axis, degrees,
  // counterclockwise seen from above. In a rotated work system that is the turn still left.
  // After applying it: the work rotation written.
  std::optional<double> edge_angle_deg{};
  struct EdgeRotation {
    std::uint64_t id;
    int wcs;
    // The G5x rotation applying writes: the present rotation plus the measured angle.
    double rotation_deg{};
  };
  // Present on a measurement that can be confirmed as a G5x rotation.
  std::optional<EdgeRotation> edge_rotation{};
  // Physical widths from the probed faces, in mm. A slot or ridge has only one measured axis.
  std::array<std::optional<double>, 2> feature_size{};
};

} // namespace coffeecnc

#endif
