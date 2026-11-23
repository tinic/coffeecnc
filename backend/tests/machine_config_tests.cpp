// load_machine_config and parse_jog_increments against INI files written here, and against the
// INI files tests/sim/run.sh assembles for its profiles. No LinuxCNC is started.
//   coffeecnc_machine_config_tests <tests/sim/run.sh> <scratch directory> <coffeecnc.ini.example>

#include <coffeecnc/linuxcnc/machine_config.hpp>
#include <coffeecnc/core/text_file.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace coffeecnc;

namespace {

int checks = 0;
std::filesystem::path scratch;
std::string runner;
std::string example;

void require(bool condition, const std::string& message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

auto near(double value, double expected) -> bool { return std::fabs(value - expected) < 1e-9; }

auto same(const std::vector<double>& values, const std::vector<double>& expected) -> bool {
  if (values.size() != expected.size())
    return false;
  for (std::size_t i = 0; i < values.size(); ++i)
    if (!near(values[i], expected[i]))
      return false;
  return true;
}

auto has(const std::string& text, std::string_view part) -> bool {
  return text.find(part) != std::string::npos;
}

// The text with one occurrence of `old` replaced; `old` must be there.
auto with(std::string text, std::string_view old, std::string_view replacement) -> std::string {
  const auto at = text.find(old);
  if (at == std::string::npos)
    throw std::runtime_error(std::format("Test INI has no \"{}\"", old));
  return text.replace(at, old.size(), replacement);
}

// The G0759's values, cut down to what the loader reads.
constexpr std::string_view g0759 = R"([EMC]
MACHINE = G0759-shaped

[DISPLAY]
MAX_FEED_OVERRIDE = 2.000000
MAX_SPINDLE_OVERRIDE = 1.000000
MIN_SPINDLE_OVERRIDE = 0.500000
PROGRAM_PREFIX = /srv/nc_files
INCREMENTS = 5mm 1mm .5mm .1mm .05mm .01mm .005mm
MAX_LINEAR_VELOCITY = 25.000000

[SPINDLE]
MAX_FREQ = 80
MAX_RPM = 1500

[COFFEECNC]
MAX_SPINDLE_RPM = 1500

[HAL]
POSTGUI_HALFILE = postgui_call_list.hal

[KINS]
JOINTS = 3
KINEMATICS = trivkins coordinates=XYZ

[TRAJ]
COORDINATES =  XYZ
LINEAR_UNITS = mm
MAX_LINEAR_VELOCITY = 25.00
NO_FORCE_HOMING = 1

[EMCIO]
TOOL_TABLE = tool.tbl

[AXIS_X]
MAX_VELOCITY = 25.0
MIN_LIMIT = -1000.0
MAX_LIMIT = 1000.0

[JOINT_0]
HOME = 0.0

[AXIS_Y]
MAX_VELOCITY = 25.0
MIN_LIMIT = -1000.0
MAX_LIMIT = 1000.0

[JOINT_1]
HOME = 0.0

[AXIS_Z]
MAX_VELOCITY = 12.5
MIN_LIMIT = -1000.0
MAX_LIMIT = 1000.0

[JOINT_2]
HOME = 0.0

[SPINDLE_0]
MAX_OUTPUT = 2000
)";

auto load(std::string_view text) -> std::expected<MachineConfig, std::string> {
  const auto file = scratch / "machine.ini";
  {
    std::ofstream out{file, std::ios::trunc};
    out << text;
  }
  return load_machine_config(file.string());
}

// The start-up error for this INI text; empty when it loads.
auto error(std::string_view text) -> std::string {
  const auto config = load(text);
  return config ? std::string() : config.error();
}

void increments() {
  const auto list = [](std::string_view text,
                       double scale = 1) -> std::expected<std::vector<double>, std::string> {
    return parse_jog_increments(text, scale);
  };
  const auto fails = [&](std::string_view text, std::string_view entry) -> bool {
    const auto parsed = list(text);
    return !parsed && has(parsed.error(), "[DISPLAY] INCREMENTS") &&
           has(parsed.error(), std::format("\"{}\"", entry));
  };
  require(same(list("5mm 1mm .5mm .1mm .05mm .01mm .005mm").value(),
               {5, 1, 0.5, 0.1, 0.05, 0.01, 0.005}),
          "the G0759 list");
  require(same(list("1mm 1cm 100um 1in 1inch 1mil").value(), {1, 10, 0.1, 25.4, 25.4, 0.0254}),
          "every suffix");
  require(same(list("0.1 0.01", 25.4).value(), {2.54, 0.254}) &&
              same(list("2 0.5").value(), {2, 0.5}),
          "a plain number is in machine units");
  require(same(list("1/16in 1/2 3/4mm", 25.4).value(), {1.5875, 12.7, 0.75}), "fractions");
  require(same(list("1 mm, .5 mm,0.1in , 1/16 in").value(), {1, 0.5, 2.54, 1.5875}),
          "with a comma, commas separate and spaces belong to the entry");
  require(same(list("continuous 1mm Continuous").value(), {1}) &&
              same(list("continuous, 1mm,").value(), {1}),
          "the word continuous is skipped");
  require(same(list("1MM 1Inch").value(), {1, 25.4}), "suffixes in any case");
  require(list("").value().empty() && list("  \t ").value().empty(), "no entries");
  require(fails("1mm abc 2mm", "abc"), "a word is not a length");
  require(fails("1mm, 2 furlongs", "2furlongs"), "an unknown suffix");
  require(fails("1/0in", "1/0in") && fails("1/in", "1/in") && fails("1/2/3mm", "1/2/3mm"),
          "a broken fraction");
  require(fails("0mm", "0mm") && fails("1mm -1mm", "-1mm"), "a length must be above zero");
  require(fails("mm", "mm") && fails("1mmm", "1mmm") && fails("1e999mm", "1e999mm"),
          "a suffix needs a finite number");
}

void g0759_shape() {
  const auto config = load(g0759);
  require(config.has_value(), config ? "" : config.error());
  require(config->machine_name == "G0759-shaped" && config->machine_units == LengthUnit::mm &&
              config->millimetres_per_unit == 1 &&
              config->axes == std::array<Axis, 3>{Axis::x, Axis::y, Axis::z},
          "name, units and axes");
  require(same(config->jog_increments, {5, 1, 0.5, 0.1, 0.05, 0.01, 0.005}), "increments in mm");
  const auto& limits = config->limits;
  require(near(limits.max_jog_velocity, 1500) && near(limits.max_axis_jog_velocity[0], 1500) &&
              near(limits.max_axis_jog_velocity[1], 1500) &&
              near(limits.max_axis_jog_velocity[2], 750),
          "jog ceilings in mm/min");
  require(near(limits.max_feed_override, 2) && near(limits.min_spindle_override, 0.5) &&
              near(limits.max_spindle_override, 1) && near(limits.max_spindle_rpm, 1500),
          "override limits and spindle ceiling");
  require(!limits.homing_required && !config->home_all_configured, "homing as configured");
  require(near(config->travel[0].min, -1000) && near(config->travel[2].max, 1000), "travel");
  require(!config->signals.spindle_rpm && !config->signals.spindle_at_speed &&
              !config->signals.probe,
          "no optional signal unless declared");
  require(config->tool_table == (scratch / "tool.tbl").string() &&
              config->postgui_halfile == (scratch / "postgui_call_list.hal").string() &&
              config->program_prefix == "/srv/nc_files",
          "paths relative to the INI file");
}

void spindle_ceiling() {
  constexpr std::string_view own = "MAX_SPINDLE_RPM = 1500\n";
  constexpr std::string_view standard = "[SPINDLE_0]\n";
  constexpr std::string_view display = "[DISPLAY]\n";
  const auto all =
      with(with(std::string(g0759), standard, "[SPINDLE_0]\nMAX_FORWARD_VELOCITY = 3000\n"),
           display, "[DISPLAY]\nMAX_SPINDLE_0_SPEED = 2000\n");
  require(near(load(all).value().limits.max_spindle_rpm, 1500), "CoffeeCNC's own setting wins");
  require(near(load(with(all, own, "")).value().limits.max_spindle_rpm, 3000),
          "then [SPINDLE_0] MAX_FORWARD_VELOCITY");
  require(near(load(with(with(all, own, ""), "MAX_FORWARD_VELOCITY = 3000\n", ""))
                   .value()
                   .limits.max_spindle_rpm,
               2000),
          "then [DISPLAY] MAX_SPINDLE_0_SPEED");
  // The machine's own [SPINDLE] MAX_RPM and [SPINDLE_0] MAX_OUTPUT are still there: not read.
  const auto none = error(with(std::string(g0759), own, ""));
  require(has(none, "[COFFEECNC] MAX_SPINDLE_RPM") &&
              has(none, "[SPINDLE_0] MAX_FORWARD_VELOCITY") &&
              has(none, "[DISPLAY] MAX_SPINDLE_0_SPEED"),
          "no ceiling names all three places: " + none);
  for (const auto bad : {"abc", "0", "-1500", "1500rpm"}) {
    const auto message = error(with(all, own, std::format("MAX_SPINDLE_RPM = {}\n", bad)));
    require(
        has(message, "[COFFEECNC] MAX_SPINDLE_RPM") && !has(message, "MAX_FORWARD_VELOCITY"),
        std::format("an invalid own setting \"{}\" is an error, never skipped: {}", bad, message));
  }
  require(has(error(with(with(all, own, ""), "MAX_FORWARD_VELOCITY = 3000",
                         "MAX_FORWARD_VELOCITY = 0")),
              "above zero"),
          "a standard key of zero is an error");
}

void homing() {
  constexpr std::string_view key = "NO_FORCE_HOMING = 1\n";
  const auto required = [&](std::string_view line) -> bool {
    return load(with(std::string(g0759), key, line)).value().limits.homing_required;
  };
  require(required(""), "absent: homing required, LinuxCNC's default");
  require(required("NO_FORCE_HOMING = 0\n") && required("NO_FORCE_HOMING = -1\n"),
          "zero or below: homing required");
  require(!required("NO_FORCE_HOMING = 1\n") && !required("NO_FORCE_HOMING = 2\n"),
          "above zero: homing not required");
  for (const auto bad : {"yes", "true", "x1"}) {
    const auto message =
        error(with(std::string(g0759), key, std::format("NO_FORCE_HOMING = {}\n", bad)));
    require(has(message, "[TRAJ] NO_FORCE_HOMING") && has(message, bad),
            "text without a number is an error: " + message);
  }
  auto sequenced = std::string(g0759);
  for (const auto joint : {"[JOINT_0]\n", "[JOINT_1]\n"})
    sequenced = with(sequenced, joint, std::string(joint) + "HOME_SEQUENCE = 1\n");
  require(!load(sequenced).value().home_all_configured, "home all needs every joint in a sequence");
  sequenced = with(sequenced, "[JOINT_2]\n", "[JOINT_2]\nHOME_SEQUENCE = 0\n");
  require(load(sequenced).value().home_all_configured, "home all with every joint in a sequence");
}

void axis_ceilings() {
  const auto fast = load(
      with(std::string(g0759), "[AXIS_X]\nMAX_VELOCITY = 25.0", "[AXIS_X]\nMAX_VELOCITY = 40.0"));
  require(near(fast.value().limits.max_axis_jog_velocity[0], 1500) &&
              near(fast.value().limits.max_axis_jog_velocity[2], 750),
          "an axis ceiling is never above the global maximum");
  const auto missing = error(with(std::string(g0759), "MAX_VELOCITY = 12.5\n", ""));
  require(has(missing, "[AXIS_Z] MAX_VELOCITY"), "a missing axis velocity names it: " + missing);
  const auto zero = error(with(std::string(g0759), "MAX_VELOCITY = 12.5", "MAX_VELOCITY = 0"));
  require(has(zero, "[AXIS_Z] MAX_VELOCITY"), "an axis velocity of zero names it: " + zero);
  // Inch machine: 1 in/s overall, Z 0.5 in/s, increments in inches unless they say otherwise.
  auto inch = with(std::string(g0759), "LINEAR_UNITS = mm", "LINEAR_UNITS = inch");
  inch = with(inch, "MAX_LINEAR_VELOCITY = 25.000000", "MAX_LINEAR_VELOCITY = 1");
  inch = with(inch, "MAX_VELOCITY = 12.5", "MAX_VELOCITY = 0.5");
  inch = with(inch, "INCREMENTS = 5mm 1mm .5mm .1mm .05mm .01mm .005mm",
              "INCREMENTS = 0.1, 0.01, 1/16, 1 mm");
  const auto config = load(inch);
  require(config.has_value(), config ? "" : config.error());
  require(config->machine_units == LengthUnit::inch &&
              near(config->limits.max_jog_velocity, 1524) &&
              near(config->limits.max_axis_jog_velocity[0], 1524) &&
              near(config->limits.max_axis_jog_velocity[2], 762) &&
              same(config->jog_increments, {2.54, 0.254, 1.5875, 1}),
          "an inch machine is reported in mm and mm/min");
}

void increments_and_signals() {
  constexpr std::string_view list = "INCREMENTS = 5mm 1mm .5mm .1mm .05mm .01mm .005mm\n";
  require(load(with(std::string(g0759), list, "")).value().jog_increments.empty(),
          "no INCREMENTS: none offered");
  const auto bad = error(with(std::string(g0759), list, "INCREMENTS = 1mm 0.5mn 0.1mm\n"));
  require(has(bad, "[DISPLAY] INCREMENTS") && has(bad, "\"0.5mn\""),
          "a malformed entry stops start-up and is named: " + bad);
  constexpr std::string_view section = "[COFFEECNC]\n";
  const auto wired = load(with(std::string(g0759), section,
                               "[COFFEECNC]\nSPINDLE_RPM_SIGNAL = yes\nPROBE_SIGNAL = YES\n"
                               "SPINDLE_AT_SPEED_SIGNAL = no\n"));
  require(wired.value().signals.spindle_rpm && wired.value().signals.probe &&
              !wired.value().signals.spindle_at_speed,
          "signals as declared");
  require(!wired.value().signals.reference_marks, "no reference marks unless declared");
  const auto marks =
      load(with(std::string(g0759), section, "[COFFEECNC]\nREFERENCE_MARKS = yes\n"));
  require(marks.value().signals.reference_marks, "reference marks as declared");
  const auto unclear =
      error(with(std::string(g0759), section, "[COFFEECNC]\nREFERENCE_MARKS = some\n"));
  require(has(unclear, "[COFFEECNC] REFERENCE_MARKS") && has(unclear, "some"),
          "an unclear reference mark setting is an error: " + unclear);
  for (const auto key : {"SPINDLE_RPM_SIGNAL", "SPINDLE_AT_SPEED_SIGNAL", "PROBE_SIGNAL"}) {
    const auto message =
        error(with(std::string(g0759), section, std::format("[COFFEECNC]\n{} = maybe\n", key)));
    require(has(message, std::format("[COFFEECNC] {}", key)) && has(message, "maybe"),
            "an unclear signal setting is an error: " + message);
  }
}

// Axes and joints: X, Y and Z only, one joint per letter of [TRAJ] COORDINATES as trivkins
// assigns them; a repeated letter is a gantry axis with more than one motor.
void axes_and_joints() {
  const auto mill = load(g0759).value();
  require(mill.axis_joints == std::array<std::vector<int>, 3>{{{0}, {1}, {2}}} &&
              mill.limits.axis_motors == std::array<int, 3>{1, 1, 1} &&
              mill.home_axis_configured == std::array<bool, 3>{true, true, true},
          "a mill: joint n is axis n, each axis homes alone");
  auto gantry = with(std::string(g0759), "JOINTS = 3\nKINEMATICS = trivkins coordinates=XYZ",
                     "JOINTS = 4\nKINEMATICS = trivkins coordinates=XYYZ kinstype=BOTH");
  gantry = with(gantry, "COORDINATES =  XYZ", "COORDINATES = X Y Y Z");
  gantry = with(gantry, "[SPINDLE_0]\n", "[JOINT_3]\nHOME = 0.0\n\n[SPINDLE_0]\n");
  const auto two = load(gantry);
  require(two.has_value(), two ? "" : two.error());
  require(two->axis_joints == std::array<std::vector<int>, 3>{{{0}, {1, 2}, {3}}} &&
              two->limits.axis_motors == std::array<int, 3>{1, 2, 1} &&
              two->axes == std::array<Axis, 3>{Axis::x, Axis::y, Axis::z},
          "pncconf's gantry (kinstype=BOTH): Y is joints 1 and 2, Z joint 3");
  require(!two->home_all_configured &&
              two->home_axis_configured == std::array<bool, 3>{true, false, true},
          "without HOME_SEQUENCE neither home all nor Y alone is configured");
  const auto sequence = [&](std::string_view y1, std::string_view y2) -> std::string {
    auto text = with(gantry, "[JOINT_0]\n", "[JOINT_0]\nHOME_SEQUENCE = 1\n");
    text = with(text, "[JOINT_1]\n", std::format("[JOINT_1]\nHOME_SEQUENCE = {}\n", y1));
    text = with(text, "[JOINT_2]\n", std::format("[JOINT_2]\nHOME_SEQUENCE = {}\n", y2));
    return with(text, "[JOINT_3]\n", "[JOINT_3]\nHOME_SEQUENCE = 0\n");
  };
  const auto synchronized = load(sequence("-2", "-2")).value();
  require(synchronized.home_all_configured &&
              synchronized.home_axis_configured == std::array<bool, 3>{true, true, true},
          "one negative HOME_SEQUENCE on both Y joints: Y homes alone, both motors together");
  for (const auto& [y1, y2] : {std::pair{"2", "2"}, std::pair{"-1", "-2"}, std::pair{"-1", "1"}})
    require(!load(sequence(y1, y2)).value().home_axis_configured[1],
            std::format("Y joints with HOME_SEQUENCE {} and {} do not home alone", y1, y2));
  // What LinuxCNC accepts the same way: case, spaces, kinstype by its first letter, identity.
  for (const auto& [coordinates, kinematics] :
       {std::pair{"XYYZ", "trivkins coordinates=xyyz"},
        std::pair{"x y y z", "TRIVKINS  coordinates=XYYZ\tkinstype=b"},
        std::pair{"X Y Y Z", "trivkins coordinates=XYYZ kinstype=1"}}) {
    auto text = with(gantry, "COORDINATES = X Y Y Z", std::format("COORDINATES = {}", coordinates));
    text = with(text, "KINEMATICS = trivkins coordinates=XYYZ kinstype=BOTH",
                std::format("KINEMATICS = {}", kinematics));
    const auto loaded = load(text);
    require(loaded && loaded->limits.axis_motors == std::array<int, 3>{1, 2, 1},
            std::format("{} / {} is the gantry: {}", coordinates, kinematics,
                        loaded ? "" : loaded.error()));
  }
  auto late = with(gantry, "COORDINATES = X Y Y Z", "COORDINATES = X Y Z Y");
  late = with(late, "coordinates=XYYZ", "coordinates=XYZY");
  require(load(late).value().axis_joints == std::array<std::vector<int>, 3>{{{0}, {1, 3}, {2}}},
          "LinuxCNC's own gantry sample (XYZY): Y is joints 1 and 3, Z joint 2");
  require(load(with(std::string(g0759), "coordinates=XYZ", "coordinates=XYZ kinstype=both"))
              .has_value() &&
              load(with(std::string(g0759), " coordinates=XYZ", "")).has_value(),
          "a mill with kinstype=both or without coordinates= loads");

  const auto refused = [&](std::string_view old, std::string_view replacement,
                           std::initializer_list<std::string_view> parts,
                           std::string_view what) -> void {
    const auto message = error(with(gantry, old, replacement));
    require(!message.empty() && std::ranges::all_of(parts, [&](std::string_view part) -> bool {
              return has(message, part);
            }),
            std::format("{}: {}", what, message));
  };
  auto rotary = with(gantry, "COORDINATES = X Y Y Z", "COORDINATES = X Y Z A");
  rotary = with(rotary, "coordinates=XYYZ", "coordinates=XYZA");
  const auto fourth = load(rotary);
  require(fourth && fourth->rotary_joints == std::array<std::vector<int>, 3>{{{3}, {}, {}}},
          "A is joint 3");
  auto six = with(rotary, "COORDINATES = X Y Z A", "COORDINATES = X C Y A Z B");
  six = with(six, "coordinates=XYZA", "coordinates=XCYAZB");
  six = with(six, "JOINTS = 4", "JOINTS = 6");
  const auto reordered = load(six);
  require(reordered && reordered->axis_joints == std::array<std::vector<int>, 3>{{{0}, {2}, {4}}} &&
              reordered->rotary_joints == std::array<std::vector<int>, 3>{{{3}, {5}, {1}}} &&
              !reordered->home_all_configured,
          "rotary joints follow trivkins order and all need a configured home sequence");
  refused("COORDINATES = X Y Y Z", "COORDINATES = X Y Y U", {"axis U is not supported"},
          "a U axis");
  refused("COORDINATES = X Y Y Z", "COORDINATES = X Y Y", {"X, Y and Z are all required"},
          "no Z");
  refused("JOINTS = 4", "JOINTS = 3", {"[KINS] JOINTS is \"3\"", "set it to 4"},
          "fewer joints than letters");
  refused("JOINTS = 4\n", "", {"[KINS] JOINTS", "set it to 4"}, "no JOINTS");
  refused(" coordinates=XYYZ", "", {"no coordinates=", "add coordinates=XYYZ"},
          "trivkins's default letters (XYZA) are not the gantry's");
  refused("coordinates=XYYZ", "coordinates=XYZY",
          {"coordinates=XYZY", "X Y Y Z", "same letters in the same order"},
          "coordinates= in another order than COORDINATES");
  refused("kinstype=BOTH", "kinstype=inverse", {"kinstype=inverse", "inverse-only", "both"},
          "inverse-only kinematics");
  refused("kinstype=BOTH", "kinstype=forward", {"forward-only"}, "forward-only kinematics");
  refused("trivkins coordinates=XYYZ kinstype=BOTH", "gantrykins coordinates=XYYZ",
          {"[KINS] KINEMATICS", "only trivkins"}, "other kinematics");
  refused("kinstype=BOTH", "kinstype=BOTH sparse=1", {"\"sparse=1\"", "coordinates= and kinstype="},
          "a word trivkins does not take");
}

// [COFFEECNC] MACHINE_ID and DATA_DIRECTORY are handed on as written: judging them is the app
// layer's, and a wrong one must not keep the panel from starting.
void tool_model_settings() {
  constexpr std::string_view own = "MAX_SPINDLE_RPM = 1500\n";
  const auto none = load(g0759);
  const auto id = none ? none->machine_id.value_or("") : std::string{};
  require(none && id.starts_with("G0759-shaped-") && valid_machine_id(id) && !none->data_directory,
          "without an explicit ID the machine name supplies it");
  const auto other_file = scratch / "other-machine.ini";
  std::filesystem::copy_file(none->ini_file, other_file, std::filesystem::copy_options::overwrite_existing);
  const auto other = load_machine_config(other_file.string());
  require(other && other->machine_id && other->machine_id != none->machine_id,
          "different profiles with the same machine name use different data folders");
  const auto normalized = load(with(std::string(g0759), "MACHINE = G0759-shaped",
                                    "MACHINE = Toolroom mill / 7"));
  require(normalized && normalized->machine_id.value_or("").starts_with("Toolroom-mill-7-"),
          "filesystem separators and spaces in the name become a single hyphen");
  const auto both = load(with(std::string(g0759), own,
                              "MAX_SPINDLE_RPM = 1500\nMACHINE_ID = Mill_7-a\n"
                              "DATA_DIRECTORY = /srv/coffeecnc data\n"));
  require(both && both->machine_id == "Mill_7-a" &&
              both->data_directory == "/srv/coffeecnc data",
          "both keys, as written, case kept");
  const auto odd = load(with(std::string(g0759), own,
                             "MAX_SPINDLE_RPM = 1500\nMACHINE_ID = ../Other Mill\n"
                             "DATA_DIRECTORY = data\n"));
  require(odd.has_value(), "values that are not usable do not fail the start: " +
                               (odd ? std::string() : odd.error()));
  require(odd->machine_id == "../Other Mill" && odd->data_directory == "data",
          "they are not repaired and not resolved against the INI file's directory");
  const auto spaced = load(with(std::string(g0759), own,
                                "MAX_SPINDLE_RPM = 1500\nMACHINE_ID =   mill-1  \t\n"));
  require(spaced && spaced->machine_id == "mill-1",
          "LinuxCNC's INI reader drops the white space around a value: \"" +
              (spaced ? spaced->machine_id.value_or("(none)") : spaced.error()) + "\"");
  const auto empty = load(with(std::string(g0759), own,
                               "MAX_SPINDLE_RPM = 1500\nMACHINE_ID =\nDATA_DIRECTORY =\n"));
  require(empty && empty->machine_id == none->machine_id && !empty->data_directory,
          "LinuxCNC's INI reader gives a key without a value as an absent key");
}

// The installed example (share/coffeecnc.ini.example): its [COFFEECNC] section, put into a
// machine's INI file as it stands, is read, and has no key the loader does not know.
void shipped_example() {
  std::ifstream in{example};
  require(in.good(), "cannot read " + example);
  std::string section;
  std::vector<std::string> keys;
  bool inside = false;
  for (std::string line; std::getline(in, line);) {
    if (line.starts_with("["))
      inside = line == "[COFFEECNC]";
    if (!inside)
      continue;
    section += line + "\n";
    if (const auto equals = line.find('='); equals != std::string::npos && !line.starts_with("#"))
      keys.push_back(line.substr(0, line.find_first_of(" =")));
  }
  const std::vector<std::string> known{"MAX_SPINDLE_RPM", "SPINDLE_RPM_SIGNAL",
                                       "SPINDLE_AT_SPEED_SIGNAL", "PROBE_SIGNAL",
                                       "MANUAL_TOOLCHANGE", "FIXED_SETTER"};
  require(keys == known, std::format("example keys: {} of {}", keys.size(), known.size()));
  const auto config =
      load(with(std::string(g0759), "[COFFEECNC]\nMAX_SPINDLE_RPM = 1500\n", section));
  require(config.has_value(), config ? "" : config.error());
  require(near(config->limits.max_spindle_rpm, 1500) && !config->signals.spindle_rpm &&
              !config->signals.spindle_at_speed && !config->signals.probe &&
              !config->manual_tool_change && !config->fixed_setter,
          "the example's values");
  // Every signal the example names can be switched on by the word it documents.
  auto yes = section;
  for (const std::string_view key : {"SPINDLE_RPM_SIGNAL", "SPINDLE_AT_SPEED_SIGNAL", "PROBE_SIGNAL"})
    yes = with(std::move(yes), std::format("{} = no", key), std::format("{} = yes", key));
  const auto wired = load(with(std::string(g0759), "[COFFEECNC]\nMAX_SPINDLE_RPM = 1500\n", yes));
  require(wired && wired->signals.spindle_rpm && wired->signals.spindle_at_speed &&
              wired->signals.probe,
          "the example's signals set to yes");
  require(config->machine_id.value_or("").starts_with("G0759-shaped-") &&
              !config->data_directory,
          "the example uses the machine name and default data root");
  // Its two commented-out keys, taken into use, are the identity it documents.
  auto named = section;
  for (const std::string_view key : {"#MACHINE_ID = ", "#DATA_DIRECTORY = "}) {
    const auto at = named.find(key);
    require(at != std::string::npos && named.find(key, at + 1) == std::string::npos &&
                (at == 0 || named[at - 1] == '\n'),
            std::format("the example has one commented-out line {}", key));
    named.erase(at, 1);
  }
  const auto identified =
      load(with(std::string(g0759), "[COFFEECNC]\nMAX_SPINDLE_RPM = 1500\n", named));
  require(identified.has_value(), identified ? "" : identified.error());
  require(identified->machine_id == "my-mill" &&
              valid_machine_id(identified->machine_id.value_or("")) &&
              identified->data_directory == "/var/lib/coffeecnc",
          "the example's identity and data directory, uncommented: a valid machine ID and an "
          "absolute path");
}

// The INI file run.sh assembles for a profile, without starting LinuxCNC.
auto assembled(const std::string& options, const std::string& name)
    -> std::expected<MachineConfig, std::string> {
  const auto directory = scratch / name;
  const auto command =
      std::format("'{}' -n {} -d '{}' /bin/true >/dev/null", runner, options, directory.string());
  if (std::system(command.c_str()) != 0)
    return std::unexpected("run.sh -n failed: " + command);
  const std::filesystem::directory_iterator files{directory};
  const auto ini = std::ranges::find_if(
      files, [](const auto& entry) -> bool { return entry.path().extension() == ".ini"; });
  if (ini == std::ranges::end(files))
    return std::unexpected(std::string("run.sh -n wrote no INI file"));
  return load_machine_config(ini->path().string());
}

void simulation_profiles() {
  for (const auto units : {"mm", "inch"}) {
    const auto config = assembled(std::format("-u {}", units), units);
    require(config.has_value(), config ? "" : config.error());
    const bool inch = std::string_view(units) == "inch";
    require(config->machine_units == (inch ? LengthUnit::inch : LengthUnit::mm) &&
                std::fabs(config->limits.max_jog_velocity - 1800) < 0.01 &&
                std::fabs(config->limits.max_axis_jog_velocity[2] - 1800) < 0.01 &&
                near(config->limits.max_spindle_rpm, 12000) && config->limits.homing_required &&
                config->home_all_configured && config->jog_increments.empty() &&
                !config->signals.spindle_rpm && !config->signals.probe,
            std::format("the {} simulation INI is accepted as before", units));
    require(config->machine_id.value_or("").starts_with("CoffeeCNC-sim-") &&
                !config->data_directory,
            std::format("the {} simulation derives its data identity from the machine name", units));
  }
  const auto config = assembled("-p g0759", "g0759");
  require(config.has_value(), config ? "" : config.error());
  require(near(config->limits.max_jog_velocity, 1500) &&
              near(config->limits.max_axis_jog_velocity[2], 750) &&
              near(config->limits.max_feed_override, 2) &&
              near(config->limits.max_spindle_override, 1) &&
              near(config->limits.max_spindle_rpm, 1500) && !config->limits.homing_required &&
              !config->home_all_configured &&
              same(config->jog_increments, {5, 1, 0.5, 0.1, 0.05, 0.01, 0.005}) &&
              config->signals.spindle_rpm && config->signals.spindle_at_speed &&
              config->signals.probe && near(config->travel[1].min, -1000),
          "the G0759 simulation INI carries the machine's values");
  require(config->machine_id == "G0759-sim" &&
              config->data_directory ==
                  std::filesystem::canonical(scratch / "g0759").string() + "/coffeecnc-data",
          "it names its machine, and keeps the tool models in its run directory: " +
              config->data_directory.value_or("(none)"));
  const auto rows = read_text_file(config->tool_table, 4096).value_or(std::string());
  require(rows == "T0 P0 ;\nT1 P1 ;\nT2 P2 ;\nT3 P3 ;\n", "its tool table has no dimensions");
  const auto refused = std::format("'{}' -n -p g0759 -u inch -d '{}' /bin/true 2>/dev/null", runner,
                                   (scratch / "refused").string());
  require(std::system(refused.c_str()) != 0, "the G0759 profile is millimetres only");
  const auto gantry = assembled("-p gantry", "gantry");
  require(gantry.has_value(), gantry ? "" : gantry.error());
  require(gantry->axis_joints == std::array<std::vector<int>, 3>{{{0}, {1, 2}, {3}}} &&
              gantry->home_all_configured &&
              gantry->home_axis_configured == std::array<bool, 3>{true, true, true} &&
              gantry->limits.homing_required && near(gantry->limits.max_jog_velocity, 1800) &&
              near(gantry->travel[2].min, -50) && gantry->machine_id.has_value(),
          "the gantry simulation INI: Y on joints 1 and 2, homed together, and a machine ID "
          "derived from [EMC] MACHINE (#158)");
  const auto inch = std::format("'{}' -n -p gantry -u inch -d '{}' /bin/true 2>/dev/null", runner,
                                (scratch / "refused").string());
  require(std::system(inch.c_str()) != 0, "the gantry profile is millimetres only");
}

void remote_viewer_port() {
  const auto defaults = load(g0759);
  require(defaults && defaults->remote_port == 0, "The LAN viewer is off by default");
  const auto enabled = with(std::string(g0759), "[COFFEECNC]\n", "[COFFEECNC]\nREMOTE_PORT = 8787\n");
  const auto viewer = load(enabled);
  require(viewer && viewer->remote_port == 8787, "The enabled LAN viewer reads the configured port");
  const auto zero = load(with(enabled, "REMOTE_PORT = 8787", "REMOTE_PORT = 0"));
  require(zero && zero->remote_port == 0, "A zero port also disables the LAN viewer");
  for (const auto* invalid : {"-1", "65536", "3.5", "open"}) {
    const auto refused = load(with(enabled, "REMOTE_PORT = 8787", std::string("REMOTE_PORT = ") + invalid));
    require(!refused && has(refused.error(), "REMOTE_PORT"), "An invalid LAN port is rejected");
  }
  const auto aliases = load(with(enabled, "REMOTE_PORT = 8787",
      "REMOTE_PORT = 8787\nREMOTE_HOSTS = Mill.example.local., spare.example.local MILL.EXAMPLE.LOCAL\n"));
  require(aliases && aliases->remote_hosts == std::vector<std::string>{"mill.example.local", "spare.example.local"},
          "LAN aliases are normalized and deduplicated");
  for (const auto* invalid : {"http://mill.example", "mill.example:8787", "mill.example/view"}) {
    const auto refused = load(with(enabled, "REMOTE_PORT = 8787",
        std::string("REMOTE_PORT = 8787\nREMOTE_HOSTS = ") + invalid + "\n"));
    require(!refused && has(refused.error(), "REMOTE_HOSTS"), "An alias must be a bare hostname");
  }
}

void fixed_setter_is_explicit_and_bounded() {
  require(!load(g0759)->fixed_setter, "ordinary machine profiles have no automatic fixed setter");
  const auto enabled = with(std::string(g0759), "[COFFEECNC]\n",
                            "[COFFEECNC]\nFIXED_SETTER = yes\n"
                            "PROBE_SIGNAL = yes\nSPINDLE_RPM_SIGNAL = yes\n"
                            "SETTER_MAX_TRAVEL_MM = 4\nSETTER_MAX_FEED_MM_MIN = 30\n"
                            "SETTER_XY_TOLERANCE_MM = 0.5\n");
  const auto setter = load(enabled);
  require(setter.has_value(), "The fixed setter INI loads");
  const auto& setter_limits = setter->fixed_setter;
  require(setter_limits && near(setter_limits->travel_mm, 4) &&
              near(setter_limits->feed_mm_min, 30) && near(setter_limits->check_tolerance_mm, 0.1),
          "the configured fixed setter carries canonical travel and feed ceilings");
  const auto check = load(with(enabled, "SETTER_MAX_TRAVEL_MM = 4",
                               "SETTER_MAX_TRAVEL_MM = 4\nSETTER_CHECK_TOLERANCE_MM = 0.04"));
  require(check.has_value(), "The fixed setter INI loads");
  const auto& check_limits = check->fixed_setter;
  require(check_limits && near(check_limits->check_tolerance_mm, 0.04),
          "the tool check tolerance is configurable in millimetres");
  require(has(error(with(enabled, "SETTER_MAX_TRAVEL_MM = 4",
                          "SETTER_MAX_TRAVEL_MM = 4\nSETTER_CHECK_TOLERANCE_MM = 0")),
              "SETTER_CHECK_TOLERANCE_MM"),
          "a tool check requires a positive tolerance");
  require(has(error(with(enabled, "PROBE_SIGNAL = yes", "PROBE_SIGNAL = no")), "FIXED_SETTER"),
          "the fixed setter requires probe feedback");
  require(has(error(with(enabled, "SETTER_MAX_TRAVEL_MM = 4", "SETTER_MAX_TRAVEL_MM = 0")),
              "SETTER_MAX_TRAVEL_MM"),
          "zero search travel is refused");
  require(has(error(with(enabled, "SETTER_MAX_FEED_MM_MIN = 30", "SETTER_MAX_FEED_MM_MIN = 751")),
              "SETTER_MAX_FEED_MM_MIN"),
          "a setter feed above the Z ceiling is refused");
  const auto two_pass = with(enabled, "SETTER_MAX_TRAVEL_MM = 4",
                             "SETTER_MAX_TRAVEL_MM = 90\nSETTER_CLEARANCE_MM = 80\n"
                             "SETTER_SEARCH_FEED_MM_MIN = 300\nSETTER_LATCH_FEED_MM_MIN = 25");
  const auto current = load(two_pass);
  require(current && current->fixed_setter == FixedSetterLimits{90, 300, 98, 0.5, 80, 25, 1},
          "two-pass setter reads the clearance and both feeds");
  const auto geometry = load(with(two_pass, "SETTER_MAX_TRAVEL_MM = 90",
      "SETTER_MAX_TRAVEL_MM = 90\nTOUCH_PROBE_CORNER_REACH_MM = 9\n"
      "TOUCH_PROBE_CORNER_MARGIN_MM = 4\nTOUCH_PROBE_CLEARANCE_MM = 6\nTOUCH_PROBE_DEPTH_MM = 3"));
  require(geometry.has_value(), "The fixed setter INI loads");
  const auto& geometry_limits = geometry->fixed_setter;
  require(geometry_limits && geometry_limits->material_geometry == MaterialProbeGeometry{9, 4, 6, 3},
          "the configured external probe approach supplies all four distances");
  require(has(error(with(two_pass, "SETTER_MAX_TRAVEL_MM = 90", "SETTER_MAX_TRAVEL_MM = 30")),
              "search must reach below"),
          "the setter rejects a search too short to reach below the reference latch");
}

// [COFFEECNC] MACRO = <label> | <MDI block> (#211), as LinuxCNC's own IniFile reads the lines.
void macros() {
  require(load(g0759)->macros.empty(), "no macro unless configured");
  const auto with_macros = [](std::string_view lines) -> std::string {
    return with(std::string(g0759), "[COFFEECNC]\n", std::format("[COFFEECNC]\n{}", lines));
  };
  const auto config = load(with_macros(
      "MACRO = Park | G53 G0 Z0\nMACRO =   Probe   Z  |  G38.2 Z-10 F100 (MSG, #<_z>)  \n"
      "MACRO = Höhe über | G0 Z5\n"));
  require(config.has_value(), config ? "" : config.error());
  require(config->macros == std::vector<Macro>{{"Park", "G53 G0 Z0"},
                                               {"Probe   Z", "G38.2 Z-10 F100 (MSG, #<_z>)"},
                                               {"Höhe über", "G0 Z5"}},
          std::format("labels and blocks in order, trimmed, with # and ( kept: {}",
                      config->macros.size() > 1 ? config->macros[1].mdi : std::string()));
  const auto refused = [&](std::string_view lines, std::string_view part) -> bool {
    const auto reason = error(with_macros(lines));
    if (!has(reason, part))
      throw std::runtime_error(std::format("\"{}\" gave \"{}\", not \"{}\"", lines, reason, part));
    return true;
  };
  require(refused("MACRO = G53 G0 Z0\n", "has no |"), "a macro needs the bar");
  require(refused("MACRO =  | G53 G0 Z0\n", "has no label"), "an empty label");
  require(refused("MACRO = Park |  \n", "\"Park\" has no MDI block"), "an empty block");
  require(refused("MACRO = 1234567890123456789012345 | G0 X0\n", "at most 24 characters"),
          "a label of 25 characters");
  require(load(with_macros("MACRO = 123456789012345678901234 | G0 X0\n")).has_value() &&
              load(with_macros("MACRO = äöüäöüäöüäöüäöüäöüäöüäöü | G0 X0\n")).has_value(),
          "24 characters, also of two bytes each");
  // LinuxCNC's IniFile cuts a line after 255 characters without a word; that is refused here.
  const auto line = [](std::size_t size) -> std::string {
    std::string text = "MACRO = Long | G0 X0 (";
    return text + std::string(size - text.size() - 1, 'x') + ")\n";
  };
  require(refused(line(256), "a MACRO line of 256 characters: LinuxCNC reads only the first 255"),
          "a line longer than LinuxCNC reads");
  const auto longest = load(with_macros(line(255)));
  require(longest && longest->macros.at(0).mdi.size() == 255 - 15 &&
              longest->macros.at(0).mdi.ends_with("x)"),
          "a line of 255 characters is read whole");
  require(refused("MACRO = Park | G0 Z0\nMACRO = Park | G0 Z1\n", "\"Park\" is there twice"),
          "labels are unique");
  std::string thirteen;
  for (int i = 1; i <= 13; ++i)
    thirteen += std::format("MACRO = M{} | G0 Z{}\n", i, i);
  require(refused(thirteen, "more than 12 MACRO lines"), "at most 12 macros");
  require(load(with_macros(thirteen.substr(0, thirteen.rfind("MACRO")))).value().macros.size() ==
              12,
          "12 macros");

  // A named O-word call needs its file where the interpreter looks for it.
  const auto subroutines = scratch / "macro-subs";
  const auto programs = scratch / "macro-programs";
  std::filesystem::create_directories(subroutines);
  std::filesystem::create_directories(programs);
  std::ofstream(subroutines / "clamp.ngc") << "o<clamp> sub\no<clamp> endsub\nM2\n";
  std::ofstream(programs / "park.ngc") << "o<park> sub\no<park> endsub\nM2\n";
  const auto search = [&](std::string_view macro) -> std::string {
    auto text = with(std::string(g0759), "PROGRAM_PREFIX = /srv/nc_files",
                     "PROGRAM_PREFIX = macro-programs");
    text += "\n[RS274NGC]\nSUBROUTINE_PATH = /nonexistent: macro-subs\n";
    return with(text, "[COFFEECNC]\n", std::format("[COFFEECNC]\nMACRO = {}\n", macro));
  };
  require(load(search("Clamp | O<clamp> call")).has_value(), "a call found in SUBROUTINE_PATH");
  require(load(search("Clamp | o < Clamp > CALL [1] [2]")).has_value(),
          "the name as the interpreter reads it: lower case, no spaces, with arguments");
  require(load(search("Park | O<park> call")).has_value(), "a call found in PROGRAM_PREFIX");
  const auto* home = std::getenv("HOME");
  require(home != nullptr, "LinuxCNC needs HOME to expand a tilde");
  const auto home_relative = [&](const std::filesystem::path& path) -> std::string {
    return "~/" + std::filesystem::relative(path, home).generic_string();
  };
  const auto home_prefix = with(search("Park | O<park> call"), "PROGRAM_PREFIX = macro-programs",
                                "PROGRAM_PREFIX = " + home_relative(programs));
  require(load(home_prefix).has_value(), "a call found in a home-relative PROGRAM_PREFIX");
  const auto home_subs = with(search("Clamp | O<clamp> call"), "SUBROUTINE_PATH = /nonexistent: macro-subs",
                              "SUBROUTINE_PATH = /nonexistent: " + home_relative(subroutines));
  require(load(home_subs).has_value(), "a call found in a home-relative SUBROUTINE_PATH");
  require(has(error(search("Lost | O<lost> call")),
              "MACRO \"Lost\" calls O<lost>, but lost.ngc is not in [DISPLAY] PROGRAM_PREFIX or "
              "[RS274NGC] SUBROUTINE_PATH."),
          "a call to a missing file is refused at start-up");
  require(load(search("Note | G0 Z0 (O<lost> call)")).has_value() &&
              load(search("Repeat | O100 call")).has_value(),
          "a comment and a numbered call are LinuxCNC's to judge");
}

void manual_tool_change_is_opt_in() {
  require(!load(g0759)->manual_tool_change, "custom HAL keeps its existing tool changer by default");
  const auto in = with(std::string(g0759), "[COFFEECNC]\n", "[COFFEECNC]\nMANUAL_TOOLCHANGE = yes\n");
  require(load(in)->manual_tool_change, "the application can wire manual tool changes explicitly");
  require(!load(with(in, "MANUAL_TOOLCHANGE = yes", "MANUAL_TOOLCHANGE = no"))->manual_tool_change,
          "no leaves tool-change wiring alone");
  require(has(error(with(in, "MANUAL_TOOLCHANGE = yes", "MANUAL_TOOLCHANGE = maybe")),
              "MANUAL_TOOLCHANGE"),
          "an invalid tool-change setting is diagnosed");
}

// LinuxCNC's integer switches as its interpreter and task read them (boost::lexical_cast<int>).
void linuxcnc_switches() {
  for (const auto* on : {"1", "2", "-1", "+1", "007"})
    require(linuxcnc_switch(on), std::format("\"{}\" is a nonzero integer: on", on));
  for (const auto* off : {"", "0", "-0", "+", "+-1", "YES", "TRUE", "ON", "yes", "1.5", "1x", " 1"})
    require(!linuxcnc_switch(off), std::format("\"{}\" is no integer to LinuxCNC: off", off));
  const auto changer = [](std::string_view value) {
    return load(with(std::string(g0759), "TOOL_TABLE = tool.tbl\n",
                     std::format("TOOL_TABLE = tool.tbl\nRANDOM_TOOLCHANGER = {}\n", value)));
  };
  require(changer("0")->remember_spindle_tool, "a nonrandom changer remembers its spindle tool");
  require(!changer("-1")->remember_spindle_tool, "-1 is a random changer to LinuxCNC");
  require(changer("YES")->remember_spindle_tool && changer("1.5")->remember_spindle_tool,
          "YES and 1.5 leave LinuxCNC's changer nonrandom");
}

void motion_settings() {
  constexpr std::string_view rpm = "MAX_SPINDLE_RPM = 1500\n";
  const auto defaulted = load(g0759);
  require(defaulted && near(defaulted->motion_tolerances.probe_position_mm, 0.1) &&
              near(defaulted->motion_tolerances.probe_off_line_mm, 0.5) &&
              near(defaulted->motion_tolerances.probe_backlash_mm, 0.5) &&
              near(defaulted->motion_tolerances.restart_position_mm, 0.5),
          "the G0759 motion tolerances remain the defaults");
  const auto tuned = load(with(std::string(g0759), rpm, "MAX_SPINDLE_RPM = 1500\n"
      "PROBE_OFF_LINE_TOLERANCE_MM = 0.2\nPROBE_BACKLASH_TOLERANCE_MM = 0.3\n"
      "PROBE_POSITION_TOLERANCE_MM = 0.04\nRESTART_POSITION_TOLERANCE_MM = 0.23\n"));
  require(tuned && near(tuned->motion_tolerances.probe_off_line_mm, 0.2) &&
              near(tuned->motion_tolerances.probe_backlash_mm, 0.3) &&
              near(tuned->motion_tolerances.probe_position_mm, 0.04) &&
              near(tuned->motion_tolerances.restart_position_mm, 0.23),
          "each motion tolerance can be tuned independently");
  const auto invalid = load(with(std::string(g0759), rpm, "MAX_SPINDLE_RPM = 1500\n"
      "PROBE_POSITION_TOLERANCE_MM = nan\n"));
  require(!invalid && has(invalid.error(), "PROBE_POSITION_TOLERANCE_MM"),
          "an invalid tolerance names the key");

  auto paused = with(std::string(g0759), "MAX_VELOCITY = 12.5\n",
                     "MAX_VELOCITY = 12.5\nMAX_ACCELERATION = 10\nOFFSET_AV_RATIO = 0.2\n");
  paused = with(std::move(paused), rpm, "MAX_SPINDLE_RPM = 1500\nSPINDLE_RPM_SIGNAL = yes\n"
                "PAUSED_Z_MAX_MM = 10\n");
  const auto missing_speed = load(paused);
  require(!missing_speed && has(missing_speed.error(), "at-speed"),
          "paused-Z needs the at-speed feedback as well as RPM");
  const auto ready = load(with(std::move(paused), "SPINDLE_RPM_SIGNAL = yes\n",
                              "SPINDLE_RPM_SIGNAL = yes\nSPINDLE_AT_SPEED_SIGNAL = yes\n"));
  require(ready && ready->paused_z, "paused-Z is enabled with both spindle signals");
}

} // namespace

auto main(int argc, char** argv) -> int {
  try {
    if (argc != 4)
      throw std::runtime_error(
          "usage: coffeecnc_machine_config_tests <run.sh> <scratch> <coffeecnc.ini.example>");
    runner = argv[1];
    scratch = argv[2];
    example = argv[3];
    std::filesystem::create_directories(scratch);
    increments();
    g0759_shape();
    spindle_ceiling();
    homing();
    axes_and_joints();
    axis_ceilings();
    increments_and_signals();
    tool_model_settings();
    shipped_example();
    simulation_profiles();
    remote_viewer_port();
    fixed_setter_is_explicit_and_bounded();
    motion_settings();
    linuxcnc_switches();
    manual_tool_change_is_opt_in();
    macros();
    std::cout << checks << " machine configuration checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Check " << checks << " failed: " << error.what() << '\n';
    return 1;
  }
}
