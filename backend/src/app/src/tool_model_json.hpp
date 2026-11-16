#ifndef COFFEECNC_APP_TOOL_MODEL_JSON_HPP
#define COFFEECNC_APP_TOOL_MODEL_JSON_HPP

#include <coffeecnc/core/tool_models.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <expected>
#include <string>

// The JSON forms the saved file and the panel's two commands share. Each reads one value and
// accepts nothing but its exact shape: a missing field, an unknown one and a value of another
// type all fail, with the reason. Ranges of dimensions are not judged here (tool_models.hpp).
namespace coffeecnc::tool_model_json {

// Seven cutter families plus the probe. Legacy documents supply only cutter families.
[[nodiscard]] auto defaults(const nlohmann::json& value)
    -> std::expected<ToolDefaults, std::string>;
// Overrides by family, without diameter. Family-specific validation follows decoding.
[[nodiscard]] auto overrides(const nlohmann::json& value)
    -> std::expected<ToolOverrides, std::string>;
// number and pocket as integers of 32 bits, diameter, comment.
[[nodiscard]] auto binding(const nlohmann::json& value)
    -> std::expected<ToolBinding, std::string>;
[[nodiscard]] auto family(const nlohmann::json& value) -> std::expected<ToolFamily, std::string>;
// An integer from 0 to 2^53 - 1.
[[nodiscard]] auto revision(const nlohmann::json& value)
    -> std::expected<std::uint64_t, std::string>;

} // namespace coffeecnc::tool_model_json

#endif // COFFEECNC_APP_TOOL_MODEL_JSON_HPP
