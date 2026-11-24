#ifndef COFFEECNC_REMOTE_SERVER_HPP
#define COFFEECNC_REMOTE_SERVER_HPP

#include <coffeecnc/app/status_publisher.hpp>
#include <coffeecnc/core/message_key.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace coffeecnc {

struct RemoteServerTiming {
  std::chrono::milliseconds file_write_timeout{std::chrono::seconds{5}};
  std::chrono::milliseconds partial_frame_timeout{std::chrono::seconds{2}};
};

// HTTP and a read-only WebSocket on their own thread. Optional program uploads go to a disk
// worker through the supplied callback.
class RemoteServer {
public:
  using Upload =
      std::function<std::expected<void, LocalizedReason>(std::string_view, std::string_view, bool)>;
  RemoteServer(const std::string& ui_directory, std::uint16_t port,
               std::span<const std::string> aliases = {}, Upload upload = {},
               RemoteServerTiming timing = {});
  ~RemoteServer();
  RemoteServer(const RemoteServer&) = delete;
  auto operator=(const RemoteServer&) -> RemoteServer& = delete;

  [[nodiscard]] auto port() const -> std::uint16_t;
  // Stops accepting clients and joins any upload in progress. May also be called before teardown
  // of the services captured by Upload.
  void stop();
  // One publisher supplies complete batches. Network clients only load immutable snapshots.
  void publish(std::span<const std::string> messages, std::span<const PanelRecord> records,
               const Status& status, bool has_program) noexcept;
  // The controller worker also emits notifications. Only message records are mirrored.
  void notifications(std::span<const std::string> messages) noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace coffeecnc

#endif
