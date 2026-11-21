#include <coffeecnc/app/operator_settings.hpp>
#include <coffeecnc/app/tool_models.hpp>

#include "tool_model_json.hpp"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>
#include <string>
#include <string_view>

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
auto layout_name(ProgramLayout value) -> std::string_view {
  switch (value) {
  case ProgramLayout::path: return "path";
  case ProgramLayout::text: return "text";
  case ProgramLayout::split: return "split";
  }
  return "";
}
constexpr std::size_t max_recent_programs = 8;
auto recent_path(std::string_view path) -> bool {
  return !path.empty() && path.size() <= 4096 && path.front() == '/' &&
         path.find('\0') == std::string_view::npos;
}
auto recent_json_path(const Json& path) -> bool {
  return path.is_string() && recent_path(std::string_view{path.get_ref<const std::string&>()});
}
constexpr std::size_t max_saved_positions = 16;
constexpr std::size_t max_position_name_bytes = 64;
auto position_name(std::string_view name) -> bool {
  return name.size() <= max_position_name_bytes &&
         name.find_first_not_of(' ') != std::string_view::npos &&
         std::ranges::none_of(name, [](char c) -> bool {
           const auto byte = static_cast<unsigned char>(c);
           return byte < 0x20 || byte == 0x7f;
         });
}
auto saved_position(const SavedPosition& saved) -> bool {
  return position_name(saved.name) &&
         std::ranges::all_of(saved.position, [](double value) -> bool {
           return std::isfinite(value) && std::abs(value) <= 1e6;
         });
}
// A damaged entry drops the whole list, never the other settings.
auto read_positions(const Json& list) -> std::vector<SavedPosition> {
  std::vector<SavedPosition> positions;
  if (!list.is_array() || list.size() > max_saved_positions)
    return {};
  for (const auto& entry : list) {
    if (!entry.is_object() || entry.size() != 2 || !entry.contains("name") ||
        !entry.contains("position") || !entry.at("name").is_string() ||
        !entry.at("position").is_array() || entry.at("position").size() != 3 ||
        !std::ranges::all_of(entry.at("position"), [](const Json& axis) -> bool { return axis.is_number(); }))
      return {};
    SavedPosition saved{entry.at("name").get<std::string>(), {}};
    for (std::size_t axis = 0; axis < 3; ++axis)
      saved.position.at(axis) = entry.at("position").at(axis).get<double>();
    if (!saved_position(saved))
      return {};
    positions.push_back(std::move(saved));
  }
  return positions;
}
auto positions_json(const std::vector<SavedPosition>& positions) -> Json {
  auto list = Json::array();
  for (const auto& saved : positions)
    list.push_back({{"name", saved.name}, {"position", saved.position}});
  return list;
}
auto theme_name(PanelTheme value) -> std::string_view {
  switch (value) {
  case PanelTheme::light: return "light";
  case PanelTheme::dark: return "dark";
  }
  return "";
}
void exact(const Json& value, std::initializer_list<std::string_view> required,
           std::initializer_list<std::string_view> optional = {}) {
  if (!value.is_object() ||
      !std::ranges::all_of(required, [&](std::string_view name) -> bool {
        return value.contains(name);
      }))
    throw std::runtime_error("Settings have missing or unknown fields.");
  for (auto field = value.begin(); field != value.end(); ++field)
    if (std::ranges::find(required, field.key()) == required.end() &&
        std::ranges::find(optional, field.key()) == optional.end())
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
  exact(value, {"version", "startup_units", "enabled_wcs", "default_spindle_rpm",
                "default_jog_velocity"},
        {"tool_reference_height", "program_layout", "theme", "recent_programs", "saved_positions"});
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
  // A missing or invalid presentation preference falls back independently.
  if (const auto layout = value.find("program_layout"); layout != value.end()) {
    if (*layout == "text")
      document.program_layout = ProgramLayout::text;
    else if (*layout == "split")
      document.program_layout = ProgramLayout::split;
  }
  if (const auto theme = value.find("theme"); theme != value.end() && *theme == "dark")
    document.theme = PanelTheme::dark;
  // Like the presentation, a damaged list is dropped without losing the other settings.
  if (const auto recent = value.find("recent_programs"); recent != value.end() && recent->is_array() &&
      recent->size() <= max_recent_programs &&
      std::ranges::all_of(*recent, [](const Json& path) -> bool { return recent_json_path(path); }))
    for (const auto& path : *recent)
      document.recent_programs.push_back(path.get<std::string>());
  if (const auto positions = value.find("saved_positions"); positions != value.end())
    document.saved_positions = read_positions(*positions);
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
          {"tool_reference_height", document.tool_reference_height},
          {"program_layout", layout_name(document.program_layout)},
          {"theme", theme_name(document.theme)},
          {"recent_programs", document.recent_programs},
          {"saved_positions", positions_json(document.saved_positions)}};
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
  if (layout_name(document.program_layout).empty() || theme_name(document.theme).empty())
    return "Invalid panel presentation.";
  if (document.recent_programs.size() > max_recent_programs ||
      !std::ranges::all_of(document.recent_programs,
                           [](const std::string& path) -> bool { return recent_path(path); }))
    return "Recent programs must be at most eight absolute paths.";
  if (document.saved_positions.size() > max_saved_positions ||
      !std::ranges::all_of(document.saved_positions, saved_position))
    return "Saved positions must be at most sixteen named, finite machine positions.";
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
