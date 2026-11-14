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
// Line numbers. Task's current and motion line are counters of task, not of a load, and they
// are not only moved by the program: an open zeroes both; a run does not; an MDI block and an
// E-stop or abort with the interpreter idle write the current line (2.9.4;
// docs/linuxcnc-interface.md). `current_line` and `motion_line` of the status are therefore
// never task's counters passed through. They are what was attributed to the loaded revision,
// each 0 until something was:
//
//   evidence    only a status that is live, was read after this source took the revision up
//               and after it noticed the last MDI block the port reports, and is newer than
//               the one judged before. Any other status (stale, older, of another file)
//               changes nothing: what was attributed last stays reported.
//   executing   such a status with task in auto and the interpreter not idle (reading,
//               paused, waiting): the program of this revision runs. A counter is attributed
//               from the first such status of the run in which it differs from what it read
//               last before the run (in the first status of the run where none was read
//               before), or in which the machine moves, dwells or waits for a tool change:
//               at the start of a run the counters still hold what they held before, until
//               task takes up the first block. From then on, to the end of the run, the
//               counter is reported as read; paused included.
//   ended       the first status after the run with task in auto and idle (finished, aborted,
//               E-stop): the attributed counters are read once more, as the lines the
//               program stopped at, and kept. Further idle statuses do not move them.
//   not auto    a status with task in manual or MDI mode, or an MDI block the port reports
//               (NmlLoads::mdi_blocks): nothing is attributed any more, both are 0 until the
//               next run moves them.
//   load        a new revision starts with both 0.
//
// What follows: a run that no status showed executing (shorter than the publishing period)
// leaves the lines 0, and the lines of a run that ended between two readings are those of
// the first idle status after it. Not excluded: a status that shows task in auto and not
// idle while the machine still moves from something before the run (nothing is known that
// does it: a mode change ends a jog) would attribute the counters one block early; and an
// MDI block sent by another client that begins and ends in auto mode between two readings.
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
