#include <coffeecnc/app/response.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <iostream>
#include <limits>
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
  const RefusalMessage refusal{"messages.policy.loadProgramFirst"};
  const auto failed =
      Json::parse(encode_result(1, CommandOutcome::failed, "Why.", {}, {}, refusal));
  require(!failed.contains("refusal"), "Only a rejection carries a refusal key");
  require(failed.at("localization").at("key") == refusal.key,
          "A refusal after writing a command can still be localized as a failure");
  const MessageKey timeout{"messages.session.linuxcncNoAnswer"};
  const auto timed_out = Json::parse(
      encode_result(8, CommandOutcome::timed_out, "LinuxCNC did not answer.", {}, {}, {}, timeout));
  require(timed_out.at("reason") == "LinuxCNC did not answer." &&
              timed_out.at("localization") ==
                  Json{{"key", timeout.key}, {"params", Json::object()}},
          "A timeout carries both the English reason and its key");
  const auto accepted =
      Json::parse(encode_result(7, CommandOutcome::accepted, "", {}, {}, {}, timeout));
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
  const SetterState setter{.calibration = std::array{0.0, 0.0, -40.0},
                           .measurement =
                               SetterMeasurement{false, 2, {0.0, 0.0, 12.0}, 52.0, 40.0, 0.03},
                           .measured_tools = {98, 2}};
  const auto measurement = Json::parse(encode_result(42, CommandOutcome::accepted, "", setter));
  require(measurement.at("setter").at("measurement").at("stopping_distance") == 0.03,
          "Fixed setter measurement reports the stopping distance in mm");
  require(measurement.at("setter").at("measured_tools") == Json{98, 2},
          "Fixed setter state lists the tools touched off against its calibration");
  SetterState slot{};
  slot.material = SetterState::Material{2, 'Y'};
  slot.feature_size[1] = 12.5;
  const auto feature = Json::parse(encode_result(42, CommandOutcome::accepted, "", slot)).at("setter");
  require(feature.at("feature_size") == Json{{"y", 12.5}} && feature.at("material").at("axis") == "Y",
          "Slot response reports the physical width in millimetres for the measured axis only");
  SetterState arc{};
  arc.material = SetterState::Material{2, 'X', true};
  arc.center_diameter = 16;
  arc.arc_span_deg = 120;
  arc.center_uncertainty_mm = 0.005;
  const auto arc_json = Json::parse(encode_result(42, CommandOutcome::accepted, "", arc)).at("setter");
  require(arc_json.at("center_diameter") == 16 && arc_json.at("arc_span") == 120 &&
              arc_json.at("center_uncertainty") == 0.005 &&
              arc_json.at("material").at("axis") == "XY",
          "A three-point result reports the diameter, the arc the contacts covered and the "
          "centre uncertainty");
  slot.feature_size[1] = std::numeric_limits<double>::quiet_NaN();
  const auto invalid = Json::parse(encode_result(42, CommandOutcome::accepted, "", slot));
  require(invalid.at("outcome") == "failed" && !invalid.contains("setter"),
          "An invalid measurement returns a failed request without corrupt JSON values");
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
  const MessageKey contact{"backend.jogStop.probeContact"};
  const auto keyed =
      Json::parse(encode_message(MessageLevel::info, "Jog stopped: probe contact", 10ms, contact));
  require(keyed.at("text") == "Jog stopped: probe contact" &&
              keyed.at("localization") == Json{{"key", contact.key}, {"params", Json::object()}},
          "A catalogued operator message retains its English text for older panels");
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
