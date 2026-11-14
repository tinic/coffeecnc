#ifndef COFFEECNC_LINUXCNC_NML_STATUS_READER_HPP
#define COFFEECNC_LINUXCNC_NML_STATUS_READER_HPP

#include <coffeecnc/core/status_source.hpp>
#include <coffeecnc/linuxcnc/hal_control.hpp>
#include <coffeecnc/linuxcnc/machine_config.hpp>
#include <coffeecnc/linuxcnc/nml_port.hpp>

#include <expected>
#include <memory>
#include <string>

namespace coffeecnc {

// A status channel of its own for one more thread: reads what LinuxCNC's task reports and never
// touches the command or error channel. `link` comes from the status buffer's write counter as
// this reader sees it: read at least a few times per second.
//
// A read takes the status buffer's semaphore and can block as long as a stopped task holds it
// (see NmlPort), so it belongs on a thread that may block: the runtime's status thread, not its
// control thread. NOT thread-safe: one thread calls read and status; connect and destroy as
// NmlPort says.
class NmlStatusReader final : public StatusSource {
public:
  // `hal` may be null: no tool change request and no optional signals are then reported. It
  // must outlive the reader.
  [[nodiscard]] static auto connect(const MachineConfig& config, const HalShared* hal)
      -> std::expected<std::unique_ptr<NmlStatusReader>, std::string>;
  ~NmlStatusReader() override;
  NmlStatusReader(const NmlStatusReader&) = delete;
  auto operator=(const NmlStatusReader&) -> NmlStatusReader& = delete;

  [[nodiscard]] auto read() -> ControllerState override;
  // The same reading with everything else the panel shows.
  [[nodiscard]] auto status() -> NmlStatus;

private:
  struct Impl;
  explicit NmlStatusReader(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

} // namespace coffeecnc

#endif // COFFEECNC_LINUXCNC_NML_STATUS_READER_HPP
