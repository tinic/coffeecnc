#ifndef COFFEECNC_LINUXCNC_SRC_NML_STATUS_HPP
#define COFFEECNC_LINUXCNC_SRC_NML_STATUS_HPP

// Shared by the two readers of LinuxCNC's status buffer. Not a public header: it needs
// LinuxCNC's.

#include <coffeecnc/linuxcnc/hal_control.hpp>
#include <coffeecnc/linuxcnc/machine_config.hpp>
#include <coffeecnc/linuxcnc/nml_port.hpp>
#include <coffeecnc/linuxcnc/nml_status_reader.hpp>

#include <coffeecnc/core/task_heartbeat.hpp>

#include "nml_gate.hpp"

#include <span>
#include <string>

#include <emc_nml.hh>
#include <stat_msg.hh>

namespace coffeecnc {

// Translate LinuxCNC's private execution file and its extra M0 line to the loaded program.
void map_breakpoint(NmlStatus& status, const BreakpointSource* source);

// Text of a fixed-size, NUL-terminated NML string field, without trailing white space.
[[nodiscard]] auto text_of(std::span<const char> field) -> std::string;

// One connection to LinuxCNC's status buffer, for one thread.
class StatusChannel {
public:
  explicit StatusChannel(const std::string& nml_file);
  [[nodiscard]] auto valid() -> bool;
  // The latest status task wrote, or null before the first. peek() returns 0 when task wrote
  // nothing since the last peek; the buffer then still holds the latest status. Whether task is
  // alive is the heartbeat's question, not this one's.
  [[nodiscard]] auto stat() -> const EMC_STAT*;
  // From the status buffer's write counter, as of the last stat().
  [[nodiscard]] auto link() const -> Link { return link_; }

private:
  Gated<RCS_STAT_CHANNEL> channel_;
  TaskHeartbeat heartbeat_;
  Link link_{Link::down};
  // The channel's buffer holds a task status from an earlier peek.
  bool have_status_{};
};

// Convert all coordinates from the same machine position using the active work frame.
[[nodiscard]] auto work_position(const Vec3& machine, const Vec3& g5x, const Vec3& g92,
                                 const Vec3& tool, double rotation) -> Vec3;

// One EMC_STAT in protocol units. `link` is the caller's own reading of the write counter.
[[nodiscard]] auto to_status(const EMC_STAT& s, const MachineConfig& config, const HalShared* hal,
                             Link link) -> NmlStatus;

} // namespace coffeecnc

#endif // COFFEECNC_LINUXCNC_SRC_NML_STATUS_HPP
