#include <coffeecnc/app/operator_settings.hpp>
#include <coffeecnc/app/tool_models.hpp>

#include "tool_model_json.hpp"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace coffeecnc {
namespace {
using Json = nlohmann::json;
auto units(StartupUnits value) -> std::string_view {
  switch (value) {
  case StartupUnits::machine:
    return "machine";
  case StartupUnits::mm:
    return "mm";
  case StartupUnits::inch:
    return "inch";
  }
  return "";
}
void exact(const Json& value, std::initializer_list<std::string_view> names) {
  if (!value.is_object() || value.size() != names.size() ||
      !std::ranges::all_of(names,
                           [&](std::string_view name) -> bool { return value.contains(name); }))
    throw std::runtime_error("Settings have missing or unknown fields.");
}
auto parse(std::string_view text) -> Json {
  if (text.size() > max_operator_settings_bytes)
    throw std::runtime_error("Settings exceed 16 KiB.");
  std::vector<std::set<std::string>> keys;
  return Json::parse(
      text.begin(), text.end(), [&](int depth, Json::parse_event_t event, Json& value) -> bool {
        using Event = Json::parse_event_t;
        if ((event == Event::object_start || event == Event::array_start) && depth >= 8)
          throw std::runtime_error("Settings are nested too deeply.");
        if (event == Event::object_start)
          keys.emplace_back();
        else if (event == Event::object_end)
          keys.pop_back();
        else if (event == Event::key && !keys.back().insert(value.get<std::string>()).second)
          throw std::runtime_error("Settings contain a duplicate key.");
        return true;
      });
}
auto read_document(const Json& value) -> OperatorPreferences {
  if (value.contains("tool_reference_height"))
    exact(value, {"version", "startup_units", "enabled_wcs", "default_spindle_rpm",
                  "default_jog_velocity", "tool_reference_height"});
  else
    exact(value,
          {"version", "startup_units", "enabled_wcs", "default_spindle_rpm", "default_jog_velocity"});
  if (!value.at("version").is_number() || value.at("version") != 1)
    throw std::runtime_error("Settings version is not supported.");
  OperatorPreferences document;
  const auto unit = value.at("startup_units").get<std::string>();
  if (unit == "machine")
    document.startup_units = StartupUnits::machine;
  else if (unit == "mm")
    document.startup_units = StartupUnits::mm;
  else if (unit == "inch")
    document.startup_units = StartupUnits::inch;
  else
    throw std::runtime_error("Startup units must be machine, mm or inch.");
  const auto& systems = value.at("enabled_wcs");
  if (!systems.is_array() || systems.empty() || systems.size() > 9)
    throw std::runtime_error("Choose one to nine work system shortcuts.");
  document.enabled_wcs.clear();
  for (const auto& system : systems) {
    if (!system.is_number() || system.get<double>() < 1 || system.get<double>() > 9 ||
        std::trunc(system.get<double>()) != system.get<double>())
      throw std::runtime_error("Work system shortcuts must be integers from 1 to 9.");
    document.enabled_wcs.push_back(system.get<int>());
  }
  for (const auto name : {"default_spindle_rpm", "default_jog_velocity"})
    if (!value.at(name).is_number())
      throw std::runtime_error("Default speeds must be numbers.");
  document.default_spindle_rpm = value.at("default_spindle_rpm").get<double>();
  document.default_jog_velocity = value.at("default_jog_velocity").get<double>();
  if (value.contains("tool_reference_height")) {
    if (!value.at("tool_reference_height").is_number())
      throw std::runtime_error("Tool reference height must be a number.");
    document.tool_reference_height = value.at("tool_reference_height").get<double>();
  }
  if (const auto error = operator_preferences_error(document); !error.empty())
    throw std::runtime_error(error);
  return document;
}
auto document_json(const OperatorPreferences& document) -> Json {
  return {{"version", 1},
          {"startup_units", units(document.startup_units)},
          {"enabled_wcs", document.enabled_wcs},
          {"default_spindle_rpm", document.default_spindle_rpm},
          {"default_jog_velocity", document.default_jog_velocity},
          {"tool_reference_height", document.tool_reference_height}};
}
template <typename Read> auto checked(Read read) -> std::expected<decltype(read()), std::string> {
  try {
    return read();
  } catch (const Json::exception&) {
    return std::unexpected("Invalid settings JSON or field type.");
  } catch (const std::runtime_error& error) {
    return std::unexpected(error.what());
  }
}
} // namespace

auto operator_preferences_error(const OperatorPreferences& document) -> std::string {
  if (units(document.startup_units).empty())
    return "Invalid startup units.";
  if (document.enabled_wcs.empty() || document.enabled_wcs.size() > 9 ||
      std::set<int>(document.enabled_wcs.begin(), document.enabled_wcs.end()).size() !=
          document.enabled_wcs.size() ||
      !std::ranges::all_of(document.enabled_wcs,
                           [](int value) -> bool { return value >= 1 && value <= 9; }))
    return "Choose one to nine unique work system shortcuts.";
  if (!std::isfinite(document.default_spindle_rpm) || document.default_spindle_rpm < 0 ||
      document.default_spindle_rpm > 1e9)
    return "Default spindle speed must be between zero and 1,000,000,000 rpm.";
  if (!std::isfinite(document.default_jog_velocity) || document.default_jog_velocity <= 0 ||
      document.default_jog_velocity > 1e9)
    return "Default jog speed must be above zero and at most 1,000,000,000 mm/min.";
  if (!std::isfinite(document.tool_reference_height) || document.tool_reference_height < 0 ||
      document.tool_reference_height > 1e9)
    return "Tool reference height must be between zero and 1,000,000,000 mm.";
  return {};
}
auto operator_settings_command_error(const SetOperatorSettings& command, const Limits& limits)
    -> std::string {
  if (!valid_machine_id(command.machine_id) || command.base_revision > max_tool_model_revision)
    return "Invalid settings identity or revision.";
  if (auto error = operator_preferences_error(command.document); !error.empty())
    return error;
  if (command.document.default_spindle_rpm > limits.max_spindle_rpm)
    return "Default spindle speed exceeds the configured machine limit.";
  if (command.document.default_jog_velocity > limits.max_jog_velocity)
    return "Default jog speed exceeds the configured machine limit.";
  return {};
}
auto decode_operator_preferences(std::string_view text)
    -> std::expected<OperatorPreferences, std::string> {
  return checked([&]() -> OperatorPreferences { return read_document(parse(text)); });
}
auto encode_operator_settings(const OperatorSettings& settings) -> Encoded {
  return checked([&]() -> std::string {
    const bool available = settings.availability == ToolModelAvailability::available;
    if (settings.revision > max_tool_model_revision ||
        (settings.machine_id && !valid_machine_id(*settings.machine_id)) ||
        (available && (!settings.document || !settings.machine_id)) ||
        (!available && (settings.document || settings.revision != 0)))
      throw std::runtime_error("Invalid settings snapshot.");
    if (settings.document)
      if (auto error = operator_preferences_error(*settings.document); !error.empty())
        throw std::runtime_error(error);
    return Json{{"type", "operator_settings"},
                {"machine_id", settings.machine_id ? Json(*settings.machine_id) : Json(nullptr)},
                {"revision", settings.revision},
                {"availability", available ? "available"
                                 : settings.availability == ToolModelAvailability::session_only
                                     ? "session_only"
                                     : "unavailable"},
                {"reason", settings.reason},
                {"document", settings.document ? document_json(*settings.document) : Json(nullptr)}}
        .dump();
  });
}
auto encode_operator_settings_file(const OperatorSettingsFile& file) -> Encoded {
  return checked([&]() -> std::string {
    if (!valid_machine_id(file.machine_id) || file.revision == 0 ||
        file.revision > max_tool_model_revision)
      throw std::runtime_error("Invalid settings file identity or revision.");
    if (auto error = operator_preferences_error(file.document); !error.empty())
      throw std::runtime_error(error);
    return Json{{"machine_id", file.machine_id},
                {"revision", file.revision},
                {"document", document_json(file.document)}}
        .dump();
  });
}
auto decode_operator_settings_file(std::string_view text)
    -> std::expected<OperatorSettingsFile, std::string> {
  return checked([&]() -> OperatorSettingsFile {
    const auto file = parse(text);
    exact(file, {"machine_id", "revision", "document"});
    const auto machine = file.at("machine_id").get<std::string>();
    const auto revision = tool_model_json::revision(file.at("revision"));
    if (!valid_machine_id(machine) || !revision || *revision == 0)
      throw std::runtime_error("Invalid settings file identity or revision.");
    return {machine, *revision, read_document(file.at("document"))};
  });
}
} // namespace coffeecnc
