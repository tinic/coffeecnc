#ifndef COFFEECNC_APP_MESSAGES_HPP
#define COFFEECNC_APP_MESSAGES_HPP

#include <coffeecnc/core/diagnostics.hpp>
#include <coffeecnc/core/panel_messages.hpp>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace coffeecnc {

// Wire text of one protocol v1 machine-to-panel message, or why it cannot be sent.
//
// Refused, never repaired: a number that is not finite (JSON has no NaN or infinity, and a silent
// null or zero would be shown as a position), a counter above 2^53 - 1, a negative
// SpindleStatus::actual_rpm, and a Hello axis without a positive axis_max_jog_velocity. The error
// names the field. Text that is not UTF-8 is replaced, as in encode_result.
//
// The optional signals (spindle.actual_rpm, spindle.at_speed, probe_input) are always present,
// `null` when the struct has none. `truncated` of ProgramText and Directory is always sent.
// `available` of every work system is always sent.
using Encoded = std::expected<std::string, std::string>;

[[nodiscard]] auto encode_hello(const Hello& hello) -> Encoded;
// `seq` must increase by one with every status sent; see StatusPublisher.
[[nodiscard]] auto encode_status(const Status& status, std::uint64_t seq) -> Encoded;
[[nodiscard]] auto encode_tool_table(const ToolTable& table) -> Encoded;
[[nodiscard]] auto encode_work_offsets(const WorkOffsets& offsets) -> Encoded;
[[nodiscard]] auto encode_program(const ProgramText& program) -> Encoded;
[[nodiscard]] auto encode_preview(const Preview& preview) -> Encoded;
[[nodiscard]] auto encode_directory(const Directory& directory) -> Encoded;
// `before` and `after` of a change are `null` when the history has none; `restores` likewise.
[[nodiscard]] auto encode_offset_history(const OffsetHistory& history) -> Encoded;

// `value` is a boolean, a number or `null` (missing, or a float that is not finite); `joint`
// is sent on the joint rows only, `label` on the configured rows only.
[[nodiscard]] auto encode_diagnostics(const Diagnostics& diagnostics) -> Encoded;
// The wire names of the diagnostics message.
[[nodiscard]] auto diagnostic_name(DiagnosticRole role) -> std::string_view;
[[nodiscard]] auto diagnostic_name(DiagnosticState state) -> std::string_view;

// The wire names of the offset history, which its file shares.
[[nodiscard]] auto history_name(HistorySource source) -> std::string_view;
[[nodiscard]] auto history_name(HistoryTarget target) -> std::string_view;
[[nodiscard]] auto history_name(HistoryField field) -> std::string_view;

} // namespace coffeecnc

#endif // COFFEECNC_APP_MESSAGES_HPP
