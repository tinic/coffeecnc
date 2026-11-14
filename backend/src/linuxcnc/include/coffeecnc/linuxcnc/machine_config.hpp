#ifndef COFFEECNC_LINUXCNC_MACHINE_CONFIG_HPP
#define COFFEECNC_LINUXCNC_MACHINE_CONFIG_HPP

#include <coffeecnc/core/command_policy.hpp>

#include <array>
#include <expected>
#include <string>

namespace coffeecnc {

struct Travel {
  double min; // mm
  double max; // mm
};

// What the adapter needs from the machine's INI file. Lengths are already millimetres.
struct MachineConfig {
  std::string ini_file;  // absolute
  std::string directory; // of the INI file; relative INI paths are relative to it
  std::string machine_name;
  std::string nml_file;
  // [TRAJ] LINEAR_UNITS: the unit of every length in EMC_STAT and in jog commands.
  LengthUnit machine_units;
  double millimetres_per_unit;
  // [AXIS_X], [AXIS_Y], [AXIS_Z] MIN_LIMIT and MAX_LIMIT.
  std::array<Travel, 3> travel;
  // Jog velocity: [DISPLAY] MAX_LINEAR_VELOCITY, else [TRAJ] MAX_LINEAR_VELOCITY.
  // Overrides: [DISPLAY] MAX_FEED_OVERRIDE (1.0 when absent), MIN_SPINDLE_OVERRIDE (0.5),
  // MAX_SPINDLE_OVERRIDE (1.0). Spindle speed: [SPINDLE_0] MAX_FORWARD_VELOCITY, else
  // [DISPLAY] MAX_SPINDLE_0_SPEED.
  Limits limits;
  std::string tool_table;      // [EMCIO] TOOL_TABLE, absolute; empty when not configured
  std::string postgui_halfile; // [HAL] POSTGUI_HALFILE, absolute; empty when not configured
  std::string program_prefix;  // [DISPLAY] PROGRAM_PREFIX, absolute; empty when not configured
};

// Reads the INI file LinuxCNC was started with. Fails with a message for the operator when the
// machine is not one this adapter drives: only a mill with [TRAJ] COORDINATES X Y Z, three joints
// and trivial kinematics (joint n is axis n) is supported, in millimetres or inches.
[[nodiscard]] auto load_machine_config(const std::string& ini_file)
    -> std::expected<MachineConfig, std::string>;

} // namespace coffeecnc

#endif // COFFEECNC_LINUXCNC_MACHINE_CONFIG_HPP
