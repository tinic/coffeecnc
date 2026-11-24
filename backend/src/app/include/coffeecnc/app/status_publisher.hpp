#ifndef COFFEECNC_APP_STATUS_PUBLISHER_HPP
#define COFFEECNC_APP_STATUS_PUBLISHER_HPP

#include <coffeecnc/core/panel_messages.hpp>
#include <coffeecnc/core/preview.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace coffeecnc {

// Status::preview_context: a number that changes exactly when the inputs of a preview change.
// It counts from 1 and goes up by one with every change, so two different sets of inputs never
// share a context and nothing depends on a hash. 0.0 and -0.0 are the same value.
//
// While a program runs, the machine's state is the program's doing so far: a G92, a G10 L2, a
// tool change of the program itself change offsets and tool table, and a preview computed
// from them would replay the program from its first line with values that exist only because
// part of it has run. With `running` every input that is machine state stays what it was
// before the run: the start state, the work systems, G92, the tool table. The preview of the
// context of the run's start stays the current one for the whole run, paused included, and
// what the machine is when the run has ended counts then, as one new context. A change made
// from outside the program during the run (another client's G10 in a pause) cannot be told
// from the program's own and is frozen with it: it counts when the run has ended.
//
// What is not machine state counts at once also while running: another program or a reload,
// a changed file (files_generation), and block delete, which the operator can switch in a
// pause.
//
// Pure logic, NOT thread-safe.
class PreviewContext {
public:
  [[nodiscard]] auto update(const PreviewInputs& inputs, bool running) -> std::uint64_t;
  // The inputs of the context: those given last, with the machine state that is in force.
  // Empty inputs before the first update.
  [[nodiscard]] auto inputs() const -> const PreviewInputs& {
    static const PreviewInputs none;
    return last_ ? *last_ : none;
  }

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
  // Null without a history store: no `offset_history` is sent.
  const OffsetHistory* offset_history{};
};

// Turns successive readings into the protocol v1 messages to send to one panel. Call it once per
// status period, and only while the current page has sent `ready`.
//
// Every call sends a `status`, numbered from 1 and increasing by one. `tool_table`,
// `work_offsets`, `program` and `preview` are sent when they differ in any field from the one
// sent last; `offset_history` when its revision does. With `snapshots` (Outcome::send_snapshots,
// the page sent `ready`) `hello` and all of them are sent. Order: hello, tool_table,
// work_offsets, offset_history, program, preview, status; the status is last so that its
// revisions and preview_context refer to messages already delivered.
//
// All or nothing: when one message cannot be encoded nothing is returned but the reason, no
// number is used up and the same messages are due on the next call. Comparing costs one pass over
// the program text and the preview points per call.
//
// Pure logic, NOT thread-safe.
enum class PanelRecord : std::uint8_t {
  hello, tool_table, work_offsets, offset_history, program, preview, status, message
};

class StatusPublisher {
public:
  [[nodiscard]] auto publish(const PanelState& state, bool snapshots,
                             std::vector<PanelRecord>* records = nullptr)
      -> std::expected<std::vector<std::string>, LocalizedReason>;

private:
  std::uint64_t seq_{};
  std::optional<ToolTable> tool_table_;
  std::optional<WorkOffsets> work_offsets_;
  std::optional<std::uint64_t> history_revision_;
  std::optional<ProgramText> program_;
  std::optional<Preview> preview_;
};

} // namespace coffeecnc

#endif // COFFEECNC_APP_STATUS_PUBLISHER_HPP
