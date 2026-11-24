#ifndef COFFEECNC_CORE_FIXED_SETTER_HPP
#define COFFEECNC_CORE_FIXED_SETTER_HPP

#include <array>
#include <cmath>
#include <cstdint>
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
  angle,
  apply_angle,
  // Stylus runout (#247): measures a ring or pin of known diameter in +X, -X, +Y and -Y and
  // keeps the per-direction compensation for the touch probe; clear_runout forgets it.
  runout,
  clear_runout
};
enum class ProbeDirection : std::uint8_t { z_top, x_minus, x_plus, y_minus, y_plus };

// A G5x rotation in (-180, 180] degrees: the same turn as `degrees`. Applying an edge angle adds
// it to the present rotation, which could otherwise pass the panel's ±360° limit (355 + 8).
inline auto wrapped_rotation(double degrees) -> double {
  const double turn = std::remainder(degrees, 360.0);
  return turn == -180.0 ? 180.0 : turn;
}

// The stylus runout of the touch probe (#247): for a search along +X, -X, +Y and -Y, how far
// the probed surface lies beyond where the ball radius of the tool table puts it, mm. Measured
// on a ring or pin of known diameter after the ball calibration, against that table diameter.
// It holds the eccentricity of the ball centre from the spindle axis (the half difference of
// opposite directions) and the trigger radius left over per axis (their mean).
struct ProbeRunout {
  int tool{};
  double table_diameter_mm{}; // the ball diameter the compensation is relative to
  std::array<double, 4> along_mm{}; // +X, -X, +Y, -Y
  bool centred{}; // the ring's centre was known (X0 Y0): eccentricity measured, not assumed 0
  friend auto operator==(const ProbeRunout&, const ProbeRunout&) -> bool = default;
  [[nodiscard]] auto eccentricity() const -> std::array<double, 2> {
    return {(along_mm[0] - along_mm[1]) / 2, (along_mm[2] - along_mm[3]) / 2};
  }
  [[nodiscard]] auto radius_left() const -> std::array<double, 2> {
    return {(along_mm[0] + along_mm[1]) / 2, (along_mm[2] + along_mm[3]) / 2};
  }
  // What a ball-centre contact of a search along the machine XY unit direction (ux, uy) moves
  // by: the eccentricity, and the radius left over along the travel (between the axes by the
  // squared direction cosines). A contact moved so gives the surface with the table radius.
  [[nodiscard]] auto correction(double ux, double uy) const -> std::array<double, 2> {
    const auto e = eccentricity();
    const auto r = radius_left();
    const double along = r[0] * ux * ux + r[1] * uy * uy;
    return {e[0] + along * ux, e[1] + along * uy};
  }
};
// More than this per direction is not runout: a wrong diameter, centre or ball.
constexpr double max_runout_mm = 0.5;

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
  std::uint64_t edge_measurement{}; // controller-issued ID for apply_angle
  // For a runout: a pin probed from outside rather than a ring from inside, and whether its
  // centre is the active work system's X0 Y0 (set with a test indicator).
  bool outside{};
  bool centred{};
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
  // The touch probe's runout compensation in use for its XY results; none when not calibrated
  // or no longer valid (another ball diameter).
  std::optional<ProbeRunout> runout{};
};

} // namespace coffeecnc

#endif
