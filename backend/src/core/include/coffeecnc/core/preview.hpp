#ifndef COFFEECNC_CORE_PREVIEW_HPP
#define COFFEECNC_CORE_PREVIEW_HPP

#include <coffeecnc/core/panel_messages.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace coffeecnc {

// What the interpreter starts a run of the program from. A preview is computed from the same
// state, so a change of any of it makes the preview stale.
struct InterpreterStart {
  int wcs{1}; // active work coordinate system, 1 is G54
  LengthUnit program_units{LengthUnit::mm};
  Vec3 position{}; // machine position the first move starts from, mm
  int tool_in_spindle{};
  std::optional<int> tool_prepared;
  Vec3 tool_offset{};
  // Active modal codes as LinuxCNC reports them (motion and distance mode, plane, feed mode, ...).
  std::vector<std::string> active_codes;
  // Settings the codes do not carry: the F and S words in effect.
  double feed{};
  double spindle_speed{};
  // Identity of the interpreter's parameter file contents (numbered and named parameters).
  std::uint64_t parameters_revision{};
  friend auto operator==(const InterpreterStart&, const InterpreterStart&) -> bool = default;
};

// One entry of LinuxCNC's loaded tool table with every field its interpreter uses, in machine
// units as LinuxCNC holds it. Index 0 is the spindle: the entry of the tool that is in it.
struct PreviewTool {
  int index{};
  int number{};
  int pocket{};
  Vec3 offset{};
  double diameter{};
  double front_angle{};
  double back_angle{};
  int orientation{};
  friend auto operator==(const PreviewTool&, const PreviewTool&) -> bool = default;
};

// Everything the path of a program depends on: the program, the work offsets with their
// rotations, G92, the tool table and the interpreter's start state.
struct PreviewInputs {
  std::optional<std::string> program_file;
  std::uint64_t program_revision{};
  // Goes up when a file the last preview was computed from (the program, a subroutine file, or
  // which file a call finds) is no longer what it was; see PreviewService.
  std::uint64_t files_generation{};
  std::array<WorkSystem, 9> systems{}; // G54 ... G59.3
  Vec3 g92{};
  // Number, pocket, length and diameter count, in table order. The comment does not.
  std::vector<Tool> tools;
  // The loaded table as a preview is computed with it: every entry with every field, the
  // spindle's included. Captured by the thread that reads the controller (the only one that
  // touches LinuxCNC's tool data) with the reading these inputs are of, counted in full for
  // the context, and handed to the engine as data: the engine reads no tool table.
  std::vector<PreviewTool> raw_tools;
  // Not part of the start state: the operator can change it during a pause, and it changes
  // which blocks the rest of the program runs.
  bool block_delete{};
  InterpreterStart start;
  std::optional<int> restart_line;
};

// Interpreter state immediately before a selected motion block. Linear values use mm.
struct RestartPoint {
  int line{};
  int tool{};
  int wcs{1};
  int h{};
  SpindleDirection spindle_direction{SpindleDirection::stopped};
  double spindle_rpm{};
  bool flood{};
  bool mist{};
  double feed{};
  LengthUnit units{LengthUnit::mm};
  int plane{17};
  bool incremental{};
  bool absolute_arc{};
  double clearance_z{};
  Vec3 start{};
  Vec3 g5x{};
  Vec3 g92{};
  Vec3 tool_offset{};
  double rotation{};
  friend auto operator==(const RestartPoint&, const RestartPoint&) -> bool = default;
};

// What a computation found: Preview without its context.
struct PreviewOutcome {
  PreviewState state{PreviewState::unavailable};
  std::optional<PreviewError> error;
  std::vector<PreviewSegment> segments;
  std::optional<Bounds> bounds;
  std::optional<RestartPoint> restart;
  std::string restart_blocked;
  friend auto operator==(const PreviewOutcome&, const PreviewOutcome&) -> bool = default;
};

// Extent of the feed moves; empty without any.
[[nodiscard]] inline auto feed_bounds(const std::vector<PreviewSegment>& segments)
    -> std::optional<Bounds> {
  std::optional<Bounds> bounds;
  for (const auto& segment : segments) {
    if (segment.kind != MoveKind::feed)
      continue;
    for (const auto& point : segment.points) {
      if (!bounds)
        bounds = Bounds{point, point};
      for (std::size_t axis = 0; axis < point.size(); ++axis) {
        bounds->min.at(axis) = std::min(bounds->min.at(axis), point.at(axis));
        bounds->max.at(axis) = std::max(bounds->max.at(axis), point.at(axis));
      }
    }
  }
  return bounds;
}

struct PreviewComputation {
  PreviewOutcome outcome;
  // The computation was given up because `cancelled` said so: the outcome is nothing.
  bool cancelled{};
  // Whether a file the outcome was computed from is no longer what it was. Empty when there is
  // nothing to watch. May read files: called only on the thread that called compute.
  // `cancelled` is asked as in compute; given up, the answer is false.
  using Watch = std::function<bool(const std::function<bool()>& cancelled)>;
  Watch files_changed;
};

// Computes the path of a program for one set of inputs. Blocking, for a thread of its own;
// `cancelled` is asked often and ends the computation early. Implementations need not be
// thread-safe: one thread calls compute and the files_changed it returned.
class PreviewEngine {
public:
  PreviewEngine() = default;
  virtual ~PreviewEngine() = default;
  [[nodiscard]] virtual auto compute(const PreviewInputs& inputs,
                                     const std::function<bool()>& cancelled)
      -> PreviewComputation = 0;

protected:
  PreviewEngine(const PreviewEngine&) = default;
  PreviewEngine(PreviewEngine&&) = default;
  auto operator=(const PreviewEngine&) -> PreviewEngine& = default;
  auto operator=(PreviewEngine&&) -> PreviewEngine& = default;
};

} // namespace coffeecnc

#endif // COFFEECNC_CORE_PREVIEW_HPP
