#ifndef COFFEECNC_LINUXCNC_NML_PANEL_SOURCE_HPP
#define COFFEECNC_LINUXCNC_NML_PANEL_SOURCE_HPP

#include <coffeecnc/core/panel_source.hpp>
#include <coffeecnc/linuxcnc/machine_config.hpp>
#include <coffeecnc/linuxcnc/nml_status_reader.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace coffeecnc {

// What the panel is shown, put together for the publisher thread from what other owners
// leave for it, without a call into LinuxCNC:
//
//   status            the latest reading the status thread left in the mailbox (that thread
//                     owns the NML status channel), with the control thread's copy of the HAL
//                     pins already in it
//   tool table        [EMCIO] TOOL_TABLE, as it is on disk: what LinuxCNC loads next, and what
//                     set_tool and remove_tool have just written
//   work offsets      the active system only, from the status. The interpreter's parameter
//                     file is not read: it is what LinuxCNC saved, not what is in force (see
//                     docs/linuxcnc-interface.md)
//   program           the file task has open, through `programs`
//
// A file is read again when its size or modification time changes. Revisions count from 1 and
// go up when the content read differs: of the tool table, and of the program whenever task names another file or the file named changes on disk. A file that
// cannot be read leaves what was read last in place and is reported once, as an operator
// message, until it can be read again.
//
// `link`: `down` before the first reading and for a reading without a status; `stale` instead
// of `live` for a reading older than `max_age`, the age rule the session applies to its own
// copy: the status thread is held up in LinuxCNC's library or has stopped.
//
// NOT thread-safe: one thread calls read.
class NmlPanelSource final : public PanelSource {
public:
  using Clock = std::chrono::steady_clock;
  static constexpr auto max_age = std::chrono::milliseconds{100};

  // The mailbox must outlive the source. `clock` is for tests.
  NmlPanelSource(const MachineConfig& config, const NmlStatusMailbox& mailbox,
                 ProgramReader programs,
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
