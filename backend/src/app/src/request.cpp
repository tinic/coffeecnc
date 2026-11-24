#include <coffeecnc/app/request.hpp>

#include "tool_model_json.hpp"
#include <coffeecnc/app/operator_settings.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <expected>
#include <format>
#include <initializer_list>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace coffeecnc {
namespace {
using Json = nlohmann::json;

class InvalidRequest : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
  explicit InvalidRequest(LocalizedReason reason)
      : std::runtime_error(reason.text), localization(std::move(reason.localization)) {}
  std::optional<MessageKey> localization{};
};

auto parse(std::string_view text) -> Json {
  std::vector<std::set<std::string>> object_keys;
  const auto callback = [&object_keys](int depth, Json::parse_event_t event, Json& value) -> bool {
    using Event = Json::parse_event_t;
    if ((event == Event::object_start || event == Event::array_start) && depth >= max_request_depth)
      throw InvalidRequest(LocalizedReason{"Request nesting limit exceeded.",
                                           MessageKey{"messages.request.nestingLimit"}});
    if (event == Event::object_start)
      object_keys.emplace_back();
    else if (event == Event::object_end)
      object_keys.pop_back();
    else if (event == Event::key && !object_keys.back().insert(value.get<std::string>()).second)
      throw InvalidRequest(LocalizedReason{"Duplicate object key in request.",
                                           MessageKey{"messages.request.duplicateKey"}});
    return true; // Never silently discard an invalid field.
  };
  return Json::parse(text.begin(), text.end(), callback);
}

auto field(const Json& object, std::string_view name) -> const Json& {
  if (!object.is_object())
    throw InvalidRequest(
        LocalizedReason{"Expected a JSON object.", MessageKey{"messages.request.expectedObject"}});
  const auto it = object.find(name);
  if (it == object.end())
    throw InvalidRequest(LocalizedReason{
        std::format("Missing required field: {}.", name),
        MessageKey{"messages.request.missingField", {{"field", std::string(name)}}}});
  return *it;
}

void fields(const Json& object, std::initializer_list<std::string_view> names,
            std::initializer_list<std::string_view> optional = {}) {
  if (!object.is_object())
    throw InvalidRequest(
        LocalizedReason{"Expected a JSON object.", MessageKey{"messages.request.expectedObject"}});
  for (const auto name : names)
    static_cast<void>(field(object, name));
  const auto present = std::ranges::count_if(optional, [&](auto name) { return object.contains(name); });
  const auto expected = names.size() + static_cast<std::size_t>(present);
  if (object.size() != expected)
    throw InvalidRequest(LocalizedReason{"Unexpected field in request.",
                                         MessageKey{"messages.request.unexpectedField"}});
}

auto string(const Json& object, std::string_view name) -> std::string {
  const auto& value = field(object, name);
  if (!value.is_string())
    throw InvalidRequest(LocalizedReason{
        std::format("Expected a string: {}.", name),
        MessageKey{"messages.request.expectedString", {{"field", std::string(name)}}}});
  auto result = value.get<std::string>();
  if (result.find('\0') != std::string::npos)
    throw InvalidRequest(LocalizedReason{"NUL is not allowed in request strings.",
                                         MessageKey{"messages.request.nulInString"}});
  return result;
}

auto number(const Json& object, std::string_view name) -> double {
  const auto& value = field(object, name);
  if (!value.is_number())
    throw InvalidRequest(LocalizedReason{
        std::format("Expected a number: {}.", name),
        MessageKey{"messages.request.expectedNumber", {{"field", std::string(name)}}}});
  const auto result = value.get<double>();
  if (!std::isfinite(result))
    throw InvalidRequest(LocalizedReason{
        std::format("Expected a finite number: {}.", name),
        MessageKey{"messages.request.expectedFinite", {{"field", std::string(name)}}}});
  return result;
}

auto integer(const Json& object, std::string_view name) -> int {
  const auto value = number(object, name);
  if (std::trunc(value) != value || value < std::numeric_limits<int>::min() ||
      value > std::numeric_limits<int>::max())
    throw InvalidRequest(LocalizedReason{
        std::format("Expected an in-range integer: {}.", name),
        MessageKey{"messages.request.expectedInteger", {{"field", std::string(name)}}}});
  return static_cast<int>(value);
}

auto request_id(const Json& object) -> RequestId {
  const auto value = number(object, "id");
  if (std::trunc(value) != value || value < -static_cast<double>(max_request_id) ||
      value > static_cast<double>(max_request_id))
    throw InvalidRequest(LocalizedReason{"Request ID must be a JavaScript safe integer.",
                                         MessageKey{"messages.request.requestIdInvalid"}});
  return static_cast<RequestId>(value);
}

auto positive_id(const Json& object, std::string_view name) -> std::uint64_t {
  const auto value = number(object, name);
  if (std::trunc(value) != value || value < 1 || value > static_cast<double>(max_request_id))
    throw InvalidRequest(LocalizedReason{
        std::format("Expected a positive JavaScript safe integer: {}.", name),
        MessageKey{"messages.request.expectedPositiveId", {{"field", std::string(name)}}}});
  return static_cast<std::uint64_t>(value);
}

auto boolean(const Json& object, std::string_view name) -> bool {
  const auto& value = field(object, name);
  if (!value.is_boolean())
    throw InvalidRequest(LocalizedReason{
        std::format("Expected a boolean: {}.", name),
        MessageKey{"messages.request.expectedBoolean", {{"field", std::string(name)}}}});
  return value.get<bool>();
}

template <typename T>
auto choice(const Json& object, std::string_view name,
            std::initializer_list<std::pair<std::string_view, T>> choices) -> T {
  const auto value = string(object, name);
  for (const auto& [label, result] : choices)
    if (value == label)
      return result;
  throw InvalidRequest(LocalizedReason{
      std::format("Unsupported value for field: {}.", name),
      MessageKey{"messages.request.unsupportedValue", {{"field", std::string(name)}}}});
}

// A value read by the forms the saved file shares with these commands.
template <typename T, typename Error> auto valid(std::expected<T, Error> read) -> T {
  if (!read)
    throw InvalidRequest(std::move(read.error()));
  return std::move(*read);
}

auto axis(const Json& object) -> Axis {
  return choice<Axis>(object, "axis", {{"x", Axis::x}, {"y", Axis::y}, {"z", Axis::z}});
}

auto command(const Json& object) -> Command {
  const auto type = string(object, "type");
  // Commands with no body still reject stray fields (for example an unsupported run-from-line).
  if (type == "ready" || type == "jog_stop_all" || type == "pause" || type == "resume" ||
      type == "step" || type == "abort" || type == "quit") {
    fields(object, {"id", "type"});
    if (type == "ready")
      return Ready{};
    if (type == "jog_stop_all")
      return JogStopAll{};
    if (type == "quit")
      return Quit{};
    if (type == "pause")
      return Pause{};
    if (type == "resume")
      return Resume{};
    if (type == "step")
      return Step{};
    return Abort{};
  }
  if (type == "tool_changed") {
    fields(object, {"id", "type"}, {"measure_request"});
    if (object.contains("measure_request"))
      return ToolChanged{positive_id(object, "measure_request")};
    return ToolChanged{};
  }
  if (type == "set_machine") {
    fields(object, {"id", "type", "state"});
    return SetMachine{choice<MachineState>(
        object, "state",
        {{"estop", MachineState::estop}, {"off", MachineState::off}, {"on", MachineState::on}})};
  }
  if (type == "home") {
    fields(object, {"id", "type", "axis"});
    if (string(object, "axis") == "all")
      return Home{std::nullopt};
    return Home{axis(object)};
  }
  if (type == "jog") {
    fields(object, {"id", "type", "axis", "direction", "velocity", "held"});
    const auto direction = integer(object, "direction");
    if (direction != -1 && direction != 1)
      throw InvalidRequest(LocalizedReason{"Jog direction must be -1 or 1.",
                                           MessageKey{"messages.request.jogDirection"}});
    return Jog{axis(object), direction, number(object, "velocity"), boolean(object, "held")};
  }
  if (type == "jog_paused_z") {
    fields(object, {"id", "type", "distance"}, {"stop_spindle"});
    return JogPausedZ{number(object, "distance"),
                      object.contains("stop_spindle") && boolean(object, "stop_spindle")};
  }
  if (type == "jog_step") {
    fields(object, {"id", "type", "axis", "distance", "velocity"});
    return JogStep{axis(object), number(object, "distance"), number(object, "velocity")};
  }
  if (type == "mdi") {
    fields(object, {"id", "type", "text"});
    return Mdi{string(object, "text")};
  }
  if (type == "list_directory") {
    fields(object, {"id", "type", "path"});
    if (field(object, "path").is_null())
      return ListDirectory{std::nullopt};
    return ListDirectory{string(object, "path")};
  }
  if (type == "backup") {
    fields(object, {"id", "type", "directory"});
    return Backup{string(object, "directory")};
  }
  if (type == "check_restore") {
    fields(object, {"id", "type", "path"});
    return CheckRestore{string(object, "path")};
  }
  if (type == "restore") {
    fields(object, {"id", "type", "path", "token"});
    return RestoreBackup{string(object, "path"), string(object, "token")};
  }
  if (type == "manage_program_file") {
    fields(object, {"id", "type", "operation", "path"}, {"name"});
    const auto operation = choice<ProgramFileOperation>(
        object, "operation", {{"create_folder", ProgramFileOperation::create_folder},
                              {"copy", ProgramFileOperation::copy},
                              {"rename", ProgramFileOperation::rename},
                              {"remove", ProgramFileOperation::remove}});
    if (object.contains("name") != (operation == ProgramFileOperation::rename))
      throw InvalidRequest(LocalizedReason{"A new file name is required for rename only.",
                                           MessageKey{"backend.programFiles.nameForRename"}});
    return ManageProgramFile{operation, string(object, "path"),
                             object.contains("name") ? string(object, "name") : std::string{}};
  }
  if (type == "open_program") {
    fields(object, {"id", "type", "path"});
    return OpenProgram{string(object, "path")};
  }
  if (type == "run") {
    fields(object, {"id", "type"},
           {"preview_context", "display_units", "confirmed", "breakpoint"});
    Run run;
    if (object.contains("preview_context"))
      run.preview_context = positive_id(object, "preview_context");
    if (object.contains("display_units"))
      run.display_units = choice<LengthUnit>(object, "display_units",
                                             {{"mm", LengthUnit::mm}, {"inch", LengthUnit::inch}});
    if (object.contains("confirmed"))
      run.confirmed = boolean(object, "confirmed");
    if (object.contains("breakpoint")) {
      const auto& breakpoint = field(object, "breakpoint");
      fields(breakpoint, {"line", "text"});
      const auto line = integer(breakpoint, "line");
      if (line < 1)
        throw InvalidRequest(LocalizedReason{"A breakpoint requires a positive program line.",
                                             MessageKey{"messages.request.breakpointLine"}});
      run.breakpoint = BreakpointRequest{line, string(breakpoint, "text")};
    }
    if ((run.confirmed || run.breakpoint) &&
        (!run.preview_context || !run.display_units))
      throw InvalidRequest(
          LocalizedReason{"Confirmation needs the reviewed preview and display units.",
                          MessageKey{"messages.request.confirmationNeeds"}});
    return run;
  }
  if (type == "plan_restart") {
    fields(object, {"id", "type", "line", "preview_context"});
    const int line = integer(object, "line");
    if (line < 1)
      throw InvalidRequest(LocalizedReason{"Restart requires a positive program line.",
                                           MessageKey{"messages.request.restartLine"}});
    return PlanRestart{line, positive_id(object, "preview_context")};
  }
  if (type == "edit_program") {
    fields(object, {"id", "type", "path", "line", "old_text", "new_text"});
    const int line = integer(object, "line");
    if (line < 1)
      throw InvalidRequest(LocalizedReason{"A program edit requires a positive line.",
                                           MessageKey{"messages.request.editLine"}});
    return EditProgram{string(object, "path"), line, string(object, "old_text"),
                       string(object, "new_text")};
  }
  if (type == "save_program") {
    fields(object, {"id", "type", "path", "text"}, {"replace", "stamp"});
    SaveProgram save{string(object, "path"), string(object, "text"), false};
    if (save.text.size() > max_saved_program_bytes)
      throw InvalidRequest(LocalizedReason{"The program text is longer than 512 KiB.",
                                           MessageKey{"messages.request.programTooLong"}});
    if (object.contains("replace"))
      save.replace = boolean(object, "replace");
    if (object.contains("stamp"))
      save.stamp = string(object, "stamp");
    return save;
  }
  if (type == "start_restart") {
    fields(object, {"id", "type", "plan_id"});
    return StartRestart{positive_id(object, "plan_id")};
  }
  if (type == "set_override") {
    fields(object, {"id", "type", "which", "value"});
    return SetOverride{
        choice<Override>(
            object, "which",
            {{"feed", Override::feed},
             {"rapid", Override::rapid},
             {"spindle", Override::spindle},
             {"velocity_limit", Override::velocity_limit}}),
        number(object, "value")};
  }
  if (type == "set_spindle") {
    fields(object, {"id", "type", "direction", "rpm"});
    return SetSpindle{choice<SpindleDirection>(object, "direction",
                                               {{"stopped", SpindleDirection::stopped},
                                                {"forward", SpindleDirection::forward},
                                                {"reverse", SpindleDirection::reverse}}),
                      number(object, "rpm")};
  }
  if (type == "set_coolant") {
    fields(object, {"id", "type", "flood", "mist"});
    return SetCoolant{boolean(object, "flood"), boolean(object, "mist")};
  }
  if (type == "set_program_option") {
    fields(object, {"id", "type", "option", "on"});
    return SetProgramOption{choice<ProgramOption>(object, "option",
                                                  {{"optional_stop", ProgramOption::optional_stop},
                                                   {"block_delete", ProgramOption::block_delete}}),
                            boolean(object, "on")};
  }
  if (type == "select_wcs") {
    fields(object, {"id", "type", "wcs"});
    return SelectWcs{integer(object, "wcs")};
  }
  if (type == "touch_off") {
    fields(object, {"id", "type", "axis", "value"}, {"origin"});
    TouchOff touch{axis(object), number(object, "value")};
    if (object.contains("origin"))
      touch.origin = choice<TouchOrigin>(object, "origin",
                                         {{"zero", TouchOrigin::zero}, {"half", TouchOrigin::half}});
    return touch;
  }
  if (type == "restore_offset") {
    fields(object, {"id", "type", "entry"});
    return RestoreOffset{positive_id(object, "entry")};
  }
  if (type == "set_work_offset") {
    fields(object, {"id", "type", "wcs", "offset"});
    const auto& offset = field(object, "offset");
    if (!offset.is_array() || offset.size() != 3)
      throw InvalidRequest(LocalizedReason{"Work offset must contain three XYZ numbers.",
                                           MessageKey{"messages.request.workOffsetXyz"}});
    std::array<double, 3> values{};
    for (std::size_t i = 0; i < values.size(); ++i) {
      if (!offset.at(i).is_number())
        throw InvalidRequest(LocalizedReason{"Work offset must contain three XYZ numbers.",
                                             MessageKey{"messages.request.workOffsetXyz"}});
      values[i] = offset.at(i).get<double>();
      if (!std::isfinite(values[i]))
        throw InvalidRequest(LocalizedReason{"Work offsets must be finite.",
                                             MessageKey{"messages.request.workOffsetsFinite"}});
    }
    return SetWorkOffset{integer(object, "wcs"), values};
  }
  if (type == "set_work_rotation") {
    fields(object, {"id", "type", "wcs", "rotation_xy"});
    return SetWorkRotation{integer(object, "wcs"), number(object, "rotation_xy")};
  }
  if (type == "clear_g92") {
    fields(object, {"id", "type"});
    return ClearG92{};
  }
  if (type == "watch_diagnostics") {
    fields(object, {"id", "type", "on"});
    return WatchDiagnostics{boolean(object, "on")};
  }
  if (type == "launch_hal_tool") {
    fields(object, {"id", "type", "tool"});
    return LaunchHalTool{choice<HalTool>(object, "tool",
                                         {{hal_tool_name(HalTool::halshow), HalTool::halshow},
                                          {hal_tool_name(HalTool::halmeter), HalTool::halmeter},
                                          {hal_tool_name(HalTool::halscope), HalTool::halscope}})};
  }
  if (type == "tool_touch_off") {
    fields(object, {"id", "type", "value"});
    return ToolTouchOff{number(object, "value")};
  }
  if (type == "fixed_setter") {
    const auto action = choice<SetterAction>(object, "action",
                                             {{"status", SetterAction::status},
                                              {"touch_off", SetterAction::touch_off},
                                              {"check", SetterAction::check},
                                              {"material", SetterAction::material},
                                              {"corner", SetterAction::corner},
                                              {"inside_corner", SetterAction::inside_corner},
                                              {"pocket", SetterAction::pocket},
                                              {"slot", SetterAction::slot},
                                              {"ridge", SetterAction::ridge},
                                              {"ball", SetterAction::ball},
                                              {"bore", SetterAction::bore},
                                              {"boss", SetterAction::boss},
                                              {"bore_arc", SetterAction::bore_arc},
                                              {"boss_arc", SetterAction::boss_arc},
                                              {"angle", SetterAction::angle},
                                              {"apply_angle", SetterAction::apply_angle},
                                              {"apply_measurement", SetterAction::apply_measurement}});
    FixedSetterRequest request{action};
    if (action == SetterAction::apply_angle || action == SetterAction::apply_measurement) {
      fields(object, {"id", "type", "action", "tool", "measurement_id"});
      request.tool = integer(object, "tool");
      request.edge_measurement = positive_id(object, "measurement_id");
      return request;
    }
    if (action != SetterAction::status) {
      if (action == SetterAction::material) {
        fields(object, {"id", "type", "action", "tool", "x", "y", "z", "direction"}, {"measure_only"});
        request.direction = choice<ProbeDirection>(object, "direction",
                                                   {{"z_top", ProbeDirection::z_top},
                                                    {"x_minus", ProbeDirection::x_minus},
                                                    {"x_plus", ProbeDirection::x_plus},
                                                    {"y_minus", ProbeDirection::y_minus},
                                                    {"y_plus", ProbeDirection::y_plus}});
      } else if (action == SetterAction::ball) {
        fields(object, {"id", "type", "action", "tool", "x", "y", "z", "gauge_width"});
        request.gauge_width_mm = number(object, "gauge_width");
      } else if (action == SetterAction::angle) {
        fields(object, {"id", "type", "action", "tool", "x", "y", "z", "direction", "spacing"},
               {"measure_only"});
        request.direction = choice<ProbeDirection>(object, "direction",
                                                   {{"x_minus", ProbeDirection::x_minus},
                                                    {"x_plus", ProbeDirection::x_plus},
                                                    {"y_minus", ProbeDirection::y_minus},
                                                    {"y_plus", ProbeDirection::y_plus}});
        request.gauge_width_mm = number(object, "spacing");
      } else if (action == SetterAction::boss) {
        fields(object, {"id", "type", "action", "tool", "x", "y", "z", "diameter"}, {"measure_only"});
        request.gauge_width_mm = number(object, "diameter");
      } else if (action == SetterAction::bore_arc || action == SetterAction::boss_arc) {
        if (action == SetterAction::boss_arc) {
          fields(object, {"id", "type", "action", "tool", "x", "y", "z", "angles", "diameter"},
                 {"measure_only"});
          request.gauge_width_mm = number(object, "diameter");
        } else {
          fields(object, {"id", "type", "action", "tool", "x", "y", "z", "angles"}, {"measure_only"});
        }
        const auto& angles = field(object, "angles");
        if (!angles.is_array() || angles.size() != request.angles_deg.size() ||
            !std::ranges::all_of(angles, [](const Json& angle) { return angle.is_number(); }))
          throw InvalidRequest(LocalizedReason{"Expected three probe angles in degrees.",
                                               MessageKey{"messages.request.expectedAngles"}});
        for (std::size_t i = 0; i < request.angles_deg.size(); ++i)
          request.angles_deg.at(i) = angles.at(i).get<double>();
      } else if (action == SetterAction::slot || action == SetterAction::ridge) {
        if (action == SetterAction::ridge) {
          fields(object, {"id", "type", "action", "tool", "x", "y", "z", "axis", "width"},
                 {"measure_only"});
          request.gauge_width_mm = number(object, "width");
        } else {
          fields(object, {"id", "type", "action", "tool", "x", "y", "z", "axis"}, {"measure_only"});
        }
        request.direction = choice<ProbeDirection>(
            object, "axis", {{"x", ProbeDirection::x_plus}, {"y", ProbeDirection::y_plus}});
      } else if (action == SetterAction::corner || action == SetterAction::inside_corner) {
        fields(object, {"id", "type", "action", "tool", "x", "y", "z", "x_direction", "y_direction"},
               {"measure_only"});
        request.direction = choice<ProbeDirection>(
            object, "x_direction", {{"x_minus", ProbeDirection::x_minus}, {"x_plus", ProbeDirection::x_plus}});
        request.y_direction = choice<ProbeDirection>(
            object, "y_direction", {{"y_minus", ProbeDirection::y_minus}, {"y_plus", ProbeDirection::y_plus}});
      } else if (action == SetterAction::pocket || action == SetterAction::bore) {
        fields(object, {"id", "type", "action", "tool", "x", "y", "z"}, {"measure_only"});
      } else {
        fields(object, {"id", "type", "action", "tool", "x", "y", "z"});
      }
      if (object.contains("measure_only"))
        request.measure_only = boolean(object, "measure_only");
      request.tool = integer(object, "tool");
      request.start = {number(object, "x"), number(object, "y"), number(object, "z")};
    } else {
      fields(object, {"id", "type", "action"});
    }
    return request;
  }
  if (type == "set_tool") {
    fields(object, {"id", "type", "tool"});
    const auto& tool = field(object, "tool");
    fields(tool, {"number", "pocket", "length", "diameter", "comment"});
    return SetTool{Tool{integer(tool, "number"), integer(tool, "pocket"), number(tool, "length"),
                        number(tool, "diameter"), string(tool, "comment")}};
  }
  if (type == "remove_tool") {
    fields(object, {"id", "type", "number"});
    return RemoveTool{integer(object, "number")};
  }
  if (type == "set_operator_settings") {
    fields(object, {"type", "id", "machine_id", "base_revision", "document"});
    return SetOperatorSettings{
        string(object, "machine_id"),
        valid(tool_model_json::revision(field(object, "base_revision"))),
        valid(decode_operator_preferences(field(object, "document").dump()))};
  }
  if (type == "set_tool_model_defaults") {
    fields(object, {"id", "type", "machine_id", "base_revision", "defaults"});
    return SetToolModelDefaults{string(object, "machine_id"),
                                valid(tool_model_json::revision(field(object, "base_revision"))),
                                valid(tool_model_json::defaults(field(object, "defaults")))};
  }
  if (type == "set_tool_model_assignment") {
    fields(object, {"id", "type", "machine_id", "base_revision", "tool", "family", "overrides"},
           {"holder"});
    SetToolModelAssignment assignment;
    assignment.machine_id = string(object, "machine_id");
    assignment.base_revision = valid(tool_model_json::revision(field(object, "base_revision")));
    assignment.tool = valid(tool_model_json::binding(field(object, "tool")));
    // `family: null` removes the tool's assignment.
    if (const auto& family = field(object, "family"); !family.is_null())
      assignment.family = valid(tool_model_json::family(family));
    assignment.overrides = valid(tool_model_json::overrides(field(object, "overrides")));
    if (object.contains("holder")) {
      assignment.holder = string(object, "holder");
      if (!valid_tool_holder(assignment.holder) || !assignment.family)
        throw InvalidRequest(
            LocalizedReason{"Choose a supported holder for a tool model assignment.",
                            MessageKey{"backend.toolModels.chooseHolder"}});
    }
    return assignment;
  }
  throw InvalidRequest(LocalizedReason{"Unknown protocol v1 command.",
                                       MessageKey{"messages.request.unknownCommand"}});
}

auto request_too_long() -> DecodeError {
  return {std::nullopt, "Request exceeds the 16 KiB limit.",
          MessageKey{"messages.request.tooLong"}};
}
} // namespace

auto decode_request(std::string_view json) -> DecodedRequest {
  if (json.size() > max_save_program_request_bytes)
    return request_too_long();
  if (json.find('\0') != std::string_view::npos)
    return DecodeError{std::nullopt, "Invalid JSON request.",
                       MessageKey{"messages.request.invalidJson"}};
  std::optional<RequestId> id;
  try {
    if (json.size() > max_request_bytes) {
      // Keep only the top-level command name before constructing a full DOM for an oversized
      // save. JSON can put the fields in any order. The full parser still validates every key.
      const auto first = json.find_first_not_of(" \t\r\n");
      if (first == std::string_view::npos || json[first] != '{')
        return request_too_long();
      const auto header = Json::parse(json.begin(), json.end(),
          [](int depth, Json::parse_event_t event, const Json& value) -> bool {
            using Event = Json::parse_event_t;
            if ((event == Event::object_start || event == Event::array_start) &&
                depth >= max_request_depth)
              throw InvalidRequest(LocalizedReason{"Request nesting limit exceeded.",
                                                   MessageKey{"messages.request.nestingLimit"}});
            if (event == Event::key)
              return depth == 1 && value == "type";
            return depth == 0 || event == Event::value || event == Event::object_end;
          });
      if (!header.is_object() || header.value("type", Json{}) != "save_program")
        return request_too_long();
    }
    const auto object = parse(json);
    id = request_id(object);
    return Request{*id, command(object)};
  } catch (const InvalidRequest& error) {
    return DecodeError{id, error.what(), error.localization};
  } catch (const Json::exception&) {
    // Parser diagnostics can contain arbitrary input. Keep the operator message bounded.
    return DecodeError{id, "Invalid JSON request.", MessageKey{"messages.request.invalidJson"}};
  }
}

} // namespace coffeecnc
