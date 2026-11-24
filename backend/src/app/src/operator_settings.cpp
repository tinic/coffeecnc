#include <coffeecnc/app/operator_settings.hpp>
#include <coffeecnc/app/tool_models.hpp>

#include "message_key_json.hpp"
#include "tool_model_json.hpp"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <optional>
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
// One damaged entry damages the whole list: nullopt.
auto read_positions(const Json& list) -> std::optional<std::vector<SavedPosition>> {
  std::vector<SavedPosition> positions;
  if (!list.is_array() || list.size() > max_saved_positions)
    return std::nullopt;
  for (const auto& entry : list) {
    if (!entry.is_object() || entry.size() != 2 || !entry.contains("name") ||
        !entry.contains("position") || !entry.at("name").is_string() ||
        !entry.at("position").is_array() || entry.at("position").size() != 3 ||
        !std::ranges::all_of(entry.at("position"), [](const Json& axis) -> bool { return axis.is_number(); }))
      return std::nullopt;
    SavedPosition saved{entry.at("name").get<std::string>(), {}};
    for (std::size_t axis = 0; axis < 3; ++axis)
      saved.position.at(axis) = entry.at("position").at(axis).get<double>();
    if (!saved_position(saved))
      return std::nullopt;
    positions.push_back(std::move(saved));
  }
  return positions;
}
auto positions_json(const std::vector<SavedPosition>& positions) -> Json {
  auto list = Json::array();
  std::ranges::transform(positions, std::back_inserter(list), [](const auto& saved) -> Json {
    return {{"name", saved.name}, {"position", saved.position}};
  });
  return list;
}
auto theme_name(PanelTheme value) -> std::string_view {
  switch (value) {
  case PanelTheme::light: return "light";
  case PanelTheme::dark: return "dark";
  case PanelTheme::sandstone: return "sandstone";
  case PanelTheme::harbor: return "harbor";
  }
  return "";
}
constexpr std::array<std::pair<PanelLanguage, std::string_view>, 7> language_names{{
    {PanelLanguage::en, "en"}, {PanelLanguage::fr, "fr"}, {PanelLanguage::de, "de"},
    {PanelLanguage::es, "es"}, {PanelLanguage::pt, "pt"}, {PanelLanguage::zh, "zh"},
    {PanelLanguage::ja, "ja"}}};
auto language_name(PanelLanguage value) -> std::string_view {
  for (const auto& [language, name] : language_names)
    if (language == value)
      return name;
  return "";
}
class InvalidSettings : public std::runtime_error {
public:
  explicit InvalidSettings(LocalizedReason reason)
      : std::runtime_error(reason.text), localization_(std::move(reason.localization)) {}
  auto localization() const -> const std::optional<MessageKey>& { return localization_; }

private:
  std::optional<MessageKey> localization_;
};

void exact(const Json& value, std::initializer_list<std::string_view> required,
           std::initializer_list<std::string_view> optional = {}) {
  if (!value.is_object() ||
      !std::ranges::all_of(required, [&](std::string_view name) -> bool {
        return value.contains(name);
      }))
    throw InvalidSettings({"Settings have missing or unknown fields.",
                           MessageKey{"backend.operatorSettings.missingFields"}});
  for (auto field = value.begin(); field != value.end(); ++field)
    if (std::ranges::find(required, field.key()) == required.end() &&
        std::ranges::find(optional, field.key()) == optional.end())
      throw InvalidSettings({"Settings have missing or unknown fields.",
                             MessageKey{"backend.operatorSettings.missingFields"}});
}
auto parse(std::string_view text) -> Json {
  if (text.size() > max_operator_settings_bytes)
    throw InvalidSettings(
        {"Settings exceed 16 KiB.", MessageKey{"backend.operatorSettings.tooLarge"}});
  std::vector<std::set<std::string>> keys;
  return Json::parse(
      text.begin(), text.end(), [&](int depth, Json::parse_event_t event, Json& value) -> bool {
        using Event = Json::parse_event_t;
        if ((event == Event::object_start || event == Event::array_start) && depth >= 8)
          throw InvalidSettings({"Settings are nested too deeply.",
                                 MessageKey{"backend.operatorSettings.nestedTooDeeply"}});
        if (event == Event::object_start)
          keys.emplace_back();
        else if (event == Event::object_end)
          keys.pop_back();
        else if (event == Event::key && !keys.back().insert(value.get<std::string>()).second)
          throw InvalidSettings({"Settings contain a duplicate key.",
                                 MessageKey{"backend.operatorSettings.duplicateKey"}});
        return true;
      });
}
// A saved file keeps its other preferences when an optional one is damaged; a save request with a
// damaged one is refused, so that it never replaces saved positions or programs with nothing.
enum class Damaged : std::uint8_t { fall_back, refuse };
auto read_document(const Json& value, Damaged damaged) -> OperatorPreferences {
  const auto fall_back = [&](LocalizedReason refusal) -> void {
    if (damaged == Damaged::refuse)
      throw InvalidSettings(std::move(refusal));
  };
  exact(value, {"version", "startup_units", "enabled_wcs", "default_spindle_rpm",
                "default_jog_velocity"},
        {"tool_reference_height", "program_layout", "theme", "language", "recent_programs",
         "saved_positions", "go_to_safe_z"});
  if (!value.at("version").is_number() || value.at("version") != 1)
    throw InvalidSettings({"Settings version is not supported.",
                           MessageKey{"backend.operatorSettings.versionNotSupported"}});
  OperatorPreferences document;
  const auto unit = value.at("startup_units").get<std::string>();
  if (unit == "machine")
    document.startup_units = StartupUnits::machine;
  else if (unit == "mm")
    document.startup_units = StartupUnits::mm;
  else if (unit == "inch")
    document.startup_units = StartupUnits::inch;
  else
    throw InvalidSettings({"Startup units must be machine, mm or inch.",
                           MessageKey{"backend.operatorSettings.startupUnitsChoice"}});
  const auto& systems = value.at("enabled_wcs");
  if (!systems.is_array() || systems.empty() || systems.size() > 9)
    throw InvalidSettings({"Choose one to nine work system shortcuts.",
                           MessageKey{"backend.operatorSettings.chooseShortcuts"}});
  document.enabled_wcs.clear();
  for (const auto& system : systems) {
    if (!system.is_number() || system.get<double>() < 1 || system.get<double>() > 9 ||
        std::trunc(system.get<double>()) != system.get<double>())
      throw InvalidSettings({"Work system shortcuts must be integers from 1 to 9.",
                             MessageKey{"backend.operatorSettings.shortcutIntegers"}});
    document.enabled_wcs.push_back(system.get<int>());
  }
  for (const auto name : {"default_spindle_rpm", "default_jog_velocity"})
    if (!value.at(name).is_number())
      throw InvalidSettings({"Default speeds must be numbers.",
                             MessageKey{"backend.operatorSettings.speedsNumbers"}});
  document.default_spindle_rpm = value.at("default_spindle_rpm").get<double>();
  document.default_jog_velocity = value.at("default_jog_velocity").get<double>();
  if (value.contains("tool_reference_height")) {
    if (!value.at("tool_reference_height").is_number())
      throw InvalidSettings({"Tool reference height must be a number.",
                             MessageKey{"backend.operatorSettings.referenceHeightNumber"}});
    document.tool_reference_height = value.at("tool_reference_height").get<double>();
  }
  // A missing presentation preference falls back independently.
  if (const auto layout = value.find("program_layout"); layout != value.end()) {
    if (*layout == "text")
      document.program_layout = ProgramLayout::text;
    else if (*layout == "split")
      document.program_layout = ProgramLayout::split;
    else if (*layout != "path")
      fall_back({"Invalid panel presentation.",
                 MessageKey{"backend.operatorSettings.invalidPresentation"}});
  }
  if (const auto theme = value.find("theme"); theme != value.end()) {
    if (*theme == "dark")
      document.theme = PanelTheme::dark;
    else if (*theme == "sandstone")
      document.theme = PanelTheme::sandstone;
    else if (*theme == "harbor")
      document.theme = PanelTheme::harbor;
    else if (*theme != "light")
      fall_back({"Invalid panel presentation.",
                 MessageKey{"backend.operatorSettings.invalidPresentation"}});
  }
  if (const auto language = value.find("language"); language != value.end()) {
    const auto known = std::ranges::find_if(language_names, [&](const auto& entry) -> bool {
      return language->is_string() && *language == entry.second;
    });
    if (known != language_names.end())
      document.language = known->first;
    else
      fall_back({"Invalid panel presentation.",
                 MessageKey{"backend.operatorSettings.invalidPresentation"}});
  }
  // Like the presentation, a damaged list falls back to none without losing the other settings.
  if (const auto recent = value.find("recent_programs"); recent != value.end()) {
    if (recent->is_array() && recent->size() <= max_recent_programs &&
        std::ranges::all_of(*recent, [](const Json& path) -> bool { return recent_json_path(path); }))
      for (const auto& path : *recent)
        document.recent_programs.push_back(path.get<std::string>());
    else
      fall_back({"Recent programs must be at most eight absolute paths.",
                 MessageKey{"backend.operatorSettings.recentPrograms"}});
  }
  if (const auto positions = value.find("saved_positions"); positions != value.end()) {
    if (auto read = read_positions(*positions))
      document.saved_positions = std::move(*read);
    else
      fall_back({"Saved positions must be at most sixteen named, finite machine positions.",
                 MessageKey{"backend.operatorSettings.savedPositions"}});
  }
  // Null or absent: the default safe height.
  if (const auto safe_z = value.find("go_to_safe_z"); safe_z != value.end() && !safe_z->is_null()) {
    if (safe_z->is_number() && std::isfinite(safe_z->get<double>()) &&
        std::abs(safe_z->get<double>()) <= 1e9)
      document.go_to_safe_z = safe_z->get<double>();
    else
      fall_back({"Safe Z for Go to must be a machine Z in mm.",
                 MessageKey{"backend.operatorSettings.safeZ"}});
  }
  if (auto error = operator_preferences_error(document); !error.empty())
    throw InvalidSettings(std::move(error));
  return document;
}
// The settings file leaves the language out: 0.1.3 and earlier read only the other fields and
// refuse a file with one more. The language has its own file (encode_panel_language_file).
enum class Language : std::uint8_t { include, leave_out };
auto document_json(const OperatorPreferences& document, Language language = Language::include)
    -> Json {
  Json json{{"version", 1},
          {"startup_units", units(document.startup_units)},
          {"enabled_wcs", document.enabled_wcs},
          {"default_spindle_rpm", document.default_spindle_rpm},
          {"default_jog_velocity", document.default_jog_velocity},
          {"tool_reference_height", document.tool_reference_height},
          {"program_layout", layout_name(document.program_layout)},
          {"theme", theme_name(document.theme)},
          {"recent_programs", document.recent_programs},
          {"saved_positions", positions_json(document.saved_positions)},
          {"go_to_safe_z", document.go_to_safe_z ? Json(*document.go_to_safe_z) : Json(nullptr)}};
  if (language == Language::include)
    json["language"] = language_name(document.language);
  return json;
}
template <typename Read>
auto checked(Read read) -> std::expected<decltype(read()), LocalizedReason> {
  try {
    return read();
  } catch (const InvalidSettings& error) {
    return std::unexpected(LocalizedReason{error.what(), error.localization()});
  } catch (const Json::exception&) {
    return std::unexpected(LocalizedReason{"Invalid settings JSON or field type.",
                                           MessageKey{"backend.operatorSettings.invalidJson"}});
  } catch (const std::runtime_error& error) {
    return std::unexpected(LocalizedReason{error.what()});
  }
}
} // namespace

auto operator_preferences_error(const OperatorPreferences& document) -> LocalizedReason {
  if (layout_name(document.program_layout).empty() || theme_name(document.theme).empty() ||
      language_name(document.language).empty())
    return {"Invalid panel presentation.",
            MessageKey{"backend.operatorSettings.invalidPresentation"}};
  if (document.recent_programs.size() > max_recent_programs ||
      !std::ranges::all_of(document.recent_programs,
                           [](const std::string& path) -> bool { return recent_path(path); }))
    return {"Recent programs must be at most eight absolute paths.",
            MessageKey{"backend.operatorSettings.recentPrograms"}};
  if (document.saved_positions.size() > max_saved_positions ||
      !std::ranges::all_of(document.saved_positions, saved_position))
    return {"Saved positions must be at most sixteen named, finite machine positions.",
            MessageKey{"backend.operatorSettings.savedPositions"}};
  if (units(document.startup_units).empty())
    return {"Invalid startup units.", MessageKey{"backend.operatorSettings.invalidStartupUnits"}};
  if (document.enabled_wcs.empty() || document.enabled_wcs.size() > 9 ||
      std::set<int>(document.enabled_wcs.begin(), document.enabled_wcs.end()).size() !=
          document.enabled_wcs.size() ||
      !std::ranges::all_of(document.enabled_wcs,
                           [](int value) -> bool { return value >= 1 && value <= 9; }))
    return {"Choose one to nine unique work system shortcuts.",
            MessageKey{"backend.operatorSettings.chooseUniqueShortcuts"}};
  if (!std::isfinite(document.default_spindle_rpm) || document.default_spindle_rpm < 0 ||
      document.default_spindle_rpm > 1e9)
    return {"Default spindle speed must be between zero and 1,000,000,000 rpm.",
            MessageKey{"backend.operatorSettings.spindleSpeedRange"}};
  if (!std::isfinite(document.default_jog_velocity) || document.default_jog_velocity <= 0 ||
      document.default_jog_velocity > 1e9)
    return {"Default jog speed must be above zero and at most 1,000,000,000 mm/min.",
            MessageKey{"backend.operatorSettings.jogSpeedRange"}};
  if (!std::isfinite(document.tool_reference_height) || document.tool_reference_height < 0 ||
      document.tool_reference_height > 1e9)
    return {"Tool reference height must be between zero and 1,000,000,000 mm.",
            MessageKey{"backend.operatorSettings.referenceHeightRange"}};
  if (document.go_to_safe_z &&
      (!std::isfinite(*document.go_to_safe_z) || std::abs(*document.go_to_safe_z) > 1e9))
    return {"Safe Z for Go to must be a machine Z in mm.",
            MessageKey{"backend.operatorSettings.safeZ"}};
  return {};
}
auto operator_settings_command_error(const SetOperatorSettings& command, const Limits& limits)
    -> LocalizedReason {
  if (!valid_machine_id(command.machine_id) || command.base_revision > max_tool_model_revision)
    return {"Invalid settings identity or revision.",
            MessageKey{"backend.operatorSettings.invalidIdentity"}};
  if (auto error = operator_preferences_error(command.document); !error.empty())
    return error;
  if (command.document.default_spindle_rpm > limits.max_spindle_rpm)
    return {"Default spindle speed exceeds the configured machine limit.",
            MessageKey{"backend.operatorSettings.spindleSpeedLimit"}};
  if (command.document.default_jog_velocity > limits.max_jog_velocity)
    return {"Default jog speed exceeds the configured machine limit.",
            MessageKey{"backend.operatorSettings.jogSpeedLimit"}};
  return {};
}
auto decode_operator_preferences(std::string_view text)
    -> std::expected<OperatorPreferences, LocalizedReason> {
  return checked([&]() -> OperatorPreferences { return read_document(parse(text), Damaged::refuse); });
}
auto encode_operator_settings(const OperatorSettings& settings) -> EncodedMessage {
  return checked([&]() -> std::string {
    const bool available = settings.availability == ToolModelAvailability::available;
    if (settings.revision > max_tool_model_revision ||
        (settings.machine_id && !valid_machine_id(*settings.machine_id)) ||
        (available && (!settings.document || !settings.machine_id)) ||
        (!available && (settings.document || settings.revision != 0)))
      throw InvalidSettings(
          {"Invalid settings snapshot.", MessageKey{"backend.operatorSettings.invalidSnapshot"}});
    if (settings.document)
      if (auto error = operator_preferences_error(*settings.document); !error.empty())
        throw InvalidSettings(std::move(error));
    Json snapshot{
        {"type", "operator_settings"},
        {"machine_id", settings.machine_id ? Json(*settings.machine_id) : Json(nullptr)},
        {"revision", settings.revision},
        {"availability", available ? "available"
                         : settings.availability == ToolModelAvailability::session_only
                             ? "session_only"
                             : "unavailable"},
        {"reason", settings.reason},
        {"document", settings.document ? document_json(*settings.document) : Json(nullptr)}};
    if (const auto& key = settings.localization)
      snapshot["localization"] = message_key_json(*key);
    return snapshot.dump();
  });
}
auto encode_operator_settings_file(const OperatorSettingsFile& file) -> EncodedMessage {
  return checked([&]() -> std::string {
    if (!valid_machine_id(file.machine_id) || file.revision == 0 ||
        file.revision > max_tool_model_revision)
      throw InvalidSettings({"Invalid settings file identity or revision.",
                             MessageKey{"backend.operatorSettings.invalidFileIdentity"}});
    if (auto error = operator_preferences_error(file.document); !error.empty())
      throw InvalidSettings(std::move(error));
    // The reader takes at most 16 KiB: a longer file would leave the settings unavailable.
    auto text = Json{{"machine_id", file.machine_id},
                     {"revision", file.revision},
                     {"document", document_json(file.document, Language::leave_out)}}
                    .dump();
    if (text.size() > max_operator_settings_bytes)
      throw InvalidSettings(
          {"Settings exceed 16 KiB.", MessageKey{"backend.operatorSettings.tooLarge"}});
    return text;
  });
}
auto decode_operator_settings_file(std::string_view text)
    -> std::expected<OperatorSettingsFile, LocalizedReason> {
  return checked([&]() -> OperatorSettingsFile {
    const auto file = parse(text);
    exact(file, {"machine_id", "revision", "document"});
    const auto machine = file.at("machine_id").get<std::string>();
    const auto revision = tool_model_json::revision(file.at("revision"));
    if (!valid_machine_id(machine) || !revision || *revision == 0)
      throw InvalidSettings({"Invalid settings file identity or revision.",
                             MessageKey{"backend.operatorSettings.invalidFileIdentity"}});
    return {machine, *revision, read_document(file.at("document"), Damaged::fall_back)};
  });
}
auto encode_panel_language_file(std::string_view machine_id, PanelLanguage language)
    -> EncodedMessage {
  return checked([&]() -> std::string {
    if (!valid_machine_id(machine_id) || language_name(language).empty())
      throw InvalidSettings({"Invalid panel presentation.",
                             MessageKey{"backend.operatorSettings.invalidPresentation"}});
    return Json{{"machine_id", machine_id}, {"language", language_name(language)}}.dump();
  });
}
auto decode_panel_language_file(std::string_view text, std::string_view machine_id)
    -> std::expected<PanelLanguage, LocalizedReason> {
  return checked([&]() -> PanelLanguage {
    const auto file = parse(text);
    exact(file, {"machine_id", "language"});
    if (file.at("machine_id") != machine_id)
      throw InvalidSettings({"Invalid settings file identity or revision.",
                             MessageKey{"backend.operatorSettings.invalidFileIdentity"}});
    const auto known = std::ranges::find_if(language_names, [&](const auto& entry) -> bool {
      return file.at("language").is_string() && file.at("language") == entry.second;
    });
    if (known == language_names.end())
      throw InvalidSettings({"Invalid panel presentation.",
                             MessageKey{"backend.operatorSettings.invalidPresentation"}});
    return known->first;
  });
}
} // namespace coffeecnc
