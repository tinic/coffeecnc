#ifndef COFFEECNC_LINUXCNC_MACHINE_CONFIG_HPP
#define COFFEECNC_LINUXCNC_MACHINE_CONFIG_HPP

#include <coffeecnc/core/command_policy.hpp>
#include <coffeecnc/core/panel_messages.hpp>
#include <coffeecnc/linuxcnc/hal_component.hpp>

#include <array>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace coffeecnc {

struct Travel {
  double min; // mm
  double max; // mm
};

struct PausedZLimits {
  double lift_mm;
  double velocity_mm_s;
  double acceleration_mm_s2;
  double count_mm;
};

// What the adapter needs from the machine's INI file. Lengths are already millimetres, velocities
// mm/min. Together with `limits` this is everything a protocol `hello` carries.
struct MachineConfig {
  std::string ini_file;  // absolute
  std::string directory; // of the INI file; relative INI paths are relative to it
  std::string machine_name;
  std::string nml_file;
  // [TRAJ] LINEAR_UNITS: the unit of every length in EMC_STAT and in jog commands.
  LengthUnit machine_units;
  double millimetres_per_unit;
  // The only machine accepted: X, Y, Z, in the order of every per-axis array.
  std::array<Axis, 3> axes;
  // [AXIS_X], [AXIS_Y], [AXIS_Z] MIN_LIMIT and MAX_LIMIT.
  std::array<Travel, 3> travel;
  // [DISPLAY] INCREMENTS in millimetres, in the order given; empty when not configured.
  std::vector<double> jog_increments;
  // Jog velocity: [DISPLAY] MAX_LINEAR_VELOCITY, else [TRAJ] MAX_LINEAR_VELOCITY. Per axis:
  // [AXIS_X], [AXIS_Y], [AXIS_Z] MAX_VELOCITY, never above the global maximum.
  // Overrides: [DISPLAY] MAX_FEED_OVERRIDE (1.0 when absent), MIN_SPINDLE_OVERRIDE (0.5),
  // MAX_SPINDLE_OVERRIDE (1.0). Spindle speed: [COFFEECNC] MAX_SPINDLE_RPM, else [SPINDLE_0]
  // MAX_FORWARD_VELOCITY, else [DISPLAY] MAX_SPINDLE_0_SPEED.
  // Homing required unless [TRAJ] NO_FORCE_HOMING is above zero, as LinuxCNC's task reads it.
  Limits limits;
  // Every joint has a [JOINT_n] HOME_SEQUENCE. LinuxCNC's "home all" homes only joints that have
  // one and reports nothing about the others, so without it axes are homed one by one.
  bool home_all_configured;
  // [COFFEECNC] SPINDLE_RPM_SIGNAL, SPINDLE_AT_SPEED_SIGNAL, PROBE_SIGNAL: `yes` where the
  // machine's HAL files connect that pin of the coffeecnc component. Absent means not connected.
  HalSignals signals;
  std::string tool_table; // [EMCIO] TOOL_TABLE, absolute; empty when not configured
  // [RS274NGC] PARAMETER_FILE, absolute; empty when not configured. The interpreter keeps the
  // work coordinate systems in it.
  std::string parameter_file;
  std::string postgui_halfile; // [HAL] POSTGUI_HALFILE, absolute; empty when not configured
  std::string program_prefix;  // [DISPLAY] PROGRAM_PREFIX, absolute; empty when not configured
  // [COFFEECNC] MACHINE_ID and DATA_DIRECTORY as written, not judged and not resolved; empty
  // when the key is absent. They say where the tool models are saved (app layer:
  // resolve_tool_model_location). `hello` carries the ID only when valid_machine_id accepts it.
  std::optional<std::string> machine_id;
  // Found in HAL once the post-GUI file is loaded; not read from the INI.
  std::optional<CoolantOutputs> coolant;
  std::optional<std::string> data_directory;
  std::optional<FixedSetterLimits> fixed_setter{};
  // [COFFEECNC] TOUCH_PROBE_TOOL: the tool number HAL routes to the spindle touch probe. It is a
  // touch probe whatever its saved tool model says (spindle lockout, default name).
  std::optional<int> touch_probe_tool{};
  bool remember_spindle_tool{}; // A nonrandom changer with a local tool table.
  std::optional<PausedZLimits> paused_z{};
};

// Reads the INI file LinuxCNC was started with. Fails with a message for the operator when the
// machine is not one this adapter drives: only a mill with [TRAJ] COORDINATES X Y Z, three joints
// and trivial kinematics (joint n is axis n) is supported, in millimetres or inches.
[[nodiscard]] auto load_machine_config(const std::string& ini_file)
    -> std::expected<MachineConfig, std::string>;

// [DISPLAY] INCREMENTS as millimetres. Entries are separated by commas or, without any comma, by
// white space. An entry is a number or a fraction (`1/16`) in machine units, or with one of the
// suffixes mm, cm, um, in, inch, mil. The word `continuous` is skipped. Fails naming the first
// entry that is not a length above zero.
[[nodiscard]] auto parse_jog_increments(std::string_view text, double millimetres_per_unit)
    -> std::expected<std::vector<double>, std::string>;

} // namespace coffeecnc

#endif // COFFEECNC_LINUXCNC_MACHINE_CONFIG_HPP
