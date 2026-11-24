#ifndef COFFEECNC_CORE_FIXED_SETTER_HPP
#define COFFEECNC_CORE_FIXED_SETTER_HPP

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
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
  angle,
  apply_angle,
  // A Z height map (#247): Z top at every point of a grid over an XY rectangle. Writes nothing.
  height_map,
  // Sets the work origin of a measure-only result (FixedSetterRequest::measure_only).
  apply_measurement
};
enum class ProbeDirection : std::uint8_t { z_top, x_minus, x_plus, y_minus, y_plus };

// A G5x rotation in (-180, 180] degrees: the same turn as `degrees`. Applying an edge angle adds
// it to the present rotation, which could otherwise pass the panel's ±360° limit (355 + 8).
inline auto wrapped_rotation(double degrees) -> double {
  const double turn = std::remainder(degrees, 360.0);
  return turn == -180.0 ? 180.0 : turn;
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
  // For a height map: the rectangle's corners X, Y and X, Y in the active work system, the
  // grid step and the work Z to traverse at between the points. Millimetres.
  std::array<double, 4> grid_mm{};
  double step_mm{};
  double clearance_mm{};
  // Controller-issued ID: of an `angle` result for apply_angle, of a measure-only result for
  // apply_measurement.
  std::uint64_t edge_measurement{};
  // Probe and report, but write no offset: the result's `measured` can be applied afterwards.
  bool measure_only{};
};

// The grid a height map probes: columns and rows of points from the first corner to the second,
// both included, at most `step` apart. Nothing when the rectangle, step or count is unusable.
struct HeightGrid {
  int columns;
  int rows;
};
constexpr int max_height_map_points = 150;
inline auto height_grid(const std::array<double, 4>& corners, double step)
    -> std::optional<HeightGrid> {
  if (!std::ranges::all_of(corners, [](double value) { return std::isfinite(value); }) ||
      !std::isfinite(step) || step < 0.5 || step > 500)
    return std::nullopt;
  const auto count = [&](double from, double to) {
    return static_cast<int>(std::ceil(std::abs(to - from) / step - 1e-9)) + 1;
  };
  const double width = std::abs(corners[2] - corners[0]);
  const double depth = std::abs(corners[3] - corners[1]);
  if (width > 2000 || depth > 2000)
    return std::nullopt;
  const HeightGrid grid{count(corners[0], corners[2]), count(corners[1], corners[3])};
  const int points = grid.columns * grid.rows;
  if (points < 2 || points > max_height_map_points)
    return std::nullopt;
  return grid;
}

// The longest a height map may take, in seconds, and the longest one of its points can take with
// these limits, at a point. A point is a search over the whole material travel, three latch
// passes (back-off at twice the search feed, up to 2 s for the release, then twice the back-off
// at the latch feed), the retract over the travel at twice the search feed with its 2 s release,
// and 10 s for the lift and the feed and interlock blocks. The panel computes the same
// (ui/src/height-map.ts) for its answer deadline.
constexpr double max_height_map_seconds = 7200;
inline auto height_map_point_seconds(const FixedSetterLimits& limits) -> double {
  if (!(limits.feed_mm_min > 0) || !(limits.latch_feed_mm_min > 0))
    return std::numeric_limits<double>::infinity();
  const double fast = 2 * limits.feed_mm_min;
  const double search = limits.material_travel_mm / limits.feed_mm_min * 60;
  const double pass = (limits.latch_backoff_mm / fast * 60) + 2 +
                      (2 * limits.latch_backoff_mm / limits.latch_feed_mm_min * 60);
  const double retract = (limits.material_travel_mm / fast * 60) + 2;
  return search + (3 * pass) + retract + 10;
}
// Every point; the XY traverses at `xy_rapid_mm_min` (`first_mm` from the start to the first
// point, then at most one `step_mm` to each next one); and the first feed down from the start to
// the clearance (`descent_mm`).
struct HeightMapTravel {
  int points;
  double step_mm;
  double first_mm;
  double descent_mm;
  double xy_rapid_mm_min;
};
inline auto height_map_seconds(const FixedSetterLimits& limits, const HeightMapTravel& travel)
    -> double {
  if (!(travel.xy_rapid_mm_min > 0))
    return std::numeric_limits<double>::infinity();
  const double xy_mm = std::max(0.0, travel.first_mm) + (std::max(0, travel.points - 1) * travel.step_mm);
  return (travel.points * height_map_point_seconds(limits)) + (xy_mm / travel.xy_rapid_mm_min * 60) +
         (std::max(0.0, travel.descent_mm) / (2 * limits.feed_mm_min) * 60);
}

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
  // After a bore or boss: its diameter, mm (the chord through the centre).
  std::optional<double> center_diameter{};
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
  // After a height map: each point's top, in the work system and in machine coordinates, mm;
  // row by row from the first corner. A map that stopped after some points is partial: it keeps
  // the points measured so far, and `partial` says why it stopped.
  struct HeightMap {
    int wcs{};
    int columns{};
    int rows{};
    double rotation_deg{}; // the work system's XY rotation
    struct Point {
      std::array<double, 3> work;
      std::array<double, 3> machine;
      bool measured{};
    };
    std::vector<Point> points;
    std::optional<std::string> partial{};
  };
  std::optional<HeightMap> height_map{};
  // A point on a probed surface: the ball-centre contact moved one ball radius along the travel.
  struct ProbedPoint {
    ProbeDirection direction; // the probe's travel
    std::array<double, 3> work;    // in the active work system at the time of the probe, mm
    std::array<double, 3> machine; // mm
  };
  // After a measure-only probe: what it found, with nothing written.
  struct Measured {
    std::uint64_t id{}; // for apply_measurement; 0 when there is nothing to apply (edge angle)
    int wcs{};
    std::string axes; // the axes applying sets: "X", "Y", "Z" or "XY"; empty for an edge angle
    // The point applying makes the origin (centre, corner, face or top), in the work system
    // at the time of the probe: only its measured axes. In machine coordinates: all three, an
    // axis not measured at the start's value, Z of an XY feature at the probing depth.
    std::array<std::optional<double>, 3> work{};
    std::optional<std::array<double, 3>> machine{};
    std::vector<ProbedPoint> faces;
  };
  std::optional<Measured> measured{};
};

} // namespace coffeecnc

#endif
