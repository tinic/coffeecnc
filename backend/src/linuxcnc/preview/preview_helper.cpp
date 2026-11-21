// coffeecnc-preview: computes the path of one program with LinuxCNC's own interpreter
// (librs274.so) and writes it to standard output. A process of its own, started per
// computation by the executable and ended by it at will: the interpreter keeps global state
// and embeds Python, and a program can loop for ever.
//
//   coffeecnc-preview <request file> <result descriptor>     (formats: src/preview_wire.hpp)
//
// The result is written to the descriptor the executable passes, never to standard output:
// the interpreter itself prints there (a `(print, ...)` comment, messages of its Python), so
// standard output and standard error are diagnostics and are not read for results.
//
// The interpreter reports motion through the "canonical" functions its host defines; this file
// defines every one librs274.so refers to. Motion becomes `seg` lines; nothing else has an
// effect: no file is written but the private parameter file the request names, no HAL, no NML.
// Comments that run Python or write files are refused before they are executed (refused_block).
// The interpreter's tool table functions are defined here too, on a table of the helper's own
// filled from the request, so LinuxCNC's tool data memory is neither read nor written.
//
// Lines: every `seg`, `error`, `stop` and `cut` names a line of the program file. A move made
// by a subroutine defined in the program carries the line of the block that moves; one made in
// a subroutine FILE carries the line of the program's call that led there, however deep, and
// an error or stop in such a file says the file and its line in the text. Which file the
// interpreter is in is its own report (InterpBase::file_name), asked after every read.
//
// Coordinates as LinuxCNC's task keeps them (emc/task/emccanon.cc): the interpreter speaks
// program coordinates in the program's unit; the controlled point in machine coordinates is
// rotate(program + G92, XY rotation) + G5x + tool offset. The path written is the tool tip:
// the controlled point less the tool offset in effect, in millimetres.
//
// The trusted path ends, with a `stop` line, at the first block whose effect depends on the
// machine: a probe move, M66, M100 to M199, a move in a work system other than the one active
// at the start, a tool change that moves to a saved position, and what ProgramWatch rejects
// (any use of a parameter, G28, G30, G92.3).
//
// Only the interpreter's public interface is used: InterpBase and the canonical functions.
//
// Exit code 0 when `end` was written, 2 for a request that cannot be used.

#include "../src/preview_wire.hpp"
#include "../src/program_watch.hpp"
#include <coffeecnc/core/text_file.hpp>

#include <array>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <canon.hh>
#include <emctool.h>
#include <interp_base.hh>
#include <interp_return.hh>
#include <linuxcnc.h>
#include <tooldata.hh>

using coffeecnc::Bounds;
using coffeecnc::HelperRequest;
using coffeecnc::ProgramFrameCheck;
using coffeecnc::ProgramRange;
using coffeecnc::RestartPoint;
using coffeecnc::Vec3;

// Defined below for the interpreter; LinuxCNC's tooldata.hh declares it where it is installed.
struct CANON_TOOL_TABLE tooldata_entry_init();
int tooldata_find_index_for_tool(int number);

namespace {

enum class Halt : std::uint8_t { none, error, stop, cut };

struct Canon {
  HelperRequest request;
  CANON_UNITS length_units{CANON_UNITS_MM};
  CANON_PLANE plane{CANON_PLANE_XY};
  Vec3 g5x{};
  Vec3 g92{};
  Vec3 tool_offset{};
  double rotation{}; // degrees
  int g5x_index{1};
  Vec3 end{}; // the controlled point, machine coordinates, mm
  int tool_in_spindle{};
  int selected_index{-1};
  double feed{}; // mm/min
  int feed_mode{}; // 0 per minute, 1 inverse time, 2 per revolution
  double spindle_rpm{};
  CANON_DIRECTION spindle{CANON_STOPPED};
  bool flood{};
  bool mist{};
  bool surface_speed{};
  CANON_MOTION_MODE path_control{CANON_CONTINUOUS};
  double path_tolerance{}; // mm
  double naivecam_tolerance{}; // mm
  std::array<CANON_TOOL_TABLE, CANON_POCKETS_MAX> tools{};
  int last_index{};

  // The line of the program (the file the request names) being executed: the line the
  // interpreter is at while it reads that file, and while it is in a subroutine file the line
  // of the program that called out of it. `elsewhere` then says where it really is.
  int line{};
  std::string elsewhere; // "name.ngc line 12: ", empty in the program itself
  // Whether motion is written (not while the start state is being put in place).
  bool recording{};
  // The run of motion being collected.
  bool open{};
  int open_line{};
  bool open_feed{};
  double open_rate{}; // mm/min of the run being collected; 0 for rapids and unknown feeds
  int open_tool{};
  std::vector<Vec3> points;
  std::size_t written{};
  std::optional<double> highest_tip_z;
  bool moved_on_restart_line{};
  bool subroutine{};
  bool changed_tool_table{};
  bool used_output{};
  bool restart_inherits_preparation{};
  bool restart_needs_preparation{};
  coffeecnc::HelperOutput::ProgramFacts check;

  Halt halt{Halt::none};
};

Canon canon;    // NOLINT: the canonical functions are free functions
FILE* sink{}; // NOLINT: where the result lines go

void put(const std::string& text) { std::fputs(text.c_str(), sink); }

void flush_run() {
  if (!canon.open)
    return;
  canon.open = false;
  std::string out = canon.open_feed && canon.open_rate > 0
      ? std::format("segf {} {} {:.6f}", canon.open_line, canon.open_tool, canon.open_rate)
      : std::format("seg {} {} {}", canon.open_line, canon.open_feed ? 'f' : 'r', canon.open_tool);
  for (const auto& p : canon.points)
    out += std::format(" {:.5f} {:.5f} {:.5f}", p[0], p[1], p[2]);
  out += '\n';
  put(out);
  canon.points.clear();
}

// Ends the trusted path, once: the first reason stands.
void halt(Halt how, const std::string& text) {
  if (canon.halt != Halt::none)
    return;
  flush_run();
  canon.halt = how;
  std::string clean = text;
  for (auto& c : clean)
    if (c == '\n' || c == '\r')
      c = ' ';
  put(std::format("{} {} {}{}\n", how == Halt::error ? "error" : how == Halt::stop ? "stop" : "cut",
                  canon.line, canon.elsewhere, clean));
}

auto program_unit() -> double {
  return canon.length_units == CANON_UNITS_INCHES ? 25.4
         : canon.length_units == CANON_UNITS_CM   ? 10.0
                                                  : 1.0;
}

auto rotated(double x, double y, double degrees) -> std::pair<double, double> {
  const double t = degrees * std::numbers::pi / 180.0;
  return {x * std::cos(t) - y * std::sin(t), x * std::sin(t) + y * std::cos(t)};
}

// Program coordinates in the program's unit to the controlled point in machine coordinates.
auto to_machine(double x, double y, double z) -> Vec3 {
  const double unit = program_unit();
  const auto [rx, ry] = rotated(x * unit + canon.g92[0], y * unit + canon.g92[1], canon.rotation);
  return {rx + canon.g5x[0] + canon.tool_offset[0], ry + canon.g5x[1] + canon.tool_offset[1],
          z * unit + canon.g92[2] + canon.g5x[2] + canon.tool_offset[2]};
}

// And back: where the interpreter is told it stands.
auto to_program(const Vec3& machine) -> Vec3 {
  const double unit = program_unit();
  const auto [rx, ry] = rotated(machine[0] - canon.tool_offset[0] - canon.g5x[0],
                                machine[1] - canon.tool_offset[1] - canon.g5x[1], -canon.rotation);
  return {(rx - canon.g92[0]) / unit, (ry - canon.g92[1]) / unit,
          (machine[2] - canon.tool_offset[2] - canon.g5x[2] - canon.g92[2]) / unit};
}

auto tip(const Vec3& controlled) -> Vec3 {
  return {controlled[0] - canon.tool_offset[0], controlled[1] - canon.tool_offset[1],
          controlled[2] - canon.tool_offset[2]};
}

void expand(std::optional<Bounds>& bounds, const Vec3& point) {
  if (!bounds)
    bounds = Bounds{point, point};
  for (std::size_t axis = 0; axis < point.size(); ++axis) {
    bounds->min[axis] = std::min(bounds->min[axis], point[axis]);
    bounds->max[axis] = std::max(bounds->max[axis], point[axis]);
  }
}

void expand(std::optional<ProgramRange>& range, double value) {
  if (!std::isfinite(value) || value <= 0)
    return;
  if (!range)
    range = ProgramRange{value, value};
  range->min = std::min(range->min, value);
  range->max = std::max(range->max, value);
}

void record_units() {
  canon.check.mm |= canon.length_units == CANON_UNITS_MM;
  canon.check.inch |= canon.length_units == CANON_UNITS_INCHES;
}

void record_tool() {
  auto& tools = canon.check.tools;
  const int tool = canon.tool_in_spindle;
  if (tool > 0 && std::ranges::find(tools, tool) == tools.end())
    tools.push_back(tool);
}

auto used_frame() -> ProgramFrameCheck& {
  auto& frames = canon.check.frames;
  const auto found = std::ranges::find(frames, canon.g5x_index, &ProgramFrameCheck::wcs);
  if (found != frames.end())
    return *found;
  return frames.emplace_back(ProgramFrameCheck{canon.g5x_index, false});
}

void record_move(bool feed, const Vec3& from, const Vec3& to) {
  record_units();
  expand(canon.check.machine, from);
  expand(canon.check.machine, to);
  auto& frame = used_frame();
  frame.available = true;
  for (const auto& point : {from, to}) {
    auto work = to_program(point);
    for (auto& coordinate : work)
      coordinate *= program_unit();
    expand(frame.bounds, work);
  }
  if (feed) {
    record_tool();
    if (canon.surface_speed)
      canon.check.variable_spindle = true;
    else if (canon.spindle != CANON_STOPPED)
      expand(canon.check.spindle, std::abs(canon.spindle_rpm));
    if (canon.feed_mode != 0)
      canon.check.feed_per_revolution = true;
    else
      expand(canon.check.feed, canon.feed);
  }
}

// Whether motion may be written now; says why not, once.
auto may_move() -> bool {
  if (canon.halt != Halt::none)
    return false;
  if (canon.g5x_index != canon.request.wcs) {
    if (canon.recording)
      static_cast<void>(used_frame());
    static constexpr std::array names{"G54", "G55", "G56", "G57", "G58", "G59",
                                      "G59.1", "G59.2", "G59.3"};
    const auto index = static_cast<std::size_t>(canon.g5x_index - 1);
    halt(Halt::stop, std::format("moves in {}: only the offsets of the work system active at the "
                                 "start are known",
                                 index < names.size() ? names.at(index) : "another work system"));
    return false;
  }
  return true;
}

// One straight piece to the controlled point `to`.
void move(bool feed, const Vec3& to) {
  const Vec3 from = std::exchange(canon.end, to);
  if (!canon.recording)
    return;
  if (!canon.request.restart_line)
    record_move(feed, from, to);
  const Vec3 a = tip(from);
  const Vec3 b = tip(to);
  if (const auto target = canon.request.restart_line) {
    if (canon.line == *target)
      canon.moved_on_restart_line = true;
    else if (canon.line < *target)
      canon.highest_tip_z = std::max(canon.highest_tip_z.value_or(b[2]), b[2]);
  }
  // Time estimates need the programmed feed; only a feed per minute is a speed.
  const double rate = feed && canon.feed_mode == 0 ? canon.feed : 0.0;
  if (canon.open && (canon.open_line != canon.line || canon.open_feed != feed ||
                     canon.open_rate != rate || canon.open_tool != canon.tool_in_spindle ||
                     canon.points.back() != a))
    flush_run();
  if (!canon.open) {
    canon.open = true;
    canon.open_line = canon.line;
    canon.open_feed = feed;
    canon.open_rate = rate;
    canon.open_tool = canon.tool_in_spindle;
    canon.points.push_back(a);
    ++canon.written;
  }
  canon.points.push_back(b);
  if (++canon.written > canon.request.max_points)
    halt(Halt::cut, std::format("the preview ends here: the path has more than {} points",
                                canon.request.max_points));
}

auto find_index(int number) -> int {
  if (number == -1)
    return -1;
  if (number == 0)
    return 0;
  int found = -1;
  for (int index = 0; index <= canon.last_index; ++index)
    if (canon.tools.at(static_cast<std::size_t>(index)).toolno == number) {
      found = index;
      if (index != 0)
        break;
    }
  return found;
}

auto valid_index(int index) -> bool { return index >= 0 && index < CANON_POCKETS_MAX; }

// As io does it without a random tool changer: the spindle is entry 0.
void load_spindle(int index) {
  if (!valid_index(index))
    return;
  auto& spindle = canon.tools.at(0);
  if (index == 0) {
    spindle = tooldata_entry_init();
    spindle.toolno = 0;
  } else {
    spindle = canon.tools.at(static_cast<std::size_t>(index));
  }
  canon.tool_in_spindle = spindle.toolno < 0 ? 0 : spindle.toolno;
}

void user_m_code(int number, double /*p*/, double /*q*/) {
  halt(Halt::stop, std::format("M{} runs a program of the machine's own: what it does is not "
                               "known to the preview",
                               100 + number));
}

} // namespace

// ---- what librs274.so needs beside the canonical functions -----------------------------------

int _task = 0; // NOLINT: the interpreter's flag for "this is not task"
extern "C" {
struct PythonModule {
  const char* name;
  void* (*init)();
};
PythonModule builtin_modules[1] = {{nullptr, nullptr}}; // NOLINT: no modules for its Python
}
USER_DEFINED_FUNCTION_TYPE USER_DEFINED_FUNCTION[USER_DEFINED_FUNCTION_NUM]; // NOLINT

// The interpreter's view of the tool table: the helper's own.
struct CANON_TOOL_TABLE tooldata_entry_init() {
  CANON_TOOL_TABLE entry{};
  entry.toolno = -1;
  entry.pocketno = -1;
  return entry;
}
toolidx_t tooldata_get(CANON_TOOL_TABLE* entry, int index) {
  if (entry == nullptr || !valid_index(index))
    return IDX_FAIL;
  *entry = canon.tools.at(static_cast<std::size_t>(index));
  return IDX_OK;
}
int tooldata_find_index_for_tool(int number) { return find_index(number); }

// ---- canonical functions: motion --------------------------------------------------------------

void STRAIGHT_TRAVERSE(int, double x, double y, double z, double, double, double, double, double,
                       double) {
  if (may_move())
    move(false, to_machine(x, y, z));
}

void STRAIGHT_FEED(int, double x, double y, double z, double, double, double, double, double,
                   double) {
  if (may_move())
    move(true, to_machine(x, y, z));
}

void ARC_FEED(int, double first_end, double second_end, double first_axis, double second_axis,
              int rotation, double axis_end_point, double, double, double, double, double, double) {
  if (!may_move())
    return;
  // The plane's two axes and the one along the helix, as indexes into x, y, z.
  std::array<std::size_t, 3> axes{0, 1, 2};
  if (canon.plane == CANON_PLANE_YZ)
    axes = {1, 2, 0};
  else if (canon.plane == CANON_PLANE_XZ)
    axes = {2, 0, 1};
  else if (canon.plane != CANON_PLANE_XY) {
    halt(Halt::error, "an arc in a UVW plane cannot be shown");
    return;
  }
  const double unit = program_unit();
  const Vec3 start = to_program(canon.end);
  const auto point = [&](double first, double second, double along) -> Vec3 {
    Vec3 program{};
    program.at(axes[0]) = first;
    program.at(axes[1]) = second;
    program.at(axes[2]) = along;
    return to_machine(program[0], program[1], program[2]);
  };
  const double f0 = start.at(axes[0]);
  const double s0 = start.at(axes[1]);
  const double l0 = start.at(axes[2]);
  const double r0 = std::hypot(f0 - first_axis, s0 - second_axis);
  const double r1 = std::hypot(first_end - first_axis, second_end - second_axis);
  if (rotation == 0 || (r0 == 0 && r1 == 0)) {
    move(true, point(first_end, second_end, axis_end_point));
    return;
  }
  const double a0 = std::atan2(s0 - second_axis, f0 - first_axis);
  double sweep = std::atan2(second_end - second_axis, first_end - first_axis) - a0;
  const double turn = 2 * std::numbers::pi;
  // More than (n - 1) turns and not more than n: equal ends are a full turn.
  if (rotation > 0) {
    while (sweep <= 1e-12)
      sweep += turn;
    sweep += (rotation - 1) * turn;
  } else {
    while (sweep >= -1e-12)
      sweep -= turn;
    sweep += (rotation + 1) * turn;
  }
  // Chords whose sagitta is within the tolerance, at the larger radius, in millimetres.
  const double radius = std::max(r0, r1) * unit;
  const double tolerance = canon.request.chord_tolerance;
  const double step = radius <= tolerance ? std::numbers::pi / 2
                                          : 2 * std::acos(1 - tolerance / radius);
  const auto pieces = static_cast<std::size_t>(
      std::min(std::max(std::ceil(std::fabs(sweep) / step), 1.0), 1e7));
  for (std::size_t i = 1; i < pieces && canon.halt == Halt::none; ++i) {
    const double t = static_cast<double>(i) / static_cast<double>(pieces);
    const double r = r0 + (r1 - r0) * t;
    const double a = a0 + sweep * t;
    move(true, point(first_axis + r * std::cos(a), second_axis + r * std::sin(a),
                     l0 + (axis_end_point - l0) * t));
  }
  if (canon.halt == Halt::none)
    move(true, point(first_end, second_end, axis_end_point));
}

void RIGID_TAP(int, double x, double y, double z, double) {
  if (!may_move())
    return;
  const Vec3 from = canon.end;
  move(true, to_machine(x, y, z));
  if (canon.halt == Halt::none)
    move(true, from);
}

void NURBS_FEED(int, std::vector<CONTROL_POINT>, unsigned int) {
  halt(Halt::stop, "a NURBS move (G5.2, G5.3): not computed by the preview");
}

void STRAIGHT_PROBE(int, double, double, double, double, double, double, double, double, double,
                    unsigned char) {
  halt(Halt::stop, "a probe move (G38): where it ends depends on the machine");
}

int WAIT(int, int, int, double) {
  halt(Halt::stop, "M66 waits for an input of the machine");
  return 0;
}
int GET_EXTERNAL_DIGITAL_INPUT(int, int def) {
  halt(Halt::stop, "reads an input of the machine (M66)");
  return def;
}
double GET_EXTERNAL_ANALOG_INPUT(int, double def) {
  halt(Halt::stop, "reads an input of the machine (M66)");
  return def;
}

// ---- canonical functions: coordinate systems, units, tools ------------------------------------

void SET_G5X_OFFSET(int index, double x, double y, double z, double, double, double, double,
                    double, double) {
  const double unit = program_unit();
  canon.g5x = {x * unit, y * unit, z * unit};
  canon.g5x_index = index;
}
void SET_G92_OFFSET(double x, double y, double z, double, double, double, double, double, double) {
  const double unit = program_unit();
  canon.g92 = {x * unit, y * unit, z * unit};
}
void SET_XY_ROTATION(double degrees) { canon.rotation = degrees; }
void USE_LENGTH_UNITS(CANON_UNITS units) {
  canon.length_units = units;
  if (canon.recording)
    record_units();
}
void SELECT_PLANE(CANON_PLANE plane) { canon.plane = plane; }
void USE_TOOL_LENGTH_OFFSET(EmcPose given) {
  flush_run(); // the tool tip is elsewhere from here on
  const double unit = program_unit();
  canon.tool_offset = {given.tran.x * unit, given.tran.y * unit, given.tran.z * unit};
}

void SELECT_TOOL(int number) {
  canon.selected_index = find_index(number);
  canon.restart_inherits_preparation = false;
}
void CHANGE_TOOL(int index) {
  if (canon.halt != Halt::none)
    return;
  if (canon.restart_inherits_preparation)
    canon.restart_needs_preparation = true;
  if (canon.request.tool_change_at_g30) {
    halt(Halt::stop, "a tool change that moves to the saved G30 position "
                     "([EMCIO] TOOL_CHANGE_AT_G30)");
    return;
  }
  // Task moves to [EMCIO] TOOL_CHANGE_POSITION first: machine coordinates of the controlled point.
  if (const auto& position = canon.request.tool_change_position)
    move(false, *position);
  flush_run();
  load_spindle(index);
  if (canon.recording)
    record_tool();
  canon.selected_index = -1;
}
void CHANGE_TOOL_NUMBER(int index) {
  flush_run();
  load_spindle(index);
}
void SET_TOOL_TABLE_ENTRY(int index, int number, EmcPose given, double diameter,
                          double front_angle, double back_angle, int orientation) {
  if (canon.request.restart_line && canon.line <= *canon.request.restart_line)
    canon.changed_tool_table = true;
  if (!valid_index(index))
    return;
  auto& entry = canon.tools.at(static_cast<std::size_t>(index));
  entry.toolno = number;
  entry.offset = given;
  entry.diameter = diameter;
  entry.frontangle = front_angle;
  entry.backangle = back_angle;
  entry.orientation = orientation;
  canon.last_index = std::max(canon.last_index, index);
  if (index != 0 && number == canon.tool_in_spindle)
    canon.tools.at(0) = entry;
}
void RELOAD_TOOLDATA() {}
void START_CHANGE() {}
CANON_TOOL_TABLE GET_EXTERNAL_TOOL_TABLE(int index) {
  return valid_index(index) ? canon.tools.at(static_cast<std::size_t>(index))
                            : tooldata_entry_init();
}
int GET_EXTERNAL_TOOL_SLOT() { return find_index(canon.tool_in_spindle); }
int GET_EXTERNAL_SELECTED_TOOL_SLOT() { return canon.selected_index; }
int GET_EXTERNAL_TC_FAULT() { return 0; }
int GET_EXTERNAL_TC_REASON() { return 0; }

// ---- canonical functions: what the interpreter asks ------------------------------------------

double GET_EXTERNAL_POSITION_X() { return to_program(canon.end)[0]; }
double GET_EXTERNAL_POSITION_Y() { return to_program(canon.end)[1]; }
double GET_EXTERNAL_POSITION_Z() { return to_program(canon.end)[2]; }
double GET_EXTERNAL_POSITION_A() { return 0; }
double GET_EXTERNAL_POSITION_B() { return 0; }
double GET_EXTERNAL_POSITION_C() { return 0; }
double GET_EXTERNAL_POSITION_U() { return 0; }
double GET_EXTERNAL_POSITION_V() { return 0; }
double GET_EXTERNAL_POSITION_W() { return 0; }
double GET_EXTERNAL_TOOL_LENGTH_XOFFSET() { return canon.tool_offset[0] / program_unit(); }
double GET_EXTERNAL_TOOL_LENGTH_YOFFSET() { return canon.tool_offset[1] / program_unit(); }
double GET_EXTERNAL_TOOL_LENGTH_ZOFFSET() { return canon.tool_offset[2] / program_unit(); }
double GET_EXTERNAL_TOOL_LENGTH_AOFFSET() { return 0; }
double GET_EXTERNAL_TOOL_LENGTH_BOFFSET() { return 0; }
double GET_EXTERNAL_TOOL_LENGTH_COFFSET() { return 0; }
double GET_EXTERNAL_TOOL_LENGTH_UOFFSET() { return 0; }
double GET_EXTERNAL_TOOL_LENGTH_VOFFSET() { return 0; }
double GET_EXTERNAL_TOOL_LENGTH_WOFFSET() { return 0; }
double GET_EXTERNAL_PROBE_POSITION_X() { return 0; }
double GET_EXTERNAL_PROBE_POSITION_Y() { return 0; }
double GET_EXTERNAL_PROBE_POSITION_Z() { return 0; }
double GET_EXTERNAL_PROBE_POSITION_A() { return 0; }
double GET_EXTERNAL_PROBE_POSITION_B() { return 0; }
double GET_EXTERNAL_PROBE_POSITION_C() { return 0; }
double GET_EXTERNAL_PROBE_POSITION_U() { return 0; }
double GET_EXTERNAL_PROBE_POSITION_V() { return 0; }
double GET_EXTERNAL_PROBE_POSITION_W() { return 0; }
double GET_EXTERNAL_PROBE_VALUE() { return 0; }
int GET_EXTERNAL_PROBE_TRIPPED_VALUE() { return 0; }
double GET_EXTERNAL_LENGTH_UNITS() { return canon.request.units_per_mm; }
double GET_EXTERNAL_ANGLE_UNITS() { return 1.0; }
CANON_UNITS GET_EXTERNAL_LENGTH_UNIT_TYPE() { return canon.length_units; }
CANON_PLANE GET_EXTERNAL_PLANE() { return canon.plane; }
double GET_EXTERNAL_FEED_RATE() { return canon.feed / program_unit(); }
double GET_EXTERNAL_TRAVERSE_RATE() { return 0; }
double GET_EXTERNAL_SPEED(int) { return canon.spindle_rpm; }
CANON_DIRECTION GET_EXTERNAL_SPINDLE(int) { return canon.spindle; }
int GET_EXTERNAL_FLOOD() { return canon.flood; }
int GET_EXTERNAL_MIST() { return canon.mist; }
CANON_MOTION_MODE GET_EXTERNAL_MOTION_CONTROL_MODE() { return canon.path_control; }
double GET_EXTERNAL_MOTION_CONTROL_TOLERANCE() { return canon.path_tolerance / program_unit(); }
double GET_EXTERNAL_MOTION_CONTROL_NAIVECAM_TOLERANCE() {
  return canon.naivecam_tolerance / program_unit();
}
int GET_EXTERNAL_QUEUE_EMPTY() { return 1; }
int GET_EXTERNAL_AXIS_MASK() { return 7; } // X, Y, Z
int GET_EXTERNAL_FEED_OVERRIDE_ENABLE() { return 1; }
int GET_EXTERNAL_SPINDLE_OVERRIDE_ENABLE(int) { return 1; }
int GET_EXTERNAL_ADAPTIVE_FEED_ENABLE() { return 0; }
int GET_EXTERNAL_FEED_HOLD_ENABLE() { return 1; }
int GET_EXTERNAL_OFFSET_APPLIED() { return 0; }
EmcPose GET_EXTERNAL_OFFSETS() { return EmcPose{}; }
bool GET_BLOCK_DELETE() { return canon.request.block_delete; }
bool GET_OPTIONAL_PROGRAM_STOP() { return false; }
void GET_EXTERNAL_PARAMETER_FILE_NAME(char* name, int size) {
  if (name != nullptr && size > 0)
    std::snprintf(name, static_cast<std::size_t>(size), "%s", canon.request.parameters.c_str());
}

// ---- canonical functions without an effect on the path ---------------------------------------

void INIT_CANON() {}
void ON_RESET() {}
void FINISH() {}
void SET_PARAMETER_FILE_NAME(const char*) {}
void SET_BLOCK_DELETE(bool) {}
void SET_OPTIONAL_PROGRAM_STOP(bool) {}
void SET_FEED_RATE(double rate) { canon.feed = rate * program_unit(); }
void SET_TRAVERSE_RATE(double) {}
void SET_FEED_REFERENCE(CANON_FEED_REFERENCE) {}
void SET_FEED_MODE(int, int mode) { canon.feed_mode = mode; }
void SET_MOTION_CONTROL_MODE(CANON_MOTION_MODE mode, double tolerance) {
  canon.path_control = mode;
  canon.path_tolerance = tolerance * program_unit();
}
void SET_NAIVECAM_TOLERANCE(double tolerance) { canon.naivecam_tolerance = tolerance * program_unit(); }
void SET_CUTTER_RADIUS_COMPENSATION(double) {}
void START_CUTTER_RADIUS_COMPENSATION(int) {}
void STOP_CUTTER_RADIUS_COMPENSATION() {}
void START_SPEED_FEED_SYNCH(int, double, bool) {}
void STOP_SPEED_FEED_SYNCH() {}
void DWELL(double seconds) {
  if (!canon.recording || canon.halt != Halt::none || !(seconds > 0))
    return;
  flush_run();
  put(std::format("dwell {} {:.6f}\n", canon.line, seconds));
}
void SET_SPINDLE_MODE(int spindle, double css_maximum) {
  if (spindle == 0) {
    canon.surface_speed = css_maximum != 0;
    if (canon.recording && canon.surface_speed)
      canon.check.variable_spindle = true;
  }
}
void SET_SPINDLE_SPEED(int spindle, double speed) {
  if (spindle == 0) {
    canon.spindle_rpm = speed;
    if (canon.recording && !canon.surface_speed)
      expand(canon.check.spindle, std::abs(speed));
  }
}
void START_SPINDLE_CLOCKWISE(int spindle, int) {
  if (spindle == 0) {
    canon.spindle = CANON_CLOCKWISE;
    if (canon.recording && !canon.surface_speed)
      expand(canon.check.spindle, std::abs(canon.spindle_rpm));
  }
}
void START_SPINDLE_COUNTERCLOCKWISE(int spindle, int) {
  if (spindle == 0) {
    canon.spindle = CANON_COUNTERCLOCKWISE;
    if (canon.recording && !canon.surface_speed)
      expand(canon.check.spindle, std::abs(canon.spindle_rpm));
  }
}
void STOP_SPINDLE_TURNING(int spindle) {
  if (spindle == 0)
    canon.spindle = CANON_STOPPED;
}
void SPINDLE_RETRACT() {}
void SPINDLE_RETRACT_TRAVERSE() {}
void ORIENT_SPINDLE(int, double, int) {}
void WAIT_SPINDLE_ORIENT_COMPLETE(int, double) {}
void USE_NO_SPINDLE_FORCE() {}
void CLAMP_AXIS(CANON_AXIS) {}
void COMMENT(const char*) {}
void MESSAGE(char*) {}
void LOG(char*) {}
void LOGOPEN(char*) {}
void LOGAPPEND(char*) {}
void LOGCLOSE() {}
void DISABLE_ADAPTIVE_FEED() {}
void ENABLE_ADAPTIVE_FEED() {}
void DISABLE_FEED_OVERRIDE() {}
void ENABLE_FEED_OVERRIDE() {}
void DISABLE_SPEED_OVERRIDE(int) {}
void ENABLE_SPEED_OVERRIDE(int) {}
void DISABLE_FEED_HOLD() {}
void ENABLE_FEED_HOLD() {}
void FLOOD_OFF() { canon.flood = false; }
void FLOOD_ON() { canon.flood = true; }
void MIST_OFF() { canon.mist = false; }
void MIST_ON() { canon.mist = true; }
void PALLET_SHUTTLE() {}
void TURN_PROBE_OFF() {}
void TURN_PROBE_ON() {}
void OPTIONAL_PROGRAM_STOP() {}
void PROGRAM_END() {}
void PROGRAM_STOP() {}
void mark_output() {
  if (canon.request.restart_line && canon.line <= *canon.request.restart_line)
    canon.used_output = true;
}
void SET_MOTION_OUTPUT_BIT(int) { mark_output(); }
void CLEAR_MOTION_OUTPUT_BIT(int) { mark_output(); }
void SET_AUX_OUTPUT_BIT(int) { mark_output(); }
void CLEAR_AUX_OUTPUT_BIT(int) { mark_output(); }
void SET_MOTION_OUTPUT_VALUE(int, double) { mark_output(); }
void SET_AUX_OUTPUT_VALUE(int, double) { mark_output(); }
int UNLOCK_ROTARY(int, int) { return 0; }
int LOCK_ROTARY(int, int) { return 0; }
void PLUGIN_CALL(int, const char*) {}
void IO_PLUGIN_CALL(int, const char*) {}
void UPDATE_TAG(StateTag) {}
void CANON_ERROR(const char* format, ...) {
  std::array<char, 512> text{};
  va_list arguments;
  va_start(arguments, format);
  std::vsnprintf(text.data(), text.size(), format, arguments); // NOLINT: LinuxCNC's signature
  va_end(arguments);
  halt(Halt::error, text.data());
}

// ---- the run ----------------------------------------------------------------------------------

namespace {

auto error_text(InterpBase& interp, int code) -> std::string {
  std::array<char, 512> text{};
  interp.error_text(code, text.data(), text.size());
  return text.data();
}

auto is_error(int code) -> bool { return code > INTERP_MIN_ERROR; }

using GCodes = std::array<int, ACTIVE_G_CODES>;

auto modal_g_codes(InterpBase& interp) -> GCodes {
  GCodes codes{};
  interp.active_g_codes(codes.data());
  return codes;
}

auto capture_restart(InterpBase& interp, const GCodes& codes) -> RestartPoint {
  std::array<double, ACTIVE_SETTINGS> settings{};
  interp.active_settings(settings.data());
  auto start = to_program(canon.end);
  for (auto& coordinate : start)
    coordinate *= program_unit();
  const auto direction = canon.spindle == CANON_CLOCKWISE          ? coffeecnc::SpindleDirection::forward
                         : canon.spindle == CANON_COUNTERCLOCKWISE ? coffeecnc::SpindleDirection::reverse
                                                                  : coffeecnc::SpindleDirection::stopped;
  const double peak = canon.highest_tip_z.value_or(tip(canon.end)[2]);
  return {.line = canon.line,
          .tool = canon.tool_in_spindle,
          .wcs = canon.g5x_index,
          .h = canon.tool_offset == Vec3{} ? 0 : canon.tool_in_spindle,
          .spindle_direction = direction,
          .spindle_rpm = canon.spindle_rpm,
          .flood = canon.flood,
          .mist = canon.mist,
          .feed = canon.feed,
          .units = canon.length_units == CANON_UNITS_INCHES ? coffeecnc::LengthUnit::inch
                                                            : coffeecnc::LengthUnit::mm,
          .plane = codes[3] / 10,
          .incremental = codes[6] == 910,
          .absolute_arc = codes[14] == 901,
          .motion = codes[1],
          .path_control = codes[11],
          .path_tolerance = std::max(0.0, settings[3]) * program_unit(),
          .naivecam_tolerance = std::max(0.0, settings[4]) * program_unit(),
          .clearance_z = std::max(start[2], peak - canon.g5x[2] - canon.g92[2]),
          .start = start,
          .g5x = canon.g5x,
          .g92 = canon.g92,
          .tool_offset = canon.tool_offset,
          .rotation = canon.rotation};
}

auto restart_refusal(const RestartPoint& point, const GCodes& before, const GCodes& after,
                     std::string_view block) -> std::string {
  const auto canned = [](int code) { return code == 730 || code == 760 ||
                                           (code >= 810 && code <= 890); };
  if (before[4] != 400 || after[4] != 400)
    return "Cutter compensation is active at this line.";
  if (canned(before[1]) || canned(after[1]))
    return "This line is inside a canned cycle.";
  if (before[7] != 940 || after[7] != 940 || before[13] != 970 || after[13] != 970)
    return "This line requires a feed or spindle mode that cannot be restored for restart.";
  if (!canon.moved_on_restart_line)
    return "Choose a line that moves the tool.";
  if (after[1] != 0 && after[1] != 10 && after[1] != 20 && after[1] != 30)
    return "Choose a rapid, feed, or arc move for restart.";
  if (before[1] != 0 && before[1] != 10 && before[1] != 20 && before[1] != 30 && before[1] != 800)
    return "The prior motion mode cannot be restored for restart.";
  if ((after[1] == 20 || after[1] == 30) &&
      coffeecnc::motion_use(block) != coffeecnc::MotionUse::sets)
    return "Choose an arc block with an explicit G2 or G3.";
  if (canon.changed_tool_table || canon.used_output)
    return "The skipped blocks change tool data or machine outputs.";
  if (point.wcs != canon.g5x_index || point.g5x != canon.g5x || point.g92 != canon.g92 ||
      point.rotation != canon.rotation || point.tool != canon.tool_in_spindle ||
      point.tool_offset != canon.tool_offset)
    return "This line changes the coordinate frame or tool before moving.";
  if (point.plane < 17 || point.plane > 19 ||
      (point.spindle_direction != coffeecnc::SpindleDirection::stopped &&
       point.spindle_rpm <= 0))
    return "The spindle or motion plane at this line is incomplete.";
  if (point.tool == 0 && point.tool_offset == Vec3{})
    return {};
  const int index = find_index(point.tool);
  if (index < 0 || point.h <= 0)
    return "The required tool length is not active at this line.";
  const auto& offset = canon.tools.at(static_cast<std::size_t>(index)).offset.tran;
  const double unit = canon.request.units_per_mm;
  if (std::abs(point.tool_offset[0] - offset.x / unit) > 1e-6 ||
      std::abs(point.tool_offset[1] - offset.y / unit) > 1e-6 ||
      std::abs(point.tool_offset[2] - offset.z / unit) > 1e-6 || point.tool_offset[2] == 0)
    return "The tool length at this line does not match the loaded tool table.";
  return {};
}

auto inherited_reason(const std::string& mode) -> std::string {
  return std::format("moves by the motion mode {} that was active before the program, which "
                     "the preview does not take over",
                     mode);
}

// Nothing can be computed: an error at line 0.
auto give_up(const std::string& why) -> int {
  halt(Halt::error, why);
  put("end error\n");
  std::fflush(sink);
  return 0;
}

auto run(const HelperRequest& request) -> int {
  canon.request = request;
  canon.end = request.position;
  canon.tool_offset = request.tool_offset;
  canon.tool_in_spindle = request.tool_in_spindle;
  canon.feed = request.feed;
  canon.length_units = request.units_per_mm == 1.0 ? CANON_UNITS_MM : CANON_UNITS_INCHES;
  canon.tools.fill(tooldata_entry_init());
  for (const auto& tool : request.tools) {
    if (!valid_index(tool.index))
      continue;
    auto& entry = canon.tools.at(static_cast<std::size_t>(tool.index));
    entry.toolno = tool.number;
    entry.pocketno = tool.pocket;
    entry.offset.tran = {tool.offset[0], tool.offset[1], tool.offset[2]};
    entry.diameter = tool.diameter;
    entry.frontangle = tool.front_angle;
    entry.backangle = tool.back_angle;
    entry.orientation = tool.orientation;
    canon.last_index = std::max(canon.last_index, tool.index);
  }
  // The spindle is entry 0; a table given without it gets the entry of the tool in the spindle.
  if (canon.tools.at(0).toolno < 0 && request.tool_in_spindle > 0)
    if (const int index = find_index(request.tool_in_spindle); index > 0)
      canon.tools.at(0) = canon.tools.at(static_cast<std::size_t>(index));
  canon.selected_index = request.tool_prepared < 0 ? -1 : find_index(request.tool_prepared);
  for (auto& function : USER_DEFINED_FUNCTION)
    function = user_m_code;

  // Before the interpreter reads anything: no file it could read holds a comment that runs
  // Python or writes a file, or an O word that is not plainly a name. The interpreter acts on
  // `;py,` while it READS a line, reads lines it never executes, and looks for the file of a
  // called name with shell expansion. The executable has scanned its copies already; this
  // does not rely on it.
  {
    // The executable bounds a program and its subroutine files at 16 MiB together.
    constexpr std::size_t max_file_bytes = std::size_t{16} << 20;
    std::vector<std::filesystem::path> files{request.program};
    std::error_code error;
    if (!request.subroutines.empty())
      for (const auto& entry : std::filesystem::directory_iterator(request.subroutines, error))
        files.push_back(entry.path());
    if (error)
      return give_up("the directory of the subroutine files cannot be read");
    for (const auto& file : files) {
      const auto text = coffeecnc::read_text_file(file.string(), max_file_bytes);
      if (!text && text.error() == coffeecnc::TextFileError::too_large)
        return give_up("a file of the program is larger than the preview reads");
      const auto found = text ? coffeecnc::first_forbidden_line(*text) : std::nullopt;
      if (!found)
        continue;
      if (file == request.program)
        canon.line = found->line;
      else
        canon.elsewhere = std::format("{} line {}: ", file.filename().string(), found->line);
      halt(Halt::stop, found->what);
      put("end stop\n");
      std::fflush(sink);
      return 0;
    }
  }

  // Never deleted: the process ends without the interpreter's exit, which writes files and
  // finalises its Python.
  InterpBase* const interp = makeInterp();
  // The interpreter reads the parameter file, and with it the work system and G92 in force.
  if (const int code = interp->init(); is_error(code))
    return give_up("the interpreter did not start: " + error_text(*interp, code));
  if (canon.g5x_index != request.wcs)
    return give_up("the parameter file does not select the active work system");
  // Task's modal state, block by block.
  for (const auto& code : request.codes) {
    int result = interp->read(code.c_str());
    if (!is_error(result))
      result = interp->execute();
    if (is_error(result) || canon.halt != Halt::none)
      return give_up(
          std::format("the start state `{}` was refused: {}", code, error_text(*interp, result)));
  }
  if (const int code = interp->open(request.program.c_str()); is_error(code))
    return give_up("the program cannot be opened: " + error_text(*interp, code));

  canon.recording = true;
  std::array<char, LINELEN + 1> text{};
  std::array<char, PATH_MAX + 1> file{};
  std::string sub_file;
  int sub_line = 0;
  bool after_o_word = false;
  std::string inherited = request.inherited_motion;
  constexpr auto flush_period = std::chrono::milliseconds{50};
  auto flushed = std::chrono::steady_clock::now();
  bool ended = false;
  std::optional<RestartPoint> restart;
  std::string restart_blocked;
  while (canon.halt == Halt::none && !ended) {
    // What is finished goes out, so that it is there if the helper is ended.
    if (const auto now = std::chrono::steady_clock::now(); now - flushed > flush_period) {
      if (canon.open && canon.open_line != canon.line)
        flush_run();
      std::fflush(sink);
      flushed = now;
    }
    int result = interp->read();
    if (is_error(result)) {
      // A line that cannot be read. The interpreter has already left every subroutine, so
      // where it was is taken from the line before: the next line of that file, unless that
      // line was an O word, after which the interpreter may be anywhere.
      if (!canon.elsewhere.empty())
        canon.elsewhere = after_o_word ? std::string("in a subroutine file: ")
                                       : std::format("{} line {}: ", sub_file, sub_line + 1);
      else if (!after_o_word)
        canon.line = interp->line();
      // The interpreter, in G80, refuses at reading a block that would move by a mode this
      // preview did not take over: that is the preview's end, not the program's error.
      text.fill(0);
      interp->line_text(text.data(), text.size() - 1);
      if (!inherited.empty() &&
          coffeecnc::motion_use(text.data()) == coffeecnc::MotionUse::inherits)
        halt(Halt::stop, inherited_reason(inherited));
      else
        halt(Halt::error, error_text(*interp, result));
      break;
    }
    // Where the interpreter is: in the program, or in a subroutine file a call led to.
    file.fill(0);
    interp->file_name(file.data(), file.size() - 1);
    if (request.program == file.data()) {
      canon.line = interp->line();
      canon.elsewhere.clear();
    } else {
      const std::string_view path{file.data()};
      sub_file = path.substr(path.rfind('/') + 1);
      sub_line = interp->line();
      canon.elsewhere = std::format("{} line {}: ", sub_file, sub_line);
    }
    if (result == INTERP_ENDFILE)
      break;
    text.fill(0);
    interp->line_text(text.data(), text.size() - 1);
    after_o_word = coffeecnc::bare_block(text.data()).starts_with('o');
    if (request.restart_line && coffeecnc::o_word_name(text.data()))
      canon.subroutine = true;
    if (const auto reason = coffeecnc::refused_block(text.data())) {
      halt(Halt::stop, *reason);
      break;
    }
    // Until the program names a motion code, a mode that was not put in place is not known
    // to this interpreter: a block that would move by it is where the preview ends.
    if (!inherited.empty()) {
      const auto use = coffeecnc::motion_use(text.data());
      if (use == coffeecnc::MotionUse::inherits) {
        halt(Halt::stop, inherited_reason(inherited));
        break;
      }
      if (use == coffeecnc::MotionUse::sets)
        inherited.clear();
    }
    const bool target = request.restart_line == canon.line && canon.elsewhere.empty();
    GCodes before{};
    if (target) {
      before = modal_g_codes(*interp);
      restart = capture_restart(*interp, before);
      // LinuxCNC parses a skipped T word but drops its IO prepare command. A subsequent
      // M6 would use the slot that IO actually has prepared.
      canon.restart_inherits_preparation = canon.selected_index >= 0;
    }
    result = interp->execute();
    if (canon.halt != Halt::none)
      break;
    if (is_error(result)) {
      halt(Halt::error, error_text(*interp, result));
      break;
    }
    if (target)
      restart_blocked = restart_refusal(*restart, before, modal_g_codes(*interp), text.data());
    if (result == INTERP_EXIT)
      ended = true;
    else if (result == INTERP_EXECUTE_FINISH)
      // Task lets the queue drain and takes the machine's state again: here it is the canon's.
      interp->synch();
  }
  flush_run();
  if (!request.restart_line)
    put(coffeecnc::write_check(canon.check));
  if (request.restart_line && canon.halt == Halt::none) {
    if (canon.subroutine)
      restart_blocked = "LinuxCNC run from line cannot safely skip blocks in a program with subroutines.";
    else if (canon.restart_needs_preparation)
      restart_blocked = "A later M6 depends on a tool selected before this restart line.";
    else if (!restart)
      restart_blocked = "The selected line is not executed in this program.";
    if (!restart_blocked.empty())
      put(std::format("restart_blocked {}\n", restart_blocked));
    else if (restart)
      put(coffeecnc::write_restart(*restart));
  }
  put(canon.halt == Halt::none    ? "end ok\n"
      : canon.halt == Halt::error ? "end error\n"
      : canon.halt == Halt::stop  ? "end stop\n"
                                  : "end cut\n");
  std::fflush(sink);
  return 0;
}

} // namespace

auto main(int argc, char** argv) -> int {
  char* end = nullptr;
  const long descriptor = argc == 3 ? std::strtol(argv[2], &end, 10) : -1;
  if (argc != 3 || end == argv[2] || *end != 0 || descriptor < 3 ||
      (sink = fdopen(static_cast<int>(descriptor), "w")) == nullptr) {
    std::fputs("usage: coffeecnc-preview <request file> <result descriptor, 3 or above>\n",
               stderr);
    return 2;
  }
  // The request: start state, work systems and the tool table as text.
  constexpr std::size_t max_request_bytes = std::size_t{16} << 20;
  const auto text = coffeecnc::read_text_file(argv[1], max_request_bytes);
  if (!text) {
    std::fputs("coffeecnc-preview: the request file cannot be read\n", stderr);
    return 2;
  }
  const auto request = coffeecnc::parse_request(*text);
  if (!request) {
    std::fprintf(stderr, "coffeecnc-preview: %s\n", request.error().c_str());
    return 2;
  }
  const int code = run(*request);
  std::fflush(sink);
  std::_Exit(code);
}
