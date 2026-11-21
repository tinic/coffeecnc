// The preview engine with the real helper and LinuxCNC's interpreter, on programs the test
// writes. No LinuxCNC runs: the tool table and every input are the test's own.
//   coffeecnc_preview_engine_tests <coffeecnc-preview> <scratch directory>

#include <coffeecnc/linuxcnc/preview_engine.hpp>
#include <coffeecnc/core/text_file.hpp>

#include "../src/linuxcnc/src/preview_wire.hpp"
#include "../src/linuxcnc/src/program_watch.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <signal.h> // kill: POSIX, not in <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace coffeecnc;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

int checks = 0;
std::string helper;
fs::path scratch;

void require(bool condition, const std::string& message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

void write(const fs::path& file, std::string_view text) {
  fs::create_directories(file.parent_path());
  std::ofstream out{file, std::ios::binary | std::ios::trunc};
  out << text;
}

auto near(double a, double b, double tolerance = 1e-4) -> bool {
  return std::fabs(a - b) <= tolerance;
}
auto near(const Vec3& a, const Vec3& b, double tolerance = 1e-4) -> bool {
  return near(a[0], b[0], tolerance) && near(a[1], b[1], tolerance) && near(a[2], b[2], tolerance);
}

auto text_of(const PreviewOutcome& outcome) -> std::string {
  std::string text = outcome.state == PreviewState::ready     ? "ready"
                     : outcome.state == PreviewState::failed  ? "failed"
                     : outcome.state == PreviewState::pending ? "pending"
                                                              : "unavailable";
  if (outcome.error)
    text += std::format(" line={} text={}", outcome.error->line, outcome.error->text);
  for (const auto& segment : outcome.segments) {
    text += std::format(" [{} {} t{}", segment.line, segment.kind == MoveKind::feed ? 'f' : 'r',
                        segment.tool);
    if (segment.points.size() <= 4)
      for (const auto& p : segment.points)
        text += std::format(" ({:.3f},{:.3f},{:.3f})", p[0], p[1], p[2]);
    else
      text += std::format(" {} points", segment.points.size());
    text += "]";
  }
  return text;
}

auto points_of(const PreviewOutcome& outcome) -> std::size_t {
  return std::accumulate(outcome.segments.begin(), outcome.segments.end(), std::size_t{0},
                         [](std::size_t sum, const PreviewSegment& segment) -> std::size_t {
                           return sum + segment.points.size();
                         });
}

// A machine of the test's own in a directory of its own.
struct Rig {
  fs::path root;
  PreviewSettings settings;
  PreviewInputs inputs;
  HelperPreviewEngine::AfterSnapshot after_snapshot;
  std::string diagnostics;
  std::size_t runs{};
  std::size_t lookups{}; // of subroutine files, by the last computation

  explicit Rig(std::string_view name) : root(scratch / name) {
    fs::remove_all(root);
    fs::create_directories(root / "config");
    fs::create_directories(root / "nc");
    fs::create_directories(root / "subs");
    settings.helper = helper;
    settings.work_directory = (root / "work").string();
    settings.search_path = {(root / "config").string(), (root / "nc").string(),
                            (root / "subs").string()};
    inputs.program_file = (root / "nc" / "part.ngc").string();
    inputs.program_revision = 1;
    inputs.systems.at(0).available = true;
    inputs.raw_tools = {{1, 1, 1, {0, 0, 50}, 6, 0, 0, 0}, {2, 2, 2, {0, 0, 40}, 3, 0, 0, 0}};
    inputs.start.active_codes = {"G80", "G17", "G40", "G21", "G90", "G94", "G54", "G49", "G99"};
  }

  auto compute(const std::function<bool()>& cancelled = {}) -> PreviewComputation {
    HelperPreviewEngine engine{settings, after_snapshot};
    auto computed = engine.compute(inputs, cancelled);
    diagnostics = engine.diagnostics();
    runs = engine.helper_runs();
    lookups = engine.file_lookups();
    return computed;
  }
  // The program file of the inputs.
  [[nodiscard]] auto file() const -> std::string { return inputs.program_file.value_or(""); }
  // The preview of a program text.
  auto preview(std::string_view program) -> PreviewOutcome {
    write(file(), program);
    return compute().outcome;
  }
  // Nothing of a computation stays in the work directory.
  [[nodiscard]] auto work_is_empty() const -> bool {
    return !fs::exists(root / "work") || fs::is_empty(root / "work");
  }
};

void lines_and_bounds() {
  Rig rig{"lines"};
  rig.inputs.start.position = {1, 2, 3};
  const auto o = rig.preview("G21 G90\nG0 X10 Y5 Z2\nG1 X20 F100\nG1 Y15 Z-1\nM2\n");
  require(o.state == PreviewState::ready && !o.error && o.segments.size() == 3, text_of(o));
  require(o.segments[0].line == 2 && o.segments[0].kind == MoveKind::rapid &&
              near(o.segments[0].points.front(), {1, 2, 3}) &&
              near(o.segments[0].points.back(), {10, 5, 2}),
          "The first move starts at the machine's position: " + text_of(o));
  require(o.segments[1].line == 3 && o.segments[1].kind == MoveKind::feed &&
              near(o.segments[1].points.back(), {20, 5, 2}) && o.segments[2].line == 4,
          "Feed moves carry their line: " + text_of(o));
  require(o.segments[0].feed == 0 && o.segments[1].feed == 100 && o.segments[2].feed == 100,
          "Feed moves carry their feed per minute, rapids none: " + text_of(o));
  const auto timed = rig.preview("G21 G90\nG1 X1 F250\nG4 P2.5\nG20\nG1 X1 F10\nS1000 M3\nG95 G1 X2 F0.1\nM5\nM2\n");
  require(timed.state == PreviewState::ready && timed.segments.size() == 3 &&
              std::abs(timed.segments[0].feed - 250) < 1e-6 &&
              std::abs(timed.segments[1].feed - 254) < 1e-6 && timed.segments[2].feed == 0 &&
              timed.dwells == std::vector<PreviewDwell>{{3, 2.5}},
          "Feeds in mm/min, inches converted, per-revolution unknown, and the dwell: " +
              text_of(timed));
  require(o.bounds && near(o.bounds->min, {10, 5, -1}) && near(o.bounds->max, {20, 15, 2}),
          "Bounds are of the feed moves only");
  require(rig.work_is_empty(), "The private directory is removed");
}

void restart_checkpoints() {
  Rig rig{"restart"};
  rig.inputs.start.position = {0, 0, 70};
  rig.inputs.raw_tools.push_back({98, 98, 98, {0, 0, 35}, 3, 0, 0, 0});
  const fs::path samples{COFFEECNC_SAMPLES_DIRECTORY};
  rig.inputs.program_file = (samples / "air/03-pocket.ngc").string();
  rig.inputs.restart_line = 24;
  auto outcome = rig.compute().outcome;
  require(outcome.state == PreviewState::ready && outcome.restart,
          "Second pocket level: " + outcome.restart_blocked + " " + text_of(outcome));
  const auto& pocket = *outcome.restart;
  require(pocket.tool == 1 && pocket.h == 1 && pocket.wcs == 1 &&
              near(pocket.spindle_rpm, 1500) && pocket.spindle_direction == SpindleDirection::forward &&
              near(pocket.feed, 360) && near(pocket.clearance_z, 30) &&
              near(pocket.start, {24, 20, 15}),
          "Pocket restart restores the modal state before the second depth block");

  rig.inputs.program_file = (samples / "air/06-toolchange.ngc").string();
  rig.inputs.restart_line = 23;
  outcome = rig.compute().outcome;
  require(outcome.state == PreviewState::ready && outcome.restart &&
              outcome.restart->tool == 98 && outcome.restart->h == 98 &&
              outcome.restart->path_control == 610 &&
              outcome.restart->spindle_direction == SpindleDirection::stopped &&
              near(outcome.restart->start, {0, 0, 30}),
          "Restart in the T98 section: " + outcome.restart_blocked + " " + text_of(outcome));

  rig.inputs.program_file = (rig.root / "nc" / "part.ngc").string();
  rig.inputs.restart_line = 3;
  outcome = rig.preview("G21 G90 G17\nG0 X10 Y0 Z1\nG2 X0 Y-10 I-10 J0 F100\nM2\n");
  require(outcome.state == PreviewState::ready && outcome.restart &&
              near(outcome.restart->start, {10, 0, 1}),
          "A complete arc can restart from its interpreted start: " + outcome.restart_blocked);
  rig.inputs.restart_line = 4;
  outcome = rig.preview("G21 G90 G17\nG0 X10 Y0 Z1\nG2 X0 Y-10 I-10 J0 F100\n"
                        "X-10 Y0 I0 J10\nM2\n");
  require(outcome.state == PreviewState::ready && !outcome.restart &&
              outcome.restart_blocked.find("explicit G2 or G3") != std::string::npos,
          "Inherited arcs block restart: " + outcome.restart_blocked);
  rig.inputs.restart_line = 5;
  outcome = rig.preview("G21 G90 G17\nT1 M6\nG0 X-10 Y0 Z3\nG41 D1 G1 X0 F100\n"
                        "G1 X10\nG40 G1 X20\nM2\n");
  require(outcome.state == PreviewState::ready && !outcome.restart &&
              outcome.restart_blocked.find("Cutter compensation") != std::string::npos,
          "Cutter compensation blocks restart: " + outcome.restart_blocked);
  rig.inputs.restart_line = 3;
  outcome = rig.preview("G21 G90 G17\nG0 Z10\nG81 X1 Y1 Z-2 R3 F100\nG80\nM2\n");
  require(outcome.state == PreviewState::ready && !outcome.restart &&
              outcome.restart_blocked.find("canned cycle") != std::string::npos,
          "Canned cycles block restart: " + outcome.restart_blocked);
  outcome = rig.preview("G21 G90 G17\nT1 M6\nG0 Z10\nM2\n");
  require(outcome.state == PreviewState::ready && !outcome.restart &&
              outcome.restart_blocked.find("tool length") != std::string::npos,
          "Missing tool length blocks restart: " + outcome.restart_blocked);

  rig.inputs.restart_line = 6;
  const std::string prefix = "G21 G90 G17\nT1 M6\nG43 H1\nG0 Z20\nT2\nG1 X10 F100\n";
  outcome = rig.preview(prefix + "M6\nG43 H2\nG0 Z20\nM2\n");
  require(outcome.state == PreviewState::ready && !outcome.restart &&
              outcome.restart_blocked.find("A later M6") != std::string::npos,
          "M6 cannot inherit a discarded tool preparation: " + outcome.restart_blocked);
  outcome = rig.preview(prefix + "T2 M6\nG43 H2\nG0 Z20\nM2\n");
  require(outcome.state == PreviewState::ready && outcome.restart,
          "An explicit T word in the suffix prepares its own M6: " + outcome.restart_blocked);
}

void program_check_touched_lengths_and_travel() {
  Rig rig{"program-check"};
  const auto table = rig.root / "config" / "tool.tbl";
  const auto marks = rig.root / "data" / "runtime" / "coffeecnc-touched-off.txt";
  rig.settings.tool_table = table.string();
  rig.settings.reference_tool = 98;
  rig.settings.touched_off_file = marks.string();
  rig.settings.max_spindle_rpm = 1500;
  rig.settings.soft_travel = Bounds{{-1000, -1000, -1000}, {1000, 1000, 1000}};
  rig.settings.physical_travel = Bounds{{-40, -40, -40}, {40, 40, 40}};
  rig.inputs.raw_tools[1].offset[2] = 0;
  rig.inputs.raw_tools.push_back({3, 98, 98, {0, 0, 25.4}, 3, 0, 0, 0});
  write(table, "T1 P1 Z50 D6\nT2 P2 Z0 D3\nT98 P98 Z25.4 D3\n");
  write(marks, "reference 25.4\nT2 0\n");
  write(table.parent_path() / marks.filename(), "reference 25.4\nT2 0\n");
  write(rig.file(), "G20 G90 G17\nT2 M6\nG43 H2\nS1200 M3\nG1 X1 F10\nM5\nM2\n");
  auto result = rig.compute();
  require(result.outcome.check.has_value(), "The interpreter reports program check facts.");
  const auto& check = *result.outcome.check;
  require(check.errors.empty() && check.warnings.empty() && check.travel == TravelCheck::fits &&
              check.tools == std::vector<ProgramToolCheck>{{2, ToolLengthCheck::touched_off}} &&
              check.units == std::vector<LengthUnit>{LengthUnit::inch} &&
              check.feed && near(check.feed->min, 254) &&
              check.spindle == ProgramRange{1200, 1200},
          "A touched-off zero length is valid. Feed is converted to mm/min.");
  write(marks, "reference 5\nT2 0\n");
  require(result.files_changed && result.files_changed({}),
          "Changing only touch-off marks invalidates the displayed check.");
  const auto rejected = rig.compute().outcome.check;
  require(rejected && !rejected->errors.empty() &&
              rejected->tools.front().length == ToolLengthCheck::missing,
          "The old mark no longer validates the zero length.");
  const auto outside = rig.preview("G21 G90\nG0 X50\nM2\n").check;
  require(outside && outside->travel == TravelCheck::outside && !outside->errors.empty(),
          "A move inside LinuxCNC soft limits can exceed the verified physical envelope.");
}

// Every chord of an arc is within the tolerance of the circle.
void require_arc(const PreviewSegment& segment, std::size_t first, std::size_t second,
                 const Vec3& centre, double radius, const std::string& what) {
  require(segment.points.size() > 8, what + ": an arc is many chords");
  for (std::size_t i = 0; i < segment.points.size(); ++i) {
    const auto& p = segment.points[i];
    const double r = std::hypot(p.at(first) - centre.at(first), p.at(second) - centre.at(second));
    require(near(r, radius, 1e-4), std::format("{}: point {} is on the circle ({})", what, i, r));
    if (i == 0)
      continue;
    const auto& q = segment.points[i - 1];
    const double mx = (p.at(first) + q.at(first)) / 2 - centre.at(first);
    const double my = (p.at(second) + q.at(second)) / 2 - centre.at(second);
    require(radius - std::hypot(mx, my) <= 0.01 + 1e-6,
            std::format("{}: chord {} is within 0.01 mm of the arc", what, i));
  }
}

void arcs_in_three_planes() {
  Rig rig{"arcs"};
  // XY: clockwise quarter from (10,0) about the origin to (0,-10).
  auto o = rig.preview("G21 G90 G17\nG0 X10 Y0 Z1\nG2 X0 Y-10 I-10 J0 F100\nM2\n");
  require(o.state == PreviewState::ready && o.segments.size() == 2, text_of(o));
  require_arc(o.segments[1], 0, 1, {0, 0, 1}, 10, "XY");
  const auto& xy = o.segments[1].points;
  const auto& middle = xy[xy.size() / 2];
  require(near(xy.back(), {0, -10, 1}) && middle[0] > 0 && middle[1] < 0 && near(middle[2], 1),
          "G2 in XY goes clockwise through the fourth quadrant: " + text_of(o));
  require(xy.size() < 200, "And not in more chords than the tolerance asks for");

  // XZ (G18) and YZ (G19): the arc stays in its plane and ends where it is told.
  o = rig.preview("G21 G90 G18\nG0 X10 Y3 Z0\nG3 X0 Z10 I-10 K0 F100\nM2\n");
  require(o.state == PreviewState::ready && o.segments.size() == 2, text_of(o));
  require_arc(o.segments[1], 0, 2, {0, 3, 0}, 10, "XZ");
  require(near(o.segments[1].points.back(), {0, 3, 10}) &&
              std::ranges::all_of(o.segments[1].points,
                                  [](const Vec3& p) -> bool { return near(p[1], 3); }),
          "An arc in G18 keeps Y: " + text_of(o));
  o = rig.preview("G21 G90 G19\nG0 X4 Y10 Z0\nG2 Y0 Z10 J-10 K0 F100\nM2\n");
  require(o.state == PreviewState::ready && o.segments.size() == 2, text_of(o));
  require_arc(o.segments[1], 1, 2, {4, 0, 0}, 10, "YZ");
  require(near(o.segments[1].points.back(), {4, 0, 10}) &&
              std::ranges::all_of(o.segments[1].points,
                                  [](const Vec3& p) -> bool { return near(p[0], 4); }),
          "An arc in G19 keeps X: " + text_of(o));

  // A helix: one full turn, Z going down evenly.
  o = rig.preview("G21 G90 G17\nG0 X10 Y0 Z0\nG3 X10 Y0 I-10 J0 Z-5 F100\nM2\n");
  require(o.state == PreviewState::ready && o.segments.size() == 2, text_of(o));
  require_arc(o.segments[1], 0, 1, {0, 0, 0}, 10, "helix");
  const auto& helix = o.segments[1].points;
  bool descending = true;
  for (std::size_t i = 1; i < helix.size(); ++i)
    descending = descending && helix[i][2] < helix[i - 1][2];
  require(descending && near(helix.back(), {10, 0, -5}) && helix.size() > 60 &&
              near(helix[helix.size() / 2][0], -10, 0.5),
          "A full turn with Z is a helix through the far side");
}

void units() {
  Rig rig{"units"};
  auto o = rig.preview("G20 G90\nG1 X1 Y0.5 F10\nG21\nG1 X10\nM2\n");
  require(o.state == PreviewState::ready && o.segments.size() == 2 &&
              near(o.segments[0].points.back(), {25.4, 12.7, 0}) &&
              near(o.segments[1].points.back(), {10, 12.7, 0}),
          "G20 and G21 in one program, in millimetres: " + text_of(o));

  // An inch machine: tool table and parameter file are in inches, the path is in millimetres.
  Rig inch{"units_inch"};
  inch.settings.units_per_mm = 1 / 25.4;
  inch.inputs.raw_tools = {{1, 1, 1, {0, 0, 2.0}, 0.25, 0, 0, 0}};
  inch.inputs.start.active_codes = {"G80", "G17", "G40", "G20", "G90", "G94", "G54", "G49"};
  inch.inputs.start.program_units = LengthUnit::inch;
  inch.inputs.systems.at(0) = WorkSystem{{25.4, 50.8, 0}, 0, true};
  o = inch.preview("G1 X1 F10\nG43 H1\nG1 Z0.5\nG21\nG1 X0\nM2\n");
  require(o.state == PreviewState::ready && o.segments.size() == 3, text_of(o));
  require(near(o.segments[0].points.back(), {50.8, 0, 0}),
          "With G20 active at the start the first block is inches, in the work system: " + text_of(o));
  require(near(o.segments[1].points.front(), {50.8, 0, -50.8}) &&
              near(o.segments[1].points.back(), {50.8, 0, 12.7}),
          "A tool length of 2 inches lowers the tip by 50.8 mm: " + text_of(o));
  require(near(o.segments[2].points.back(), {25.4, 0, 12.7}), "G21 on an inch machine");

  // G10 L1 in the other unit: the interpreter hands SET_TOOL_TABLE_ENTRY the machine's unit
  // (PROGRAM_TO_USER_LEN, interp_convert.cc), the unit the helper's table is in.
  o = rig.preview("G20 G90\nG1 X1 F10\nG10 L1 P1 Z2\nG43 H1\nG1 Z0.5\nM2\n");
  require(o.state == PreviewState::ready && o.segments.size() == 2 &&
              near(o.segments[1].points.front(), {25.4, 0, -50.8}) &&
              near(o.segments[1].points.back(), {25.4, 0, 12.7}),
          "G10 L1 Z2 in an inch program on a millimetre machine is 50.8 mm: " + text_of(o));
  o = inch.preview("G21 G90\nG1 X10 F100\nG10 L1 P1 Z25\nG43 H1\nG1 Z5\nM2\n");
  require(o.state == PreviewState::ready && o.segments.size() == 2 &&
              near(o.segments[1].points.front(), {35.4, 0, -25}) &&
              near(o.segments[1].points.back(), {35.4, 0, 5}),
          "G10 L1 Z25 in a millimetre program on an inch machine is 25 mm: " + text_of(o));
}

void offsets_rotation_and_g92() {
  Rig rig{"offsets"};
  rig.inputs.systems.at(0) = WorkSystem{{100, 50, 10}, 90, true};
  rig.inputs.start.position = {100, 50, 10};
  auto o = rig.preview("G21 G90\nG1 X10 Y0 Z0 F100\nG1 X10 Y5\nM2\n");
  require(o.state == PreviewState::ready && o.segments.size() == 2 &&
              near(o.segments[0].points.front(), {100, 50, 10}) &&
              near(o.segments[0].points.back(), {100, 60, 10}) &&
              near(o.segments[1].points.back(), {95, 60, 10}),
          "Work offset and a rotation of 90 degrees: X of the program is Y of the machine: " +
              text_of(o));

  rig.inputs.g92 = {1, 2, 3};
  o = rig.preview("G21 G90\nG1 X10 Y0 Z0 F100\nM2\n");
  require(o.state == PreviewState::ready &&
              near(o.segments[0].points.back(), {100 - 2, 50 + 11, 13}),
          "G92 is added before the rotation: " + text_of(o));

  // Another work system: its offsets are whatever the file saved, not known live.
  rig.inputs.g92 = {};
  o = rig.preview("G21 G90\nG1 X1 F100\nG55\nG1 X2\nM2\n");
  require(o.state == PreviewState::failed && o.error && o.error->line == 4 &&
              o.error->text.find("G55") != std::string::npos && o.segments.size() == 1,
          "A move in another work system ends the preview at that line: " + text_of(o));
  // G54 active at the start is G54 at M2: no stop; and with G55 active, M2's return to G54
  // moves nothing.
  rig.inputs.start.wcs = 2;
  rig.inputs.systems.at(0).available = false;
  rig.inputs.systems.at(1) = WorkSystem{{7, 0, 0}, 0, true};
  rig.inputs.start.active_codes = {"G80", "G17", "G21", "G90", "G94", "G55"};
  o = rig.preview("G21 G90\nG1 X1 F100\nM2\n");
  require(o.state == PreviewState::ready && near(o.segments[0].points.back(), {8, 50, 10}),
          "G55 active at the start: its live offset is used: " + text_of(o));
  rig.inputs.systems.at(1).available = false;
  require(rig.preview("G1 X1 F100\nM2\n").state == PreviewState::unavailable,
          "Without the offsets of the active system no preview is computed");
}

void tool_length_and_tool_change() {
  Rig rig{"tools"};
  rig.inputs.start.tool_in_spindle = 1;
  auto o = rig.preview("G21 G90\nG1 X5 F100\nG43\nG1 Z10\nT2 M6\nG43\nG1 Z0\nG49\nG1 Z1\nM2\n");
  require(o.state == PreviewState::ready && o.segments.size() == 4, text_of(o));
  require(o.segments[0].tool == 1 && near(o.segments[0].points.back(), {5, 0, 0}) &&
              near(o.segments[1].points.front(), {5, 0, -50}) &&
              near(o.segments[1].points.back(), {5, 0, 10}),
          "G43 applies the length of the tool in the spindle, once: " + text_of(o));
  require(o.segments[2].tool == 2 && o.segments[2].line == 7 &&
              near(o.segments[2].points.front(), {5, 0, 20}) &&
              near(o.segments[2].points.back(), {5, 0, 0}),
          "After T2 M6 and G43 the tool is 2 and its length 40: " + text_of(o));
  require(near(o.segments[3].points.front(), {5, 0, 40}) &&
              near(o.segments[3].points.back(), {5, 0, 1}),
          "G49 takes the length off: " + text_of(o));

  // Every field of the table that was given is used: an X offset of the tool.
  rig.inputs.raw_tools[0].offset = {0.5, 0, 50};
  o = rig.preview("G21 G90\nG1 X5 F100\nG43 H1\nG1 X5 Z10\nM2\n");
  require(o.state == PreviewState::ready && near(o.segments[1].points.front(), {4.5, 0, -50}) &&
              near(o.segments[1].points.back(), {5, 0, 10}),
          "The tool's X offset as the inputs have it: " + text_of(o));
  rig.inputs.raw_tools[0].offset = {0, 0, 50};

  // The offset in effect at the start is the machine's, as given.
  rig.inputs.start.tool_offset = {0, 0, 50};
  rig.inputs.start.position = {0, 0, 60};
  o = rig.preview("G21 G90\nG1 Z0 F100\nM2\n");
  require(o.state == PreviewState::ready && near(o.segments[0].points.front(), {0, 0, 10}) &&
              near(o.segments[0].points.back(), {0, 0, 0}),
          "With a tool offset in effect at the start the tip is below the controlled point: " +
              text_of(o));

  // [EMCIO] TOOL_CHANGE_POSITION: task goes there first.
  rig.inputs.start.tool_offset = {};
  rig.inputs.start.position = {};
  rig.settings.tool_change_position = Vec3{0, 0, 100};
  o = rig.preview("G21 G90\nG1 X5 F100\nT2 M6\nG1 X6\nM2\n");
  require(o.state == PreviewState::ready && o.segments.size() == 3 &&
              o.segments[1].kind == MoveKind::rapid && o.segments[1].line == 3 &&
              near(o.segments[1].points.back(), {0, 0, 100}) && o.segments[2].tool == 2,
          "A tool change goes to the tool change position: " + text_of(o));
  rig.settings.tool_change_at_g30 = true;
  o = rig.preview("G21 G90\nG1 X5 F100\nT2 M6\nG1 X6\nM2\n");
  require(o.state == PreviewState::failed && o.error && o.error->line == 3 &&
              o.segments.size() == 1,
          "A tool change that goes to the saved G30 position ends the preview: " + text_of(o));
}

void subroutines() {
  Rig rig{"subs"};
  write(rig.root / "subs" / "square.ngc",
        "o<square> sub\nG91\nG1 X5 F100\nG1 Y5\nG1 X-5\nG1 Y-5\nG90\no<square> endsub\nM2\n");
  auto o = rig.preview("G21 G90\nG0 X10 Y10\no<square> call\nG0 X0\no100 sub\nG1 Z-1 F50\n"
                       "o100 endsub\no100 call\no200 repeat [2]\nG91 G1 X1\nG90\no200 endrepeat\n"
                       "M2\n");
  require(o.state == PreviewState::ready, text_of(o));
  std::vector<int> lines;
  std::ranges::transform(o.segments, std::back_inserter(lines), &PreviewSegment::line);
  require(lines == std::vector<int>{2, 3, 4, 6, 10},
          "Moves of a subroutine file carry the line of the call, those of a subroutine in the "
          "program their own line: " + text_of(o));
  require(o.segments[1].points.size() == 5 && near(o.segments[1].points[2], {15, 15, 0}) &&
              near(o.segments[1].points.back(), {10, 10, 0}),
          "A subroutine file without parameters is previewed: " + text_of(o));
  require(near(o.segments.back().points.back(), {2, 10, -1}), "Flow control without parameters");

  // Two files deep: still the line of the program's call. An error in the inner file says
  // where it is, and carries the line of the program's call.
  write(rig.root / "subs" / "outer.ngc",
        "o<outer> sub\nG91 G1 X1 F100\no<inner> call\nG90\no<outer> endsub\nM2\n");
  write(rig.root / "subs" / "inner.ngc",
        "o<inner> sub\n(nothing)\nG91 G1 Y1 F100\nG1 Y\no<inner> endsub\nM2\n");
  o = rig.preview("G21 G90\nG0 X0 Y0\n\n\no<outer> call\nG1 X9\nM2\n");
  require(o.state == PreviewState::failed && o.error && o.error->line == 5 &&
              o.error->text.starts_with("inner.ngc line 4: ") && o.segments.size() == 2 &&
              o.segments[1].line == 5 && o.segments[1].points.size() == 3 &&
              near(o.segments[1].points.back(), {1, 1, 0}),
          "Two subroutine files deep: line of the outermost call, the file and its line in "
          "the text: " + text_of(o));
  write(rig.root / "subs" / "inner.ngc",
        "o<inner> sub\nG91 G1 Y1 F100\n#1=2\no<inner> endsub\nM2\n");
  o = rig.preview("G21 G90\nG0 X0 Y0\n\n\no<outer> call\nG1 X9\nM2\n");
  require(o.state == PreviewState::failed && o.error && o.error->line == 5 &&
              o.error->text.find("inner.ngc line 3: ") != std::string::npos,
          "A refused block in a subroutine file likewise: " + text_of(o));

  // A subroutine that is not found is the interpreter's error, at the call.
  o = rig.preview("G21 G90\nG1 X1 F100\no<nowhere> call\nM2\n");
  require(o.state == PreviewState::failed && o.error && o.error->line == 3 &&
              o.segments.size() == 1,
          "A missing subroutine file fails at its call: " + text_of(o));
}

void errors_and_limits() {
  Rig rig{"errors"};
  auto o = rig.preview("G21 G90\nG1 X5 F100\nG1 X\nG1 X9\nM2\n");
  require(o.state == PreviewState::failed && o.error && o.error->line == 3 &&
              !o.error->text.empty() && o.segments.size() == 1 && o.segments[0].line == 2,
          "A broken line fails with its number and the path before it: " + text_of(o));
  o = rig.preview("G21 G90\nG1 X5 F100\nG1 Y2\nG777 X1\n");
  require(o.state == PreviewState::failed && o.error && o.error->line == 4 &&
              o.segments.size() == 2,
          "An unknown code likewise: " + text_of(o));

  // A loop without end that moves: the point limit.
  rig.settings.max_points = 1000;
  o = rig.preview("G21 G91\no1 while [1]\nG1 X1 F100\nG1 Y1\no1 endwhile\nM2\n");
  const auto points = points_of(o);
  require(o.state == PreviewState::failed && o.error &&
              o.error->text.find("more than 1000 points") != std::string::npos &&
              points >= 1000 && points <= 1002,
          std::format("A loop that moves for ever is cut at the point limit ({} points): {}",
                      points, o.error ? o.error->text : ""));

  // One that does not move: the time limit, and the helper is gone.
  rig.settings.timeout = 1500ms;
  const auto begun = std::chrono::steady_clock::now();
  o = rig.preview("G21 G90\nG1 X3 F100\no1 while [1]\no1 endwhile\nM2\n");
  const auto took = std::chrono::steady_clock::now() - begun;
  require(o.state == PreviewState::failed && o.error &&
              o.error->text.find("longer than 1 s") != std::string::npos &&
              o.segments.size() == 1 && took > 1400ms && took < 4s,
          "A loop that never ends is ended after the time limit, with the path so far: " +
              text_of(o));
  require(rig.work_is_empty(), "And leaves nothing behind");

  // Given up when nobody wants it any more.
  rig.settings.timeout = 20s;
  write(rig.file(), "o1 while [1]\no1 endwhile\n");
  const auto asked = std::chrono::steady_clock::now();
  const auto computed = rig.compute([&]() -> bool {
    return std::chrono::steady_clock::now() - asked > 200ms;
  });
  require(computed.cancelled && std::chrono::steady_clock::now() - asked < 2s,
          "A cancelled computation ends its helper at once");
}

void helper_with_closed_standard_descriptors() {
  Rig rig{"closed-descriptors"};
  write(rig.file(), "G21 G90\nG0 X5\nM2\n");
  const pid_t child = ::fork();
  require(child >= 0, "Fork a private preview caller");
  if (child == 0) {
    ::close(STDIN_FILENO);
    ::close(STDOUT_FILENO);
    ::close(STDERR_FILENO);
    const auto result = rig.compute().outcome;
    ::_exit(result.state == PreviewState::ready && result.segments.size() == 1 ? 0 : 1);
  }
  int status = 0;
  require(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "The real helper returns its result when the caller has closed standard descriptors");
}

void parameters_are_refused() {
  Rig rig{"parameters"};
  const std::string head = "G21 G90\nG1 X1 F100\n";
  const auto stops = [&](std::string_view line, std::string_view what) -> void {
    const auto o = rig.preview(head + std::string(line) + "\nG1 X2\nM2\n");
    require(o.state == PreviewState::failed && o.error && o.error->line == 3 &&
                o.error->text.find(what) != std::string::npos && o.segments.size() == 1 &&
                o.segments[0].line == 2,
            std::format("`{}` ends the preview at its line with the path before it: {}", line,
                        text_of(o)));
  };
  stops("#1=10", "parameter");
  stops("G1 X#1", "parameter");
  stops("G1 X#<_x>", "parameter");
  stops("#<depth> = 3", "parameter");
  stops("G1 X[#5220]", "parameter");
  stops("o100 call [5]", "arguments");
  stops("G28", "G28");
  stops("G30 X1", "G30");
  stops("G92.3", "G92.3");
  require(refused_block("G+28") == refused_block("G28") &&
              refused_block("G+92.3") == refused_block("G92.3") &&
              motion_use("G+1 X5") == MotionUse::sets,
          "An explicit positive G word follows the same rules as the unsigned form");
  stops("G38.2 X5 F10", "probe");
  stops("M66 P0 L0", "M66");
  stops("M101", "M101");
  stops("M199 P1", "M199");

  auto o = rig.preview(head + "(a # in a comment, and #1=5) G1 X3 ; and #2 here\nG1 X4\nM2\n");
  require(o.state == PreviewState::ready && o.segments.size() == 3 &&
              near(o.segments[1].points.back(), {3, 0, 0}),
          "A `#` in an ordinary comment is text: " + text_of(o));
}

void active_comments_are_refused() {
  Rig rig{"comments"};
  const std::string head = "G21 G90\nG1 X1 F100\n";
  const auto log = rig.root / "written.log";
  const std::vector<std::string> forms{
      "(py,print('x'))",
      "(PY,print('x'))",
      "(  Py,print('x'))",
      "(pyrun,x.py)",
      "(PyRun, x.py)",
      "(pyreload)",
      " ( PYRELOAD )",
      ";py,print('x')",
      "  ;Py,print('x')",
      "(LOGOPEN," + log.string() + ")",
      "(logopen," + log.string() + ")",
      "(  LogOpen," + log.string() + ")",
      "(LOGAPPEND," + log.string() + ")",
      "(log,text)",
      "(LOG,text)",
      "(LOGCLOSE)",
      "(logclose)",
      "(PROBEOPEN " + log.string() + ")",
      "(probeclose)",
      "G1 X2 (py,print('x'))",
      "(an ordinary one) (Log,x)"};
  for (const auto& form : forms) {
    const auto o = rig.preview(head + form + "\nG1 X2\nM2\n");
    require(o.state == PreviewState::failed && o.error && o.error->line == 3 &&
                o.segments.empty() && rig.runs == 0,
            std::format("`{}` anywhere in the program: no preview, its line named: {}", form,
                        text_of(o)));
  }
  require(!fs::exists(log), "No file a comment names was made");

  // The interpreter acts on `;py,` while it reads, and it reads lines it never executes: a
  // marker such a line would make stays absent, in the program and in a subroutine file,
  // executed or not; the engine does not even start the helper.
  const auto marker = rig.root / "marker";
  const std::string make = std::format("open('{}','w').close()", marker.string());
  const auto absent = [&](std::string_view program, std::string_view what, int line) -> void {
    const auto o = rig.preview(program);
    require(o.state == PreviewState::failed && o.error && o.error->line == line &&
                o.segments.empty() && rig.runs == 0 && !fs::exists(marker),
            std::format("{}: refused before anything is read, and no marker: {}", what, text_of(o)));
  };
  absent("G21 G90\nG1 X1 F100\n;py," + make + "\nM2\n", "`;py,` in the program", 3);
  absent("G21 G90\n(py," + make + ")\nM2\n", "(py,...) in the program", 2);
  absent("G21 G90\nM2\n;py," + make + "\n", "`;py,` after the end of the program", 3);
  absent("G21 G90\no1 if [0]\n(PyRun," + make + ")\no1 endif\nM2\n",
         "(pyrun,...) in a branch not taken", 3);
  absent("G21 G90\no9 sub\n ;PY," + make + "\no9 endsub\nM2\n",
         "`;py,` in a subroutine never called", 3);
  write(rig.root / "subs" / "effect.ngc",
        "o<effect> sub\nG1 X5 F100\n;py," + make + "\no<effect> endsub\nM2\n");
  auto in_sub = rig.preview("G21 G90\nG1 X1 F100\no<effect> call\nM2\n");
  require(in_sub.state == PreviewState::failed && in_sub.error && in_sub.error->line == 0 &&
              in_sub.error->text.find("effect.ngc line 3") != std::string::npos &&
              rig.runs == 0 && !fs::exists(marker),
          "`;py,` in a subroutine file: refused with the file and its line: " + text_of(in_sub));
  write(rig.root / "subs" / "effect.ngc",
        "o<effect> sub\no2 if [0]\n(py," + make + ")\no2 endif\no<effect> endsub\nM2\n");
  in_sub = rig.preview("G21 G90\no3 if [0]\no<effect> call\no3 endif\nM2\n");
  require(in_sub.state == PreviewState::failed && rig.runs == 0 && !fs::exists(marker),
          "(py,...) in a subroutine file that is never called: " + text_of(in_sub));
  fs::remove(rig.root / "subs" / "effect.ngc");
  // The helper does not rely on the engine: given such a file directly it reads nothing.
  {
    const auto direct = rig.root / "direct";
    write(direct / "p.ngc", "G21 G90\nG1 X1 F100\nM2\n");
    write(direct / "subs" / "other.ngc", "o<other> sub\n;py," + make + "\no<other> endsub\n");
    write(direct / "p.var", "");
    HelperRequest request;
    request.program = (direct / "p.ngc").string();
    request.parameters = (direct / "p.var").string();
    request.subroutines = (direct / "subs").string();
    write(direct / "request", write_request(request));
    const auto out = direct / "out";
    const auto command = std::format("cd '{}' && '{}' request 3 3>'{}' >/dev/null 2>&1",
                                     direct.string(), helper, out.string());
    const int status = std::system(command.c_str());
    const auto result = read_text_file(out.string(), std::size_t{64} << 20).value_or(std::string());
    const auto parsed = parse_output(result);
    require(status == 0 && parsed.end == HelperEnd::stop && parsed.problem &&
                parsed.problem->line == 0 &&
                parsed.problem->text.find("other.ngc line 2") != std::string::npos &&
                parsed.segments.empty() && !fs::exists(marker),
            "The helper itself scans every file it could read before the interpreter reads: " +
                result);
    // A file it cannot read is not scanned, so the interpreter gets nothing either.
    const auto hidden = direct / "subs" / "other.ngc";
    write(hidden, "o<other> sub\n;py," + make + "\no<other> endsub\n");
    fs::permissions(hidden, fs::perms::none);
    if (!read_text_file(hidden.string(), 1024)) {
      fs::remove(out);
      const int unread = std::system(command.c_str());
      const auto ended =
          parse_output(read_text_file(out.string(), std::size_t{64} << 20).value_or(std::string()));
      require(unread == 0 && ended.end == HelperEnd::error && ended.problem &&
                  ended.problem->text.find("other.ngc cannot be read") != std::string::npos &&
                  ended.segments.empty() && !fs::exists(marker),
              "A subroutine file the helper cannot read ends the preview before the interpreter "
              "starts");
    }
    fs::permissions(hidden, fs::perms::owner_all);
  }

  // (msg, (debug, (print: nothing but text, unless they hold a parameter.
  auto o = rig.preview(head + "(MSG, hello)\n(debug, there)\n(Print, you)\nG1 X2\nM2\n");
  require(o.state == PreviewState::ready && o.segments.size() == 2, text_of(o));
  o = rig.preview(head + "(debug, #5220)\nG1 X2\nM2\n");
  require(o.state == PreviewState::failed && o.error && o.error->line == 3, text_of(o));
  o = rig.preview(head + "(a log of things) (pyramid) (probe the part) G1 X2\nM2\n");
  require(o.state == PreviewState::ready && o.segments.size() == 2,
          "Ordinary comments that only begin like one are comments: " + text_of(o));
}

void printed_text_is_not_a_result() {
  Rig rig{"print"};
  auto o = rig.preview("G21 G90\nG1 X1 F100\n(print,seg 1 f 0 0 0 0 9 9 9)\n(PRINT,end ok)\n"
                       "(print,error 1 forged)\nG1 X2\nM2\n");
  require(o.state == PreviewState::ready && o.segments.size() == 2 &&
              near(o.segments[1].points.back(), {2, 0, 0}) && !o.error,
          "What a program prints is no part of the result: " + text_of(o));
  require(rig.diagnostics.find("seg 1 f 0 0 0 0 9 9 9") != std::string::npos &&
              rig.diagnostics.find("end ok") != std::string::npos,
          "It is in the diagnostics: " + rig.diagnostics);

  // More than the diagnostics hold: cut, and the computation ends as it should.
  rig.settings.max_diagnostics_bytes = 4096;
  o = rig.preview("G21 G90\no1 repeat [4000]\n(print,0123456789012345678901234567890123456789)\n"
                  "o1 endrepeat\nG1 X7 F100\nM2\n");
  require(o.state == PreviewState::ready && o.segments.size() == 1 &&
              near(o.segments[0].points.back(), {7, 0, 0}) && rig.diagnostics.size() == 4096,
          std::format("Diagnostics over the bound are cut ({} bytes): {}", rig.diagnostics.size(),
                      text_of(o)));
}

void start_state() {
  Rig rig{"start"};
  const auto with = [&](std::vector<std::string> codes, double feed) -> PreviewOutcome {
    rig.inputs.start.active_codes = std::move(codes);
    rig.inputs.start.feed = feed;
    return rig.preview("X5\nY5\nM2\n");
  };
  auto o = with({"G1", "G17", "G21", "G90", "G94", "G54"}, 300);
  require(o.state == PreviewState::ready && o.segments.size() == 2 &&
              o.segments[0].kind == MoveKind::feed && near(o.segments[0].points.back(), {5, 0, 0}),
          "G1 active at the start: a block of axis words is a feed move: " + text_of(o));
  o = with({"G0", "G17", "G21", "G90", "G94", "G54"}, 0);
  require(o.state == PreviewState::ready && o.segments.size() == 2 &&
              o.segments[0].kind == MoveKind::rapid,
          "G0 active: a rapid: " + text_of(o));
  o = with({"G80", "G17", "G21", "G90", "G94", "G54"}, 0);
  require(o.state == PreviewState::failed && o.error && o.error->line == 1 && o.segments.empty(),
          "G80 active: the interpreter's error at line 1: " + text_of(o));
  o = with({"G1", "G17", "G20", "G91", "G94", "G54"}, 254);
  require(o.state == PreviewState::ready && near(o.segments[0].points.back(), {127, 0, 0}) &&
              near(o.segments[1].points.back(), {127, 127, 0}),
          "G20 and G91 active: inches, relative: " + text_of(o));
  o = with({"G1", "G18", "G21", "G90", "G94", "G54"}, 100);
  require(o.state == PreviewState::ready, text_of(o));
  // A mode that no block can put in place: the preview ends where the program would move by
  // it, and not at all when the program names its own first.
  o = with({"G81", "G17", "G21", "G90", "G94", "G54"}, 100);
  require(o.state == PreviewState::failed && o.error && o.error->line == 1 &&
              o.error->text.find("G81") != std::string::npos && o.segments.empty(),
          "A canned cycle active from before, used by the first block: " + text_of(o));
  o = with({"G38.2", "G17", "G21", "G90", "G94", "G54"}, 100);
  require(o.state == PreviewState::failed && o.error &&
              o.error->text.find("G38.2") != std::string::npos,
          "A probe mode active from before likewise: " + text_of(o));
  rig.inputs.start.active_codes = {"G2", "G17", "G21", "G90", "G94", "G54"};
  o = rig.preview("G21 G90 (no word that moves)\nF200 S100 M3\nG1 X5\nX6\nM2\n");
  require(o.state == PreviewState::ready && o.segments.size() == 2 &&
              near(o.segments[1].points.back(), {6, 0, 0}),
          "A program that names its motion mode before it moves is previewed whatever was "
          "active: " + text_of(o));
  o = rig.preview("G21 G90\nG4 P0\nY[2+1]\nG1 X5 F100\nM2\n");
  require(o.state == PreviewState::failed && o.error && o.error->line == 3 && o.segments.empty(),
          "Axis words before it do end the preview: " + text_of(o));
  o = with({"G1", "G17", "G21", "G90", "G94", "G54", "G41"}, 100);
  require(o.state == PreviewState::unavailable, "Cutter compensation active: no preview");
  rig.inputs.start.spindle_speed = 120;
  rig.inputs.start.spindle_direction = SpindleDirection::forward;
  o = with({"G1", "G17", "G21", "G90", "G94", "G54", "G96"}, 100);
  require(o.state == PreviewState::ready && o.segments.size() == 2,
          "An active constant surface speed mode seeds its S word: " + text_of(o));
  rig.inputs.start.spindle_speed = 0;
  rig.inputs.start.spindle_direction = SpindleDirection::stopped;
  // Block delete.
  rig.inputs.start.active_codes = {"G1", "G21", "G90", "G94", "G54"};
  rig.inputs.start.feed = 100;
  o = rig.preview("G1 X1\n/G1 X2\nG1 X3\nM2\n");
  require(o.state == PreviewState::ready && o.segments.size() == 3, text_of(o));
  rig.inputs.block_delete = true;
  o = rig.preview("G1 X1\n/G1 X2\nG1 X3\nM2\n");
  require(o.state == PreviewState::ready && o.segments.size() == 2 &&
              near(o.segments[1].points.front(), {1, 0, 0}),
          "With block delete on a `/` block is not run: " + text_of(o));
}

// One look at the files of a result.
auto changed(const PreviewComputation& computed) -> bool { return computed.files_changed({}); }

void files_are_bound_to_the_result() {
  Rig rig{"files"};
  const auto program = fs::path(rig.file());
  const auto sub = rig.root / "subs" / "step.ngc";
  write(sub, "o<step> sub\nG91 G1 X1 F100\nG90\no<step> endsub\nM2\n");
  write(program, "G21 G90\no<step> call\nM2\n");
  auto computed = rig.compute();
  require(computed.outcome.state == PreviewState::ready && rig.runs == 1 &&
              near(computed.outcome.segments.at(0).points.back(), {1, 0, 0}),
          text_of(computed.outcome));
  require(computed.files_changed != nullptr && !changed(computed) && !changed(computed),
          "Unchanged files are unchanged, every time they are looked at");

  // The same size, another byte, the old time: the bytes decide.
  const auto time = fs::last_write_time(sub);
  write(sub, "o<step> sub\nG91 G1 X2 F100\nG90\no<step> endsub\nM2\n");
  fs::last_write_time(sub, time);
  require(fs::last_write_time(sub) == time && changed(computed),
          "A subroutine file edited to the same size with its time restored is a change");
  write(sub, "o<step> sub\nG91 G1 X1 F100\nG90\no<step> endsub\nM2\n");
  require(!changed(computed), "Put back, it is none");
  write(program, "G21 G90\no<step> call\nM2 \n");
  require(changed(computed), "The program changed is a change");
  write(program, "G21 G90\no<step> call\nM2\n");
  require(!changed(computed), "Put back");
  // A file of that name earlier in the search path: the call would find another file.
  write(rig.root / "nc" / "step.ngc", "o<step> sub\no<step> endsub\nM2\n");
  require(changed(computed), "A new file that shadows the subroutine is a change");
  fs::remove(rig.root / "nc" / "step.ngc");
  require(!changed(computed), "Gone again");
  // A subroutine that was not found, and now is.
  write(program, "G21 G90\nG1 X1 F100\no<later> call\nM2\n");
  computed = rig.compute();
  require(computed.outcome.state == PreviewState::failed && !changed(computed),
          text_of(computed.outcome));
  write(rig.root / "subs" / "later.ngc", "o<later> sub\no<later> endsub\nM2\n");
  require(changed(computed), "A subroutine file that appears is a change");

  // Changed between the snapshot and the result: discarded, computed again.
  write(program, "G21 G90\no<step> call\nM2\n");
  rig.after_snapshot = [&](int attempt) -> void {
    if (attempt == 1)
      write(sub, "o<step> sub\nG91 G1 X3 F100\nG90\no<step> endsub\nM2\n");
  };
  computed = rig.compute();
  require(computed.outcome.state == PreviewState::ready && rig.runs == 2 &&
              near(computed.outcome.segments.at(0).points.back(), {3, 0, 0}) &&
              !changed(computed),
          "A file changed during the computation: the result is of the files as they are now: " +
              text_of(computed.outcome));
  // A file that changes during every computation: bounded.
  int edits = 0;
  rig.after_snapshot = [&](int) -> void {
    write(program, std::format("G21 G90\nG1 X{} F100\nM2\n", ++edits));
  };
  computed = rig.compute();
  require(computed.outcome.state == PreviewState::failed && computed.outcome.error &&
              computed.outcome.error->text.find("changed on disk") != std::string::npos &&
              rig.runs == 3 && computed.outcome.segments.empty() && changed(computed),
          "A file that keeps changing: three computations, then failed with the reason");
}

// A called name is a name, or the program is refused before anything reads it.
void names_are_plain_or_refused() {
  Rig rig{"names"};
  const auto canary = rig.root / "canary.ngc";
  write(canary, "o<canary> sub\nG1 X99 F100\no<canary> endsub\nM2\n");
  write(rig.root / "x.ngc", "o<x> sub\no<x> endsub\nM2\n");
  const std::vector<std::string> names{
      (rig.root / "canary").string(), "../canary", "../x", "~/x", "$HOME/x", "a*", "x?", "[ab]",
      "`id`", "$(id)", "a;b", "a'b", "a\"b", "a\\b", "{a,b}", "a|b", "a&b", "a>b", "!a", "a=b"};
  for (const auto& name : names) {
    for (const auto* word : {"call", "sub", "if [1]"}) {
      const auto o = rig.preview(
          std::format("G21 G90\nG1 X1 F100\nM2\no<{}> {}\n", name, word));
      require(o.state == PreviewState::failed && o.error && o.error->line == 4 &&
                  o.segments.empty() && rig.runs == 0 && rig.lookups == 0,
              std::format("`o<{}> {}` is refused before anything is read or looked up: {}", name,
                          word, text_of(o)));
    }
  }
  auto o = rig.preview("G21 G90\no[1+1] call\nM2\n");
  require(o.state == PreviewState::failed && o.error && o.error->line == 2 && rig.runs == 0,
          "A computed O word likewise: " + text_of(o));
  // In a subroutine file: refused with the file and its line.
  write(rig.root / "subs" / "outside.ngc",
        std::format("o<outside> sub\no<{}> call\no<outside> endsub\nM2\n",
                    (rig.root / "canary").string()));
  o = rig.preview("G21 G90\no<outside> call\nM2\n");
  require(o.state == PreviewState::failed && o.error && o.error->line == 0 &&
              o.error->text.find("outside.ngc line 2") != std::string::npos && rig.runs == 0,
          "A name that is a path, in a subroutine file: " + text_of(o));
  // Names that are names: letters, digits, `_`, `-`, `.`, in any case and with blanks.
  write(rig.root / "subs" / "my_sub-2.a.ngc",
        "o<my_sub-2.a> sub\nG91 G1 X1 F100\nG90\no<my_sub-2.a> endsub\nM2\n");
  write(rig.root / "subs" / "123.ngc", "o123 sub\nG91 G1 Y1 F100\nG90\no123 endsub\nM2\n");
  o = rig.preview("G21 G90\nO<My_Sub - 2.A> CALL\no123 call\nM2\n");
  require(o.state == PreviewState::ready && o.segments.size() == 2 &&
              near(o.segments[1].points.back(), {1, 1, 0}) && rig.lookups == 12,
          std::format("Plain names are looked up and called ({} lookups): {}", rig.lookups,
                      text_of(o)));

  // A subroutine called like the program's private copy cannot take its place.
  for (const auto* name : {"program", "coffeecnc-program", "preview", "request"}) {
    write(rig.root / "subs" / (std::string(name) + ".ngc"),
          std::format("o<{0}> sub\nG91 G1 X2 F100\nG90\no<{0}> endsub\nM2\n", name));
    o = rig.preview(std::format("G21 G90\nG1 Y3 F100\no<{}> call\nG1 Y4\nM2\n", name));
    require(o.state == PreviewState::ready && o.segments.size() == 3 &&
                near(o.segments[0].points.back(), {0, 3, 0}) &&
                near(o.segments[1].points.back(), {2, 3, 0}) &&
                near(o.segments[2].points.back(), {2, 4, 0}),
            std::format("A subroutine file named {}.ngc: the program is intact: {}", name,
                        text_of(o)));
  }
  // The work directory's own path must be plain: the interpreter expands it.
  rig.settings.work_directory = (rig.root / "work with $HOME").string();
  o = rig.preview("G21 G90\nG1 X1 F100\nM2\n");
  require(o.state == PreviewState::unavailable && rig.runs == 0,
          "A work directory whose path the interpreter would expand: no preview");
}

void input_bounds() {
  Rig rig{"bounds"};
  rig.settings.max_input_bytes = 200;
  auto o = rig.preview(std::string(300, '\n') + "M2\n");
  require(o.state == PreviewState::failed && o.error && rig.runs == 0 &&
              o.error->text.find("larger than") != std::string::npos,
          "A program over the size bound is refused with the reason: " + text_of(o));
  write(rig.root / "subs" / "big.ngc", "o<big> sub\n" + std::string(300, '\n') + "o<big> endsub\n");
  o = rig.preview("G21\no<big> call\nM2\n");
  require(o.state == PreviewState::failed && o.error && rig.runs == 0,
          "The bound is of the program and its subroutine files together: " + text_of(o));

  rig.settings.max_input_bytes = 100000;
  rig.settings.max_files = 3;
  for (const auto* name : {"a", "b", "c"})
    write(rig.root / "subs" / (std::string(name) + ".ngc"),
          std::format("o<{0}> sub\no<{0}> endsub\nM2\n", name));
  o = rig.preview("G21\no<a> call\no<b> call\nM2\n");
  require(o.state == PreviewState::ready, "Three files are within a bound of three");
  o = rig.preview("G21\no<a> call\no<b> call\no<c> call\nM2\n");
  require(o.state == PreviewState::failed && o.error && rig.runs == 0 &&
              o.error->text.find("more than 3 files") != std::string::npos,
          "A program over the file bound is refused with the reason: " + text_of(o));
}

void call_bounds_and_cancellation() {
  Rig rig{"calls"};
  // More different names than the bound, none of them a file: refused before any lookup.
  std::string program = "G21 G90\n";
  for (int i = 0; i < 100000; ++i)
    program += std::format("o<missing{}> call\n", i);
  program += "M2\n";
  auto begun = std::chrono::steady_clock::now();
  auto o = rig.preview(program);
  auto took = std::chrono::steady_clock::now() - begun;
  require(o.state == PreviewState::failed && o.error && o.error->line == 0 &&
              o.error->text.find("more than 256 different subroutines") != std::string::npos &&
              rig.runs == 0 && rig.lookups == 0 && took < 5s,
          std::format("100000 different missing calls: refused with no lookup, in {} ms: {}",
                      std::chrono::duration_cast<std::chrono::milliseconds>(took).count(),
                      text_of(o)));
  // The same name many times is one name; at the bound it is looked up, once per directory.
  program = "G21 G90\n";
  for (int i = 0; i < 20000; ++i)
    program += std::format("o<missing{}> call\n", i % 256);
  program += "M2\n";
  write(rig.file(), program);
  HelperPreviewEngine engine{rig.settings};
  auto computed = engine.compute(rig.inputs, {});
  require(computed.outcome.state == PreviewState::failed && engine.helper_runs() == 1 &&
              engine.file_lookups() == std::size_t{2} * 256 * 3,
          std::format("256 names: looked up once each per directory, at the snapshot and at "
                      "the check ({} lookups): {}",
                      engine.file_lookups(), text_of(computed.outcome)));

  // A look at the files that is given up returns at once and reports no change.
  // (Through a call of its own: what the engine counted is not a constant between two looks.)
  const auto looked = [&engine]() -> std::size_t { return engine.file_lookups(); };
  const auto before = looked();
  write(rig.file(), "changed\n");
  begun = std::chrono::steady_clock::now();
  const bool given_up = computed.files_changed([]() -> bool { return true; });
  took = std::chrono::steady_clock::now() - begun;
  require(!given_up && looked() == before && took < 100ms,
          "A cancelled look at the files does nothing");
  int asked = 0;
  require(!computed.files_changed([&]() -> bool { return ++asked > 10; }) &&
              looked() == before + std::size_t{30},
          "Cancellation is asked before every name");
  require(computed.files_changed({}), "Not cancelled, the change is found");

  // A computation is given up between its files.
  write(rig.file(), "G21 G90\no<a> call\no<b> call\nM2\n");
  for (const auto* name : {"a", "b"})
    write(rig.root / "subs" / (std::string(name) + ".ngc"),
          std::format("o<{0}> sub\no<{0}> endsub\nM2\n", name));
  asked = 0;
  const auto cancelled = engine.compute(rig.inputs, [&]() -> bool { return ++asked > 3; });
  require(cancelled.cancelled && engine.helper_runs() == 1,
          "A computation cancelled during its snapshot starts no helper");
}

// The cost of one look at the files of the largest input allowed.
void cost_of_a_check() {
  Rig rig{"cost"};
  const auto bytes = rig.settings.max_input_bytes;
  std::string program;
  program.reserve(bytes);
  while (program.size() + 40 < bytes)
    program += "(123456789012345678901234567890)\n";
  program += "M2\n";
  write(rig.file(), program);
  auto begun = std::chrono::steady_clock::now();
  const auto computed = rig.compute();
  const auto compute_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begun).count();
  require(computed.outcome.state == PreviewState::ready && computed.files_changed != nullptr,
          text_of(computed.outcome));
  double worst = 0;
  for (int i = 0; i < 5; ++i) {
    begun = std::chrono::steady_clock::now();
    require(!changed(computed), "Unchanged");
    worst = std::max(worst, std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - begun)
                                .count());
  }
  std::cout << std::format("fact=preview_input_bound bytes={} compute_ms={:.0f} "
                           "files_check_ms_worst_of_5={:.1f}\n",
                           program.size(), compute_ms, worst);
}

// `ready` is the helper's own good end, not its word for it. Helpers of the test's own: shell
// scripts that write a result to descriptor 3 and end in different ways.
void how_the_helper_ended() {
  Rig rig{"ended"};
  rig.settings.exit_grace = 400ms;
  write(rig.file(), "G21 G90\nG1 X1 F100\nM2\n");
  const auto pid_file = rig.root / "pid";
  const auto with = [&](std::string_view body) -> PreviewOutcome {
    const auto script = rig.root / "helper.sh";
    write(script, std::format("#!/bin/bash\necho $$ > '{}'\n{}\n", pid_file.string(), body));
    fs::permissions(script, fs::perms::owner_all);
    rig.settings.helper = script.string();
    return rig.compute().outcome;
  };
  const auto gone = [&]() -> bool {
    std::ifstream in{pid_file};
    int pid = 0;
    in >> pid;
    return pid > 0 && kill(pid, 0) != 0;
  };
  const std::string result = "echo 'seg 2 f 0 0 0 0 1 0 0' >&3; echo 'end ok' >&3; ";

  auto o = with(result + "exit 0");
  require(o.state == PreviewState::ready && o.segments.size() == 1 && gone(),
          "A whole result and exit 0: ready: " + text_of(o));
  o = with(result + "exit 7");
  require(o.state == PreviewState::failed && o.error && o.error->line == 0 &&
              o.error->text.find("exit code 7") != std::string::npos && o.segments.size() == 1,
          "`end ok`, then exit 7: failed, with the path: " + text_of(o));
  o = with(result + "kill -SEGV $$");
  require(o.state == PreviewState::failed && o.error && o.error->line == 0 &&
              o.error->text.find("signal 11") != std::string::npos && o.segments.size() == 1,
          "`end ok`, then death by a signal: failed: " + text_of(o));
  o = with("echo 'seg 2 f 0 0 0 0 1 0 0' >&3; exit 0");
  require(o.state == PreviewState::failed && o.error && o.error->line == 0 &&
              o.error->text.find("without finishing") != std::string::npos &&
              o.segments.size() == 1,
          "No `end`, exit 0: failed, with what was received: " + text_of(o));
  o = with("echo 'seg 2 f 0 0 0 0 1 0 0' >&3; echo 'error 2 broken' >&3; echo 'end error' >&3; "
           "exit 3");
  require(o.state == PreviewState::failed && o.error && o.error->line == 2 &&
              o.error->text == "broken",
          "An error with its line stays the reason whatever the exit code: " + text_of(o));
  // Closes its result and stays: ended after the grace, and nothing of it is left.
  const auto begun = std::chrono::steady_clock::now();
  o = with(result + "exec 3>&- 1>&- 2>&-; exec sleep 100");
  const auto took = std::chrono::steady_clock::now() - begun;
  require(o.state == PreviewState::failed && o.error && o.error->line == 0 &&
              o.error->text.find("did not end") != std::string::npos && o.segments.size() == 1 &&
              took > 350ms && took < 5s && gone(),
          "`end ok` without exiting: failed after the grace, and the process is gone: " +
              text_of(o));
  // Runs past the timeout with its result open.
  rig.settings.timeout = 500ms;
  o = with("echo 'seg 2 f 0 0 0 0 1 0 0' >&3; exec sleep 100");
  require(o.state == PreviewState::failed && o.error &&
              o.error->text.find("longer than") != std::string::npos && o.segments.size() == 1 &&
              gone(),
          "Past the timeout: ended, failed, gone: " + text_of(o));
  rig.settings.timeout = 20s;
  // A process it left behind does not make a good result bad, and does not stay.
  const auto stray_file = rig.root / "stray";
  o = with(std::format("sleep 100 3>&- >/dev/null 2>&1 & echo $! > '{}'; ", stray_file.string()) +
           result + "exit 0");
  int stray = 0;
  std::ifstream{stray_file} >> stray;
  // The engine ends it; init reaps it, asynchronously: a killed orphan answers kill(pid, 0)
  // for a while (about 0.6 ms on a PREEMPT_RT kernel), so its end is waited for, bounded.
  bool stray_ended = false;
  for (const auto until = std::chrono::steady_clock::now() + 2s;
       stray > 0 && !stray_ended && std::chrono::steady_clock::now() < until;) {
    stray_ended = kill(stray, 0) != 0;
    if (!stray_ended)
      std::this_thread::sleep_for(1ms);
  }
  require(o.state == PreviewState::ready && stray > 0 && stray_ended &&
              rig.diagnostics.find("left processes behind") != std::string::npos,
          "A process left behind after a good exit: ready, ended, noted: " + text_of(o));
  // A result that is not valid, from a helper that exits 0.
  o = with("echo 'seg 2 f 0 0 0 0 1 0 0' >&3; echo 'seg 1 f 0 0 bad 0 1 0 0' >&3; "
           "echo 'end ok' >&3; exit 0");
  require(o.state == PreviewState::failed && o.error && o.error->line == 0 &&
              o.error->text.find("not valid") != std::string::npos && o.segments.size() == 1,
          "A result with a bad record: failed, with the records before it: " + text_of(o));
  o = with("echo 'seg 9 f 0 0 0 0 1 0 0' >&3; echo 'end ok' >&3; exit 0");
  require(o.state == PreviewState::failed && o.segments.empty(),
          "A segment of a line the program does not have: not valid: " + text_of(o));
}

// Programs of the repository's sample corpus, as they are: what the preview makes of each.
// One fact line per sample and start state.
void repository_samples() {
  const fs::path samples{COFFEECNC_SAMPLES_DIRECTORY};
  const auto task17 = samples / "upstream/engiworld-cam/task/single-software-execution/cli/"
                                "freecad-path/task-17/ground_truth";
  const auto face = samples / "upstream/camcraft/fixtures/nc/ops_block_face.nc";
  if (!fs::exists(task17 / "task-17_A.nc") || !fs::exists(face)) {
    std::cout << "fact=preview_samples skipped=the-sample-corpus-is-not-in-this-checkout\n";
    return;
  }
  Rig rig{"samples"};
  rig.inputs.raw_tools = {{1, 1, 1, {0, 0, 50}, 6, 0, 0, 0},
                          {2, 2, 2, {0, 0, 40}, 5, 0, 0, 0},
                          {3, 3, 3, {0, 0, 45}, 8, 0, 0, 0}};
  rig.inputs.start.position = {0, 0, 100};
  const auto run = [&](const fs::path& file, int wcs, std::string_view label) -> PreviewOutcome {
    rig.inputs.program_file = file.string();
    rig.settings.search_path = {file.parent_path().string()};
    rig.inputs.start.wcs = wcs;
    rig.inputs.systems = {};
    rig.inputs.systems.at(static_cast<std::size_t>(wcs - 1)) = WorkSystem{{100, 50, -20}, 0, true};
    rig.inputs.start.active_codes = {"G80", "G17", "G40", "G21", "G90", "G94",
                                     wcs == 1 ? "G54" : "G55", "G49", "G99"};
    const auto o = rig.compute().outcome;
    const auto points = points_of(o);
    auto text = o.error ? o.error->text : std::string("-");
    std::ranges::replace(text, ' ', '_');
    std::cout << std::format(
        "fact=preview_sample sample={} start={} state={} error_line={} error_text={} segments={} "
        "points={} bounds={}\n",
        file.filename().string(), label,
        o.state == PreviewState::ready ? "ready" : o.state == PreviewState::failed ? "failed"
                                                                                    : "unavailable",
        o.error ? o.error->line : 0, text, o.segments.size(), points,
        o.bounds ? std::format("{:.3f},{:.3f},{:.3f}..{:.3f},{:.3f},{:.3f}", o.bounds->min[0],
                               o.bounds->min[1], o.bounds->min[2], o.bounds->max[0],
                               o.bounds->max[1], o.bounds->max[2])
                 : std::string("none"));
    return o;
  };
  // Side A: G54 throughout.
  auto o = run(task17 / "task-17_A.nc", 1, "G54");
  require(o.state == PreviewState::ready && !o.segments.empty() && o.bounds,
          "task-17_A with G54 active is previewed whole: " + text_of(o).substr(0, 300));
  // Side B cuts in G55 and returns to G54 in its last block. With G55 active its offsets are
  // known and the return moves nothing: whole. With G54 active the first move in G55 is where
  // the preview ends.
  o = run(task17 / "task-17_B.nc", 2, "G55");
  require(o.state == PreviewState::ready && !o.segments.empty(),
          "task-17_B with G55 active is previewed whole: " + text_of(o).substr(0, 300));
  o = run(task17 / "task-17_B.nc", 1, "G54");
  require(o.state == PreviewState::failed && o.error && o.error->line > 0 &&
              o.error->text.find("G55") != std::string::npos,
          "task-17_B with G54 active ends at its first move in G55: " +
              text_of(o).substr(0, 300));
  // The Camcraft facing program returns to its reference with G28 in line 5: refused there,
  // as it is; nothing of the sample is changed to get a picture.
  o = run(face, 1, "G54");
  require(o.state == PreviewState::failed && o.error && o.error->line == 5 &&
              o.error->text.find("G28") != std::string::npos && o.segments.empty(),
          "ops_block_face is refused at its G28: " + text_of(o).substr(0, 300));
}

void refusals_of_the_settings() {
  Rig rig{"settings"};
  rig.settings.refusal = "Not on this machine.";
  auto o = rig.preview("G1 X1 F100\nM2\n");
  require(o.state == PreviewState::unavailable && o.error &&
              o.error->text == "Not on this machine." && rig.runs == 0,
          "A refusal of the settings is the preview's text");
  rig.settings.refusal.clear();
  rig.inputs.program_file.reset();
  o = rig.compute().outcome;
  require(o.state == PreviewState::unavailable && !o.error, "No program: unavailable, no text");

  // From an INI file.
  MachineConfig config{
      .ini_file = (rig.root / "config" / "mill.ini").string(),
      .directory = (rig.root / "config").string(),
      .machine_name = "Mill",
      .nml_file = {},
      .machine_units = LengthUnit::inch,
      .millimetres_per_unit = 25.4,
      .axes = {Axis::x, Axis::y, Axis::z},
      .travel = {Travel{0, 1}, Travel{0, 1}, Travel{0, 1}},
      .jog_increments = {},
      .limits = Limits{1500, {1500, 1500, 750}, 2.0, 0.5, 1.0, 1500, false},
      .home_all_configured = false,
      .signals = {},
      .tool_table = {},
      .parameter_file = (rig.root / "config" / "mill.var").string(),
      .postgui_halfile = {},
      .program_prefix = (rig.root / "nc").string(),
      .machine_id = {},
      .coolant = {},
      .data_directory = {},
  };
  write(config.ini_file, "[RS274NGC]\nPARAMETER_FILE = mill.var\nSUBROUTINE_PATH = ../subs:macros\n"
                         "RETAIN_G43 = 1\n[EMCIO]\nTOOL_CHANGE_POSITION = 1 2 3\n"
                         "TOOL_CHANGE_QUILL_UP = 1\n");
  auto settings = load_preview_settings(config, helper, (rig.root / "work").string());
  require(settings.refusal.empty() && near(settings.units_per_mm, 1 / 25.4, 1e-12) &&
              settings.search_path ==
                  std::vector<std::string>{config.directory, config.program_prefix,
                                           (rig.root / "subs").string(),
                                           (rig.root / "config" / "macros").string()} &&
              settings.tool_change_position && near(*settings.tool_change_position, {25.4, 50.8, 76.2}) &&
              settings.tool_change_quill_up && !settings.tool_change_at_g30 &&
              settings.interpreter_options == std::vector<std::string>{"RETAIN_G43 = 1"},
          "The settings of an INI file");
  // The tool change switches as the interpreter reads them: a nonzero integer, nothing else.
  write(config.ini_file, "[EMCIO]\nTOOL_CHANGE_AT_G30 = -1\nTOOL_CHANGE_QUILL_UP = YES\n");
  settings = load_preview_settings(config, helper, "x");
  require(settings.tool_change_at_g30 && !settings.tool_change_quill_up,
          "-1 moves to G30 at a tool change, YES does not lift the quill, as in LinuxCNC");
  write(config.ini_file, "[EMCIO]\nRANDOM_TOOLCHANGER = -1\n");
  require(!load_preview_settings(config, helper, "x").refusal.empty(),
          "-1 is a random tool changer");
  write(config.ini_file, "[RS274NGC]\nREMAP = M6 modalgroup=6 ngc=change\n");
  require(load_preview_settings(config, helper, "x").refusal.find("REMAP") != std::string::npos,
          "A machine that remaps codes gets no preview");
  write(config.ini_file, "[EMCIO]\nRANDOM_TOOLCHANGER = 1\n");
  require(!load_preview_settings(config, helper, "x").refusal.empty(),
          "Nor one with a random tool changer");
  write(config.ini_file, "[EMCIO]\n");
  require(load_preview_settings(config, (rig.root / "no-helper").string(), "x")
                  .refusal.find("not installed") != std::string::npos,
          "Nor one without the helper program");
}

} // namespace

auto main(int argc, char** argv) -> int {
  try {
    if (argc != 3)
      throw std::runtime_error(
          "usage: coffeecnc_preview_engine_tests <coffeecnc-preview> <scratch directory>");
    helper = fs::absolute(argv[1]).string();
    scratch = fs::absolute(argv[2]) / "preview_engine";
    fs::create_directories(scratch);
    lines_and_bounds();
    restart_checkpoints();
    program_check_touched_lengths_and_travel();
    arcs_in_three_planes();
    units();
    offsets_rotation_and_g92();
    tool_length_and_tool_change();
    subroutines();
    errors_and_limits();
    parameters_are_refused();
    helper_with_closed_standard_descriptors();
    active_comments_are_refused();
    printed_text_is_not_a_result();
    start_state();
    files_are_bound_to_the_result();
    names_are_plain_or_refused();
    input_bounds();
    call_bounds_and_cancellation();
    cost_of_a_check();
    how_the_helper_ended();
    repository_samples();
    refusals_of_the_settings();
    std::cout << checks << " preview engine checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
