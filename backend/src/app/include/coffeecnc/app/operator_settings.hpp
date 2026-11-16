#ifndef COFFEECNC_APP_OPERATOR_SETTINGS_HPP
#define COFFEECNC_APP_OPERATOR_SETTINGS_HPP

#include <coffeecnc/app/messages.hpp>
#include <coffeecnc/core/operator_settings.hpp>

namespace coffeecnc {

inline constexpr std::size_t max_operator_settings_bytes = 16384;
[[nodiscard]] auto operator_preferences_error(const OperatorPreferences& document) -> std::string;
[[nodiscard]] auto operator_settings_command_error(const SetOperatorSettings& command,
                                                   const Limits& limits) -> std::string;
[[nodiscard]] auto decode_operator_preferences(std::string_view text)
    -> std::expected<OperatorPreferences, std::string>;
[[nodiscard]] auto encode_operator_settings(const OperatorSettings& settings) -> Encoded;

struct OperatorSettingsFile {
  std::string machine_id;
  std::uint64_t revision{};
  OperatorPreferences document;
};
[[nodiscard]] auto encode_operator_settings_file(const OperatorSettingsFile& file) -> Encoded;
[[nodiscard]] auto decode_operator_settings_file(std::string_view text)
    -> std::expected<OperatorSettingsFile, std::string>;

} // namespace coffeecnc
#endif
