#ifndef COFFEECNC_APP_REQUEST_HPP
#define COFFEECNC_APP_REQUEST_HPP

#include <coffeecnc/core/command_policy.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace coffeecnc {

using RequestId = std::int64_t;
inline constexpr RequestId max_request_id = 9007199254740991; // JavaScript MAX_SAFE_INTEGER
inline constexpr std::size_t max_request_bytes = 16384;
// Only `save_program` may be longer, up to this: its text (max_saved_program_bytes) with every
// line end escaped, and the other fields.
inline constexpr std::size_t max_save_program_request_bytes =
    2 * max_saved_program_bytes + max_request_bytes;
inline constexpr int max_request_depth = 8;

struct Request {
  Request() = delete;
  Request(RequestId request_id, Command body) : id(request_id), command(std::move(body)) {}
  RequestId id;
  Command command;
};

struct DecodeError {
  // Only present when a complete, unambiguous JSON object supplied a valid ID.
  // The app can reject that ID; otherwise it must emit an operator message, not invent an ID.
  std::optional<RequestId> id;
  std::string reason;
  std::optional<MessageKey> localization{};
};

using DecodedRequest = std::variant<Request, DecodeError>;

// Decodes protocol v1 only. No state checks, I/O, controller action or acknowledgement.
// Valid JSON shape is not permission to execute: apply CommandPolicy to a fresh snapshot next.
[[nodiscard]] auto decode_request(std::string_view json) -> DecodedRequest;

} // namespace coffeecnc

#endif // COFFEECNC_APP_REQUEST_HPP
