#ifndef COFFEECNC_LINUXCNC_NML_STATUS_READER_HPP
#define COFFEECNC_LINUXCNC_NML_STATUS_READER_HPP

#include <coffeecnc/core/status_source.hpp>
#include <coffeecnc/linuxcnc/hal_control.hpp>
#include <coffeecnc/linuxcnc/machine_config.hpp>
#include <coffeecnc/linuxcnc/nml_port.hpp>

#include <chrono>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace coffeecnc {

// One full reading and when it was taken.
struct TimedStatus {
  NmlStatus status;
  std::chrono::steady_clock::time_point read_at;
};

// Where the thread that owns a status reader leaves each reading for the threads that do not:
// the publisher takes the panel's status from here and never calls a reader. The time tells an
// old reading from a new one. Thread-safe; its mutex is held for a copy only.
class NmlStatusMailbox {
public:
  void put(const NmlStatus& status, std::chrono::steady_clock::time_point read_at);
  // Nothing before the first reading.
  [[nodiscard]] auto latest() const -> std::optional<TimedStatus>;

private:
  mutable std::mutex mutex_;
  std::optional<TimedStatus> latest_;
};

// A status channel of its own for one more thread: reads what LinuxCNC's task reports and never
// touches the command or error channel. `link` comes from the status buffer's write counter as
// this reader sees it: read at least a few times per second.
//
// A read can block for the semaphore timeout of the NML file, 10 s, while a stopped task holds
// the status buffer's semaphore, and as long again while another thread's NML call waits in the
// gate (see NmlPort), so it belongs on a thread that may block: the runtime's status thread, not
// its control thread. A reading that is late is old, and the session treats an old one as a
// stale link. NOT thread-safe: one thread calls read and status.
class NmlStatusReader final : public StatusSource {
public:
  // `hal` may be null: no tool change request and no optional signals are then reported. Every
  // reading, also one without a status, is left in `mailbox` when there is one. Both must
  // outlive the reader.
  [[nodiscard]] static auto connect(const MachineConfig& config, const HalShared* hal,
                                    NmlStatusMailbox* mailbox = nullptr)
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
