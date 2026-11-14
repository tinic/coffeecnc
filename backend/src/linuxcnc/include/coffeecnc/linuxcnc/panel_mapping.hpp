#ifndef COFFEECNC_LINUXCNC_PANEL_MAPPING_HPP
#define COFFEECNC_LINUXCNC_PANEL_MAPPING_HPP

#include <coffeecnc/core/panel_messages.hpp>
#include <coffeecnc/linuxcnc/machine_config.hpp>
#include <coffeecnc/linuxcnc/nml_port.hpp>

#include <array>
#include <cstdint>
#include <span>

namespace coffeecnc {

// From LinuxCNC's view of the machine to the panel's messages. Pure: no LinuxCNC, no I/O.

// The protocol `hello` of a machine driven through LinuxCNC.
[[nodiscard]] auto to_hello(const MachineConfig& config) -> Hello;

// The protocol `status` of one reading. `tools` is the tool table in the order of its rows:
// LinuxCNC reports the prepared tool as an index into its table, row n being index n (index 0
// is the spindle), and the panel is told the tool number. An index beyond the rows is reported
// as no prepared tool. `preview_context` is left 0.
//
// With Link::down the reading holds nothing: the placeholders are zero, with the configured
// travel as limits and G54 as the work system.
//
// `wait` is the first that applies of: a tool change LinuxCNC asks for on the HAL pins, paused,
// a dwell with time left.
[[nodiscard]] auto to_panel_status(const NmlStatus& reading, const MachineConfig& config,
                                   std::span<const Tool> tools, std::uint64_t program_revision)
    -> Status;

// G54 ... G59.3 as one reading knows them: the active system is available, with the offset
// and rotation task reports; every other one is not, and holds zeros. With Link::down none is.
[[nodiscard]] auto to_work_systems(const NmlStatus& reading) -> std::array<WorkSystem, 9>;

} // namespace coffeecnc

#endif // COFFEECNC_LINUXCNC_PANEL_MAPPING_HPP
