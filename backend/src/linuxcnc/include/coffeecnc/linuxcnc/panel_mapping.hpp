#ifndef COFFEECNC_LINUXCNC_PANEL_MAPPING_HPP
#define COFFEECNC_LINUXCNC_PANEL_MAPPING_HPP

#include <coffeecnc/core/panel_messages.hpp>
#include <coffeecnc/linuxcnc/loaded_tools.hpp>
#include <coffeecnc/linuxcnc/machine_config.hpp>
#include <coffeecnc/linuxcnc/nml_port.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace coffeecnc {

// From LinuxCNC's view of the machine to the panel's messages. Pure: no LinuxCNC, no I/O.

// The protocol `hello` of a machine driven through LinuxCNC.
[[nodiscard]] auto to_hello(const MachineConfig& config) -> Hello;

// From `halcmd -s show pin <pin>`: signal name, empty for a confirmed unlinked pin,
// nullopt when no valid exact pin row was found. Unknown is never treated as unlinked.
[[nodiscard]] auto pin_signal(std::string_view shown, std::string_view pin)
    -> std::optional<std::string>;
// From `halcmd -s show sig <signal>`: reader presence, or nullopt for unknown output.
[[nodiscard]] auto signal_has_reader(std::string_view shown, std::string_view signal)
    -> std::optional<bool>;

// The protocol `status` of one reading. `tools` is the table LinuxCNC has loaded: it reports
// the prepared tool as an index into that table, and the panel is told the tool number of the
// entry with that index. An index the table does not hold (and index 0, the spindle) is
// reported as no prepared tool. The loaded tool is task's own report, whatever is prepared or
// asked for. `preview_context` is left 0.
//
// With Link::down the reading holds nothing: the placeholders are zero, with the configured
// travel as limits and G54 as the work system.
//
// `wait` is the first that applies of: a tool change LinuxCNC asks for on the HAL pins, paused,
// a dwell with time left.
[[nodiscard]] auto to_panel_status(const NmlStatus& reading, const MachineConfig& config,
                                   std::span<const LoadedTool> tools,
                                   std::uint64_t program_revision) -> Status;

// G54 ... G59.3 in the text of LinuxCNC's parameter file: lines `<number> <value>`, values in
// machine units. System n is parameters 5201 + 20 n (X), + 1 (Y), + 2 (Z) and + 9 (the XY
// rotation, degrees); its offset is given in millimetres. A system any of whose four
// parameters is missing or not a finite number is not there; nor is any in a text longer than
// max_parameter_file_bytes. Other lines are passed over.
inline constexpr std::size_t max_parameter_file_bytes = std::size_t{1024} * 1024;
[[nodiscard]] auto parameter_work_systems(std::string_view text, double millimetres_per_unit)
    -> std::array<std::optional<WorkSystem>, 9>;

// G54 ... G59.3 as one reading knows them. The active system is available, with the offset and
// rotation task reports, current while the link is live. Every other one is what `saved` holds
// for it (parameter_work_systems), current when `saved_current` and the link is live; else it
// is not available and holds zeros. With Link::down none is available.
[[nodiscard]] auto to_work_systems(const NmlStatus& reading,
                                   const std::array<std::optional<WorkSystem>, 9>& saved = {},
                                   bool saved_current = false) -> std::array<WorkSystem, 9>;

} // namespace coffeecnc

#endif // COFFEECNC_LINUXCNC_PANEL_MAPPING_HPP
