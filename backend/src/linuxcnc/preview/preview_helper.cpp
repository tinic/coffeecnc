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

#include <array>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

using coffeecnc::HelperRequest;
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
  std::array<CANON_TOOL_TABLE, CANON_POCKETS_MAX> tools{};
  int last_index{};

  // The program line being executed, and whether motion is written (not while the start state
  // is being put in place).
  int line{};
  bool recording{};
  // The run of motion being collected.
  bool open{};
  int open_line{};
  bool open_feed{};
  int open_tool{};
  std::vector<Vec3> points;
  std::size_t written{};

  Halt halt{Halt::none};
};

Canon canon;    // NOLINT: the canonical functions are free functions
FILE* sink{}; // NOLINT: where the result lines go

void put(const std::string& text) { std::fputs(text.c_str(), sink); }

void flush_run() {
  if (!canon.open)
    return;
  canon.open = false;
  std::string out = std::format("seg {} {} {}", canon.open_line, canon.open_feed ? 'f' : 'r',
                                canon.open_tool);
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
  put(std::format("{} {} {}\n", how == Halt::error ? "error" : how == Halt::stop ? "stop" : "cut",
                  canon.line, clean));
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

// Whether motion may be written now; says why not, once.
auto may_move() -> bool {
  if (canon.halt != Halt::none)
    return false;
  if (canon.g5x_index != canon.request.wcs) {
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
  const Vec3 a = tip(from);
  const Vec3 b = tip(to);
  if (canon.open && (canon.open_line != canon.line || canon.open_feed != feed ||
                     canon.open_tool != canon.tool_in_spindle || canon.points.back() != a))
    flush_run();
  if (!canon.open) {
    canon.open = true;
    canon.open_line = canon.line;
    canon.open_feed = feed;
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
void USE_LENGTH_UNITS(CANON_UNITS units) { canon.length_units = units; }
void SELECT_PLANE(CANON_PLANE plane) { canon.plane = plane; }
void USE_TOOL_LENGTH_OFFSET(EmcPose given) {
  flush_run(); // the tool tip is elsewhere from here on
  const double unit = program_unit();
  canon.tool_offset = {given.tran.x * unit, given.tran.y * unit, given.tran.z * unit};
}

void SELECT_TOOL(int number) { canon.selected_index = find_index(number); }
void CHANGE_TOOL(int index) {
  if (canon.halt != Halt::none)
    return;
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
  canon.selected_index = -1;
}
void CHANGE_TOOL_NUMBER(int index) {
  flush_run();
  load_spindle(index);
}
void SET_TOOL_TABLE_ENTRY(int index, int number, EmcPose given, double diameter,
                          double front_angle, double back_angle, int orientation) {
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
double GET_EXTERNAL_SPEED(int) { return 0; }
CANON_DIRECTION GET_EXTERNAL_SPINDLE(int) { return CANON_STOPPED; }
int GET_EXTERNAL_FLOOD() { return 0; }
int GET_EXTERNAL_MIST() { return 0; }
CANON_MOTION_MODE GET_EXTERNAL_MOTION_CONTROL_MODE() { return CANON_CONTINUOUS; }
double GET_EXTERNAL_MOTION_CONTROL_TOLERANCE() { return 0; }
double GET_EXTERNAL_MOTION_CONTROL_NAIVECAM_TOLERANCE() { return 0; }
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
void SET_FEED_MODE(int, int) {}
void SET_MOTION_CONTROL_MODE(CANON_MOTION_MODE, double) {}
void SET_NAIVECAM_TOLERANCE(double) {}
void SET_CUTTER_RADIUS_COMPENSATION(double) {}
void START_CUTTER_RADIUS_COMPENSATION(int) {}
void STOP_CUTTER_RADIUS_COMPENSATION() {}
void START_SPEED_FEED_SYNCH(int, double, bool) {}
void STOP_SPEED_FEED_SYNCH() {}
void DWELL(double) {}
void SET_SPINDLE_MODE(int, double) {}
void SET_SPINDLE_SPEED(int, double) {}
void START_SPINDLE_CLOCKWISE(int, int) {}
void START_SPINDLE_COUNTERCLOCKWISE(int, int) {}
void STOP_SPINDLE_TURNING(int) {}
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
void FLOOD_OFF() {}
void FLOOD_ON() {}
void MIST_OFF() {}
void MIST_ON() {}
void PALLET_SHUTTLE() {}
void TURN_PROBE_OFF() {}
void TURN_PROBE_ON() {}
void OPTIONAL_PROGRAM_STOP() {}
void PROGRAM_END() {}
void PROGRAM_STOP() {}
void SET_MOTION_OUTPUT_BIT(int) {}
void CLEAR_MOTION_OUTPUT_BIT(int) {}
void SET_AUX_OUTPUT_BIT(int) {}
void CLEAR_AUX_OUTPUT_BIT(int) {}
void SET_MOTION_OUTPUT_VALUE(int, double) {}
void SET_AUX_OUTPUT_VALUE(int, double) {}
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
  canon.selected_index = request.tool_prepared < 0 ? -1 : find_index(request.tool_prepared);
  for (auto& function : USER_DEFINED_FUNCTION)
    function = user_m_code;

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
  bool ended = false;
  while (canon.halt == Halt::none && !ended) {
    int result = interp->read();
    if (interp->call_level() == 0)
      canon.line = interp->line();
    if (result == INTERP_ENDFILE)
      break;
    if (is_error(result)) {
      halt(Halt::error, error_text(*interp, result));
      break;
    }
    text.fill(0);
    interp->line_text(text.data(), text.size() - 1);
    if (const auto reason = coffeecnc::refused_block(text.data())) {
      halt(Halt::stop, *reason);
      break;
    }
    result = interp->execute();
    if (canon.halt != Halt::none)
      break;
    if (is_error(result)) {
      halt(Halt::error, error_text(*interp, result));
      break;
    }
    if (result == INTERP_EXIT)
      ended = true;
    else if (result == INTERP_EXECUTE_FINISH)
      // Task lets the queue drain and takes the machine's state again: here it is the canon's.
      interp->synch();
    if (canon.written % 4096 < 2)
      std::fflush(sink);
  }
  flush_run();
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
  std::ifstream in{argv[1], std::ios::binary};
  const std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
  const auto request = coffeecnc::parse_request(text);
  if (!in || !request) {
    std::fprintf(stderr, "coffeecnc-preview: %s\n",
                 request ? "the request file cannot be read" : request.error().c_str());
    return 2;
  }
  const int code = run(*request);
  std::fflush(sink);
  std::_Exit(code);
}
