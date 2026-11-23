#ifndef COFFEECNC_APP_RESPONSE_HPP
#define COFFEECNC_APP_RESPONSE_HPP

#include <coffeecnc/app/request.hpp>
#include <coffeecnc/core/preview.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace coffeecnc {

// Protocol v1 CommandResult.outcome.
enum class CommandOutcome : std::uint8_t {
  accepted,
  rejected,  // not valid in the current state; nothing was sent to the controller
  failed,    // the controller reported an error
  timed_out, // the controller did not answer
};

enum class MessageLevel : std::uint8_t { error, info };

struct RestartPlan {
  std::uint64_t plan_id{};
  bool needs_tool_change{};
  RestartPoint point;
};

// Wire text of the one CommandResult a request gets. Bytes that are not UTF-8 in the reason are
// replaced, never rejected: a result must always be sent.
[[nodiscard]] auto encode_result(RequestId id, CommandOutcome outcome, std::string_view reason,
                                 const std::optional<SetterState>& setter = {},
                                 const std::optional<RestartPlan>& restart = {},
                                 const std::optional<RefusalMessage>& refusal = {}) -> std::string;

// Wire text of an OperatorMessage. The time is since the Unix epoch.
[[nodiscard]] auto encode_message(MessageLevel level, std::string_view text,
                                  std::chrono::milliseconds unix_time) -> std::string;

} // namespace coffeecnc

#endif // COFFEECNC_APP_RESPONSE_HPP
