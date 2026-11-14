#ifndef COFFEECNC_APP_STATUS_PUBLISHER_HPP
#define COFFEECNC_APP_STATUS_PUBLISHER_HPP

#include <coffeecnc/core/panel_messages.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace coffeecnc {

// Everything the path of a program depends on, as protocol v1 lists it: the program, the work
// offsets with their rotations, G92 and the tool table.
struct PreviewInputs {
  std::optional<std::string> program_file;
  std::uint64_t program_revision{};
  std::array<WorkSystem, 9> systems{}; // G54 ... G59.3
  Vec3 g92{};
  // Number, pocket, length and diameter count, in table order. The comment does not.
  std::vector<Tool> tools;
};

// Status::preview_context for these inputs: a 64-bit FNV-1a hash of their values, folded to 53
// bits. Equal inputs give the same context in every process and on every host (0.0 and -0.0 are
// equal). Different inputs give a different context except for a hash collision, one in 2^53 per
// pair; no 53-bit value can do better without remembering earlier inputs.
[[nodiscard]] auto preview_context(const PreviewInputs& inputs) -> std::uint64_t;

// The panel's state at one reading. The publisher keeps none of the references.
struct PanelState {
  const Hello& hello;
  const Status& status;
  const ToolTable& tool_table;
  const WorkOffsets& work_offsets;
  const ProgramText* program{}; // null while no program is loaded: protocol v1 then has no message
  const Preview& preview;
};

// Turns successive readings into the protocol v1 messages to send to one panel. Call it once per
// status period, and only while the current page has sent `ready`.
//
// Every call sends a `status`, numbered from 1 and increasing by one. `tool_table`,
// `work_offsets`, `program` and `preview` are sent when they differ in any field from the one
// sent last. With `snapshots` (Outcome::send_snapshots, the page sent `ready`) `hello` and all of
// them are sent. Order: hello, tool_table, work_offsets, program, preview, status; the status is
// last so that its revisions and preview_context refer to messages already delivered.
//
// All or nothing: when one message cannot be encoded nothing is returned but the reason, no
// number is used up and the same messages are due on the next call. Comparing costs one pass over
// the program text and the preview points per call.
//
// Pure logic, NOT thread-safe.
class StatusPublisher {
public:
  [[nodiscard]] auto publish(const PanelState& state, bool snapshots)
      -> std::expected<std::vector<std::string>, std::string>;

private:
  std::uint64_t seq_{};
  std::optional<ToolTable> tool_table_;
  std::optional<WorkOffsets> work_offsets_;
  std::optional<ProgramText> program_;
  std::optional<Preview> preview_;
};

} // namespace coffeecnc

#endif // COFFEECNC_APP_STATUS_PUBLISHER_HPP
