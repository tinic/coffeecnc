#include <coffeecnc/app/tool_models.hpp>

#include "message_key_json.hpp"
#include "tool_model_json.hpp"

#include <coffeecnc/core/command_policy.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <initializer_list>
#include <limits>
#include <numbers>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace coffeecnc {
namespace {
using Json = nlohmann::json;

constexpr std::array<std::string_view, tool_family_count> family_names{
    "end-mill", "ball-mill", "drill", "spot-drill", "chamfer", "face-mill", "generic", "touch-probe"};
// As ui/src/tool-model.ts words them.
constexpr auto length_range_reason = "Dimensions must be between 0.001 and 10,000 mm.";
constexpr auto angle_range_reason = "Included tip angle must be between 1° and 179°.";
constexpr double min_length = 0.001;
constexpr double max_length = 10000;
constexpr double min_angle = 1;
constexpr double max_angle = 179;
// The saved file nests five deep; the same bound as a request has.
constexpr int max_file_depth = 8;
constexpr auto too_large_reason = "The tool models are larger than 1 MiB.";

auto length_ok(double value) -> bool {
  return std::isfinite(value) && value >= min_length && value <= max_length;
}
auto angle_ok(double value) -> bool {
  return std::isfinite(value) && value >= min_angle && value <= max_angle;
}
auto pointed(ToolFamily family) -> bool {
  return family == ToolFamily::drill || family == ToolFamily::spot_drill ||
         family == ToolFamily::chamfer;
}
auto index(ToolFamily family) -> std::size_t { return static_cast<std::size_t>(family); }

class Invalid : public std::runtime_error {
public:
  Invalid(const std::string& reason, std::optional<MessageKey> localization)
      : std::runtime_error(reason), localization_(std::move(localization)) {}
  [[nodiscard]] auto localization() const -> const std::optional<MessageKey>& {
    return localization_;
  }

private:
  std::optional<MessageKey> localization_;
};

// The members of an object that has exactly these.
void exact(const Json& value, std::string_view what, std::initializer_list<std::string_view> names,
           std::initializer_list<std::string_view> optional = {}) {
  if (!value.is_object())
    throw Invalid(std::format("{} must be an object.", what),
                  MessageKey{"backend.toolModels.mustBeObject", {{"object", std::string(what)}}});
  const auto* const missing = std::ranges::find_if(
      names, [&](std::string_view name) -> bool { return !value.contains(name); });
  if (missing != names.end())
    throw Invalid(std::format("{} lacks the field {}.", what, *missing),
                  MessageKey{"backend.toolModels.lacksField",
                             {{"object", std::string(what)}, {"field", std::string(*missing)}}});
  const auto present = std::ranges::count_if(optional, [&](auto name) { return value.contains(name); });
  const auto expected = names.size() + static_cast<std::size_t>(present);
  if (value.size() != expected)
    throw Invalid(std::format("{} has an unknown field.", what),
                  MessageKey{"backend.toolModels.unknownField", {{"object", std::string(what)}}});
}

auto real(const Json& object, std::string_view what, std::string_view name) -> double {
  const auto& value = object.at(name);
  if (!value.is_number())
    throw Invalid(std::format("{} {} must be a number.", what, name),
                  MessageKey{"backend.toolModels.fieldNumber",
                             {{"object", std::string(what)}, {"field", std::string(name)}}});
  const auto number = value.get<double>();
  if (!std::isfinite(number))
    throw Invalid(std::format("{} {} must be a finite number.", what, name),
                  MessageKey{"backend.toolModels.fieldFinite",
                             {{"object", std::string(what)}, {"field", std::string(name)}}});
  return number;
}

auto whole(const Json& value, std::string_view what, double low, double high) -> double {
  if (!value.is_number())
    throw Invalid(std::format("{} must be a number.", what),
                  MessageKey{"backend.toolModels.mustBeNumber", {{"object", std::string(what)}}});
  const auto number = value.get<double>();
  if (!std::isfinite(number) || std::trunc(number) != number || number < low || number > high)
    throw Invalid(std::format("{} must be an integer from {:.0f} to {:.0f}.", what, low, high),
                  MessageKey{"backend.toolModels.mustBeInteger",
                             {{"object", std::string(what)},
                              {"low", std::format("{:.0f}", low)},
                              {"high", std::format("{:.0f}", high)}}});
  return number;
}

auto read_dimensions(const Json& value, std::string_view what, ToolFamily family) -> ToolDimensions {
  if (family == ToolFamily::touch_probe) {
    exact(value, what, {"diameter", "shankDiameter", "bodyDiameter", "bodyLength", "stylusLength"});
    return {.diameter = real(value, what, "diameter"),
            .shank_diameter = real(value, what, "shankDiameter"),
            .body_diameter = real(value, what, "bodyDiameter"),
            .body_length = real(value, what, "bodyLength"),
            .stylus_length = real(value, what, "stylusLength")};
  }
  exact(value, what,
        {"diameter", "cuttingLength", "shankDiameter", "overallLength", "stickout", "tipAngle"});
  return {real(value, what, "diameter"),      real(value, what, "cuttingLength"),
          real(value, what, "shankDiameter"), real(value, what, "overallLength"),
          real(value, what, "stickout"),      real(value, what, "tipAngle")};
}

auto read_defaults(const Json& value) -> ToolDefaults {
  if (!value.is_object())
    throw Invalid("defaults must be an object.", MessageKey{"backend.toolModels.defaultsObject"});
  auto defaults = factory_tool_defaults();
  std::size_t seen = 0;
  for (const auto family : tool_families) {
    const auto name = family_names.at(index(family));
    // Version 1 catalogs written before probe templates have all seven cutter families.
    if (family == ToolFamily::touch_probe && !value.contains(name))
      continue;
    if (!value.contains(name))
      throw Invalid(
          std::format("defaults lacks the family {}.", name),
          MessageKey{"backend.toolModels.defaultsLacksFamily", {{"family", std::string(name)}}});
    defaults.at(index(family)) = read_dimensions(value.at(name), name, family);
    ++seen;
  }
  if (value.size() != seen)
    throw Invalid("defaults has an unknown family.",
                  MessageKey{"backend.toolModels.defaultsUnknownFamily"});
  return defaults;
}

auto read_overrides(const Json& value) -> ToolOverrides {
  if (!value.is_object())
    throw Invalid("overrides must be an object.", MessageKey{"backend.toolModels.overridesObject"});
  ToolOverrides overrides;
  std::size_t known = 0;
  for (const auto& [name, member] :
       {std::pair{"cuttingLength", &overrides.cutting_length},
        std::pair{"shankDiameter", &overrides.shank_diameter},
        std::pair{"overallLength", &overrides.overall_length},
        std::pair{"stickout", &overrides.stickout}, std::pair{"tipAngle", &overrides.tip_angle},
        std::pair{"bodyDiameter", &overrides.body_diameter}, std::pair{"bodyLength", &overrides.body_length},
        std::pair{"stylusLength", &overrides.stylus_length}}) {
    if (!value.contains(name))
      continue;
    *member = real(value, "overrides", name);
    ++known;
  }
  if (value.size() != known)
    throw Invalid("overrides has an unknown field.",
                  MessageKey{"backend.toolModels.overridesUnknownField"});
  return overrides;
}

auto read_binding(const Json& value) -> ToolBinding {
  exact(value, "tool", {"number", "pocket", "diameter", "comment"});
  constexpr double low = std::numeric_limits<int>::min();
  constexpr double high = std::numeric_limits<int>::max();
  const auto& comment = value.at("comment");
  if (!comment.is_string())
    throw Invalid("tool comment must be a string.", MessageKey{"backend.toolModels.commentString"});
  return {static_cast<int>(whole(value.at("number"), "tool number", low, high)),
          static_cast<int>(whole(value.at("pocket"), "tool pocket", low, high)),
          real(value, "tool", "diameter"), comment.get<std::string>()};
}

auto read_family(const Json& value) -> ToolFamily {
  if (value.is_string())
    if (const auto found = tool_family(value.get<std::string>()))
      return *found;
  throw Invalid("family must be a supported tool family.",
                MessageKey{"backend.toolModels.familySupported"});
}

auto read_revision(const Json& value, std::string_view what) -> std::uint64_t {
  return static_cast<std::uint64_t>(
      whole(value, what, 0, static_cast<double>(max_tool_model_revision)));
}

template <typename Read>
auto checked(Read&& read) -> std::expected<std::invoke_result_t<Read>, LocalizedReason> {
  try {
    return std::forward<Read>(read)();
  } catch (const Invalid& error) {
    return std::unexpected(LocalizedReason{error.what(), error.localization()});
  } catch (const Json::exception&) {
    return std::unexpected(
        LocalizedReason{"Invalid JSON.", MessageKey{"backend.toolModels.invalidJson"}});
  }
}

// Builds the values of one text and keeps the first number that cannot be written.
class Numbers {
public:
  auto real(double value, std::string_view field) -> Json {
    if (!std::isfinite(value)) {
      if (error_.empty())
        error_ = {std::format("{} is not a finite number.", field),
                  MessageKey{"backend.toolModels.notFinite", {{"field", std::string(field)}}}};
      return 0;
    }
    return value;
  }
  [[nodiscard]] auto error() const -> const LocalizedReason& { return error_; }

private:
  LocalizedReason error_;
};

auto dimensions_json(Numbers& numbers, const ToolDimensions& d, ToolFamily family) -> Json {
  if (family == ToolFamily::touch_probe) {
    return {{"diameter", numbers.real(d.diameter, "diameter")},
            {"shankDiameter", numbers.real(d.shank_diameter, "shankDiameter")},
            {"bodyDiameter", numbers.real(d.body_diameter, "bodyDiameter")},
            {"bodyLength", numbers.real(d.body_length, "bodyLength")},
            {"stylusLength", numbers.real(d.stylus_length, "stylusLength")}};
  }
  return {{"diameter", numbers.real(d.diameter, "diameter")},
          {"cuttingLength", numbers.real(d.cutting_length, "cuttingLength")},
          {"shankDiameter", numbers.real(d.shank_diameter, "shankDiameter")},
          {"overallLength", numbers.real(d.overall_length, "overallLength")},
          {"stickout", numbers.real(d.stickout, "stickout")},
          {"tipAngle", numbers.real(d.tip_angle, "tipAngle")}};
}

auto document_json(Numbers& numbers, const ToolModelDocument& document) -> Json {
  auto defaults = Json::object();
  for (const auto family : tool_families)
    defaults[std::string(family_names.at(index(family)))] =
        dimensions_json(numbers, document.defaults.at(index(family)), family);
  auto assignments = Json::array();
  for (const auto& assignment : document.assignments) {
    auto overrides = Json::object();
    const auto& given = assignment.overrides;
    for (const auto& [name, member] :
         {std::pair{"cuttingLength", &given.cutting_length},
          std::pair{"shankDiameter", &given.shank_diameter},
          std::pair{"overallLength", &given.overall_length}, std::pair{"stickout", &given.stickout},
          std::pair{"tipAngle", &given.tip_angle}, std::pair{"bodyDiameter", &given.body_diameter},
          std::pair{"bodyLength", &given.body_length}, std::pair{"stylusLength", &given.stylus_length}})
      if (*member)
        overrides[name] = numbers.real(**member, name);
    assignments.push_back(
        {{"tool",
          {{"number", assignment.tool.number},
           {"pocket", assignment.tool.pocket},
           {"diameter", numbers.real(assignment.tool.diameter, "tool.diameter")},
           {"comment", assignment.tool.comment}}},
         {"family", family_names.at(index(assignment.family))},
         {"overrides", std::move(overrides)}});
    if (!assignment.holder.empty())
      assignments.back()["holder"] = assignment.holder;
  }
  return {{"version", tool_model_version},
          {"defaults", std::move(defaults)},
          {"assignments", std::move(assignments)}};
}

auto finish(const Numbers& numbers, const Json& value)
    -> std::expected<std::string, LocalizedReason> {
  if (!numbers.error().empty())
    return std::unexpected(numbers.error());
  try {
    // Strict: a comment whose bytes were replaced would be another tool's identity.
    auto text = value.dump();
    if (text.size() > max_tool_model_bytes)
      return std::unexpected(
          LocalizedReason{too_large_reason, MessageKey{"backend.toolModels.tooLarge"}});
    return text;
  } catch (const Json::exception&) {
    return std::unexpected(LocalizedReason{"A tool comment is not UTF-8.",
                                           MessageKey{"backend.toolModels.commentNotUtf8"}});
  }
}

auto parse_file(std::string_view text) -> Json {
  std::vector<std::set<std::string>> object_keys;
  const auto callback = [&object_keys](int depth, Json::parse_event_t event, Json& value) -> bool {
    using Event = Json::parse_event_t;
    if ((event == Event::object_start || event == Event::array_start) && depth >= max_file_depth)
      throw Invalid("It is nested too deeply.", MessageKey{"backend.toolModels.nestedTooDeeply"});
    if (event == Event::object_start)
      object_keys.emplace_back();
    else if (event == Event::object_end)
      object_keys.pop_back();
    else if (event == Event::key && !object_keys.back().insert(value.get<std::string>()).second)
      throw Invalid("It has a duplicate key.", MessageKey{"backend.toolModels.duplicateKey"});
    return true;
  };
  try {
    return Json::parse(text.begin(), text.end(), callback);
  } catch (const Json::exception&) {
    throw Invalid("It is not valid JSON.", MessageKey{"backend.toolModels.notValidJson"});
  }
}

} // namespace

auto tool_family_name(ToolFamily family) -> std::string_view {
  return family_names.at(index(family));
}

auto tool_family(std::string_view name) -> std::optional<ToolFamily> {
  const auto* const found = std::ranges::find(family_names, name);
  if (found == family_names.end())
    return {};
  return tool_families.at(static_cast<std::size_t>(found - family_names.begin()));
}

auto factory_tool_defaults() -> ToolDefaults {
  // diameter, cutting length, shank diameter, overall length, stick-out, tip angle
  return {{{10, 22, 10, 72, 35, 90},    // end-mill
           {10, 20, 10, 72, 35, 90},    // ball-mill
           {10, 60, 10, 100, 75, 118},  // drill
           {10, 15, 10, 60, 30, 90},    // spot-drill
           {10, 12, 10, 60, 30, 90},    // chamfer
           {50, 12, 20, 70, 40, 90},    // face-mill
           {10, 20, 10, 70, 35, 90},    // generic
           {.diameter = 4, .shank_diameter = 12, .body_diameter = 28, .body_length = 32,
            .stylus_length = 20}}};  // touch probe
}

auto tool_dimension_error(ToolFamily family, const ToolDimensions& d) -> LocalizedReason {
  if (family == ToolFamily::touch_probe) {
    return std::ranges::all_of(std::array{d.diameter, d.shank_diameter, d.body_diameter,
                                          d.body_length, d.stylus_length},
                               length_ok)
               ? LocalizedReason{}
               : LocalizedReason{length_range_reason, MessageKey{"backend.toolModels.lengthRange"}};
  }
  if (!std::ranges::all_of(std::array{d.diameter, d.cutting_length, d.shank_diameter,
                                      d.overall_length, d.stickout},
                           length_ok))
    return {length_range_reason, MessageKey{"backend.toolModels.lengthRange"}};
  if (!angle_ok(d.tip_angle))
    return {angle_range_reason, MessageKey{"backend.toolModels.angleRange"}};
  if (d.cutting_length > d.stickout || d.stickout > d.overall_length ||
      d.cutting_length >= d.overall_length)
    return {"Cutting length must fit within stick-out, and stick-out within overall length. "
            "Leave some shank above the cutting section.",
            MessageKey{"backend.toolModels.cuttingLengthFit"}};
  if (family == ToolFamily::ball_mill && d.cutting_length < d.diameter / 2)
    return {"Cutting length must include the full ball radius.",
            MessageKey{"backend.toolModels.ballRadius"}};
  // The height of the cone, written as the panel computes it.
  constexpr double degrees_per_turn = 360;
  if (pointed(family) &&
      d.diameter / 2 / std::tan(d.tip_angle * std::numbers::pi / degrees_per_turn) >
          d.cutting_length)
    return {"The pointed tip does not fit within the cutting length at this diameter and angle.",
            MessageKey{"backend.toolModels.pointedTip"}};
  return {};
}

auto tool_defaults_error(const ToolDefaults& defaults) -> LocalizedReason {
  for (const auto family : tool_families)
    if (auto error = tool_dimension_error(family, defaults.at(index(family))); !error.empty())
      return {std::format("{}: {}", family_names.at(index(family)), error.text),
              MessageKey{
                  "backend.toolModels.familyProblem",
                  {{"family", std::string(family_names.at(index(family)))}, {"detail", error.text}},
                  parameter_localization("detail", error.localization)}};
  return {};
}

auto tool_overrides_error(const ToolOverrides& o, ToolFamily family) -> LocalizedReason {
  if (family == ToolFamily::touch_probe && (o.cutting_length || o.overall_length || o.stickout || o.tip_angle))
    return {"Cutter dimensions cannot be assigned to a touch probe.",
            MessageKey{"backend.toolModels.cutterOnProbe"}};
  if (family != ToolFamily::touch_probe && (o.body_diameter || o.body_length || o.stylus_length))
    return {"Probe dimensions require the Touch probe family.",
            MessageKey{"backend.toolModels.probeFamily"}};
  for (const auto& length : {o.cutting_length, o.shank_diameter, o.overall_length, o.stickout,
                             o.body_diameter, o.body_length, o.stylus_length})
    if (length && !length_ok(*length))
      return {length_range_reason, MessageKey{"backend.toolModels.lengthRange"}};
  if (o.tip_angle && !angle_ok(*o.tip_angle))
    return {angle_range_reason, MessageKey{"backend.toolModels.angleRange"}};
  return {};
}

auto tool_binding_error(const ToolBinding& tool) -> LocalizedReason {
  if (tool.number <= 0)
    return {"The tool number must be above zero.",
            MessageKey{"backend.toolModels.toolNumberPositive"}};
  if (tool.pocket < 0)
    return {"The tool pocket must not be below zero.",
            MessageKey{"backend.toolModels.pocketNotNegative"}};
  if (!std::isfinite(tool.diameter) || tool.diameter < 0)
    return {"The tool diameter must be a finite number, not below zero.",
            MessageKey{"backend.toolModels.diameterFinite"}};
  if (tool.comment.size() > max_tool_comment_bytes)
    return {"The tool description is longer than 255 bytes.",
            MessageKey{"backend.toolModels.descriptionTooLong"}};
  if (tool.comment.find_first_of(std::string_view{"\0\r\n", 3}) != std::string::npos)
    return {"The tool description contains a NUL or a line break.",
            MessageKey{"backend.toolModels.descriptionCharacters"}};
  return {};
}

auto tool_document_error(const ToolModelDocument& document) -> LocalizedReason {
  if (auto error = tool_defaults_error(document.defaults); !error.empty())
    return error;
  if (document.assignments.size() > max_tool_model_assignments)
    return {"There are more than 1001 tool model assignments.",
            MessageKey{"backend.toolModels.tooManyAssignments"}};
  std::set<int> numbers;
  for (const auto& assignment : document.assignments) {
    auto error = tool_binding_error(assignment.tool);
    if (error.empty())
      error = tool_overrides_error(assignment.overrides, assignment.family);
    if (error.empty() && !valid_tool_holder(assignment.holder))
      error = {"Unknown tool holder.", MessageKey{"backend.toolModels.unknownHolder"}};
    if (error.empty() && !numbers.insert(assignment.tool.number).second)
      error = {"It is assigned more than once.", MessageKey{"backend.toolModels.assignedTwice"}};
    if (!error.empty())
      return {std::format("Tool {}: {}", assignment.tool.number, error.text),
              MessageKey{"backend.toolModels.toolProblem",
                         {{"tool", std::to_string(assignment.tool.number)}, {"detail", error.text}},
                         parameter_localization("detail", error.localization)}};
  }
  return {};
}

auto tool_model_command_error(const ToolModelCommand& command) -> LocalizedReason {
  return std::visit(
      [](const auto& c) -> LocalizedReason {
        if constexpr (std::is_same_v<std::decay_t<decltype(c)>, SetToolModelDefaults>) {
          return tool_defaults_error(c.defaults);
        } else {
          if (auto error = tool_binding_error(c.tool); !error.empty())
            return error;
          if (!c.family && !c.overrides.empty())
            return {"Overrides must be empty when the assignment is removed.",
                    MessageKey{"backend.toolModels.overridesEmpty"}};
          if (!valid_tool_holder(c.holder) || (!c.family && !c.holder.empty()))
            return {"Choose a supported holder for a tool model assignment.",
                    MessageKey{"backend.toolModels.chooseHolder"}};
          return tool_overrides_error(c.overrides, c.family.value_or(ToolFamily::generic));
        }
      },
      command);
}

auto encode_tool_models(const ToolModels& models) -> std::expected<std::string, LocalizedReason> {
  const bool available = models.availability == ToolModelAvailability::available;
  if (available && (!models.machine_id || !models.document))
    return std::unexpected(LocalizedReason{"An available tool model snapshot needs a machine ID "
                                           "and a document.",
                                           MessageKey{"backend.toolModels.snapshotNeedsDocument"}});
  if (!available && (models.document || models.revision != 0))
    return std::unexpected(LocalizedReason{"A tool model snapshot that is not available has no "
                                           "document and revision 0.",
                                           MessageKey{"backend.toolModels.unavailableSnapshot"}});
  if (models.revision > max_tool_model_revision)
    return std::unexpected(LocalizedReason{"revision is above 2^53 - 1.",
                                           MessageKey{"backend.toolModels.revisionTooHigh"}});
  Numbers numbers;
  // The reason may quote a path or the system's words, which need not be UTF-8: replaced here,
  // so that the strict dump below judges the comments alone.
  const auto reason = Json::parse(Json(models.reason).dump(-1, ' ', false,
                                                           Json::error_handler_t::replace));
  Json machine = nullptr;
  if (models.machine_id)
    machine = *models.machine_id;
  Json document = nullptr;
  if (models.document)
    document = document_json(numbers, *models.document);
  Json message{{"type", "tool_models"},
               {"machine_id", std::move(machine)},
               {"revision", models.revision},
               {"availability", available ? "available"
                                : models.availability == ToolModelAvailability::session_only
                                    ? "session_only"
                                    : "unavailable"},
               {"reason", reason},
               {"document", std::move(document)}};
  return finish(numbers, message);
}

auto encode_tool_models_reason(const ToolModels& models) -> std::optional<std::string> {
  if (!models.reason_localization || models.reason.empty())
    return {};
  const Json message{{"type", "tool_models_reason"},
                     {"machine_id", models.machine_id ? Json(*models.machine_id) : Json(nullptr)},
                     {"revision", models.revision},
                     {"reason", models.reason},
                     {"localization", message_key_json(*models.reason_localization)}};
  return message.dump(-1, ' ', false, Json::error_handler_t::replace);
}

auto encode_tool_model_file(const ToolModelFile& file)
    -> std::expected<std::string, LocalizedReason> {
  if (file.revision > max_tool_model_revision)
    return std::unexpected(LocalizedReason{"revision is above 2^53 - 1.",
                                           MessageKey{"backend.toolModels.revisionTooHigh"}});
  Numbers numbers;
  return finish(numbers, {{"machine_id", file.machine_id},
                          {"revision", file.revision},
                          {"document", document_json(numbers, file.document)}});
}

auto decode_tool_model_file(std::string_view text)
    -> std::expected<ToolModelFile, LocalizedReason> {
  if (text.size() > max_tool_model_bytes)
    return std::unexpected(
        LocalizedReason{"It is larger than 1 MiB.", MessageKey{"backend.toolModels.fileTooLarge"}});
  return checked([&]() -> ToolModelFile {
    const auto file = parse_file(text);
    // The version first: a later one may have fields this one does not know.
    if (!file.is_object() || !file.contains("document") || !file.at("document").is_object() ||
        !file.at("document").contains("version"))
      throw Invalid("It is not a tool model document.",
                    MessageKey{"backend.toolModels.notDocument"});
    const auto& document = file.at("document");
    constexpr double max_version = 9007199254740991;
    const auto version = whole(document.at("version"), "version", 1, max_version);
    if (version != tool_model_version)
      throw Invalid(std::format("Its version {:.0f} is not supported by this CoffeeCNC.", version),
                    MessageKey{"backend.toolModels.versionNotSupported",
                               {{"version", std::format("{:.0f}", version)}}});
    exact(file, "The file", {"machine_id", "revision", "document"});
    exact(document, "document", {"version", "defaults", "assignments"});
    ToolModelFile result;
    if (!file.at("machine_id").is_string() ||
        !valid_machine_id(file.at("machine_id").get<std::string>()))
      throw Invalid("Its machine_id is not a valid machine ID.",
                    MessageKey{"backend.toolModels.invalidMachineId"});
    result.machine_id = file.at("machine_id").get<std::string>();
    // Revision 0 is the state without a file; a saved one counts from 1.
    result.revision = read_revision(file.at("revision"), "revision");
    if (result.revision == 0)
      throw Invalid("revision must be an integer from 1 to 9007199254740991.",
                    MessageKey{"backend.toolModels.revisionRange"});
    result.document.defaults = read_defaults(document.at("defaults"));
    const auto& assignments = document.at("assignments");
    if (!assignments.is_array())
      throw Invalid("assignments must be an array.",
                    MessageKey{"backend.toolModels.assignmentsArray"});
    if (assignments.size() > max_tool_model_assignments)
      throw Invalid("There are more than 1001 tool model assignments.",
                    MessageKey{"backend.toolModels.tooManyAssignments"});
    for (const auto& entry : assignments) {
      exact(entry, "An assignment", {"tool", "family", "overrides"}, {"holder"});
      std::string holder;
      if (entry.contains("holder")) {
        if (!entry.at("holder").is_string())
          throw Invalid("A tool holder must be a string.",
                        MessageKey{"backend.toolModels.holderString"});
        holder = entry.at("holder").get<std::string>();
      }
      result.document.assignments.push_back(
          {read_binding(entry.at("tool")), read_family(entry.at("family")),
           read_overrides(entry.at("overrides")), std::move(holder)});
    }
    if (const auto error = tool_document_error(result.document); !error.empty())
      throw Invalid(error.text, error.localization);
    std::ranges::sort(result.document.assignments, {},
                      [](const ToolModelAssignment& a) -> int { return a.tool.number; });
    return result;
  });
}

namespace tool_model_json {

auto defaults(const Json& value) -> std::expected<ToolDefaults, LocalizedReason> {
  return checked([&]() -> ToolDefaults { return read_defaults(value); });
}
auto overrides(const Json& value) -> std::expected<ToolOverrides, LocalizedReason> {
  return checked([&]() -> ToolOverrides { return read_overrides(value); });
}
auto binding(const Json& value) -> std::expected<ToolBinding, LocalizedReason> {
  return checked([&]() -> ToolBinding { return read_binding(value); });
}
auto family(const Json& value) -> std::expected<ToolFamily, LocalizedReason> {
  return checked([&]() -> ToolFamily { return read_family(value); });
}
auto revision(const Json& value) -> std::expected<std::uint64_t, LocalizedReason> {
  return checked([&]() -> std::uint64_t { return read_revision(value, "base_revision"); });
}

} // namespace tool_model_json

} // namespace coffeecnc
