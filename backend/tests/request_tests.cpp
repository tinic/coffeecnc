#include <coffeecnc/app/request.hpp>
#include <coffeecnc/app/tool_models.hpp>

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
  const auto origin = body<SetWorkOffset>(
      R"({"id":42,"type":"set_work_offset","wcs":2,"offset":[25.4,-50.8,0]})");
  require(origin.wcs == 2 && origin.offset == std::array<double, 3>{25.4, -50.8, 0},
          "Explicit system and XYZ machine origins in millimetres");
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
       R"({"id":42,"type":"set_work_offset","wcs":1,"offset":[1,2]})",
       R"({"id":42,"type":"set_work_offset","wcs":1,"offset":[1,2,3,4]})",
       R"({"id":42,"type":"set_work_offset","wcs":1,"offset":[1,2,"3"]})",
       R"({"id":42,"type":"set_work_offset","wcs":1,"offset":[1,null,3]})",
       R"({"id":42,"type":"set_work_offset","wcs":1.5,"offset":[1,2,3]})",
       R"({"id":42,"type":"set_work_offset","wcs":1,"offset":[1,2,3],"rotation":90})",
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

constexpr std::string_view factory_defaults_json =
    R"({"end-mill":{"diameter":10,"cuttingLength":22,"shankDiameter":10,"overallLength":72,"stickout":35,"tipAngle":90},)"
    R"("ball-mill":{"diameter":10,"cuttingLength":20,"shankDiameter":10,"overallLength":72,"stickout":35,"tipAngle":90},)"
    R"("drill":{"diameter":10,"cuttingLength":60,"shankDiameter":10,"overallLength":100,"stickout":75,"tipAngle":118},)"
    R"("spot-drill":{"diameter":10,"cuttingLength":15,"shankDiameter":10,"overallLength":60,"stickout":30,"tipAngle":90},)"
    R"("chamfer":{"diameter":10,"cuttingLength":12,"shankDiameter":10,"overallLength":60,"stickout":30,"tipAngle":90},)"
    R"("face-mill":{"diameter":50,"cuttingLength":12,"shankDiameter":20,"overallLength":70,"stickout":40,"tipAngle":90},)"
    R"("generic":{"diameter":10,"cuttingLength":20,"shankDiameter":10,"overallLength":70,"stickout":35.25,"tipAngle":90}})";

// `text` with the first `old` replaced.
auto with(std::string text, std::string_view old, std::string_view replacement) -> std::string {
  const auto at = text.find(old);
  require(at != std::string::npos, "The test text has: " + std::string(old));
  return at == std::string::npos ? text : text.replace(at, old.size(), replacement);
}

void tool_model_commands() {
  const auto defaults_wire =
      R"({"id":42,"type":"set_tool_model_defaults","machine_id":"mill-1","base_revision":7,"defaults":)" +
      std::string(factory_defaults_json) + "}";
  const auto defaults = body<SetToolModelDefaults>(defaults_wire);
  auto expected = factory_tool_defaults();
  expected.at(static_cast<std::size_t>(ToolFamily::generic)).stickout = 35.25;
  require(defaults.machine_id == "mill-1" && defaults.base_revision == 7 &&
              defaults.defaults == expected,
          "Legacy defaults preserve all seven cutters and get the factory probe template");
  const std::string assignment_wire =
      R"({"id":42,"type":"set_tool_model_assignment","machine_id":"mill-1","base_revision":0,)"
      R"("tool":{"number":3,"pocket":4,"diameter":6.35,"comment":"Fräser \"6\""},)"
      R"("family":"spot-drill","overrides":{"stickout":30,"tipAngle":120}})";
  const auto assignment = body<SetToolModelAssignment>(assignment_wire);
  require(assignment.machine_id == "mill-1" && assignment.base_revision == 0 &&
              assignment.tool == ToolBinding{3, 4, 6.35, "Fräser \"6\""} &&
              assignment.family == ToolFamily::spot_drill && assignment.overrides.stickout == 30 &&
              assignment.overrides.tip_angle == 120 && !assignment.overrides.cutting_length &&
              !assignment.overrides.shank_diameter && !assignment.overrides.overall_length,
          "Assignment: the tool row, the family and the overrides given");
  const auto with_holder = body<SetToolModelAssignment>(with(
      assignment_wire, R"("family":"spot-drill")", R"("family":"spot-drill","holder":"tts-er20")"));
  require(with_holder.holder == "tts-er20" && tool_model_command_error(with_holder).empty(),
          "Assignment accepts a supported holder");
  const auto bad_holder = decode_request(with(assignment_wire, R"("family":"spot-drill")",
                                              R"("family":"spot-drill","holder":"unknown")"));
  require(std::holds_alternative<DecodeError>(bad_holder), "Assignment refuses an unknown holder");
  const auto all = body<SetToolModelAssignment>(
      with(assignment_wire, R"({"stickout":30,"tipAngle":120})",
           R"({"cuttingLength":1,"shankDiameter":2,"overallLength":3,"stickout":4,"tipAngle":5})"));
  require(all.overrides.cutting_length == 1 && all.overrides.shank_diameter == 2 &&
              all.overrides.overall_length == 3 && all.overrides.stickout == 4 &&
              all.overrides.tip_angle == 5,
          "All five overrides");
  const auto removal = body<SetToolModelAssignment>(
      R"({"id":42,"type":"set_tool_model_assignment","machine_id":"mill-1","base_revision":9007199254740991,)"
      R"("tool":{"number":2147483647,"pocket":0,"diameter":0,"comment":""},"family":null,"overrides":{}})");
  require(!removal.family && removal.overrides.empty() && removal.tool.number == 2147483647 &&
              removal.base_revision == 9007199254740991 &&
              tool_model_command_error(removal).empty(),
          "family null with empty overrides removes; the largest revision and tool number");
  const auto contradictory = body<SetToolModelAssignment>(
      with(assignment_wire, R"("family":"spot-drill")", R"("family":null)"));
  require(!tool_model_command_error(contradictory).empty(),
          "family null with overrides is decoded and then refused as a command");
  // Decoding judges the shape; the ranges are the command's rules.
  const auto out_of_range =
      body<SetToolModelAssignment>(with(assignment_wire, R"("stickout":30)", R"("stickout":0)"));
  require(!tool_model_command_error(out_of_range).empty() &&
              !tool_model_command_error(body<SetToolModelAssignment>(
                   with(assignment_wire, R"("number":3)", R"("number":0)"))).empty() &&
              !tool_model_command_error(body<SetToolModelDefaults>(
                   with(defaults_wire, R"("tipAngle":118)", R"("tipAngle":180)"))).empty(),
          "Values out of range decode and are refused as commands");

  for (const auto& wire :
       {with(defaults_wire, R"("base_revision":7)", R"("base_revision":7,"extra":1)"),
        with(defaults_wire, R"("machine_id":"mill-1",)", ""),
        with(defaults_wire, R"("base_revision":7,)", ""),
        with(defaults_wire, R"("machine_id":"mill-1")", R"("machine_id":7)"),
        with(defaults_wire, R"("machine_id":"mill-1")", R"("machine_id":null)"),
        with(defaults_wire, R"("machine_id":"mill-1")", R"("machine_id":"mill\u0000-1")"),
        with(defaults_wire, R"("base_revision":7)", R"("base_revision":-1)"),
        with(defaults_wire, R"("base_revision":7)", R"("base_revision":1.5)"),
        with(defaults_wire, R"("base_revision":7)", R"("base_revision":"7")"),
        with(defaults_wire, R"("base_revision":7)", R"("base_revision":null)"),
        with(defaults_wire, R"("base_revision":7)", R"("base_revision":9007199254740992)"),
        with(defaults_wire, R"("drill":)", R"("lathe":)"),
        with(defaults_wire, R"("generic":)", R"("Generic":)"),
        with(defaults_wire, R"({"end-mill":)", R"({"lathe":{},"end-mill":)"),
        with(defaults_wire, R"("tipAngle":118)", R"("tipAngle":118,"flutes":2)"),
        with(defaults_wire, R"(,"tipAngle":118)", ""),
        with(defaults_wire, R"("tipAngle":118)", R"("tipAngle":"118")"),
        with(defaults_wire, R"("tipAngle":118)", R"("tipAngle":null)"),
        with(defaults_wire, R"("tipAngle":118)", R"("tip_angle":118)"),
        with(defaults_wire, std::string(factory_defaults_json), "[]"),
        with(defaults_wire, std::string(factory_defaults_json), "null"),
        with(assignment_wire, R"("base_revision":0,)", R"("base_revision":0,"identity":"x",)"),
        with(assignment_wire, R"(,"overrides":{"stickout":30,"tipAngle":120})", ""),
        with(assignment_wire, R"("family":"spot-drill",)", ""),
        with(assignment_wire, R"("family":"spot-drill")", R"("family":"lathe")"),
        with(assignment_wire, R"("family":"spot-drill")", R"("family":"Spot-Drill")"),
        with(assignment_wire, R"("family":"spot-drill")", R"("family":3)"),
        with(assignment_wire, R"("stickout":30)", R"("diameter":6)"),
        with(assignment_wire, R"("stickout":30)", R"("flutes":2)"),
        with(assignment_wire, R"("stickout":30)", R"("stickout":"30")"),
        with(assignment_wire, R"("stickout":30)", R"("stickout":null)"),
        with(assignment_wire, R"({"stickout":30,"tipAngle":120})", "[]"),
        with(assignment_wire, R"({"stickout":30,"tipAngle":120})", "null"),
        with(assignment_wire, R"("pocket":4,)", R"("pocket":4,"length":50,)"),
        with(assignment_wire, R"("pocket":4,)", ""),
        with(assignment_wire, R"("number":3)", R"("number":1.5)"),
        with(assignment_wire, R"("number":3)", R"("number":"3")"),
        with(assignment_wire, R"("number":3)", R"("number":2147483648)"),
        with(assignment_wire, R"("pocket":4)", R"("pocket":-2147483649)"),
        with(assignment_wire, R"("pocket":4)", R"("pocket":0.5)"),
        with(assignment_wire, R"("diameter":6.35)", R"("diameter":"6.35")"),
        with(assignment_wire, R"("diameter":6.35)", R"("diameter":null)"),
        with(assignment_wire, R"("comment":"Fräser \"6\"")", R"("comment":6)"),
        with(assignment_wire, R"("comment":"Fräser \"6\"")", R"("comment":null)")})
    rejected(wire, 42);
  rejected(with(assignment_wire, R"("diameter":6.35)", R"("diameter":1e999)"), std::nullopt);
  rejected(with(assignment_wire, R"("stickout":30)", R"("stickout":30,"stickout":31)"),
           std::nullopt);

  // The request bounds hold for these as for every command.
  auto padded = defaults_wire;
  require(padded.size() < 2000, "A defaults command is far below the limit");
  padded.resize(max_request_bytes, ' ');
  body<SetToolModelDefaults>(padded);
  padded.push_back(' ');
  require(rejected(padded, std::nullopt).find("16 KiB") != std::string::npos,
          "A defaults command above 16 KiB is refused");
  const auto long_comment = [&](std::size_t bytes) -> std::string {
    return with(assignment_wire, R"(Fräser \"6\")", std::string(bytes, 'x'));
  };
  require(body<SetToolModelAssignment>(long_comment(255)).tool.comment.size() == 255,
          "The longest valid description fits a request");
  const auto filling = max_request_bytes - long_comment(0).size();
  require(body<SetToolModelAssignment>(long_comment(filling)).tool.comment.size() == filling &&
              !tool_model_command_error(body<SetToolModelAssignment>(long_comment(filling)))
                   .empty(),
          "A request of exactly 16 KiB decodes; its description is then too long");
  rejected(long_comment(filling + 1), std::nullopt);
  const auto deep = with(assignment_wire, R"("stickout":30)",
                         R"("stickout":)" + std::string(7, '[') + "1" + std::string(7, ']'));
  require(rejected(deep, std::nullopt).find("nesting") != std::string::npos,
          "The nesting limit of 8 holds inside a tool-model command");
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
  ControllerState running;
  running.link = Link::live;
  running.machine = MachineState::on;
  running.interp = InterpState::reading;
  running.in_position = false;
  for (const auto& state : {disconnected, running})
    require(policy.evaluate(SetToolModelDefaults{}, state).allowed &&
                policy.evaluate(SetToolModelAssignment{}, state).allowed &&
                !policy.evaluate(SetToolModelDefaults{}, state).required_mode,
            "Tool-model commands are permitted whatever the machine and the link do");
}
} // namespace

auto main() -> int {
  try {
    protocol_forms();
    malformed_and_ambiguous();
    strict_shapes();
    identity_and_limits();
    tool_model_commands();
    policy_still_required();
    std::cout << checks << " request boundary checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
