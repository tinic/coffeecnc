#include <coffeecnc/app/tool_model_store.hpp>

#include <algorithm>
#include <format>
#include <utility>

namespace coffeecnc {
namespace {
constexpr std::string_view file_name = "operator-settings.json";
constexpr std::string_view temporary_name = "operator-settings.json.tmp";
// The panel language, apart from the settings file (encode_panel_language_file).
constexpr std::string_view language_name = "panel-language.json";
constexpr std::string_view language_temporary_name = "panel-language.json.tmp";
} // namespace

OperatorSettingsArchive::OperatorSettingsArchive(ToolModelFiles& files, Limits limits)
    : files_(files), limits_(limits) {}
auto OperatorSettingsArchive::path(std::string_view name) const -> std::string {
  return location_.directory + '/' + std::string(name);
}
auto OperatorSettingsArchive::unavailable(std::string reason) const -> OperatorSettings {
  return {location_.machine_id, 0, ToolModelAvailability::unavailable, std::move(reason), {}};
}
void OperatorSettingsArchive::show(OperatorSettings settings) {
  auto encoded = encode_operator_settings(settings);
  if (!encoded) {
    settings = unavailable("Operator settings cannot be shown: " + encoded.error());
    encoded = encode_operator_settings(settings);
  }
  settings_ = std::move(settings);
  snapshot_ = encoded.value_or(std::string());
}
void OperatorSettingsArchive::open(ToolModelLocation location) {
  location_ = std::move(location);
  if (location_.availability != ToolModelAvailability::available) {
    show({location_.machine_id, 0, location_.availability, location_.reason, {}});
    return;
  }
  show(read());
}
// The language file is optional: missing, unreadable or damaged, the panel is in English.
auto OperatorSettingsArchive::read_language() const -> PanelLanguage {
  const auto result = files_.read(path(language_name), max_panel_language_bytes);
  if (!result || !*result || (*result)->more)
    return PanelLanguage::en;
  return decode_panel_language_file((*result)->bytes, location_.machine_id.value_or(""))
      .value_or(PanelLanguage::en);
}
auto OperatorSettingsArchive::read() const -> OperatorSettings {
  auto settings = read_settings();
  if (settings.document)
    settings.document->language = read_language();
  return settings;
}
auto OperatorSettingsArchive::replace(std::string_view name, std::string_view temporary,
                                      const std::string& text) -> ToolModelFiles::Done {
  const auto temporary_path = path(temporary);
  auto step = files_.open_temporary(temporary_path);
  if (step)
    step = files_.write(text);
  if (step)
    step = files_.sync();
  if (step)
    step = files_.rename(temporary_path, path(name));
  if (!step)
    files_.discard(temporary_path);
  return step;
}
auto OperatorSettingsArchive::read_settings() const -> OperatorSettings {
  const auto file = path(file_name);
  const auto result = files_.read(file, max_operator_settings_bytes);
  if (!result)
    return unavailable(file + " cannot be read: " + result.error());
  if (!*result) {
    OperatorPreferences defaults;
    defaults.default_jog_velocity = std::min(600.0, limits_.max_jog_velocity);
    return {location_.machine_id, 0, ToolModelAvailability::available, location_.reason, defaults};
  }
  if ((*result)->more)
    return unavailable(file + " exceeds 16 KiB. The file was left alone.");
  const auto saved = decode_operator_settings_file((*result)->bytes);
  if (!saved)
    return unavailable(file + " is not usable: " + saved.error() + " The file was left alone.");
  if (saved->machine_id != location_.machine_id)
    return unavailable(file + " belongs to another machine. The file was left alone.");
  // Retain the saved document if INI limits shrink. The UI clamps effective startup values.
  return {location_.machine_id, saved->revision, ToolModelAvailability::available, location_.reason,
          saved->document};
}
auto OperatorSettingsArchive::apply(const SetOperatorSettings& command) -> ToolModelReply {
  if (settings_.availability != ToolModelAvailability::available)
    return {CommandOutcome::rejected, settings_.reason, false};
  if (command.machine_id != location_.machine_id)
    return {CommandOutcome::rejected, "This settings change belongs to another machine.", false};
  if (auto error = operator_settings_command_error(command, limits_); !error.empty())
    return {CommandOutcome::rejected, std::move(error), false};
  if (read() != settings_) {
    show(unavailable(path(file_name) +
                     " changed outside CoffeeCNC. Nothing was saved. Restart to read it again."));
    return {CommandOutcome::rejected, settings_.reason, true};
  }
  if (command.base_revision != settings_.revision)
    return {CommandOutcome::rejected,
            "Settings changed since this editor opened. Nothing was saved.", true};
  if (settings_.revision >= max_tool_model_revision)
    return {CommandOutcome::rejected, "Settings reached their last revision. Nothing was saved.",
            false};
  OperatorSettings next{location_.machine_id, settings_.revision + 1,
                        ToolModelAvailability::available, location_.reason, command.document};
  std::ranges::sort(next.document->enabled_wcs);
  const auto text =
      encode_operator_settings_file({command.machine_id, next.revision, *next.document});
  if (!text)
    return {CommandOutcome::rejected, text.error(), false};
  if (const auto step = replace(file_name, temporary_name, *text); !step)
    return {CommandOutcome::failed,
            "Settings were not saved: " + step.error() + ". The saved file is unchanged.", false};
  // Only a change of language writes the language file, after the settings file.
  if (next.document->language != settings_.document->language) {
    const auto language = encode_panel_language_file(command.machine_id, next.document->language);
    const auto step = language ? replace(language_name, language_temporary_name, *language)
                               : ToolModelFiles::Done{std::unexpected(language.error())};
    if (!step) {
      show(read());
      return {CommandOutcome::failed,
              "The save outcome is uncertain: " + step.error() +
                  ". Displayed settings were read back from disk.",
              true};
    }
  }
  if (const auto durable = files_.sync_directory(location_.directory); !durable) {
    show(read());
    return {CommandOutcome::failed,
            "The save outcome is uncertain: " + durable.error() +
                ". Displayed settings were read back from disk.",
            true};
  }
  show(std::move(next));
  return {CommandOutcome::accepted, {}, true};
}
} // namespace coffeecnc
