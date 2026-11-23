#ifndef COFFEECNC_LINUXCNC_PREVIEW_ENGINE_HPP
#define COFFEECNC_LINUXCNC_PREVIEW_ENGINE_HPP

#include <coffeecnc/core/preview.hpp>
#include <coffeecnc/linuxcnc/machine_config.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace coffeecnc {

// The name the adapter uses for an entry of the loaded tool table with every field.
using RawTool = PreviewTool;

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
  // The codes [RS274NGC] REMAP names, lower case ("m6", "t", "g88.1"): the preview ends at
  // the first block that uses one.
  std::vector<std::string> remapped;
  // Why no preview is computed on this machine at all; empty when one is.
  std::string refusal;
  std::optional<Bounds> soft_travel{};
  std::optional<Bounds> physical_travel{};
  double max_spindle_rpm{};
  std::string tool_table;
  std::optional<int> reference_tool{};
  std::string touched_off_file{};

  std::chrono::milliseconds timeout{20000};
  // How long the helper may take to exit after it has closed its result.
  std::chrono::milliseconds exit_grace{2000};
  std::size_t max_points{400000};
  double chord_tolerance{0.01}; // mm
  // The program and its subroutine files together.
  std::size_t max_input_bytes{std::size_t{16} * 1024 * 1024};
  std::size_t max_files{64};
  // Different subroutine names called, found or not.
  std::size_t max_calls{256};
  std::size_t max_diagnostics_bytes{std::size_t{64} * 1024};
  int attempts{3}; // computations for one result while the files keep changing
};

// The settings of a machine: its INI file read for what the interpreter and task's canon take
// from it. A machine with a random tool changer, with NO_DOWNCASE_OWORD, on rotary joints, or
// one whose helper program is missing gets a `refusal`. [RS274NGC] REMAP is not a refusal: the
// remapped codes are `remapped`.
[[nodiscard]] auto load_preview_settings(const MachineConfig& config, std::string helper,
                                         std::string work_directory) -> PreviewSettings;

// The preview of a program by LinuxCNC's own interpreter, in a helper process per computation
// (src/linuxcnc/preview/preview_helper.cpp).
//
// One computation:
//
//   snapshot  the program task names and every subroutine file its `o<name> call` blocks reach
//             are read once, within max_input_bytes, max_files and max_calls (different names
//             called, found or not; counted before any of them is looked up), and each text
//             is scanned whole before it is accepted (first_forbidden_line): a comment that
//             runs Python or writes a file, or an O word that is not plainly a name, refuses
//             the program before the interpreter reads anything. The content manifest keeps
//             the bytes read, and for every called name which file the search path gave, or
//             that none did; a name is never passed over.
//   start     the machine's parameter file is read with the snapshot and copied, and the live
//             work system, its rotation and G92 are written over the copy. The other work
//             systems, the G28 and G30 positions and every other saved parameter are the
//             file's: a move in a system, or a G28 or G30, whose values the file does not have
//             ends the preview. Task writes the file each time it synchronises its
//             interpreter: on a switch to MDI or Auto, before each MDI block, after a program
//             ends or is aborted. Run switches to Auto, and then compares the file again
//             (the watch, below): what the file did not yet have when the preview was made
//             refuses that Run, and a new preview follows. Task's modal state becomes blocks
//             to replay (units,
//             plane, distance modes, feed mode, retract mode, motion mode G0 or G1; any other
//             motion mode is named to the helper, which ends the preview at a block that
//             would move by it). The tool table is the one of the inputs: this class reads
//             none for interpretation. Program check reads the saved touch-off marks.
//   private   a directory per computation with the program in `program/`, the subroutine
//             copies in `subroutines/`, which is the only PROGRAM_PREFIX and SUBROUTINE_PATH
//             of the private INI file, and `empty/`, the helper's working directory (the
//             interpreter looks there first). A subroutine cannot take the program's place,
//             whatever it is called. The directory's own path must be plain characters: the
//             interpreter expands the paths of its INI file as a shell would.
//   helper    started with no descriptor but its three standard ones and the result pipe, a
//             small environment, CPU, memory and file size limits, lower priority, in a
//             process group of its own; ended with SIGKILL after `timeout`, when `cancelled`
//             says so, or when it has written more than the points allow
//   end       when the helper has closed its result it is given `exit_grace` to exit, and
//             its exit is collected before anything is ended by force: `ready` needs a whole
//             valid result ending `ok` AND the helper's own exit with status 0. An exit code,
//             a signal, the timeout, a helper that closes its result and stays, are each
//             `failed` with line 0 and their own text, with the valid part of the path. Its
//             process group is ended in every case; processes left after a good exit are
//             noted in the diagnostics and change nothing.
//   parameters a program that uses a parameter (`#`) is computed a second time, with every
//             parameter that task keeps between programs and the parameter file does not hold
//             (#1-#5000, the probe results #5061-#5070, #5399) set to a value of its own. When
//             the two results differ, the program reads one of them before it sets it, and
//             its value in task is not known: a third computation ends the preview at the
//             first line where the results differ (`failed`, that line). A named parameter the
//             program reads and does not define ends the preview at that line too.
//   check     before the result is returned the originals are compared, byte for byte, with
//             the manifest, and every call is searched again; the parameter file is compared
//             by its values, without those the live state writes over. A difference discards
//             the result and starts over, `attempts` times in all; after that the outcome is
//             `failed`.
//
// The watch of the result makes the same comparison again: at most max_input_bytes read and
// (max_calls x search path) lookups per call. It binds the preview to the files; it does not
// bind what LinuxCNC runs: task opens the program by its path when it runs it.
//
// `cancelled` is asked before every file read, every name lookup and every file comparison
// (one file is at most max_input_bytes), and every 20 ms while the helper runs.
//
// Outcomes: `ready`; `failed` with the line and the text for an interpreter error, a block the
// preview does not go past, a limit, the timeout, files that keep changing; `unavailable` with
// the text when no preview is computed at all (the refusal of the settings, no program, a
// start state that cannot be replayed). A reason that is not a line of the program has line 0.
//
// NOT thread-safe: one thread, the preview worker's.
class HelperPreviewEngine final : public PreviewEngine {
public:
  // For tests: called after the snapshot was taken and before the helper is started, with
  // the number of the attempt, from 1.
  using AfterSnapshot = std::function<void(int attempt)>;

  explicit HelperPreviewEngine(PreviewSettings settings, AfterSnapshot after_snapshot = {});

  [[nodiscard]] auto compute(const PreviewInputs& inputs, const std::function<bool()>& cancelled)
      -> PreviewComputation override;

  // Lookups of a subroutine file in a directory made so far, by this engine and the watches
  // it returned.
  [[nodiscard]] auto file_lookups() const -> std::size_t;
  // Helper processes started so far.
  [[nodiscard]] auto helper_runs() const -> std::size_t { return runs_; }
  // What the last helper wrote to its standard output and error, cut at max_diagnostics_bytes.
  [[nodiscard]] auto diagnostics() const -> const std::string& { return diagnostics_; }

private:
  PreviewSettings settings_;
  AfterSnapshot after_snapshot_;
  std::shared_ptr<std::atomic<std::size_t>> lookups_;
  std::size_t runs_{};
  std::string diagnostics_;
};

} // namespace coffeecnc

#endif // COFFEECNC_LINUXCNC_PREVIEW_ENGINE_HPP
