#ifndef COFFEECNC_CORE_FIXED_SETTER_HPP
#define COFFEECNC_CORE_FIXED_SETTER_HPP

#include <coffeecnc/core/message_key.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
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
  // A bore or boss from three contacts at chosen angles: also a partial arc (#247).
  bore_arc,
  boss_arc,
  angle,
  apply_angle,
  // Stylus runout (#247): finds the centre of a ring or pin of known diameter, then measures it
  // in +X, -X, +Y and -Y and keeps the trigger radius correction per axis for the touch probe;
  // clear_runout forgets it.
  runout,
  clear_runout,
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

// A probe search line in machine coordinates: the work axis it measures, its sign along that
// axis, and its direction in machine axes.
struct ProbeLine {
  std::size_t axis;
  double sign;
  std::array<double, 3> forward; // Signed direction in machine axes.

  [[nodiscard]] auto distance(const std::array<double, 3>& from,
                              const std::array<double, 3>& to) const -> double {
    return (to[0] - from[0]) * forward[0] + (to[1] - from[1]) * forward[1] +
           (to[2] - from[2]) * forward[2];
  }

  [[nodiscard]] auto advance(std::array<double, 3> point, double travel) const
      -> std::array<double, 3> {
    for (std::size_t i = 0; i < point.size(); ++i)
      point.at(i) += travel * forward.at(i);
    return point;
  }

  [[nodiscard]] auto word() const -> char { return "XYZ"[axis]; }
};

// The line along `direction` of the work axes, in a work system turned by `rotation_xy` degrees.
inline auto probe_line(ProbeDirection direction, double rotation_xy) -> ProbeLine {
  const double angle = rotation_xy * std::numbers::pi / 180;
  const double sign =
      direction == ProbeDirection::x_plus || direction == ProbeDirection::y_plus ? 1.0 : -1.0;
  switch (direction) {
  case ProbeDirection::x_minus:
  case ProbeDirection::x_plus:
    return {0, sign, {sign * std::cos(angle), sign * std::sin(angle), 0}};
  case ProbeDirection::y_minus:
  case ProbeDirection::y_plus:
    return {1, sign, {-sign * std::sin(angle), sign * std::cos(angle), 0}};
  case ProbeDirection::z_top:
    break;
  }
  return {2, -1, {0, 0, -1}};
}

// The line a probe move searches along in machine coordinates, in a work system turned by
// `rotation_xy`: along `material` of the work axes (Z top without one), or with a heading, along
// the work +X axis turned by `heading_deg` (the contacts of a three-point bore or boss). The
// runout compensation takes the same line as the search, so a contact is corrected along its
// actual travel.
inline auto search_line(std::optional<ProbeDirection> material, std::optional<double> heading_deg,
                        double rotation_xy) -> ProbeLine {
  if (heading_deg)
    return probe_line(ProbeDirection::x_plus, rotation_xy + *heading_deg);
  return probe_line(material.value_or(ProbeDirection::z_top), rotation_xy);
}

// The trigger radius correction of the touch probe (#247): per machine axis, how far the probed
// surface lies beyond where the ball radius of the tool table puts it, mm. Measured on a ring or
// pin of known diameter after the ball calibration, against that table diameter.
// Only the radius is kept. The ball's eccentricity from the spindle axis turns with the spindle,
// and no configured machine can orient the spindle (no M19): after a spindle run, a tool change
// or a turn by hand, a stored eccentricity points the wrong way and doubles the error.
struct ProbeRunout {
  int tool{};
  double table_diameter_mm{}; // the ball diameter the compensation is relative to
  std::array<double, 2> radius_mm{}; // X, Y
  friend auto operator==(const ProbeRunout&, const ProbeRunout&) -> bool = default;
  // What a ball-centre contact of a search along the machine XY unit direction (ux, uy) moves
  // by: the radius correction along the travel, between the axes by the squared direction
  // cosines. A contact moved so gives the surface with the table radius.
  [[nodiscard]] auto correction(double ux, double uy) const -> std::array<double, 2> {
    const double along = radius_mm[0] * ux * ux + radius_mm[1] * uy * uy;
    return {along * ux, along * uy};
  }
};
// More than this per axis is not the trigger radius left after a ball calibration: a wrong
// gauge diameter (19.00 entered for 19.05 is 0.025 mm) or no ball calibration.
constexpr double max_runout_mm = 0.05;
// Before any motion: whether the material search travel reaches the gauge. In a ring the ball
// searches from near its centre to the wall; beside a pin it starts `outside` beyond the
// surface, as for a boss (reach, margin and the ball radius).
constexpr auto runout_travel_reaches(bool pin, double diameter_mm, double outside_mm,
                                     double ball_radius_mm, double travel_mm) -> bool {
  return pin ? travel_mm >= outside_mm + ball_radius_mm : travel_mm >= diameter_mm / 2;
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
// Fewer degrees than these between two directions, or a smaller arc, give a poor centre. With
// 2.5 µm of radial noise per contact the 95th-percentile centre error is 0.40 mm over a 20° arc,
// 0.10 mm over 40°, 0.021 mm over 90° and 0.005 mm over 240° (#318 review).
constexpr double arc_min_separation_deg = 10;
constexpr double arc_min_span_deg = 90;
// The measured contacts must cover at least this arc around the centre found from them; a
// smaller arc means they lie almost on a line.
constexpr double arc_min_measured_span_deg = 90;
// The largest estimated centre uncertainty (95 %) a three-point cycle writes, mm.
constexpr double arc_max_centre_uncertainty_mm = 0.02;
// The radial error of one contact (one standard deviation) estimated from the range of its three
// latch passes: the expected range of three normal samples is 1.693 standard deviations. A
// contact is never taken as better than 1 µm.
constexpr double latch_range_per_sigma = 1.693;
constexpr double arc_min_contact_sigma_mm = 0.001;
inline auto contact_sigma(double latch_spread_mm) -> double {
  return std::max(latch_spread_mm / latch_range_per_sigma, arc_min_contact_sigma_mm);
}
// The 95 % uncertainty of the centre of the circle through `points`, mm, when each point has an
// independent radial error with standard deviation `sigma_mm`. A radial error d_i at the point
// in direction n_i from the centre moves the centre by dc and the radius by dR where
// n_i · dc + dR = d_i: the centre is the first two rows of the inverse of [n_i 1] applied to d.
// Nothing when the points have no circle or the system is singular.
inline auto arc_centre_uncertainty(const std::array<std::array<double, 2>, 3>& points,
                                   std::array<double, 2> centre,
                                   const std::array<double, 3>& sigma_mm) -> std::optional<double> {
  std::array<std::array<double, 2>, 3> n{};
  for (std::size_t i = 0; i < points.size(); ++i) {
    const double dx = points.at(i)[0] - centre[0];
    const double dy = points.at(i)[1] - centre[1];
    const double length = std::hypot(dx, dy);
    if (!(length > 0))
      return std::nullopt;
    n.at(i) = {dx / length, dy / length};
  }
  // Rows of the matrix are (n_ix, n_iy, 1); its inverse by cofactors.
  const auto& [a, b, c] = n;
  const double det = a[0] * (b[1] - c[1]) - a[1] * (b[0] - c[0]) + (b[0] * c[1] - c[0] * b[1]);
  if (!std::isfinite(det) || std::abs(det) < 1e-12)
    return std::nullopt;
  // The first two rows of the inverse: d(centre x) and d(centre y) per unit radial error.
  const std::array<std::array<double, 3>, 2> gain{
      std::array{(b[1] - c[1]) / det, (c[1] - a[1]) / det, (a[1] - b[1]) / det},
      std::array{(c[0] - b[0]) / det, (a[0] - c[0]) / det, (b[0] - a[0]) / det}};
  double variance = 0;
  for (std::size_t i = 0; i < sigma_mm.size(); ++i)
    variance += (gain[0].at(i) * gain[0].at(i) + gain[1].at(i) * gain[1].at(i)) *
                sigma_mm.at(i) * sigma_mm.at(i);
  const double uncertainty = 1.96 * std::sqrt(variance);
  return std::isfinite(uncertainty) ? std::optional{uncertainty} : std::nullopt;
}
// A three-point boss whose circle differs from the entered approximate diameter by more than
// this (0.5 mm or 10 %, whichever is larger) touched something other than the boss.
inline auto boss_diameter_plausible(double measured_mm, double entered_mm) -> bool {
  return std::abs(measured_mm - entered_mm) <= std::max(0.5, 0.1 * entered_mm);
}

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
struct CircleFit {
  Circle circle;
  double max_residual_mm;
};
constexpr double circle_check_limit_mm = 0.02;
// Each bad contact must retain at least this fraction of its radial error in its own residual.
constexpr double circle_min_residual_gain = 0.2;
constexpr double circle_max_centre_error_mm = 0.05;

struct CircleQuality {
  double min_residual_gain;
  double centre_uncertainty_mm;
  // Worst first-order centre shift from one contact error at the residual limit.
  double single_contact_centre_error_mm;
};

[[nodiscard]] auto planned_circle_quality(std::span<const double> headings)
    -> std::optional<CircleQuality>;
// Sensitivity and 95 % centre uncertainty of the fitted circle, using every contact's latch.
[[nodiscard]] auto measured_circle_quality(std::span<const std::array<double, 2>> points,
                                           std::array<double, 2> centre,
                                           std::span<const double> latch_spreads)
    -> std::optional<CircleQuality>;

// Orthogonal least squares on four or five ball-centre contacts. Reject a singular fit or a
// fit that does not converge. The residual is the largest radial miss, in millimetres.
[[nodiscard]] auto fit_probe_circle(std::span<const std::array<double, 2>> points)
    -> std::optional<CircleFit>;

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
  // along the edge it probes; for a runout, the ring's or pin's known diameter. Millimetres.
  double gauge_width_mm{};
  // For a runout: a pin probed from outside rather than a ring from inside.
  bool outside{};
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
  // For bore_arc and boss_arc: the three probing directions, degrees counterclockwise from the
  // active work system's +X axis. A bore is probed outward from the start along them; a boss
  // inward from beyond `gauge_width_mm` / 2 along them.
  std::array<double, 3> angles_deg{};
  // One extra contact for a bore or boss. Degrees from work +X, just as for the three angles.
  std::optional<double> check_angle_deg{};
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
  // The circle was sampled, but the contact check refused the origin write.
  std::optional<LocalizedReason> origin_refusal{};
  // After a ball calibration: the effective ball diameter written to the tool table, mm.
  std::optional<double> ball_diameter{};
  // After a bore or boss: its diameter, mm (the chord through the centre). After a three-point
  // bore or boss: the diameter of the circle through the contacts.
  std::optional<double> center_diameter{};
  // After a three-point bore or boss: the arc the contacts cover around the centre, degrees.
  std::optional<double> arc_span_deg{};
  // After a three-point bore or boss: the estimated 95 % uncertainty of the centre, mm, from the
  // latch spread of each contact and the arc geometry.
  std::optional<double> center_uncertainty_mm{};
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
  // The touch probe's runout compensation in use for its XY results; none when not calibrated
  // or no longer valid (another ball diameter).
  std::optional<ProbeRunout> runout{};
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
