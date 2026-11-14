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

// What the interpreter starts a run of the program from. A preview is computed from the same
// state, so a change of any of it makes the preview stale.
struct InterpreterStart {
  int wcs{1}; // active work coordinate system, 1 is G54
  LengthUnit program_units{LengthUnit::mm};
  bool block_delete{};
  int tool_in_spindle{};
  Vec3 tool_offset{};
  // Active modal codes as LinuxCNC reports them (distance mode, plane, feed mode, ...).
  std::vector<std::string> active_codes;
  friend auto operator==(const InterpreterStart&, const InterpreterStart&) -> bool = default;
};

// Everything the path of a program depends on: the program, the work offsets with their
// rotations, G92, the tool table and the interpreter's start state.
struct PreviewInputs {
  std::optional<std::string> program_file;
  std::uint64_t program_revision{};
  std::array<WorkSystem, 9> systems{}; // G54 ... G59.3
  Vec3 g92{};
  // Number, pocket, length and diameter count, in table order. The comment does not.
  std::vector<Tool> tools;
  InterpreterStart start;
};

// Status::preview_context: a number that changes exactly when the inputs of a preview change.
// It counts from 1 and goes up by one with every change, so two different sets of inputs never
// share a context and nothing depends on a hash. 0.0 and -0.0 are the same value.
//
// While a program runs the interpreter's state is the program's doing, not a new start state:
// with `running` the start state seen last before the run stays in force, and a change of it
// counts once the run has ended. The other inputs count at once in either case.
//
// Pure logic, NOT thread-safe.
class PreviewContext {
public:
  [[nodiscard]] auto update(const PreviewInputs& inputs, bool running) -> std::uint64_t;

private:
  std::optional<PreviewInputs> last_;
  std::uint64_t context_{};
};

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
