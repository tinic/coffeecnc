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

OperatorSettingsArchive::OperatorSettingsArchive(ToolModelFiles& files, const Limits& limits)
    : files_(files), limits_(limits) {}
auto OperatorSettingsArchive::path(std::string_view name) const -> std::string {
  return location_.directory + '/' + std::string(name);
}
auto OperatorSettingsArchive::unavailable(std::string reason, MessageKey localization) const
    -> OperatorSettings {
  return {location_.machine_id, 0,  ToolModelAvailability::unavailable,
          std::move(reason),    {}, std::move(localization)};
}
void OperatorSettingsArchive::show(OperatorSettings settings) {
  auto encoded = encode_operator_settings(settings);
  if (!encoded) {
    settings = unavailable(
        std::format("Operator settings cannot be shown: {}", encoded.error().text),
        MessageKey{"backend.operatorSettingsStore.cannotShow", {{"detail", encoded.error().text}}});
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
  if (!result)
    return PanelLanguage::en;
  const auto& content = *result;
  if (!content || content->more)
    return PanelLanguage::en;
  return decode_panel_language_file(content->bytes, location_.machine_id.value_or(""))
      .value_or(PanelLanguage::en);
}
auto OperatorSettingsArchive::read() const -> OperatorSettings {
  auto loaded = read_settings();
  if (loaded.document)
    loaded.document->language = read_language();
  return loaded;
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
    return unavailable(std::format("{} cannot be read: {}", file, result.error()),
                       MessageKey{"backend.operatorSettingsStore.cannotRead",
                                  {{"file", file}, {"detail", result.error()}}});
  const auto& content = *result;
  if (!content) {
    OperatorPreferences defaults;
    defaults.default_jog_velocity = std::min(600.0, limits_.max_jog_velocity);
    return {location_.machine_id, 0, ToolModelAvailability::available, location_.reason, defaults};
  }
  if (content->more)
    return unavailable(std::format("{} exceeds 16 KiB. The file was left alone.", file),
                       MessageKey{"backend.operatorSettingsStore.fileTooLarge", {{"file", file}}});
  const auto saved = decode_operator_settings_file(content->bytes);
  if (!saved)
    return unavailable(
        std::format("{} is not usable: {} The file was left alone.", file, saved.error()),
        MessageKey{"backend.operatorSettingsStore.notUsable",
                   {{"file", file}, {"detail", saved.error()}}});
  if (saved->machine_id != location_.machine_id)
    return unavailable(
        std::format("{} belongs to another machine. The file was left alone.", file),
        MessageKey{"backend.operatorSettingsStore.otherMachineFile", {{"file", file}}});
  // Retain the saved document if INI limits shrink. The UI clamps effective startup values.
  return {location_.machine_id, saved->revision, ToolModelAvailability::available, location_.reason,
          saved->document};
}
auto OperatorSettingsArchive::apply(const SetOperatorSettings& command) -> ToolModelReply {
  if (settings_.availability != ToolModelAvailability::available)
    return {CommandOutcome::rejected, settings_.reason, false, settings_.localization};
  if (command.machine_id != location_.machine_id)
    return {CommandOutcome::rejected, "This settings change belongs to another machine.", false,
            MessageKey{"backend.operatorSettingsStore.otherMachineChange"}};
  if (auto error = operator_settings_command_error(command, limits_); !error.empty())
    return {CommandOutcome::rejected, std::move(error.text), false, std::move(error.localization)};
  if (read() != settings_) {
    const auto file = path(file_name);
    show(unavailable(
        std::format("{} changed outside CoffeeCNC. Nothing was saved. Restart to read it again.",
                    file),
        MessageKey{"backend.operatorSettingsStore.changedOutside", {{"file", file}}}));
    return {CommandOutcome::rejected, settings_.reason, true, settings_.localization};
  }
  if (command.base_revision != settings_.revision)
    return {CommandOutcome::rejected,
            "Settings changed since this editor opened. Nothing was saved.", true,
            MessageKey{"backend.operatorSettingsStore.changedSinceOpened"}};
  if (settings_.revision >= max_tool_model_revision)
    return {CommandOutcome::rejected, "Settings reached their last revision. Nothing was saved.",
            false, MessageKey{"backend.operatorSettingsStore.lastRevision"}};
  OperatorSettings next{location_.machine_id, settings_.revision + 1,
                        ToolModelAvailability::available, location_.reason, command.document};
  std::ranges::sort(next.document->enabled_wcs);
  const auto text =
      encode_operator_settings_file({command.machine_id, next.revision, *next.document});
  if (!text)
    return {CommandOutcome::rejected, text.error().text, false};
  const bool language_changed =
      settings_.document && settings_.document->language != command.document.language;
  if (const auto step = replace(file_name, temporary_name, *text); !step)
    return {CommandOutcome::failed,
            std::format("Settings were not saved: {}. The saved file is unchanged.", step.error()),
            false,
            MessageKey{"backend.operatorSettingsStore.notSaved", {{"detail", step.error()}}}};
  const auto uncertain = [&](const std::string& detail) -> ToolModelReply {
    show(read());
    return {CommandOutcome::failed,
            std::format(
                "The save outcome is uncertain: {}. Displayed settings were read back from disk.",
                detail),
            true, MessageKey{"backend.operatorSettingsStore.uncertain", {{"detail", detail}}}};
  };
  // Only a change of language writes the language file, after the settings file.
  if (language_changed) {
    const auto language = encode_panel_language_file(command.machine_id, command.document.language);
    const auto step = language ? replace(language_name, language_temporary_name, *language)
                               : ToolModelFiles::Done{std::unexpected(language.error().text)};
    if (!step)
      return uncertain(step.error());
  }
  if (const auto durable = files_.sync_directory(location_.directory); !durable)
    return uncertain(durable.error());
  show(std::move(next));
  return {CommandOutcome::accepted, {}, true};
}
} // namespace coffeecnc
