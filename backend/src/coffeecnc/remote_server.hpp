#ifndef COFFEECNC_REMOTE_SERVER_HPP
#define COFFEECNC_REMOTE_SERVER_HPP

#include <coffeecnc/app/status_publisher.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace coffeecnc {

// HTTP and a read-only WebSocket on their own thread. The only application inputs are encoded
// snapshots and notifications. No Runtime, Session, window, or controller is reachable here.
class RemoteServer {
public:
  RemoteServer(const std::string& ui_directory, std::uint16_t port,
               std::span<const std::string> aliases = {});
  ~RemoteServer();
  RemoteServer(const RemoteServer&) = delete;
  auto operator=(const RemoteServer&) -> RemoteServer& = delete;

  [[nodiscard]] auto port() const -> std::uint16_t;
  // One publisher supplies complete batches. Network clients only load immutable snapshots.
  void publish(std::span<const std::string> messages, std::span<const PanelRecord> records,
               bool has_program) noexcept;
  // The controller worker also emits notifications. Only message records are mirrored.
  void notifications(std::span<const std::string> messages) noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace coffeecnc

#endif
