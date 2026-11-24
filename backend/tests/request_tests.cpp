#include <coffeecnc/app/request.hpp>
#include <coffeecnc/app/tool_models.hpp>

#include <format>
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
  const auto rotation = body<SetWorkRotation>(
      R"({"id":42,"type":"set_work_rotation","wcs":3,"rotation_xy":-12.5})");
  require(rotation.wcs == 3 && rotation.degrees == -12.5, "Explicit system and XY rotation in degrees");
  static_cast<void>(body<ClearG92>(R"({"id":42,"type":"clear_g92"})"));
  // HAL diagnostics (#251): the view opens and closes; a HAL tool by its program name.
  require(body<WatchDiagnostics>(R"({"id":42,"type":"watch_diagnostics","on":true})").on &&
              !body<WatchDiagnostics>(R"({"id":42,"type":"watch_diagnostics","on":false})").on,
          "Diagnostics watch on and off");
  require(body<LaunchHalTool>(R"({"id":42,"type":"launch_hal_tool","tool":"halscope"})").tool ==
                  HalTool::halscope &&
              body<LaunchHalTool>(R"({"id":42,"type":"launch_hal_tool","tool":"halmeter"})")
                      .tool == HalTool::halmeter &&
              body<LaunchHalTool>(R"({"id":42,"type":"launch_hal_tool","tool":"halshow"})")
                      .tool == HalTool::halshow,
          "Each HAL tool by name");
  rejected(R"({"id":42,"type":"launch_hal_tool","tool":"halcmd"})", 42);
  rejected(R"({"id":42,"type":"launch_hal_tool","tool":"/usr/bin/halscope"})", 42);
  rejected(R"({"id":42,"type":"launch_hal_tool"})", 42);
  rejected(R"({"id":42,"type":"watch_diagnostics","on":1})", 42);
  rejected(R"({"id":42,"type":"watch_diagnostics","on":true,"rate":50})", 42);
  const auto touch = body<TouchOff>(R"({"id":42,"type":"touch_off","axis":"z","value":-2.5})");
  require(touch.axis == Axis::z && touch.value == -2.5 && touch.origin == TouchOrigin::typed,
          "Touch-off value");
  require(body<TouchOff>(R"({"id":42,"type":"touch_off","axis":"x","value":0,"origin":"zero"})")
                  .origin == TouchOrigin::zero &&
              body<TouchOff>(R"({"id":42,"type":"touch_off","axis":"x","value":5,"origin":"half"})")
                      .origin == TouchOrigin::half,
          "A quick action names itself for the offset history");
  rejected(R"({"id":42,"type":"touch_off","axis":"x","value":0,"origin":"typed"})", 42);
  require(body<RestoreOffset>(R"({"id":42,"type":"restore_offset","entry":7})").entry == 7,
          "A restore names its history entry");
  rejected(R"({"id":42,"type":"restore_offset","entry":0})", 42);
  rejected(R"({"id":42,"type":"restore_offset","entry":7,"wcs":1})", 42);
  require(body<ToolTouchOff>(R"({"id":42,"type":"tool_touch_off","value":25.4})").value == 25.4,
          "Tool touch-off millimetres");
  const auto inside = body<FixedSetterRequest>(
      R"({"id":42,"type":"fixed_setter","action":"inside_corner","tool":99,"x":1,"y":2,"z":3,"x_direction":"x_minus","y_direction":"y_plus"})");
  require(inside.action == SetterAction::inside_corner && inside.direction == ProbeDirection::x_minus &&
              inside.y_direction == ProbeDirection::y_plus && inside.start == std::array{1.0, 2.0, 3.0},
          "Inside corners specify the travel towards each wall");
  const auto ridge = body<FixedSetterRequest>(
      R"({"id":42,"type":"fixed_setter","action":"ridge","tool":99,"x":1,"y":2,"z":3,"axis":"y","width":25.4})");
  require(ridge.action == SetterAction::ridge && ridge.direction == ProbeDirection::y_plus &&
              ridge.gauge_width_mm == 25.4,
          "Ridge requests carry the axis and approximate width in millimetres");
  const auto runout = body<FixedSetterRequest>(
      R"({"id":42,"type":"fixed_setter","action":"runout","tool":99,"x":1,"y":2,"z":3,"diameter":20,"outside":false})");
  require(runout.action == SetterAction::runout && runout.gauge_width_mm == 20 && !runout.outside,
          "A runout calibration names the ring diameter and its side");
  rejected(R"({"id":42,"type":"fixed_setter","action":"runout","tool":99,"x":1,"y":2,"z":3,"diameter":20})", 42);
  // The centre is always found: no eccentricity is kept, so there is no centred mode (#247).
  rejected(R"({"id":42,"type":"fixed_setter","action":"runout","tool":99,"x":1,"y":2,"z":3,"diameter":20,"outside":false,"centred":true})", 42);
  require(body<FixedSetterRequest>(R"({"id":42,"type":"fixed_setter","action":"clear_runout","tool":99})").action ==
              SetterAction::clear_runout,
          "Clearing the runout names only the probe");
  for (const auto* action : {R"("action":"material","direction":"z_top")", R"("action":"pocket")",
                             R"("action":"bore")", R"("action":"boss","diameter":20)",
                             R"("action":"slot","axis":"x")",
                             R"("action":"corner","x_direction":"x_plus","y_direction":"y_plus")"}) {
    const auto measure = body<FixedSetterRequest>(std::format(
        R"({{"id":42,"type":"fixed_setter",{},"tool":99,"x":1,"y":2,"z":3,"measure_only":true}})",
        action));
    require(measure.measure_only, std::format("{} can probe without writing an offset", action));
  }
  require(!body<FixedSetterRequest>(
               R"({"id":42,"type":"fixed_setter","action":"bore","tool":99,"x":1,"y":2,"z":3})")
               .measure_only,
          "A probe writes its result unless measure_only is given");
  rejected(R"({"id":42,"type":"fixed_setter","action":"ball","tool":99,"x":1,"y":2,"z":3,"gauge_width":20,"measure_only":true})", 42);
  rejected(R"({"id":42,"type":"fixed_setter","action":"touch_off","tool":98,"x":1,"y":2,"z":3,"measure_only":true})", 42);
  rejected(R"({"id":42,"type":"fixed_setter","action":"bore","tool":99,"x":1,"y":2,"z":3,"measure_only":1})", 42);
  const auto apply = body<FixedSetterRequest>(
      R"({"id":42,"type":"fixed_setter","action":"apply_measurement","tool":99,"measurement_id":7})");
  require(apply.action == SetterAction::apply_measurement && apply.edge_measurement == 7 &&
              apply.tool == 99,
          "Applying a measurement names the controller's measurement ID");
  rejected(R"({"id":42,"type":"fixed_setter","action":"apply_measurement","tool":99,"measurement_id":0})", 42);
  rejected(R"({"id":42,"type":"fixed_setter","action":"slot","tool":99,"x":1,"y":2,"z":3,"axis":"z"})", 42);
  rejected(R"({"id":42,"type":"fixed_setter","action":"ridge","tool":99,"x":1,"y":2,"z":3,"axis":"x"})", 42);
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
       R"({"id":42,"type":"run","preview_context":7,"display_units":"mm","confirmed":true,"air_z":25})",
       R"({"id":42,"type":"set_air_cut","air_z":25})",
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
       R"({"id":42,"type":"set_work_rotation","wcs":1})",
       R"({"id":42,"type":"set_work_rotation","wcs":1,"rotation_xy":"15"})",
       R"({"id":42,"type":"set_work_rotation","wcs":1,"rotation_xy":null})",
       R"({"id":42,"type":"set_work_rotation","wcs":1.5,"rotation_xy":15})",
       R"({"id":42,"type":"set_work_rotation","wcs":1,"rotation_xy":15,"offset":[1,2,3]})",
       R"({"id":42,"type":"clear_g92","wcs":1})",
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
  for (const std::string_view holder : {"tts-er20", "er16", "er20", "er32", "bt30", "generic"}) {
    const auto with_holder = body<SetToolModelAssignment>(with(
        assignment_wire, R"("family":"spot-drill")",
        std::format(R"("family":"spot-drill","holder":"{}")", holder)));
    require(with_holder.holder == holder && tool_model_command_error(with_holder).empty(),
            "Assignment accepts a supported holder");
  }
  const auto bad_holder = decode_request(with(assignment_wire, R"("family":"spot-drill")",
                                              R"("family":"spot-drill","holder":"unknown")"));
  const auto* holder_error = std::get_if<DecodeError>(&bad_holder);
  require(holder_error && holder_error->id == 42 &&
              holder_error->reason == "Choose a supported holder for a tool model assignment." &&
              holder_error->localization == MessageKey{"backend.toolModels.chooseHolder"},
          "An invalid holder has a fallback and a catalog key");
  const auto wrong_diameter =
      decode_request(with(assignment_wire, R"("diameter":6.35)", R"("diameter":"6.35")"));
  const auto* binding_error = std::get_if<DecodeError>(&wrong_diameter);
  require(binding_error && binding_error->reason == "tool diameter must be a number." &&
              binding_error->localization ==
                  MessageKey{"backend.toolModels.fieldNumber",
                             {{"object", "tool"}, {"field", "diameter"}}},
          "A decoder failure keeps both structured field arguments");
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

void save_program_requests() {
  const auto save = body<SaveProgram>(
      R"({"id":42,"type":"save_program","path":"facing.ngc","text":"(Facing)\nG21\nM2\n"})");
  require(save.path == "facing.ngc" && save.text == "(Facing)\nG21\nM2\n" && !save.replace,
          "A program to save keeps its path and text; nothing is replaced unasked");
  require(body<SaveProgram>(R"({"id":42,"type":"save_program","path":"a.ngc","text":"M2","replace":true})")
              .replace,
          "Replacing is asked for explicitly");
  rejected(R"({"id":42,"type":"save_program","path":"a.ngc","text":"M2","replace":1})", 42);
  rejected(R"({"id":42,"type":"save_program","path":"a.ngc"})", 42);
  rejected(R"({"id":42,"type":"save_program","path":"a.ngc","text":"M2","open":true})", 42);
  const auto edited = body<SaveProgram>(R"({"id":42,"type":"save_program","path":"part.ngc",)"
                                        R"("text":"M2\n","stamp":"9f6b1c2e04d3a587"})");
  require(edited.stamp == "9f6b1c2e04d3a587" && !save.stamp,
          "The editor's save names the stamp it opened; a new program has none");
  rejected(R"({"id":42,"type":"save_program","path":"a.ngc","text":"M2","stamp":7})", 42);

  // Above 16 KiB only a program: one of 200 KiB decodes, anything else of that size does not.
  const auto wire = [](std::string_view type, std::size_t bytes) -> std::string {
    return std::format(R"({{"id":42,"type":"{}","path":"big.ngc","text":"{}"}})", type,
                       std::string(bytes, 'G'));
  };
  require(body<SaveProgram>(wire("save_program", std::size_t{200} * 1024)).text.size() ==
              std::size_t{200} * 1024,
          "A long program decodes");
  const auto reordered = R"({"text":")" + std::string(std::size_t{32} * 1024, 'G') +
                         R"(","path":"big.ngc","id":42,"type":"save_program"})";
  require(body<SaveProgram>(reordered).text.size() == std::size_t{32} * 1024,
          "A long program still decodes when its type follows the text");
  require(rejected(wire("mdi", std::size_t{200} * 1024), std::nullopt).find("16 KiB") !=
              std::string::npos,
          "Another long command is refused for its size");
  require(rejected(wire("save_program", max_saved_program_bytes + 1), 42).find("512 KiB") !=
              std::string::npos,
          "A program above 512 KiB is refused");
  require(rejected(wire("save_program", max_save_program_request_bytes), std::nullopt)
                  .find("16 KiB") != std::string::npos,
          "A request above the program limit is not parsed");

  CommandPolicy policy{Limits{3000, 1.2, 0.5, 1.2, 24000}};
  const ControllerState disconnected;
  require(policy.evaluate(save, disconnected).allowed,
          "Saving a new file needs no connection: nothing of the machine changes");
  require(!policy.evaluate(SaveProgram{"", "M2", false}, disconnected).allowed &&
              !policy.evaluate(SaveProgram{"a.ngc", "", false}, disconnected).allowed &&
              !policy.evaluate(SaveProgram{"a\nb.ngc", "M2", false}, disconnected).allowed,
          "An empty path or text, or a path with a line end, is refused");

  // Over the loaded program it is an edit of it: only on a live, idle machine.
  require(!policy.evaluate(edited, disconnected).allowed,
          "Saving over the loaded program needs live machine state");
  ControllerState idle;
  idle.link = Link::live;
  idle.machine = MachineState::on;
  idle.program_loaded = true;
  require(policy.evaluate(edited, idle).allowed, "An idle machine with a program allows the save");
  auto running = idle;
  running.interp = InterpState::reading;
  const auto refused = policy.evaluate(edited, running);
  require(!refused.allowed && refused.reason == "Stop the program before saving it.",
          "Refused while the program runs");
  running.interp = InterpState::paused;
  require(!policy.evaluate(edited, running).allowed, "Refused while the program is paused");
  require(policy.evaluate(save, running).allowed,
          "A new program can still be written while one runs: nothing of it is loaded");
  auto unloaded = idle;
  unloaded.program_loaded = false;
  require(!policy.evaluate(edited, unloaded).allowed, "Refused without a loaded program");
  auto changing = idle;
  changing.requested_tool = 3;
  require(!policy.evaluate(edited, changing).allowed, "Refused while a tool change waits");
  auto bad_stamp = edited;
  bad_stamp.stamp = "";
  require(!policy.evaluate(bad_stamp, idle).allowed, "An empty stamp is refused");
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
    save_program_requests();
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
