#include <coffeecnc/core/command_policy.hpp>
#include <coffeecnc/core/controller_port.hpp>
#include <coffeecnc/core/fixed_setter.hpp>
#include <coffeecnc/core/jog_lease.hpp>
#include <coffeecnc/core/jog_stop_report.hpp>
#include <coffeecnc/core/probe_modal.hpp>
#include <coffeecnc/core/task_heartbeat.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <iostream>
#include <limits>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

using namespace coffeecnc;
using namespace std::chrono_literals;

namespace {
int checks = 0;
auto simulator_limits() -> Limits { return {3000, 1.2, 0.5, 1.2, 24000}; }
void require(bool condition, const std::string& message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}
auto ready() -> ControllerState {
  ControllerState s;
  s.link = Link::live;
  s.machine = MachineState::on;
  s.program_loaded = true;
  for (auto& a : s.axes) {
    a.enabled = true;
    a.homed = true;
    a.min_limit = -1000;
    a.max_limit = 1000;
  }
  return s;
}
void execution_gates() {
  CommandPolicy policy{simulator_limits()};
  auto s = ready();
  auto d = policy.evaluate(Run{}, s);
  require(d.allowed && d.required_mode == TaskMode::automatic,
          "run requests AUTO after validation");
  s.axes[2].homed = false;
  require(!policy.evaluate(Run{}, s).allowed, "unhomed run rejected");
  require(!policy.evaluate(Mdi{"G0 X10"}, s).allowed, "unhomed MDI rejected");
  require(policy.evaluate(Jog{Axis::z, -1, 100, true}, s).allowed,
          "manual pre-home jog can locate machine");
  s = ready();
  s.interp = InterpState::reading;
  s.mode = TaskMode::automatic;
  require(!policy.evaluate(TouchOff{Axis::x, 0}, s).allowed, "cannot change offsets while reading");
  require(!policy.evaluate(Mdi{"G0 X10"}, s).allowed, "cannot switch busy AUTO into MDI");
  require(!policy.evaluate(OpenProgram{"part.ngc"}, s).allowed, "cannot replace executing program");
  require(policy.evaluate(Pause{}, s).allowed, "can hold executing program");
  s.interp = InterpState::paused;
  s.in_position = false;
  require(policy.evaluate(Step{}, s).allowed, "mid-move feed hold can single-step remaining block");
  require(policy.evaluate(Resume{}, s).allowed, "paused program can resume");
  s.requested_tool = 2;
  require(!policy.evaluate(Resume{}, s).allowed, "tool handshake is not ordinary resume");
  require(!policy.evaluate(Step{}, s).allowed, "single block does not bypass tool handshake");
  require(policy.evaluate(ToolChanged{}, s).allowed, "requested tool change can be acknowledged");
  s.requested_tool = 0;
  require(policy.evaluate(ToolChanged{}, s).allowed, "tool zero is a valid unload request");
}
void pending_manual_tool_change() {
  CommandPolicy policy{simulator_limits()};
  auto s = ready();
  s.tool_in_spindle = 1;
  s.requested_tool = 2;
  // LinuxCNC can report a settled trajectory and an idle interpreter while I/O waits at M6.
  for (const Command& command : {
           Command{SetSpindle{SpindleDirection::forward, 500}}, Command{Home{}},
           Command{Jog{Axis::x, 1, 100, true}}, Command{JogStep{Axis::x, 1, 100}},
           Command{Mdi{"G0 X10"}}, Command{Run{}}, Command{Step{}},
           Command{OpenProgram{"part.ngc"}}, Command{EditProgram{"part.ngc", 1, "G0", "G1"}},
           Command{PlanRestart{1, 1}}, Command{StartRestart{1}},
           Command{SelectWcs{2}}, Command{TouchOff{Axis::x, 0}}, Command{ToolTouchOff{0}},
           Command{Quit{}}})
    require(!policy.evaluate(command, s).allowed, "pending manual tool change blocks motion or program changes");
  for (const Command& command : {Command{ToolChanged{}}, Command{Abort{}},
           Command{SetMachine{MachineState::off}}, Command{SetMachine{MachineState::estop}},
           Command{SetSpindle{SpindleDirection::stopped, 0}}, Command{Jog{Axis::x, 1, 100, false}}})
    require(policy.evaluate(command, s).allowed, "pending change leaves confirmation and stop actions available");
}

void reference_mark_homing() {
  CommandPolicy policy{simulator_limits()};
  auto s = ready();
  for (auto& a : s.axes)
    a.homed = false;
  require(policy.evaluate(Home{Axis::x}, s).allowed && policy.evaluate(Home{}, s).allowed,
          "a machine without reference marks homes as before");
  s.axes[0].reference_mark = true;
  s.axes[1].reference_mark = false;
  s.axes[2].reference_mark = true;
  require(policy.evaluate(Home{Axis::x}, s).allowed && policy.evaluate(Home{Axis::z}, s).allowed,
          "an axis whose reference mark has been found can be homed");
  const auto y = policy.evaluate(Home{Axis::y}, s);
  require(!y.allowed &&
              y.reason == "Move Y across its reference mark first: homing cannot find it yet.",
          "an axis whose reference mark is not known is not homed: " + y.reason);
  require(policy.evaluate(Home{}, s).reason == y.reason, "nor is it homed with the others");
  s.axes[1].reference_mark = true;
  require(policy.evaluate(Home{}, s).allowed, "all marks found: home all");
  s.machine = MachineState::off;
  s.axes[1].reference_mark = false;
  require(policy.evaluate(Home{Axis::y}, s).reason != y.reason,
          "machine power is asked for before the reference mark");
}

// After a stopped probe cycle (#338): each item is restored only while it is what the cycle
// left and no command ran since; the rest is reported.
void probe_modal_restore() {
  const ProbeModalState operator_state{true, 100, true, true};
  const ProbeModalState left{false, 240, false, false};
  const auto all = plan_probe_restore(operator_state, left, left, false);
  require(!all.unknown && all.distance && all.feed_override && all.adaptive_feed && all.changed.empty(),
          "an unchanged state is restored item by item");
  // Somebody set G91 only, after the stop (another client, or ON_ABORT_COMMAND).
  const auto g91 = plan_probe_restore(operator_state, left, {true, 240, false, false}, false);
  std::cout << std::format("fact=probe_modal case=g91_only distance={} m50={} m52={} changed=\"{}\"",
                           g91.distance, g91.feed_override, g91.adaptive_feed, g91.changed)
            << '\n';
  require(!g91.distance && g91.feed_override && g91.adaptive_feed && g91.changed == "G90 F240",
          "G91 set after the stop keeps M50 and M52 restored and reports G90 and F");
  const auto feed = plan_probe_restore(operator_state, left, {false, 300, false, false}, false);
  require(!feed.distance && feed.feed_override && feed.changed == "G90 F240",
          "a changed F is not overwritten");
  const auto unknown = plan_probe_restore(operator_state, std::nullopt, left, false);
  std::cout << std::format("fact=probe_modal case=left_unknown unknown={} distance={} m50={} m52={}",
                           unknown.unknown, unknown.distance, unknown.feed_override,
                           unknown.adaptive_feed)
            << '\n';
  require(unknown.unknown && !unknown.distance && !unknown.feed_override && !unknown.adaptive_feed,
          "without a known left state nothing is restored and the operator is told");
  const auto commands = plan_probe_restore(operator_state, left, left, true);
  std::cout << std::format("fact=probe_modal case=command_since distance={} m50={} m52={} changed=\"{}\"",
                           commands.distance, commands.feed_override, commands.adaptive_feed,
                           commands.changed)
            << '\n';
  require(!commands.distance && !commands.feed_override && !commands.adaptive_feed &&
              commands.changed == "G90 F240, M50 P0, M52 P0",
          "a command since the stop drops every item, also one that matches, and reports them");
  // An item the cycle did not change is neither restored nor reported.
  const auto kept = plan_probe_restore({false, 240, true, false}, left, left, false);
  require(!kept.distance && kept.feed_override && !kept.adaptive_feed && kept.changed.empty(),
          "only the items the cycle changed are restored");

  // A later probe cycle takes over a pending restore only while nothing changed since the stop.
  const auto takeover = plan_probe_takeover(operator_state, left, left, false);
  require(takeover.take_over && takeover.report.changed.empty() && !takeover.report.unknown,
          "a later cycle takes over an unchanged pending restore");
  // M50 set from outside, no command through the port: the cycle restores none of the stopped
  // cycle's items, so it names each of them, also G90 F240 and M52 P0 that were still restorable.
  const auto outside = plan_probe_takeover(operator_state, left, {false, 240, true, false}, false);
  std::cout << std::format("fact=probe_modal case=takeover_one_changed take_over={} changed=\"{}\"",
                           outside.take_over, outside.report.changed)
            << '\n';
  require(!outside.take_over && !outside.report.unknown &&
              outside.report.changed == "G90 F240, M50 P0, M52 P0",
          "a later cycle over one changed item names every item the stopped cycle changed");
  const auto takeover_unknown = plan_probe_takeover(operator_state, std::nullopt, left, false);
  require(!takeover_unknown.take_over && takeover_unknown.report.unknown,
          "a later cycle over an unknown left state reports it as not restored");

  // An unconfirmed stop keeps the probe guard set, so no restore follows: tell the operator now.
  // The key of the one message, or "none".
  const auto only_key = [](const std::vector<ControllerMessage>& messages) -> std::string {
    if (messages.size() != 1)
      return "none";
    const auto& localization = messages.front().localization;
    return localization ? localization->key : "none";
  };
  const auto unconfirmed = plan_probe_stop(false);
  const auto unconfirmed_messages =
      unconfirmed ? probe_modal_messages(*unconfirmed) : std::vector<ControllerMessage>{};
  std::cout << std::format("fact=probe_modal case=stop_unconfirmed messages={} key={}",
                           unconfirmed_messages.size(), only_key(unconfirmed_messages))
            << '\n';
  require(only_key(unconfirmed_messages) == "backend.nmlPort.probeModalNotRestored" &&
              unconfirmed_messages.front().error,
          "an unconfirmed stop reports one keyed \"not restored\" message at once");
  require(!plan_probe_stop(true), "a confirmed stop leaves the decision to idle_maintenance");
  const auto changed_messages = probe_modal_messages(outside.report);
  require(only_key(changed_messages) == "backend.nmlPort.probeModalChanged" &&
              !changed_messages.front().error,
          "the unrestored items go out as the keyed probeModalChanged message");
}

// Applying an edge angle adds it to the present rotation: the sum stays in (-180, 180] (#245).
void edge_rotation_wraps() {
  for (const auto& [sum, wrapped] : {std::pair{363.0, 3.0}, std::pair{-363.0, -3.0},
                                     std::pair{181.8624, -178.1376}, std::pair{-180.0, 180.0},
                                     std::pair{180.0, 180.0}, std::pair{540.0, 180.0},
                                     std::pair{10.3624, 10.3624}, std::pair{0.0, 0.0}})
    require(std::abs(wrapped_rotation(sum) - wrapped) < 1e-9,
            std::format("rotation {} wraps to {}, not {}", sum, wrapped, wrapped_rotation(sum)));
}

// The runout correction (#247): only the trigger radius, along the travel. On the axes it is the
// measured radius of that axis; between them it is weighted by the squared direction cosines.
// No eccentricity: the spindle's angle is not known after the calibration (no M19).
void runout_correction() {
  const ProbeRunout runout{.tool = 99, .table_diameter_mm = 2, .radius_mm = {-0.01, -0.02}};
  const std::array<std::array<double, 2>, 4> axes{{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}};
  for (std::size_t i = 0; i < axes.size(); ++i) {
    const auto [ux, uy] = axes.at(i);
    const auto shift = runout.correction(ux, uy);
    const double expected = runout.radius_mm.at(i / 2);
    require(std::abs(shift[0] * ux + shift[1] * uy - expected) < 1e-12 &&
                std::abs(shift[0] * uy - shift[1] * ux) < 1e-12,
            "along a calibrated direction the correction is that axis's radius, along the travel");
  }
  const double c = std::cos(std::numbers::pi / 6);
  const double s = std::sin(std::numbers::pi / 6);
  const double along = -0.01 * c * c - 0.02 * s * s;
  const auto turned = runout.correction(c, s);
  require(std::abs(turned[0] - along * c) < 1e-12 && std::abs(turned[1] - along * s) < 1e-12,
          "between the axes: the radius weighted by the squared direction cosines, along the travel");
  // A heading of 210° (the boss and bore arcs of #318) is corrected along that heading.
  const auto back = runout.correction(-c, -s);
  require(std::abs(back[0] + along * c) < 1e-12 && std::abs(back[1] + along * s) < 1e-12,
          "the opposite heading is corrected by the same radius, along its own travel");
  require(std::abs(max_runout_mm - 0.05) < 1e-12, "a gauge-diameter typo of 0.05 mm (19.00 for 19.05) is the limit");
}

// The line of a search (#247): along its work axis direction, or with a heading (the three-point
// bore and boss of #318, material +X), along that heading. The runout correction follows it.
void search_line_heading() {
  const auto near = [](const ProbeLine& line, double ux, double uy) {
    return std::abs(line.forward[0] - ux) < 1e-12 && std::abs(line.forward[1] - uy) < 1e-12 &&
           line.forward[2] == 0;
  };
  const double c = std::cos(std::numbers::pi / 6);
  const double s = std::sin(std::numbers::pi / 6);
  require(near(search_line(ProbeDirection::y_minus, std::nullopt, 0), 0, -1),
          "without a heading the search runs along its work axis direction");
  require(search_line(std::nullopt, std::nullopt, 30).axis == 2,
          "without a direction or heading the search is Z top");
  require(near(search_line(ProbeDirection::x_plus, 120, 0), -s, c),
          "with a heading the search runs along it, not along +X");
  require(near(search_line(ProbeDirection::x_plus, 210, 0), -c, -s),
          "a boss heading of 30° + 180° runs inward, the opposite way");
  require(near(search_line(ProbeDirection::x_plus, 90, -60), c, s),
          "a heading turns with the work system: 90° in a system turned -60° is machine 30°");
  // The correction of a contact along its heading.
  const ProbeRunout runout{.tool = 99, .table_diameter_mm = 2, .radius_mm = {-0.04, 0.02}};
  const auto line = search_line(ProbeDirection::x_plus, 120, 0);
  const auto shift = runout.correction(line.forward[0], line.forward[1]);
  const double along = -0.04 * s * s + 0.02 * c * c;
  require(std::abs(shift[0] - along * -s) < 1e-12 && std::abs(shift[1] - along * c) < 1e-12,
          "a contact at 120° is corrected along 120°, not along +X");
}

// Before any motion the runout calibration checks that the search reaches the gauge: in a ring
// its radius, beside a pin the boss's approach (outside plus the ball radius).
void runout_travel() {
  require(runout_travel_reaches(false, 20, 14, 1, 20) && runout_travel_reaches(false, 40, 14, 1, 20),
          "a ring of radius up to the search travel is reachable");
  require(!runout_travel_reaches(false, 44, 14, 1, 20), "a ring wider than twice the travel is refused");
  require(runout_travel_reaches(true, 200, 14, 1, 15), "beside a pin, the diameter does not matter");
  require(!runout_travel_reaches(true, 12, 14, 1, 14.9),
          "beside a pin, the travel must reach from outside to the surface, as for a boss");
}

// The height-map grid (#247): both corners included, at most one step apart, 2 to 150 points.
void height_map_grid() {
  const auto grid = height_grid({0, 0, 20, 16}, 10);
  require(grid && grid->columns == 3 && grid->rows == 3, "20 x 16 at 10 mm: 3 x 3 points");
  const auto uneven = height_grid({-5, 2, 12, 2}, 4);
  require(uneven && uneven->columns == 6 && uneven->rows == 1,
          "17 mm at 4 mm: 6 columns, a single row when Y is the same");
  const auto reversed = height_grid({20, 16, 0, 0}, 10);
  require(reversed && reversed->columns == 3 && reversed->rows == 3, "the corners may come in any order");
  require(!height_grid({0, 0, 0, 0}, 10), "one point is not a map");
  require(!height_grid({0, 0, 100, 100}, 5), "21 x 21 points is more than 150");
  require(!height_grid({0, 0, 20, 16}, 0.25), "a step below 0.5 mm is refused");
  require(!height_grid({0, 0, std::numeric_limits<double>::infinity(), 16}, 10), "corners are finite");
  // The worst case of one point: 20 mm at 240 mm/min (5 s), three passes of 1 mm back at
  // 480 mm/min, 2 s release and 2 mm at 60 mm/min (4.125 s each), the 20 mm retract at 480 mm/min
  // and its 2 s release (4.5 s), and 10 s: 31.875 s.
  const FixedSetterLimits limits{.travel_mm = 15, .feed_mm_min = 240, .latch_feed_mm_min = 60,
                                 .latch_backoff_mm = 1, .material_travel_mm = 20};
  require(std::abs(height_map_point_seconds(limits) - 31.875) < 1e-9, "one point takes at most 31.875 s");
  // 15 x 10 points 10 mm apart, the first 50 mm away, XY at 6000 mm/min: 50 + 149 x 10 =
  // 1540 mm in 15.4 s.
  const HeightMapTravel small{.points = 150, .step_mm = 10, .first_mm = 50, .descent_mm = 5,
                              .xy_rapid_mm_min = 6000};
  require(std::abs(height_map_seconds(limits, small) - ((150 * 31.875) + 15.4 + 0.625)) < 1e-9,
          "150 points, 1540 mm of XY at 6000 mm/min and a 5 mm descent at 480 mm/min");
  require(height_map_seconds(limits, small) <= max_height_map_seconds,
          "150 points at these limits fit the 2 h limit");
  // The review's case: 9 x 9 points 250 mm apart. The 80 traverses of 250 mm at 10000 mm/min
  // add 120 s to the 81 points' 2581.875 s.
  const HeightMapTravel wide{.points = 81, .step_mm = 250, .first_mm = 0, .descent_mm = 0,
                             .xy_rapid_mm_min = 10000};
  require(std::abs(height_map_seconds(limits, wide) - (2581.875 + 120)) < 1e-9,
          "81 points 250 mm apart take 2701.875 s with their XY traverses");
  require(!std::isfinite(height_map_seconds(limits, {.points = 2, .step_mm = 10, .first_mm = 0,
                                                     .descent_mm = 0, .xy_rapid_mm_min = 0})),
          "no XY speed: no bound");
  auto slow = limits;
  slow.latch_feed_mm_min = 5; // 24 s a latch pass
  require(height_map_seconds(slow, small) > max_height_map_seconds,
          "a slow latch makes 150 points exceed the 2 h limit");
  slow.latch_feed_mm_min = 0;
  require(!std::isfinite(height_map_point_seconds(slow)), "no latch feed: no bound");
}

void circle_verification() {
  const auto check = []<std::size_t Size>(const std::array<double, Size>& angles) {
    std::array<std::array<double, 2>, Size> points{};
    for (std::size_t i = 0; i < angles.size(); ++i) {
      const double angle = angles.at(i) * std::numbers::pi / 180;
      points.at(i) = {250 + 7 * std::cos(angle), -180 + 7 * std::sin(angle)};
    }
    const auto fit = fit_probe_circle(points);
    require(fit && std::abs(fit->circle.radius - 7) < 1e-8 &&
                std::hypot(fit->circle.centre[0] - 250, fit->circle.centre[1] + 180) < 1e-8 &&
                fit->max_residual_mm < 1e-8,
            "verified contacts on a circle return its centre and diameter");
    for (std::size_t i = 0; i < angles.size(); ++i) {
      const double angle = angles.at(i) * std::numbers::pi / 180;
      for (const double sign : {-1.0, 1.0}) {
        auto early = points;
        early.at(i)[0] += sign * std::cos(angle);
        early.at(i)[1] += sign * std::sin(angle);
        const auto result = fit_probe_circle(early);
        require(result && result->max_residual_mm > circle_check_limit_mm,
                "the fitted circle rejects a 1 mm early trip at each bore and boss contact");
      }
    }
  };
  check(std::array{0.0, 120.0, 240.0, 60.0});
  check(std::array{0.0, 90.0, 180.0, 270.0, 45.0});
  const std::array line{std::array{0.0, 0.0}, std::array{1.0, 1.0}, std::array{2.0, 2.0},
                        std::array{3.0, 3.0}};
  require(!fit_probe_circle(line), "collinear verification contacts are rejected");
}

// Three-point bore and boss geometry (#247): the circle, the arc it covers, the spread checks.
void arc_geometry() {
  const auto on_circle = [](double degrees) {
    const double a = degrees * std::numbers::pi / 180;
    return std::array{4 + 7 * std::cos(a), -2 + 7 * std::sin(a)};
  };
  for (const auto& angles : {std::array{0.0, 120.0, 240.0}, std::array{-80.0, -40.0, 0.0},
                             std::array{200.0, 240.0, 280.0}, std::array{10.0, 25.0, 40.0}}) {
    const std::array points{on_circle(angles[0]), on_circle(angles[1]), on_circle(angles[2])};
    const auto circle = circle_through(points);
    require(circle && std::abs(circle->centre[0] - 4) < 1e-9 &&
                std::abs(circle->centre[1] + 2) < 1e-9 && std::abs(circle->radius - 7) < 1e-9,
            std::format("the circle through {}°, {}°, {}° has its centre at (4, -2) and radius 7",
                        angles[0], angles[1], angles[2]));
    require(circle && std::abs(measured_span(points, circle->centre) - arc_spread(angles).span_deg) < 1e-9,
            "the measured arc equals the requested one for contacts on the circle");
  }
  require(!circle_through({std::array{0.0, 0.0}, std::array{1.0, 1.0}, std::array{2.0, 2.0}}),
          "three points on a line have no circle");
  require(!circle_through({std::array{0.0, 0.0}, std::array{1.0, 0.0}, std::array{2.0, 1e-9}}),
          "three points almost on a line give no usable circle");
  const auto spread = arc_spread({350, 365, 10});
  require(std::abs(spread.closest_deg - 5) < 1e-9 && std::abs(spread.span_deg - 20) < 1e-9,
          "directions are compared around the full turn");
  const auto full = arc_spread({0, 120, 240});
  require(std::abs(full.closest_deg - 120) < 1e-9 && std::abs(full.span_deg - 240) < 1e-9,
          "three even directions cover 240° and are 120° apart");
  const auto partial = arc_spread({-80, -40, 0});
  require(std::abs(partial.closest_deg - 40) < 1e-9 && std::abs(partial.span_deg - 80) < 1e-9,
          "a partial arc covers the angle between its outer directions");
  require(arc_spread({-45, 0, 45}).span_deg >= arc_min_span_deg &&
              !(arc_spread({-44, 0, 44}).span_deg >= arc_min_span_deg) &&
              !(arc_spread({-80, -40, 0}).span_deg >= arc_min_span_deg),
          "three directions must cover at least 90°");

  // The centre uncertainty against an independent Monte Carlo with 2.5 µm radial noise on r = 8
  // (95th percentile: 0.40 mm at 20°, 0.18 mm at 30°, 0.10 mm at 40°, 0.026 mm at 80°, 0.005 mm
  // at 240°).
  const auto on_8 = [](double degrees) {
    const double a = degrees * std::numbers::pi / 180;
    return std::array{8 * std::cos(a), 8 * std::sin(a)};
  };
  const auto uncertainty = [&](double span, double sigma) {
    const std::array points{on_8(-span / 2), on_8(0), on_8(span / 2)};
    return arc_centre_uncertainty(points, {0, 0}, {sigma, sigma, sigma}).value_or(-1);
  };
  for (const auto& [span, expected] : {std::pair{20.0, 0.40}, std::pair{30.0, 0.18},
                                       std::pair{40.0, 0.10}, std::pair{80.0, 0.026},
                                       std::pair{240.0, 0.0057}}) {
    const double found = uncertainty(span, 0.0025);
    std::cout << std::format("fact=arc_uncertainty span={:.0f} sigma_mm=0.0025 expected_mm={:.4f} "
                             "computed_mm={:.4f}",
                             span, expected, found)
              << '\n';
    require(std::abs(found - expected) <= 0.12 * expected,
            std::format("the centre uncertainty over {}° is about {} mm", span, expected));
  }
  // The refusal: a 90° arc writes with a clean latch, not with a latch spread of 6 µm; a wide
  // arc writes with that spread.
  const auto refused = [&](double span, double latch_spread) {
    return !(uncertainty(span, contact_sigma(latch_spread)) <= arc_max_centre_uncertainty_mm);
  };
  for (const auto& [span, latch, refuse] :
       {std::tuple{90.0, 0.0, false}, std::tuple{90.0, 0.006, true}, std::tuple{240.0, 0.006, false},
        std::tuple{120.0, 0.01, true}}) {
    std::cout << std::format("fact=arc_uncertainty_refusal span={:.0f} latch_spread_mm={:.4f} "
                             "uncertainty_mm={:.4f} limit_mm=0.02 refused={}",
                             span, latch, uncertainty(span, contact_sigma(latch)), refused(span, latch))
              << '\n';
    require(refused(span, latch) == refuse,
            std::format("an arc of {}° with a latch spread of {} mm is {}", span, latch,
                        refuse ? "refused" : "written"));
  }
  require(!arc_centre_uncertainty({std::array{1.0, 0.0}, std::array{0.0, 0.0}, std::array{0.0, 1.0}},
                                  {0, 0}, {0.001, 0.001, 0.001}),
          "a contact at the centre has no uncertainty estimate");
  // A three-point boss within 0.5 mm or 10 % of the entered diameter.
  require(boss_diameter_plausible(12.4, 12) && !boss_diameter_plausible(13.3, 12) &&
              boss_diameter_plausible(10.9, 12) && boss_diameter_plausible(3.4, 3) &&
              !boss_diameter_plausible(3.6, 3),
          "a boss diameter is plausible within the larger of 0.5 mm and 10 %");
}

void fixed_setter_homing() {
  CommandPolicy homing{simulator_limits()};
  CommandPolicy no_homing{Limits{3000, {3000, 3000, 3000}, 1.2, 0.5, 1.2, 24000, false}};
  auto s = ready();
  s.tool_in_spindle = 99;
  s.axes[0].homed = false;
  for (const auto action : {SetterAction::touch_off, SetterAction::check, SetterAction::material,
                           SetterAction::bore, SetterAction::apply_angle}) {
    const FixedSetterRequest request{.action = action, .tool = 99};
    require(homing.evaluate(request, s).reason == "Home all configured axes first.",
            "all setter and material actions honor configured homing");
    require(no_homing.evaluate(request, s).allowed, "NO_FORCE_HOMING is honored for the setter");
  }
  require(homing.evaluate(FixedSetterRequest{.action = SetterAction::status}, s).allowed,
          "read-only setter status is available before homing");
  // Clearing the runout (#247) only removes a file: another tool, the spindle on, a program
  // reading and no homing do not refuse it.
  auto busy = s;
  busy.tool_in_spindle = 3;
  busy.spindle_active = true;
  busy.interp = InterpState::reading;
  busy.in_position = false;
  const auto clear = homing.evaluate(FixedSetterRequest{.action = SetterAction::clear_runout, .tool = 99}, busy);
  require(clear.allowed && !clear.required_mode, "clearing the runout needs no probe, rest or mode");
  require(!homing.evaluate(FixedSetterRequest{.action = SetterAction::runout, .tool = 99}, busy).allowed,
          "a runout calibration still needs the probe at rest");
  s.axes[0].homed = true;
  const auto nan = std::numeric_limits<double>::quiet_NaN();
  require(!homing.evaluate(FixedSetterRequest{.action = SetterAction::bore,
                                              .tool = 99, .start = {nan, 1, 2}}, s).allowed,
          "a probe request has finite XYZ");
  require(!homing.evaluate(FixedSetterRequest{.action = SetterAction::boss,
                                              .tool = 99, .gauge_width_mm = nan}, s).allowed,
          "a probe gauge width is finite");
  require(!homing.evaluate(FixedSetterRequest{.action = SetterAction::bore_arc, .tool = 99,
                                              .angles_deg = {0, nan, 240}}, s).allowed,
          "three-point probe angles are finite");
  require(homing.evaluate(FixedSetterRequest{.action = SetterAction::bore, .tool = 99,
                                             .measure_only = true}, s).allowed,
          "a feature probe can run as a measurement only");
  for (const auto action : {SetterAction::ball, SetterAction::touch_off, SetterAction::check,
                            SetterAction::apply_angle, SetterAction::apply_measurement})
    require(homing.evaluate(FixedSetterRequest{.action = action, .tool = 99, .measure_only = true}, s)
                    .reason == "This probe action always writes its result.",
            "the ball calibration, the tool setter and the apply actions cannot measure only");
  s.axes[1].on_hard_limit = true;
  require(!homing.evaluate(FixedSetterRequest{.action = SetterAction::material, .tool = 99}, s).allowed,
          "a hard limit blocks a probe cycle");
}

void paused_z_gates() {
  CommandPolicy policy{simulator_limits()};
  auto s = ready();
  s.mode = TaskMode::automatic;
  s.interp = InterpState::paused;
  s.program_loaded = true;
  const MessageKey stop_spindle{"backend.nmlStatus.stopSpindle"};
  s.paused_z = PausedZStatus{
      0,           20, false, true, "Stop the spindle before jogging Z.", PausedSpindle::can_stop,
      stop_spindle};
  const auto blocked = policy.evaluate(JogPausedZ{5, false}, s);
  require(!blocked.allowed && blocked.reason == s.paused_z->reason &&
              blocked.refusal == stop_spindle,
          "A refused lift carries the paused Z reason and key");
  const JogPausedZ lift{5, true};
  require(policy.evaluate(lift, s).allowed, "An otherwise valid paused lift may stop the spindle");
  s.axes[0].homed = false;
  require(!policy.evaluate(lift, s).allowed, "Stopping the spindle never waives the homing check");
  s.axes[0].homed = true;
  s.paused_z->max_lift = std::numeric_limits<double>::quiet_NaN();
  require(!policy.evaluate(lift, s).allowed, "Paused lift needs a finite configured limit");
  s.paused_z->max_lift = 20;
  s.paused_z->spindle = PausedSpindle::stopping;
  require(!policy.evaluate(Step{}, s).allowed, "Single block waits for the spindle to stop");
  s.paused_z->feedback_available = false;
  require(!policy.evaluate(Resume{}, s).allowed &&
              policy.evaluate(SetOverride{Override::feed, 0.8}, s).allowed,
          "Missing paused Z feedback gates motion, while feed override remains available");
}

void work_origin_gates() {
  CommandPolicy policy{simulator_limits()};
  const SetWorkOffset command{2, {25.4, -50.8, 0}};
  auto s = ready();
  const auto allowed = policy.evaluate(command, s);
  require(allowed.allowed && allowed.required_mode == TaskMode::mdi, "Work origins use MDI");
  for (int scenario = 0; scenario < 8; ++scenario) {
    s = ready();
    switch (scenario) {
    case 0: s.link = Link::stale; break;
    case 1: s.machine = MachineState::off; break;
    case 2: s.interp = InterpState::reading; break;
    case 3: s.in_position = false; break;
    case 4: s.requested_tool = 2; break;
    case 5: s.axes[2].homing = true; break;
    case 6: s.axes[2].homed = false; break;
    case 7: s.axes[2].enabled = false; break;
    default: std::unreachable();
    }
    require(!policy.evaluate(command, s).allowed, "Work origin write respects execution gates");
  }
  s = ready();
  for (const int wcs : {0, 10})
    require(!policy.evaluate(SetWorkOffset{wcs, {0, 0, 0}}, s).allowed, "No implicit/invalid WCS");
  for (const double value : {std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN()})
    require(!policy.evaluate(SetWorkOffset{1, {0, value, 0}}, s).allowed, "No nonfinite origins");
}

// The XY rotation and the G92/G52 clear: the gates of the other offset edits.
void rotation_and_shift_gates() {
  CommandPolicy policy{simulator_limits()};
  const Command rotate = SetWorkRotation{2, 15};
  const Command clear = ClearG92{};
  for (const auto& command : {rotate, clear}) {
    auto s = ready();
    const auto allowed = policy.evaluate(command, s);
    require(allowed.allowed && allowed.required_mode == TaskMode::mdi,
            "Rotation and shift clear use MDI");
    for (int scenario = 0; scenario < 9; ++scenario) {
      s = ready();
      switch (scenario) {
      case 0: s.link = Link::stale; break;
      case 1: s.machine = MachineState::off; break;
      case 2: s.machine = MachineState::estop; break;
      case 3: s.interp = InterpState::reading; break;
      case 4: s.interp = InterpState::paused; s.mode = TaskMode::automatic; break;
      case 5: s.in_position = false; break;
      case 6: s.requested_tool = 2; break;
      case 7: s.axes[2].homing = true; break;
      case 8: s.axes[2].homed = false; break;
      default: std::unreachable();
      }
      require(!policy.evaluate(command, s).allowed,
              std::format("Rotation and shift clear respect execution gates ({})", scenario));
    }
  }
  const Limits unforced{3000, {3000, 3000, 3000}, 1.2, 0.5, 1.2, 24000, false};
  auto unhomed = ready();
  unhomed.axes[2].homed = false;
  require(CommandPolicy{unforced}.evaluate(rotate, unhomed).allowed &&
              CommandPolicy{unforced}.evaluate(clear, unhomed).allowed,
          "Unhomed with NO_FORCE_HOMING, as the other offset edits");
  auto rotary = ready();
  rotary.rotary_axes[0].enabled = true;
  rotary.rotary_axes[0].homed = true;
  const auto refused = policy.evaluate(clear, rotary);
  require(!refused.allowed && refused.reason.starts_with("On a machine with A, B or C axes") &&
              policy.evaluate(rotate, rotary).allowed,
          "G92.1 would clear an A shift the panel does not show: refused with A, B or C axes; "
          "the rotation is not affected");
  const auto s = ready();
  require(policy.evaluate(SetWorkRotation{1, 0}, s).allowed &&
              policy.evaluate(SetWorkRotation{9, -360}, s).allowed &&
              policy.evaluate(SetWorkRotation{3, 360}, s).allowed,
          "Zero and the full turn either way are rotations");
  for (const int wcs : {0, 10})
    require(!policy.evaluate(SetWorkRotation{wcs, 0}, s).allowed, "No implicit/invalid WCS");
  for (const double degrees : {360.001, -400.0, std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::quiet_NaN()})
    require(!policy.evaluate(SetWorkRotation{1, degrees}, s).allowed,
            "No rotation beyond a full turn, none that is not finite");
}
// A machine LinuxCNC runs unhomed ([TRAJ] NO_FORCE_HOMING): execution is allowed, jog rules stay.
void homing_capability() {
  const Limits unforced{3000, {3000, 3000, 3000}, 1.2, 0.5, 1.2, 24000, false};
  CommandPolicy forced{simulator_limits()};
  CommandPolicy policy{unforced};
  auto s = ready();
  for (auto& a : s.axes)
    a.homed = false;
  s.tool_in_spindle = 1;
  const auto mdi = policy.evaluate(Mdi{"G0 X10"}, s);
  require(mdi.allowed && mdi.required_mode == TaskMode::mdi, "unhomed MDI allowed");
  const auto run = policy.evaluate(Run{}, s);
  require(run.allowed && run.required_mode == TaskMode::automatic, "unhomed run allowed");
  require(policy.evaluate(Step{}, s).allowed, "unhomed single block allowed");
  // Permission to work unhomed is not permission to move while an axis is homing.
  auto paused_homing = s;
  paused_homing.mode = TaskMode::automatic;
  paused_homing.interp = InterpState::paused;
  require(policy.evaluate(Step{}, paused_homing).allowed, "unhomed step from a pause allowed");
  paused_homing.axes[1].homing = true;
  const auto stepping = policy.evaluate(Step{}, paused_homing);
  require(!stepping.allowed && stepping.reason == "Wait for homing to finish.",
          "no step from a pause while an axis is homing");
  auto idle_homing = s;
  idle_homing.axes[2].homing = true;
  require(!policy.evaluate(Step{}, idle_homing).allowed &&
              !policy.evaluate(Run{}, idle_homing).allowed &&
              !policy.evaluate(Mdi{"G0 X1"}, idle_homing).allowed,
          "no step, run or MDI while an axis is homing, homing required or not");
  require(policy.evaluate(SelectWcs{2}, s).allowed, "unhomed WCS selection allowed");
  require(policy.evaluate(TouchOff{Axis::x, 0}, s).allowed, "unhomed touch-off allowed");
  require(policy.evaluate(ToolTouchOff{0}, s).allowed, "unhomed tool touch-off allowed");
  for (const Command& command :
       {Command{Mdi{"G0 X10"}}, Command{Run{}}, Command{Step{}}, Command{SelectWcs{2}},
        Command{TouchOff{Axis::x, 0}}, Command{ToolTouchOff{0}}})
    require(forced.evaluate(command, s).reason == "Home all configured axes first.",
            "the same command needs homing where LinuxCNC forces it");

  require(policy.evaluate(Jog{Axis::x, 1, 100, true}, s).jog_space == JogSpace::joint,
          "jogs stay joint jogs until every axis is homed");
  s.axes[0].machine = 5000;
  require(policy.evaluate(Jog{Axis::x, 1, 100, true}, s).allowed,
          "an unhomed axis has no soft limit to check");
  s.axes[0].homed = true;
  require(!policy.evaluate(Jog{Axis::x, 1, 100, true}, s).allowed,
          "a homed axis keeps its soft limit");
  require(policy.evaluate(Jog{Axis::x, -1, 100, true}, s).jog_space == JogSpace::joint,
          "partial homing is still joint space");

  s = ready();
  for (auto& a : s.axes)
    a.homed = false;
  s.axes[1].homing = true;
  require(!policy.evaluate(Mdi{"G0 X10"}, s).allowed, "no MDI while an axis is homing");
  require(!policy.evaluate(Run{}, s).allowed, "no run while an axis is homing");
  s.axes[1].homing = false;
  s.machine = MachineState::off;
  require(!policy.evaluate(Mdi{"G0 X10"}, s).allowed, "unhomed MDI still needs machine power");
  s.machine = MachineState::on;
  s.program_loaded = false;
  require(!policy.evaluate(Run{}, s).allowed && !policy.evaluate(Step{}, s).allowed,
          "unhomed run and step still need a program");
}
// X and Y 1500 mm/min, Z 750 mm/min, 1500 mm/min overall: the G0759's values.
void axis_jog_ceilings() {
  CommandPolicy policy{Limits{1500, {1500, 1500, 750}, 2.0, 0.5, 1.0, 1500, false}};
  const auto s = ready();
  require(policy.evaluate(Jog{Axis::x, 1, 1500, true}, s).allowed, "X jogs at its ceiling");
  require(policy.evaluate(Jog{Axis::z, 1, 750, true}, s).allowed, "Z jogs at its ceiling");
  const auto z = policy.evaluate(Jog{Axis::z, 1, 750.5, true}, s);
  require(!z.allowed && z.reason == "Jog velocity is above the Z axis limit of 750 mm/min.",
          "a Z jog above its ceiling names the axis and the limit");
  require(z.refusal && z.refusal->key == "messages.policy.jogVelocityAxisLimit" &&
              z.refusal->params.at("axis") == "Z" && z.refusal->params.at("limit") == "750",
          "The jog refusal keeps its named parameters separately from the English reason.");
  const auto step = policy.evaluate(JogStep{Axis::z, -1, 1500}, s);
  require(!step.allowed && step.reason == "Jog velocity is above the Z axis limit of 750 mm/min.",
          "a Z step above its ceiling names the axis and the limit");
  require(policy.evaluate(JogStep{Axis::y, -1, 1500}, s).allowed, "Y steps at its ceiling");
  require(policy.evaluate(Jog{Axis::x, 1, 1501, true}, s).reason ==
              "Jog velocity is outside the configured range.",
          "the global maximum still applies");
  require(policy.evaluate(Jog{Axis::z, 1, 0, false}, s).allowed,
          "a release is never a velocity question");
  // An axis ceiling above the global maximum cannot raise it.
  CommandPolicy wide{Limits{1500, {3000, 1500, 750}, 2.0, 0.5, 1.0, 1500, true}};
  require(!wide.evaluate(Jog{Axis::x, 1, 2000, true}, s).allowed,
          "the lower of both ceilings decides");
  // The short form: every axis at the global maximum, homing required.
  const Limits plain{3000, 1.2, 0.5, 1.2, 24000};
  require(plain.max_axis_jog_velocity == std::array<double, 3>{3000, 3000, 3000} &&
              plain.homing_required,
          "limits without per-axis values use the global maximum and require homing");
  for (const double bad : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN()}) {
    bool invalid = false;
    try {
      CommandPolicy broken{Limits{1500, {1500, 1500, bad}, 2.0, 0.5, 1.0, 1500, true}};
    } catch (const std::invalid_argument&) {
      invalid = true;
    }
    require(invalid, "an invalid axis ceiling fails construction");
  }
}
void stops_survive_lost_state() {
  CommandPolicy policy{simulator_limits()};
  auto s = ready();
  for (const auto link : {Link::stale, Link::down}) {
    s.link = link;
    s.machine = MachineState::estop;
    require(!policy.evaluate(Jog{Axis::x, 1, 100, true}, s).allowed,
            "lost status blocks jog starts");
    require(!policy.evaluate(Run{}, s).allowed, "lost status blocks cycle start");
    require(policy.evaluate(Jog{Axis::x, 1, 0, false}, s).allowed,
            "lost status does not block release");
    require(policy.evaluate(JogStopAll{}, s).allowed, "lost status does not block stop-all");
    require(policy.evaluate(Abort{}, s).allowed, "lost status does not block abort attempt");
    require(policy.evaluate(SetMachine{MachineState::estop}, s).allowed,
            "lost status does not block E-stop attempt");
    require(policy.evaluate(SetSpindle{SpindleDirection::stopped, 0}, s).allowed,
            "lost status does not block spindle stop");
  }
}
void jog_constraints() {
  CommandPolicy policy{simulator_limits()};
  auto s = ready();
  s.axes[0].machine = 999;
  require(!policy.evaluate(JogStep{Axis::x, 2, 100}, s).allowed,
          "increment cannot cross travel limit");
  require(policy.evaluate(JogStep{Axis::x, -2, 100}, s).allowed,
          "increment can move away from soft limit");
  s.axes[0].machine = 1000;
  require(!policy.evaluate(Jog{Axis::x, 1, 100, true}, s).allowed, "outward jog at limit rejected");
  require(policy.evaluate(Jog{Axis::x, -1, 100, true}, s).allowed, "inward jog at limit allowed");
  s.axes[0].on_hard_limit = true;
  require(!policy.evaluate(Jog{Axis::x, -1, 100, true}, s).allowed,
          "hard limit needs explicit recovery workflow");
  s = ready();
  s.in_position = false;
  require(!policy.evaluate(JogStep{Axis::x, 2, 100}, s).allowed,
          "incremental moves cannot accumulate during motion");
  require(policy.evaluate(Jog{Axis::x, 1, 100, true}, s).allowed,
          "held jog refresh remains valid while moving");
  require(policy.evaluate(SetSpindle{SpindleDirection::forward, 5000}, s).allowed,
          "spindle may start during a manual jog");
  require(policy.evaluate(Jog{Axis::x, 1, 100, true}, s).jog_space == JogSpace::axis,
          "homed jog requires axis-space teleop");
  s.axes[2].homed = false;
  require(policy.evaluate(Jog{Axis::x, 1, 100, true}, s).jog_space == JogSpace::joint,
          "partial homing keeps all jogs in joint space");
  s.axes[1].homing = true;
  require(!policy.evaluate(Jog{Axis::x, 1, 100, true}, s).allowed,
          "other axis homing prevents manual jog");
}
void rotary_homing_gates() {
  CommandPolicy policy{simulator_limits()};
  auto s = ready();
  auto& rotary = s.rotary_axes[1];
  rotary.enabled = true;
  const Jog jog{Axis::x, 1, 100, true};
  require(!policy.evaluate(Mdi{"G0 X10"}, s).allowed &&
              policy.evaluate(jog, s).jog_space == JogSpace::joint,
          "an unhomed B axis blocks MDI and keeps XYZ jogs in joint space");
  rotary.homing = true;
  require(!policy.evaluate(jog, s).allowed, "B homing blocks an XYZ jog");
  rotary.homing = false;
  rotary.homed = true;
  require(policy.evaluate(Mdi{"G0 X10"}, s).allowed &&
              policy.evaluate(jog, s).jog_space == JogSpace::axis,
          "homing B completes the homing requirements");
}

// Y driven by two motors, as on a gantry router: one joint alone would rack it.
void gantry_jogs() {
  Limits limits{3000, 1.2, 0.5, 1.2, 24000};
  limits.axis_motors = {1, 2, 1};
  CommandPolicy policy{limits};
  auto s = ready();
  for (auto& a : s.axes)
    a.homed = false;
  const auto y = policy.evaluate(Jog{Axis::y, 1, 100, true}, s);
  require(!y.allowed && y.reason == "Home the machine before jogging Y: two motors drive it.",
          "an unhomed two-motor axis does not jog in joint space: " + y.reason);
  require(policy.evaluate(JogStep{Axis::y, 1, 100}, s).reason == y.reason,
          "nor does it step");
  require(policy.evaluate(Jog{Axis::y, 1, 0, false}, s).allowed, "its release is never refused");
  require(policy.evaluate(Jog{Axis::x, 1, 100, true}, s).jog_space == JogSpace::joint &&
              policy.evaluate(JogStep{Axis::z, 1, 100}, s).jog_space == JogSpace::joint,
          "the one-motor axes jog their joints before homing");
  s.axes[1].homed = true;
  require(!policy.evaluate(Jog{Axis::y, 1, 100, true}, s).allowed,
          "Y homed with another axis unhomed is still joint space: refused");
  s = ready();
  require(policy.evaluate(Jog{Axis::y, 1, 100, true}, s).jog_space == JogSpace::axis,
          "homed, the axis jogs in world space and moves both motors");
}
void hostile_values() {
  CommandPolicy policy{simulator_limits()};
  const auto s = ready();
  for (const auto value : {std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity(), -1.0, 50000.0}) {
    require(!policy.evaluate(Jog{Axis::x, 1, value, true}, s).allowed,
            "invalid jog velocity rejected");
    require(!policy.evaluate(SetOverride{Override::rapid, value}, s).allowed,
            "invalid rapid factor rejected");
  }
  // Deliberately exercise rejection of an invalid decoded enum value.
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  require(!policy.evaluate(Jog{static_cast<Axis>(99), 1, 100, true}, s).allowed,
          "invalid axis is bounded");
  require(!policy.evaluate(Jog{Axis::x, 0, 100, true}, s).allowed, "zero direction rejected");
  require(!policy.evaluate(Mdi{"G0 X0\nM3"}, s).allowed, "MDI is a single block");
  require(!policy.evaluate(Mdi{std::string("G0 X0\0M3", 8)}, s).allowed,
          "MDI rejects embedded null");
  require(!policy.evaluate(SetTool{Tool{1, 1, 0, 6, "cutter\nT2 P2 Z0"}}, s).allowed,
          "tool comment cannot inject an extra row");
  require(!policy.evaluate(ToolTouchOff{0}, s).allowed, "tool length needs a loaded tool");
  require(!policy.evaluate(SetTool{Tool{}}, s).allowed,
          "unspecified tool dimensions remain invalid");
  bool invalid_limits = false;
  try {
    CommandPolicy invalid{Limits{0, 1.2, 0.5, 1.2, 24000}};
  } catch (const std::invalid_argument&) {
    invalid_limits = true;
  }
  require(invalid_limits, "invalid configuration fails construction");
  auto missing_limits = ready();
  missing_limits.axes[0].min_limit = std::numeric_limits<double>::quiet_NaN();
  require(!policy.evaluate(Jog{Axis::x, 1, 100, true}, missing_limits).allowed,
          "homed axis with missing travel limits cannot jog");
}
void independent_jog_leases() {
  JogLease leases;
  const JogLease::Time start{};
  leases.refresh(Axis::x, start);
  leases.refresh(Axis::y, start + 50ms);
  require(!leases.expired(start + 149ms)[0], "jog is held before timeout");
  const auto first = leases.expired(start + 150ms);
  require(first[0] && !first[1] && !first[2], "only expired axis stops at exactly 150 ms");
  require(!leases.expired(start + 151ms)[0], "expired lease produces one stop request");
  leases.refresh(Axis::y, start + 180ms);
  require(!leases.expired(start + 200ms)[1], "fresh hold extends only that axis lease");
  leases.refresh(Axis::z, start + 200ms);
  leases.release(Axis::z);
  require(!leases.active(Axis::z), "release clears its own lease");
  const auto cancelled = leases.cancel_all();
  require(!cancelled[0] && cancelled[1] && !cancelled[2],
          "disconnect cancels remaining active jogs");
  require(!leases.active(Axis::y), "new panel starts without old held jog");
  require(!leases.expired(start + 10s)[1], "cancel does not replay old jog intent");
}

void task_heartbeat() {
  TaskHeartbeat heartbeat;
  const TaskHeartbeat::Clock::time_point start{};
  require(heartbeat.observe(7, start) == Link::stale, "one reading proves nothing");
  require(heartbeat.observe(7, start + 10ms) == Link::stale,
          "an unchanged counter is not a running task");
  require(heartbeat.observe(8, start + 20ms) == Link::live, "an advancing counter is");
  require(heartbeat.observe(8, start + 1019ms) == Link::live,
          "live for a second after the last write");
  require(heartbeat.observe(8, start + 1020ms) == Link::stale, "stale from then on");
  require(heartbeat.observe(9, start + 1030ms) == Link::live, "task writes again");
  require(heartbeat.observe(10, start + 5s) == Link::stale,
          "an advance since a reading seconds ago may be seconds old");
  require(heartbeat.observe(11, start + 5s + 10ms) == Link::live, "the next reading settles it");
}

// An E-stop request is answered by the state reported after it, in these words.
void estop_request_results() {
  const std::string unreadable = "LinuxCNC's status cannot be read; E-stop was not confirmed.";
  // A state from a status that is not evidence confirms and names nothing: a stale or absent
  // link, or a status from before the command, whatever state is still readable in it.
  for (const auto state : {MachineState::estop, MachineState::off, MachineState::on}) {
    for (const auto link : {Link::stale, Link::down}) {
      const auto stale = estop_request_result(state, link, true);
      require(stale.status == ActionStatus::failed && stale.reason == unreadable,
              "a state of a link that is not live is no evidence");
    }
    const auto earlier = estop_request_result(state, Link::live, false);
    require(earlier.status == ActionStatus::failed && earlier.reason == unreadable,
            "a state published before the command is no evidence");
  }
  const auto confirmed = estop_request_result(MachineState::estop, Link::live, true);
  require(confirmed.status == ActionStatus::accepted && confirmed.reason.empty(),
          "E-stop reported: accepted");
  const auto off = estop_request_result(MachineState::off, Link::live, true);
  require(off.status == ActionStatus::failed &&
              off.reason == "LinuxCNC reports machine off; E-stop was not confirmed.",
          "off reported: failed, and says off");
  const auto on = estop_request_result(MachineState::on, Link::live, true);
  require(on.status == ActionStatus::failed &&
              on.reason == "LinuxCNC still reports machine on; E-stop was not confirmed.",
          "on reported: failed, and says still on");
  const auto unread = estop_request_result(std::nullopt, Link::live, true);
  require(unread.status == ActionStatus::failed && unread.reason == unreadable,
          "no status: failed, and says so");
}

// What follows a G10 L1 of a tool length so that the offset in effect follows the table (#287).
void tool_length_follow_up() {
  const auto follows = [](int written, int loaded, const std::array<double, 3>& offset,
                          std::optional<double> loaded_length) {
    return after_tool_length_write(written, loaded, {offset, loaded_length});
  };
  // The loaded tool's length in effect (G43 H<tool>): G43 H<tool> follows.
  require(follows(3, 3, {0, 0, 48.2}, 48.2) == "G43 H3", "G43 of the loaded tool: G43 H3");
  require(follows(3, 3, {0, 0, 48.2005}, 48.2) == "G43 H3",
          "within 0.001 mm is the loaded tool's length");
  require(follows(3, 3, {0, 0, -12.5}, -12.5) == "G43 H3", "a negative length");
  // Another offset: another H, G43.1 with its own value, a G43.2 sum. It stays.
  require(!follows(3, 3, {0, 0, 40}, 48.2), "another H: nothing");
  require(!follows(3, 3, {0, 0, 88.2}, 48.2), "a G43.2 sum that contains the length: nothing");
  require(!follows(3, 3, {0, 0, 48.202}, 48.2), "beyond 0.001 mm: nothing");
  require(!follows(3, 3, {1, 0, 48.2}, 48.2), "an X offset (G43.1 X): nothing");
  require(!follows(3, 3, {0, -1, 48.2}, 48.2), "a Y offset: nothing");
  // No offset in effect (G49, or a zero length, which LinuxCNC reports as G49).
  require(!follows(3, 3, {0, 0, 0}, 48.2), "G49: nothing");
  require(!follows(3, 3, {0, 0, 0}, 0), "a zero length reads as G49: nothing");
  require(!follows(3, 3, {0, 0, 48.2}, std::nullopt), "no row for the loaded tool: nothing");
  // Another tool than the loaded one, or an empty spindle.
  require(!follows(2, 3, {0, 0, 48.2}, 48.2), "another tool: nothing");
  require(!follows(0, 0, {0, 0, 48.2}, 48.2), "empty spindle: nothing");
}

// Motion's refusal of a start under motion.jog-inhibit, as the port's reason carries it.
void jog_inhibit_refusal() {
  require(is_jog_inhibit_refusal("Cannot jog while jog-inhibit is active."), "LinuxCNC's text");
  require(is_jog_inhibit_refusal("Cannot jog while jog-inhibit is active"), "without the period");
  require(is_jog_inhibit_refusal(
              "Cannot jog while jog-inhibit is active.; Cannot jog while jog-inhibit is active."),
          "published twice for one start");
  require(!is_jog_inhibit_refusal(""), "no reason");
  require(!is_jog_inhibit_refusal("Cannot jog while jog-inhibit is active.; Joint 0 following "
                                  "error."),
          "with another error: not only the inhibit");
  require(!is_jog_inhibit_refusal("Jog aborted by jog-stop"), "another text");
  require(!is_jog_inhibit_refusal("cannot jog while jog-inhibit is active."), "exact text only");
}

// Whose stop a "Jog aborted by jog-stop" text reports.
void jog_stop_attribution() {
  using Time = std::chrono::steady_clock::time_point;
  const Time read{std::chrono::hours{1}};
  const std::string stop = "Jog aborted by jog-stop";
  const std::string immediate = "Jog aborted by jog-stop-immediate";
  const std::optional<Time> never;
  require(classify_jog_stop(stop, read, read - 12ms, never) == JogStopCause::own,
          "own level high 12 ms before the read: own");
  require(classify_jog_stop(immediate, read, read - 12ms, never) == JogStopCause::not_jog_stop,
          "the immediate text is not ours: own level high in the window, passed on unchanged");
  require(classify_jog_stop(immediate, read, read - 12ms, read - 5ms) ==
              JogStopCause::not_jog_stop,
          "the immediate text is not the watchdog's either: passed on unchanged");
  require(classify_jog_stop(stop, read, read - 12ms, read - 5ms) == JogStopCause::watchdog,
          "a watchdog seen tripped in the window wins over the own level of the same stall");
  require(classify_jog_stop(stop, read, read - 12ms, read - jog_stop_window - 1ns) ==
              JogStopCause::own,
          "a trip before the window does not take the own level's text");
  require(classify_jog_stop(stop, read, read + 1ms, never) == JogStopCause::own,
          "a level still high right after the read: own");
  require(classify_jog_stop(stop, read, read - jog_stop_window, never) == JogStopCause::own,
          "window boundary: high exactly jog_stop_window before the read is own");
  require(classify_jog_stop(stop, read, read - jog_stop_window - 1ns, never) ==
              JogStopCause::unattributed,
          "window boundary: high just before the window is not own");
  require(classify_jog_stop(stop, read, read - 1s, read + 8ms) == JogStopCause::watchdog,
          "no own level in the window, watchdog seen tripped after the read: watchdog");
  require(classify_jog_stop(stop, read, never, read - jog_stop_window) == JogStopCause::watchdog,
          "watchdog seen tripped exactly jog_stop_window before: watchdog");
  require(classify_jog_stop(stop, read, never, read - jog_stop_window - 1ns) ==
              JogStopCause::unattributed,
          "watchdog seen tripped only before the window: unattributed");
  require(classify_jog_stop(stop, read, read - 1s, never) == JogStopCause::unattributed,
          "no own level in the window, watchdog fine: unattributed, the text unchanged");
  require(classify_jog_stop(stop, read, never, never) == JogStopCause::unattributed,
          "no history at all: unattributed");
  require(classify_jog_stop(stop, read, read - 10ms, never, read + 5ms) ==
              JogStopCause::probe_contact,
          "the contact can be sampled after the stop text and during the app's stop response");
  require(classify_jog_stop(stop, read, never, read - 2ms, read - 10ms) ==
              JogStopCause::watchdog,
          "an actual watchdog trip takes precedence over a recent contact");
  require(classify_jog_stop(stop, read, never, never, read - jog_stop_window - 1ns) ==
              JogStopCause::unattributed,
          "an earlier contact does not explain a subsequent stop");
  for (const std::string other :
       {"Cannot jog while jog-inhibit is active", "Jog aborted by jog-stop.",
        "jog aborted by jog-stop", "Joint 0 following error", ""})
    require(classify_jog_stop(other, read, read, read) == JogStopCause::not_jog_stop &&
                !is_jog_stop_text(other),
            "another text is not a jog-stop text: " + other);
  require(watchdog_stop_text.find("watchdog") != std::string_view::npos &&
              watchdog_stop_text.find("Jog aborted") == std::string_view::npos,
          "the watchdog text is CoffeeCNC's own");
}

} // namespace

auto main() -> int {
  try {
    execution_gates();
    pending_manual_tool_change();
    reference_mark_homing();
    runout_correction();
    search_line_heading();
    runout_travel();
    height_map_grid();
    fixed_setter_homing();
    edge_rotation_wraps();
    probe_modal_restore();
    arc_geometry();
    circle_verification();
    work_origin_gates();
    rotation_and_shift_gates();
    paused_z_gates();
    homing_capability();
    axis_jog_ceilings();
    stops_survive_lost_state();
    jog_constraints();
    rotary_homing_gates();
    gantry_jogs();
    hostile_values();
    independent_jog_leases();
    task_heartbeat();
    jog_stop_attribution();
    jog_inhibit_refusal();
    estop_request_results();
    tool_length_follow_up();
    std::cout << checks << " core behavior checks passed\n";
  } catch (const std::exception& error) {
    std::cerr << "Check " << checks << " failed: " << error.what() << '\n';
    return 1;
  }
}
