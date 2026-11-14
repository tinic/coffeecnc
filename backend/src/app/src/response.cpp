#include <coffeecnc/app/response.hpp>

#include <nlohmann/json.hpp>

namespace coffeecnc {
namespace {
using Json = nlohmann::json;

auto name(CommandOutcome outcome) -> std::string_view {
  switch (outcome) {
  case CommandOutcome::accepted:
    return "accepted";
  case CommandOutcome::rejected:
    return "rejected";
  case CommandOutcome::failed:
    return "failed";
  case CommandOutcome::timed_out:
    return "timed_out";
  }
  return "failed";
}

auto text(const Json& message) -> std::string {
  // Controller text is not guaranteed to be UTF-8; dump() must not throw on it.
  return message.dump(-1, ' ', false, Json::error_handler_t::replace);
}
} // namespace

auto encode_result(RequestId id, CommandOutcome outcome, std::string_view reason) -> std::string {
  return text(Json{{"type", "result"}, {"id", id}, {"outcome", name(outcome)}, {"reason", reason}});
}

auto encode_message(MessageLevel level, std::string_view text_for_operator,
                    std::chrono::milliseconds unix_time) -> std::string {
  return text(Json{{"type", "message"},
                   {"level", level == MessageLevel::info ? "info" : "error"},
                   {"text", text_for_operator},
                   {"time", unix_time.count()}});
}

} // namespace coffeecnc
