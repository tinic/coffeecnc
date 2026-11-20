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
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
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
  for (const auto key : {"SPINDLE_RPM_SIGNAL", "SPINDLE_AT_SPEED_SIGNAL", "PROBE_SIGNAL"}) {
    const auto message =
        error(with(std::string(g0759), section, std::format("[COFFEECNC]\n{} = maybe\n", key)));
    require(has(message, std::format("[COFFEECNC] {}", key)) && has(message, "maybe"),
            "an unclear signal setting is an error: " + message);
  }
}

// [COFFEECNC] MACHINE_ID and DATA_DIRECTORY are handed on as written: judging them is the app
// layer's, and a wrong one must not keep the panel from starting.
void tool_model_settings() {
  constexpr std::string_view own = "MAX_SPINDLE_RPM = 1500\n";
  const auto none = load(g0759);
  require(none && !none->machine_id && !none->data_directory,
          "neither key: neither value");
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
  require(empty && !empty->machine_id && !empty->data_directory,
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
                                       "SPINDLE_AT_SPEED_SIGNAL", "PROBE_SIGNAL", "FIXED_SETTER"};
  require(keys == known, std::format("example keys: {} of {}", keys.size(), known.size()));
  const auto config =
      load(with(std::string(g0759), "[COFFEECNC]\nMAX_SPINDLE_RPM = 1500\n", section));
  require(config.has_value(), config ? "" : config.error());
  require(near(config->limits.max_spindle_rpm, 1500) && !config->signals.spindle_rpm &&
              !config->signals.spindle_at_speed && !config->signals.probe && !config->fixed_setter,
          "the example's values");
  // Every signal the example names can be switched on by the word it documents.
  auto yes = section;
  for (const std::string_view key : {"SPINDLE_RPM_SIGNAL", "SPINDLE_AT_SPEED_SIGNAL", "PROBE_SIGNAL"})
    yes = with(std::move(yes), std::format("{} = no", key), std::format("{} = yes", key));
  const auto wired = load(with(std::string(g0759), "[COFFEECNC]\nMAX_SPINDLE_RPM = 1500\n", yes));
  require(wired && wired->signals.spindle_rpm && wired->signals.spindle_at_speed &&
              wired->signals.probe,
          "the example's signals set to yes");
  // As shipped the example names no machine: nothing is saved, no directory is configured.
  require(!config->machine_id && !config->data_directory,
          "the example as it stands keeps settings for the session only");
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
    require(!config->machine_id && !config->data_directory,
            std::format("the {} simulation INI names no machine: tool models are session-only",
                        units));
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
}

void fixed_setter_is_explicit_and_bounded() {
  require(!load(g0759)->fixed_setter, "ordinary machine profiles have no automatic fixed setter");
  const auto enabled = with(std::string(g0759), "[COFFEECNC]\n",
                            "[COFFEECNC]\nFIXED_SETTER = yes\n"
                            "PROBE_SIGNAL = yes\nSPINDLE_RPM_SIGNAL = yes\n"
                            "SETTER_MAX_TRAVEL_MM = 4\nSETTER_MAX_FEED_MM_MIN = 30\n"
                            "SETTER_XY_TOLERANCE_MM = 0.5\n");
  const auto setter = load(enabled);
  require(setter && setter->fixed_setter && near(setter->fixed_setter->travel_mm, 4) &&
              near(setter->fixed_setter->feed_mm_min, 30),
          "the configured fixed setter carries canonical travel and feed ceilings");
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
  require(current && current->fixed_setter &&
              *current->fixed_setter == FixedSetterLimits{90, 300, 98, 0.5, 80, 25, 1},
          "two-pass setter reads the clearance and both feeds");
  require(has(error(with(two_pass, "SETTER_MAX_TRAVEL_MM = 90", "SETTER_MAX_TRAVEL_MM = 30")),
              "search must reach below"),
          "the setter rejects a search too short to reach below the reference latch");
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
    axis_ceilings();
    increments_and_signals();
    tool_model_settings();
    shipped_example();
    simulation_profiles();
    fixed_setter_is_explicit_and_bounded();
    std::cout << checks << " machine configuration checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Check " << checks << " failed: " << error.what() << '\n';
    return 1;
  }
}
