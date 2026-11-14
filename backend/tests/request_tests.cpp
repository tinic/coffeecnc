#include <coffeecnc/app/request.hpp>

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace coffeecnc;

namespace {
int checks = 0;
void require(bool condition, const std::string& message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

template <typename T> auto body(std::string_view wire) -> T {
  const auto decoded = decode_request(wire);
  const auto* request = std::get_if<Request>(&decoded);
  require(request && std::holds_alternative<T>(request->command),
          "Expected typed request: " + std::string(wire));
  require(request->id == 42, "Request ID preserved");
  return std::get<T>(request->command);
}

auto rejected(std::string_view wire, std::optional<RequestId> id) -> std::string {
  const auto decoded = decode_request(wire);
  const auto* error = std::get_if<DecodeError>(&decoded);
  require(error && error->id == id && !error->reason.empty(),
          "Expected rejection with an unambiguous ID only: " + std::string(wire));
  return error->reason;
}

void protocol_forms() {
  // Protocol v1 fixtures use the panel's actual field names, not a C++ JSON serializer.
  body<Ready>(R"({"id":42,"type":"ready"})");
  require(body<SetMachine>(R"({"id":42,"type":"set_machine","state":"off"})").state ==
              MachineState::off,
          "Off is E-stop reset, not power-on");
  require(!body<Home>(R"({"id":42,"type":"home","axis":"all"})").axis, "Home all");
  require(body<Home>(R"({"id":42,"type":"home","axis":"z"})").axis == Axis::z, "Home Z");
  const auto jog =
      body<Jog>(R"({"id":42,"type":"jog","axis":"y","direction":-1,"velocity":123.5,"held":true})");
  require(jog.axis == Axis::y && jog.direction == -1 && jog.velocity == 123.5 && jog.held,
          "Signed jog keeps direction, units and hold state");
  const auto step =
      body<JogStep>(R"({"id":42,"type":"jog_step","axis":"x","distance":-0.01,"velocity":60})");
  require(step.axis == Axis::x && step.distance == -0.01 && step.velocity == 60,
          "Signed increment");
  body<JogStopAll>(R"({"id":42,"type":"jog_stop_all"})");
  require(body<Mdi>(R"({"id":42,"type":"mdi","text":"G0 X12.5"})").text == "G0 X12.5",
          "MDI unchanged");
  require(!body<ListDirectory>(R"({"id":42,"type":"list_directory","path":null})").path,
          "Explicit null selects program directory");
  require(body<ListDirectory>(R"({"id":42,"type":"list_directory","path":"parts"})").path ==
              "parts",
          "Directory path preserved");
  require(body<OpenProgram>(R"({"id":42,"type":"open_program","path":"pièce.ngc"})").path ==
              "pièce.ngc",
          "UTF-8 filenames preserved");
  body<Run>(R"({"id":42,"type":"run"})");
  body<Pause>(R"({"id":42,"type":"pause"})");
  body<Resume>(R"({"id":42,"type":"resume"})");
  body<Step>(R"({"id":42,"type":"step"})");
  body<Abort>(R"({"id":42,"type":"abort"})");
  const auto override =
      body<SetOverride>(R"({"id":42,"type":"set_override","which":"spindle","value":0.75})");
  require(override.which == Override::spindle && override.value == 0.75, "Override factor");
  const auto spindle =
      body<SetSpindle>(R"({"id":42,"type":"set_spindle","direction":"reverse","rpm":3000})");
  require(spindle.direction == SpindleDirection::reverse && spindle.rpm == 3000,
          "Spindle direction");
  const auto coolant =
      body<SetCoolant>(R"({"id":42,"type":"set_coolant","flood":false,"mist":true})");
  require(!coolant.flood && coolant.mist, "Independent coolant states");
  const auto option = body<SetProgramOption>(
      R"({"id":42,"type":"set_program_option","option":"block_delete","on":true})");
  require(option.option == ProgramOption::block_delete && option.on, "Program option");
  require(body<SelectWcs>(R"({"id":42,"type":"select_wcs","wcs":9})").wcs == 9, "G59.3 index");
  const auto touch = body<TouchOff>(R"({"id":42,"type":"touch_off","axis":"z","value":-2.5})");
  require(touch.axis == Axis::z && touch.value == -2.5, "Touch-off value");
  require(body<ToolTouchOff>(R"({"id":42,"type":"tool_touch_off","value":25.4})").value == 25.4,
          "Tool touch-off millimetres");
  const auto tool =
      body<SetTool>(
          R"({"id":42,"type":"set_tool","tool":{"number":7,"pocket":3,"length":50.5,"diameter":6.35,"comment":"¼ inch end mill"}})")
          .tool;
  require(tool.number == 7 && tool.pocket == 3 && tool.length == 50.5 && tool.diameter == 6.35 &&
              tool.comment == "¼ inch end mill",
          "Tool table fields stay independent");
  require(body<RemoveTool>(R"({"id":42,"type":"remove_tool","number":7})").number == 7,
          "Tool removal number");
  body<ToolChanged>(R"({"id":42,"type":"tool_changed"})");
}

void malformed_and_ambiguous() {
  for (const auto wire :
       {"", "null", "[]", "1", "{}", "true", R"({"id":42,"type":"run"} {})",
        R"({"id":42,"type":"run",})", R"({"id":42,"type":"run","id":43})",
        R"({"id":42,"type":"run","\u0069d":42})", R"({"id":42,"type":"run","extra":{"a":1,"a":2}})",
        R"({"id":42,"type":"run","extra":[{"a":1,"a":2}]})",
        R"({"id":42,"type":"run","extra":"\uD800"})",
        R"({"id":42,"type":"set_override","which":"feed","value":1e999})"})
    rejected(wire, std::nullopt);
  auto nul = std::string(R"({"id":42,"type":"run"})");
  nul.push_back('\0');
  nul += "{}";
  rejected(nul, std::nullopt);
  auto invalid_utf8 = std::string(R"({"id":42,"type":"mdi","text":")");
  invalid_utf8.push_back(static_cast<char>(0xFF));
  invalid_utf8 += "\"}";
  rejected(invalid_utf8, std::nullopt);
}

void strict_shapes() {
  for (
      const auto wire :
      {R"({"id":42,"type":"run","line":100})",
       R"({"id":42,"type":"set_mode","mode":"auto"})",
       R"({"id":42,"type":null})",
       R"({"id":42,"type":"set_machine","state":"ON"})",
       R"({"id":42,"type":"home","axis":"a"})",
       R"({"id":42,"type":"home","axis":0})",
       R"({"id":42,"type":"list_directory"})",
       R"({"id":42,"type":"open_program","path":null})",
       R"({"id":42,"type":"open_program","path":"part.ngc\u0000ignored"})",
       R"({"id":42,"type":"mdi","text":["G0 X1"]})",
       R"({"id":42,"type":"jog","axis":"x","direction":0,"velocity":1,"held":true})",
       R"({"id":42,"type":"jog","axis":"x","direction":1,"velocity":"100","held":true})",
       R"({"id":42,"type":"jog","axis":"x","direction":1,"velocity":100,"held":1})",
       R"({"id":42,"type":"set_coolant","flood":true,"mist":"false"})",
       R"({"id":42,"type":"set_override","which":"rapid","value":true})",
       R"({"id":42,"type":"set_override","which":"feed","value":null})",
       R"({"id":42,"type":"select_wcs","wcs":1.5})",
       R"({"id":42,"type":"remove_tool","number":2147483648})",
       R"({"id":42,"type":"remove_tool","number":-2147483649})",
       R"({"id":42,"type":"set_tool","tool":[]})",
       R"({"id":42,"type":"set_tool","tool":{"number":1,"pocket":1,"length":0,"diameter":6}})",
       R"({"id":42,"type":"set_tool","tool":{"number":1,"pocket":1,"length":0,"diameter":6,"comment":"","extra":1}})"})
    rejected(wire, 42);
}

void identity_and_limits() {
  for (const auto token : {"null", "true", "\"42\"", "1.5", "9007199254740992", "-9007199254740992",
                           "18446744073709551615"})
    rejected("{\"type\":\"ready\",\"id\":" + std::string(token) + "}", std::nullopt);
  for (const auto id : {RequestId{0}, RequestId{-1}, -max_request_id, max_request_id}) {
    const auto decoded = decode_request("{\"type\":\"ready\",\"id\":" + std::to_string(id) + "}");
    const auto* request = std::get_if<Request>(&decoded);
    require(request && request->id == id, "Exact safe integer ID survives both range boundaries");
  }
  body<Ready>(R"({"type":"ready","id":42.0})");
  auto padded = std::string(R"({"type":"ready","id":42})");
  padded.resize(max_request_bytes, ' ');
  body<Ready>(padded);
  padded.push_back(' ');
  rejected(padded, std::nullopt);
  const auto nested = std::string(R"({"id":42,"type":"run","extra":)") + std::string(1000, '[') +
                      "0" + std::string(1000, ']') + '}';
  require(rejected(nested, std::nullopt).find("nesting") != std::string::npos,
          "Deep nesting stops before DOM construction, even under byte limit");
  // An otherwise-valid tree at depth eight parses; the unsupported field is then rejected.
  const auto shallow = std::string(R"({"id":42,"type":"run","extra":)") + std::string(7, '[') +
                       "0" + std::string(7, ']') + '}';
  rejected(shallow, 42);
}

void policy_still_required() {
  CommandPolicy policy{Limits{3000, 1.2, 0.5, 1.2, 24000}};
  const ControllerState disconnected;
  const auto run = body<Run>(R"({"id":42,"type":"run"})");
  require(!policy.evaluate(run, disconnected).allowed, "Decoding never authorizes motion");
  const auto release =
      body<Jog>(R"({"id":42,"type":"jog","axis":"x","direction":1,"velocity":0,"held":false})");
  require(policy.evaluate(release, disconnected).allowed,
          "Wire layer preserves stale-state stop attempts");
  const auto invalid_wcs = body<SelectWcs>(R"({"id":42,"type":"select_wcs","wcs":10})");
  require(!policy.evaluate(invalid_wcs, disconnected).allowed, "Semantic limits belong to policy");
}
} // namespace

auto main() -> int {
  try {
    protocol_forms();
    malformed_and_ambiguous();
    strict_shapes();
    identity_and_limits();
    policy_still_required();
    std::cout << checks << " request boundary checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
