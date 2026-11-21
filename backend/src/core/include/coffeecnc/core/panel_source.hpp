#ifndef COFFEECNC_CORE_PANEL_SOURCE_HPP
#define COFFEECNC_CORE_PANEL_SOURCE_HPP

#include <coffeecnc/core/controller_port.hpp>
#include <coffeecnc/core/panel_messages.hpp>
#include <coffeecnc/core/preview.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace coffeecnc {

// Everything the panel is shown, as the controller adapter read it at one moment.
struct PanelReading {
  // `preview_context` is left 0: the publisher numbers the preview inputs.
  Status status;
  ToolTable tool_table;
  // Set while the source shows no rows although a table is accepted (its comments could not
  // be read): the accepted rows, which count for the preview context in place of the empty
  // table shown. Their comments are not to be shown.
  std::optional<std::vector<Tool>> accepted_tools;
  // G54 ... G59.3, each marked available or not. LinuxCNC's status carries the active system
  // alone; the others are not available while task runs and hold zero placeholders.
  std::array<WorkSystem, 9> work_systems{};
  // The text of the program `status.program` names, with its revision. Null while no program is
  // loaded and when the text cannot be read.
  std::shared_ptr<const ProgramText> program;
  // The loaded tool table with every field a preview is computed with, the spindle's entry
  // included, in machine units; empty where the source has none.
  std::vector<PreviewTool> preview_tools;
  // False while a pass over the tool table that differs from preview_tools waits to be
  // accepted: the machine's state is then not one coherent set, and no preview input is taken
  // from it.
  bool tools_settled{true};
  // The controller can finish interpreting while there are still commands to execute.
  bool task_executing{};
  // What the source itself has to tell the operator (a file it cannot read), each once.
  std::vector<ControllerMessage> messages;
  // The point task plans from when the axes are at rest, independent of encoder backlash.
  std::optional<Vec3> commanded_position{};
};

// The text of a program for the `program` message: ProgramStore::read_program, handed to an
// adapter that must not depend on the app layer.
using ProgramReader =
    std::function<std::expected<ProgramText, std::string>(std::string_view path,
                                                          std::uint64_t revision)>;

// A read-only view of the controller for the publisher thread, its only caller. Never touches
// the command or error channel and no HAL pin. Implementations need not be thread-safe.
class PanelSource {
public:
  PanelSource() = default;
  virtual ~PanelSource() = default;

  // What does not change while the machine runs.
  [[nodiscard]] virtual auto hello() const -> Hello = 0;
  // May block as long as the controller holds its status buffer.
  [[nodiscard]] virtual auto read() -> PanelReading = 0;

protected:
  PanelSource(const PanelSource&) = default;
  PanelSource(PanelSource&&) = default;
  auto operator=(const PanelSource&) -> PanelSource& = default;
  auto operator=(PanelSource&&) -> PanelSource& = default;
};

} // namespace coffeecnc

#endif // COFFEECNC_CORE_PANEL_SOURCE_HPP
