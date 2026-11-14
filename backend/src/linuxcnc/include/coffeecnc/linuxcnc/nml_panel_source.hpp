#ifndef COFFEECNC_LINUXCNC_NML_PANEL_SOURCE_HPP
#define COFFEECNC_LINUXCNC_NML_PANEL_SOURCE_HPP

#include <coffeecnc/core/panel_source.hpp>
#include <coffeecnc/linuxcnc/loaded_tools.hpp>
#include <coffeecnc/linuxcnc/machine_config.hpp>
#include <coffeecnc/linuxcnc/nml_port.hpp>
#include <coffeecnc/linuxcnc/nml_status_reader.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace coffeecnc {

// What the panel is shown, put together for the publisher thread from what other owners leave
// for it. No NML call and no HAL pin:
//
//   status        the latest reading the status thread left in the mailbox (that thread owns
//                 the NML status channel), with the control thread's copy of the HAL pins in it
//   tool table    the table LinuxCNC has loaded, through `tools` (LoadedToolTable::read): the
//                 same table task's prepared index refers to. The file on disk is not shown: it
//                 is what set_tool and remove_tool edit before LinuxCNC loads it again. Only the
//                 comments come from the file, by tool number: LinuxCNC does not keep them with
//                 the loaded table. They are read when the loaded table is first read, when its
//                 content changes, and when the port reports a load (NmlLoads::tool_tables)
//   work offsets  the active system only, from the status. The interpreter's parameter file is
//                 not read: it is what LinuxCNC saved last, not what is in force (see
//                 docs/linuxcnc-interface.md)
//   program       the text of the file task has open, through `programs`. It is read once per
//                 load: when task names another file, and when the port reports that LinuxCNC
//                 accepted an open_program (NmlLoads::programs), also of the same file
//
// Revisions count from 1. The tool table's goes up when the table shown differs; the program's
// with every load as above. A program text that cannot be read leaves the program without text
// and is reported once, as an operator message; so is a tool table that cannot be read, which
// leaves the last one read in place.
//
// `link` is decided last, after every read above, from the age of the latest status in the
// mailbox at that moment: `down` before the first reading and for a reading without a status;
// `stale` instead of `live` when it is older than `max_age`, the bound the session applies to
// its own copy: the status thread is held up in LinuxCNC's library or has stopped, or this
// thread was held up reading.
//
// NOT thread-safe: one thread calls read.
class NmlPanelSource final : public PanelSource {
public:
  using Clock = std::chrono::steady_clock;
  using ToolReader = std::function<std::optional<std::vector<LoadedTool>>()>;
  static constexpr auto max_age = std::chrono::milliseconds{100};

  // The mailbox and `loads` must outlive the source. `tools` may be empty: no table is then
  // shown. `loads` may be null. `clock` is for tests.
  NmlPanelSource(const MachineConfig& config, const NmlStatusMailbox& mailbox, ToolReader tools,
                 ProgramReader programs, const NmlLoads* loads,
                 std::function<Clock::time_point()> clock = Clock::now);
  ~NmlPanelSource() override;
  NmlPanelSource(const NmlPanelSource&) = delete;
  auto operator=(const NmlPanelSource&) -> NmlPanelSource& = delete;

  [[nodiscard]] auto hello() const -> Hello override;
  [[nodiscard]] auto read() -> PanelReading override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace coffeecnc

#endif // COFFEECNC_LINUXCNC_NML_PANEL_SOURCE_HPP
