#ifndef COFFEECNC_APP_TOOL_MODELS_HPP
#define COFFEECNC_APP_TOOL_MODELS_HPP

#include <coffeecnc/core/message_key.hpp>
#include <coffeecnc/core/tool_models.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace coffeecnc {

// Rules and wire text of the tool models; docs/design/native-tool-model-storage.md. No I/O.

inline constexpr int tool_model_version = 1;
// The saved file and the `tool_models` message, each as encoded.
inline constexpr std::size_t max_tool_model_bytes = 1048576;
inline constexpr std::size_t max_tool_model_assignments = 1001;
inline constexpr std::uint64_t max_tool_model_revision = 9007199254740991; // 2^53 - 1

[[nodiscard]] auto tool_family_name(ToolFamily family) -> std::string_view;
[[nodiscard]] auto tool_family(std::string_view name) -> std::optional<ToolFamily>;

// The values of factoryToolDefaults in ui/src/tool-model.ts.
[[nodiscard]] auto factory_tool_defaults() -> ToolDefaults;

// Each of these: empty when valid, else the reason in words for the operator.
//
// toolDimensionError of ui/src/tool-model.ts, rule for rule and with its texts: the five
// lengths 0.001 to 10000 mm, the angle 1 to 179 degrees, cutting length within stick-out within
// overall length and below it, a ball's radius and a pointed tip within the cutting length.
[[nodiscard]] auto tool_dimension_error(ToolFamily family, const ToolDimensions& dimensions)
    -> LocalizedReason;
// All seven families; the reason names the family.
[[nodiscard]] auto tool_defaults_error(const ToolDefaults& defaults) -> LocalizedReason;
// Each field by itself, in the range of its kind. Whether the resolved model is valid depends
// on the family defaults and the tool table of the moment and is not decided here.
[[nodiscard]] auto tool_overrides_error(const ToolOverrides& overrides, ToolFamily family)
    -> LocalizedReason;
// A tool number above zero, a pocket not below zero, a finite diameter not below zero, a
// comment of at most max_tool_comment_bytes bytes without NUL, line feed or carriage return.
[[nodiscard]] auto tool_binding_error(const ToolBinding& tool) -> LocalizedReason;
// All of the above, at most max_tool_model_assignments assignments, their tool numbers unique.
[[nodiscard]] auto tool_document_error(const ToolModelDocument& document) -> LocalizedReason;
// What a command carries: its defaults, or its tool row and overrides. A removal (no family)
// must come with empty overrides.
[[nodiscard]] auto tool_model_command_error(const ToolModelCommand& command) -> LocalizedReason;

// Wire text of the `tool_models` message. Refused: a number that is not finite, a revision
// above 2^53 - 1, an available snapshot without machine ID or document, any other with a
// document or a revision, and a text above max_tool_model_bytes.
[[nodiscard]] auto encode_tool_models(const ToolModels& models)
    -> std::expected<std::string, LocalizedReason>;
// Older panels validate the exact tool_models shape. Send localization after the snapshot.
[[nodiscard]] auto encode_tool_models_reason(const ToolModels& models)
    -> std::optional<std::string>;

// The saved file: `{"machine_id", "revision", "document"}`.
struct ToolModelFile {
  std::string machine_id;
  std::uint64_t revision{};
  ToolModelDocument document;
  friend auto operator==(const ToolModelFile&, const ToolModelFile&) -> bool = default;
};
// Refused like encode_tool_models. Numbers are written so that reading gives the same double.
[[nodiscard]] auto encode_tool_model_file(const ToolModelFile& file)
    -> std::expected<std::string, LocalizedReason>;
// The file's text, at most max_tool_model_bytes. Fails with the reason, which does not name the
// file: not JSON, a duplicate key, a field missing, unknown or of another type, a version other
// than 1 ("version N is not supported" for a higher one), anything tool_document_error
// refuses, a machine ID that is not valid. The assignments come back in rising tool order.
[[nodiscard]] auto decode_tool_model_file(std::string_view text)
    -> std::expected<ToolModelFile, LocalizedReason>;

} // namespace coffeecnc

#endif // COFFEECNC_APP_TOOL_MODELS_HPP
