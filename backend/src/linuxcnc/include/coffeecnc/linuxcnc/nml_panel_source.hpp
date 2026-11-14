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
//                 the loaded table. They are read when a table is accepted, and when the port
//                 reports a load (NmlLoads::tool_tables)
//   work offsets  the active system only, from the status. The interpreter's parameter file is
//                 not read: it is what LinuxCNC saved last, not what is in force (see
//                 docs/linuxcnc-interface.md)
//   program       the text of the file task has open, through `programs`. It is read once per
//                 load: when task names another file, and when the port reports that LinuxCNC
//                 accepted an open_program (NmlLoads::programs), also of the same file
//
// Line numbers. Task's current and motion line are counters of task, not of a load: an open
// zeroes both, and an E-stop or abort with the interpreter idle brings the current line of the
// program run before back (2.9.4; docs/linuxcnc-interface.md). A counter is therefore reported
// only when the loaded revision produced it, and is 0 until then:
//
//   started   the revision counts as started once the port reports a run or step it had accepted
//             after the load (NmlLoads::program_starts), or a status shows task in auto with
//             the interpreter not idle. An MDI block is neither. The port's count also covers a
//             run too short for any status to show it; the status covers a start by another
//             client
//   reported  each counter from the first status, after the start, in which it differs from
//             what it read last before the start (0 where no status came before it)
//
// A run or step accepted in the same publishing period as its load is not counted for it: the
// status must then show the execution, or the lines stay 0. Withholding is the safe side.
//
// Revisions count from 1. The tool table's goes up when the table shown differs; the program's
// with every load as above. A program text that cannot be read leaves the program without text
// and is reported once, as an operator message; so is a tool table that cannot be read, which
// leaves the last one read in place.
//
// When a pass over the loaded table is shown. LinuxCNC's tool data memory cannot say whether a
// reload is in progress (see LoadedToolTable), so a pass is accepted only when all of this
// holds: a later call has read the same table again (the calls are a publishing period apart);
// and a status that the status thread read after the first of the two passes says that task
// has nothing in execution (NmlStatus::task_executing). What that is worth:
//
//   a load asked for through LinuxCNC's task (set_tool and remove_tool of this executable,
//   another client's EMC_TOOL_LOAD_TOOL_TABLE, an MDI `G10 L1`, a tool change): task keeps the
//   command in execution until io has answered it, whatever command it takes up after it, so
//   a table read during the load is never accepted. The port, on its side, reports
//   set_tool and remove_tool as accepted only when task reports the load done.
//
//   a writer that does not go through task (another program that maps the tool data memory
//   and writes it): nothing brackets it. The table is then as read, twice, a period apart.
//
// The price: while a program or an MDI block executes, a table that has changed (a `G10 L1`
// in the program) is not shown until execution has ended. Until a pass is accepted the table
// accepted before stays shown.
//
// The prepared tool is resolved only in a table that is current: the pass of this call equals
// the accepted table. Otherwise (a reload, a changed table not yet accepted, a table that
// cannot be read) it is reported as unknown, never as a number from an older table.
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
  // One pass over the loaded table; nothing when it cannot be read.
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
