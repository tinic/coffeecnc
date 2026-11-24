#ifndef COFFEECNC_APP_OPERATOR_SETTINGS_HPP
#define COFFEECNC_APP_OPERATOR_SETTINGS_HPP

#include <coffeecnc/app/messages.hpp>
#include <coffeecnc/core/operator_settings.hpp>

namespace coffeecnc {

inline constexpr std::size_t max_operator_settings_bytes = 16384;
[[nodiscard]] auto operator_preferences_error(const OperatorPreferences& document)
    -> LocalizedReason;
[[nodiscard]] auto operator_settings_command_error(const SetOperatorSettings& command,
                                                   const Limits& limits) -> LocalizedReason;
[[nodiscard]] auto decode_operator_preferences(std::string_view text)
    -> std::expected<OperatorPreferences, LocalizedReason>;
[[nodiscard]] auto encode_operator_settings(const OperatorSettings& settings) -> Encoded;

struct OperatorSettingsFile {
  std::string machine_id;
  std::uint64_t revision{};
  OperatorPreferences document;
};
[[nodiscard]] auto encode_operator_settings_file(const OperatorSettingsFile& file) -> Encoded;
[[nodiscard]] auto decode_operator_settings_file(std::string_view text)
    -> std::expected<OperatorSettingsFile, std::string>;

// The panel language is kept in its own small file, `{machine_id, language}`, next to the settings
// file: the settings file stays in the shape that earlier versions read, so that a downgrade keeps
// the operator's settings. A missing or damaged language file means English.
inline constexpr std::size_t max_panel_language_bytes = 1024;
[[nodiscard]] auto encode_panel_language_file(std::string_view machine_id, PanelLanguage language)
    -> Encoded;
[[nodiscard]] auto decode_panel_language_file(std::string_view text, std::string_view machine_id)
    -> std::expected<PanelLanguage, std::string>;

} // namespace coffeecnc
#endif
