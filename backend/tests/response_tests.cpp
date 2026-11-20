#include <coffeecnc/app/response.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

using namespace coffeecnc;
using namespace std::chrono_literals;

namespace {
using Json = nlohmann::json;

int checks = 0;
void require(bool condition, const std::string& message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

void command_results() {
  for (const auto& [outcome, name] : {std::pair{CommandOutcome::accepted, "accepted"},
                                      std::pair{CommandOutcome::rejected, "rejected"},
                                      std::pair{CommandOutcome::failed, "failed"},
                                      std::pair{CommandOutcome::timed_out, "timed_out"}}) {
    const auto result = Json::parse(encode_result(42, outcome, "Why."));
    require(result == Json{{"type", "result"}, {"id", 42}, {"outcome", name}, {"reason", "Why."}},
            std::string("CommandResult fields as protocol v1: ") + name);
  }
  const auto accepted = Json::parse(encode_result(7, CommandOutcome::accepted, ""));
  require(accepted.at("reason") == "" && accepted.size() == 4, "Accepted keeps an empty reason");
  for (const auto id : {RequestId{0}, RequestId{-1}, -max_request_id, max_request_id}) {
    const auto result = Json::parse(encode_result(id, CommandOutcome::rejected, "No."));
    require(result.at("id").is_number_integer() && result.at("id").get<RequestId>() == id,
            "Safe integer ID survives encoding");
  }
  const auto quoted = Json::parse(encode_result(1, CommandOutcome::failed, "Say \"no\"\n\\ pièce"));
  require(quoted.at("reason") == "Say \"no\"\n\\ pièce", "Reason text is escaped, not altered");
  auto latin1 = std::string("Fr");
  latin1.push_back(static_cast<char>(0xE4));
  latin1 += "ser";
  const auto replaced = Json::parse(encode_result(1, CommandOutcome::failed, latin1));
  require(replaced.at("reason") == "Fr\xEF\xBF\xBDser",
          "Controller text that is not UTF-8 still produces a result");
  const SetterState setter{.reference_captured = false,
                           .calibration = std::array{0.0, 0.0, -40.0},
                           .review = SetterReview{9, false, 2, {0.0, 0.0, 12.0}, 52.0, 40.0, 0.03}};
  const auto measurement = Json::parse(encode_result(42, CommandOutcome::accepted, "", setter));
  require(measurement.at("setter").at("review").at("stopping_distance") == 0.03,
          "Fixed setter review reports the stopping distance in mm");
}

void operator_messages() {
  const auto error =
      Json::parse(encode_message(MessageLevel::error, "Joint 0 on limit", 1700000000123ms));
  require(error == Json{{"type", "message"},
                        {"level", "error"},
                        {"text", "Joint 0 on limit"},
                        {"time", 1700000000123}},
          "OperatorMessage fields as protocol v1");
  const auto info = Json::parse(encode_message(MessageLevel::info, "Probe tripped", 0ms));
  require(info.at("level") == "info" && info.at("time") == 0, "Info level and epoch time");
  require(info.at("time").is_number_integer(), "Time is integer milliseconds");
}
} // namespace

auto main() -> int {
  try {
    command_results();
    operator_messages();
    std::cout << checks << " response encoding checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
