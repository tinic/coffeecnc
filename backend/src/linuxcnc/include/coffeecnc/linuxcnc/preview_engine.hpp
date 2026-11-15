#ifndef COFFEECNC_LINUXCNC_PREVIEW_ENGINE_HPP
#define COFFEECNC_LINUXCNC_PREVIEW_ENGINE_HPP

#include <coffeecnc/core/preview.hpp>
#include <coffeecnc/linuxcnc/machine_config.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace coffeecnc {

// One entry of LinuxCNC's loaded tool table with every field the interpreter uses, in machine
// units. Index 0 is the spindle.
struct RawTool {
  int index{};
  int number{};
  int pocket{};
  Vec3 offset{};
  double diameter{};
  double front_angle{};
  double back_angle{};
  int orientation{};
  friend auto operator==(const RawTool&, const RawTool&) -> bool = default;
};

// What a preview needs of the machine besides the inputs of one computation.
struct PreviewSettings {
  std::string helper;         // the coffeecnc-preview program
  std::string work_directory; // a private directory per computation is made in it
  // Where the interpreter looks for a subroutine file `name.ngc`, in its order: the directory
  // task runs in (the configuration's), [DISPLAY] PROGRAM_PREFIX, [RS274NGC] SUBROUTINE_PATH.
  std::vector<std::string> search_path;
  std::string parameter_file; // [RS274NGC] PARAMETER_FILE; may be empty or missing
  double units_per_mm{1};     // machine units per millimetre
  bool tool_change_at_g30{};  // [EMCIO] TOOL_CHANGE_AT_G30
  bool tool_change_quill_up{}; // [EMCIO] TOOL_CHANGE_QUILL_UP
  std::optional<Vec3> tool_change_position; // [EMCIO] TOOL_CHANGE_POSITION, mm
  // [RS274NGC] keys handed to the helper's interpreter as they are, `KEY = value` lines.
  std::vector<std::string> interpreter_options;
  // Why no preview is computed on this machine at all; empty when one is.
  std::string refusal;

  std::chrono::milliseconds timeout{20000};
  std::size_t max_points{400000};
  double chord_tolerance{0.01}; // mm
  // The program and its subroutine files together.
  std::size_t max_input_bytes{std::size_t{16} * 1024 * 1024};
  std::size_t max_files{64};
  std::size_t max_diagnostics_bytes{std::size_t{64} * 1024};
  int attempts{3}; // computations for one result while the files keep changing
};

// The settings of a machine: its INI file read for what the interpreter and task's canon take
// from it. A machine with [RS274NGC] REMAP, with a random tool changer, or one whose helper
// program is missing gets a `refusal`.
[[nodiscard]] auto load_preview_settings(const MachineConfig& config, std::string helper,
                                         std::string work_directory) -> PreviewSettings;

// The preview of a program by LinuxCNC's own interpreter, in a helper process per computation
// (src/linuxcnc/preview/preview_helper.cpp).
//
// One computation:
//
//   snapshot  the program task names and every subroutine file its `o<name> call` blocks reach
//             are read once, within max_input_bytes and max_files, and written to a private
//             directory. The content manifest keeps the bytes read, and for every called name
//             which file the search path gave, or that none did.
//   start     the parameter file is copied and the live work system, its rotation and G92 are
//             written over the copy; a private INI file names only the private directory;
//             task's modal state becomes blocks to replay (units, plane, distance modes, feed
//             mode, retract mode, motion mode G0 or G1)
//   helper    started with a private working directory, no descriptor but its three standard
//             ones and the result pipe, a small environment, CPU, memory and file size limits,
//             lower priority, in a process group of its own; ended with SIGKILL after
//             `timeout`, when `cancelled` says so, or when it has written more than the points
//             allow
//   check     before the result is returned the originals are compared, byte for byte, with
//             the manifest, and every call is searched again. A difference discards the
//             result and starts over, `attempts` times in all; after that the outcome is
//             `failed`.
//
// files_changed of the result makes the same comparison again: at most max_input_bytes read
// and (max_files x search path) lookups per call. It binds the preview to the files; it does
// not bind what LinuxCNC runs: task opens the program by its path when it runs it.
//
// Outcomes: `ready`; `failed` with the line and the text for an interpreter error, a block the
// preview does not go past, a limit, the timeout, files that keep changing; `unavailable` with
// the text when no preview is computed at all (the refusal of the settings, no program, a
// start state that cannot be replayed, the tool table unreadable).
//
// NOT thread-safe: one thread, the preview worker's.
class HelperPreviewEngine final : public PreviewEngine {
public:
  using ToolReader = std::function<std::optional<std::vector<RawTool>>()>;
  // For tests: called after the snapshot was taken and before the helper is started, with
  // the number of the attempt, from 1.
  using AfterSnapshot = std::function<void(int attempt)>;

  HelperPreviewEngine(PreviewSettings settings, ToolReader tools,
                      AfterSnapshot after_snapshot = {});

  [[nodiscard]] auto compute(const PreviewInputs& inputs, const std::function<bool()>& cancelled)
      -> PreviewComputation override;

  // Helper processes started so far.
  [[nodiscard]] auto helper_runs() const -> std::size_t { return runs_; }
  // What the last helper wrote to its standard output and error, cut at max_diagnostics_bytes.
  [[nodiscard]] auto diagnostics() const -> const std::string& { return diagnostics_; }

private:
  PreviewSettings settings_;
  ToolReader tools_;
  AfterSnapshot after_snapshot_;
  std::size_t runs_{};
  std::string diagnostics_;
};

} // namespace coffeecnc

#endif // COFFEECNC_LINUXCNC_PREVIEW_ENGINE_HPP
