#include <coffeecnc/app/tool_model_store.hpp>

#include <algorithm>
#include <format>
#include <utility>

namespace coffeecnc {
namespace {
constexpr std::string_view file_name = "operator-settings.json";
constexpr std::string_view temporary_name = "operator-settings.json.tmp";
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
auto OperatorSettingsArchive::read() const -> OperatorSettings {
  const auto file = path(file_name);
  const auto result = files_.read(file, max_operator_settings_bytes);
  if (!result)
    return unavailable(file + " cannot be read: " + result.error());
  const auto& content = *result;
  if (!content) {
    OperatorPreferences defaults;
    defaults.default_jog_velocity = std::min(600.0, limits_.max_jog_velocity);
    return {location_.machine_id, 0, ToolModelAvailability::available, location_.reason, defaults};
  }
  if (content->more)
    return unavailable(file + " exceeds 16 KiB. The file was left alone.");
  const auto saved = decode_operator_settings_file(content->bytes);
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
  const auto temporary = path(temporary_name);
  auto step = files_.open_temporary(temporary);
  if (step)
    step = files_.write(*text);
  if (step)
    step = files_.sync();
  if (step)
    step = files_.rename(temporary, path(file_name));
  if (!step) {
    files_.discard(temporary);
    return {CommandOutcome::failed,
            "Settings were not saved: " + step.error() + ". The saved file is unchanged.", false};
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
