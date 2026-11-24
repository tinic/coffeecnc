// The text formats between the executable and its preview helper, and the text checks on a
// program, alone: no helper, no interpreter, no LinuxCNC.

#include "../src/linuxcnc/src/preview_wire.hpp"
#include "../src/linuxcnc/src/program_watch.hpp"

#include <array>
#include <chrono>
#include <exception>
#include <format>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace coffeecnc;
using namespace std::chrono_literals;

namespace {

int checks = 0;
void require(bool condition, const std::string& message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

void wire_formats() {
  HelperRequest request;
  request.program = "/a path/with spaces.ngc";
  request.subroutines = "/subs";
  request.inherited_motion = "G38.2";
  request.parameters = "/p.var";
  request.units_per_mm = 1 / 25.4;
  request.position = {1.5, -2, 1e-7};
  request.tool_in_spindle = 3;
  request.tool_prepared = 4;
  request.tool_offset = {0, 0, 50.123456789};
  request.feed = 300;
  request.block_delete = true;
  request.wcs = 9;
  request.tool_change_at_g30 = true;
  request.known_systems = 0b100000011;
  request.saved_g30 = true;
  request.remapped = {"m6", "g88.1"};
  request.tool_change_position = Vec3{1, 2, 3};
  request.max_points = 77;
  request.chord_tolerance = 0.005;
  request.codes = {"G21", "F1.000000", "G1"};
  request.tools = {{0, 3, 0, {0, 0, 1}, 2, 0, 0, 0}, {5, 9, 7, {0.1, 0.2, 0.3}, 6, 1, 2, 3}};
  const auto parsed = parse_request(write_request(request));
  require(parsed && *parsed == request, "A request is read as it was written");
  require(!parse_request("something else\n") && !parse_request("") &&
              !parse_request(write_request(request) + "wcs 12\n") &&
              !parse_request(write_request(request) + "known_systems 512\n") &&
              !parse_request(write_request(request) + "stop_line 3\n") &&
              !parse_request(write_request(request) + "saved_g28 2\n") &&
              !parse_request(write_request(request) + "unknown 1\n"),
          "Another header, an empty text, a bad value and an unknown key are refused");

  auto output = parse_output("seg 2 r 0 0 0 0 1 2 3\nseg 3 f 5 1 2 3 4 5 6 7 8 9\nend ok\n");
  require(output.end == HelperEnd::ok && output.segments.size() == 2 &&
              output.segments[1].tool == 5 && output.segments[1].points.size() == 3 &&
              !output.problem && output.defect.empty(),
          "Segments and the end");
  output = parse_output("seg 2 r 0 0 0 0 1 2 3\nstop 7 reads a thing\nend stop\n");
  require(output.end == HelperEnd::stop && output.problem && output.problem->line == 7 &&
              output.problem->text == "reads a thing" && output.segments.size() == 1 &&
              output.defect.empty(),
          "A stop with its line and text");
  output = parse_output("segf 3 2 450.5 0 0 0 1 0 0\ndwell 4 1.5\nseg 5 f 2 0 0 0 2 0 0\nend ok\n");
  require(output.end == HelperEnd::ok && output.defect.empty() && output.segments.size() == 2 &&
              output.segments[0].kind == MoveKind::feed && output.segments[0].tool == 2 &&
              output.segments[0].feed == 450.5 && output.segments[1].feed == 0 &&
              output.dwells == std::vector<PreviewDwell>{{4, 1.5}},
          "A feed move with its feed, a dwell, and a feed move whose feed is unknown");
  require(!parse_output("segf 3 2 -1 0 0 0 1 0 0\nend ok\n").defect.empty() &&
              !parse_output("segf 3 2 nan 0 0 0 1 0 0\nend ok\n").defect.empty() &&
              !parse_output("dwell 4 -1\nend ok\n").defect.empty() &&
              !parse_output("dwell 4\nend ok\n").defect.empty(),
          "A negative or missing feed or dwell is a defect");
  require(parse_output("").end == HelperEnd::missing && parse_output("").defect.empty() &&
              parse_output("end ok\n").end == HelperEnd::ok,
          "Nothing, and nothing but the end");
}

void preview_stop_codes() {
  const auto probe = KnownPreviewStop::probe_move;
  const std::string fallback = "a probe move (G38): where it ends depends on the machine";
  require(write_preview_stop(3, probe, {}, "1") == "stop_known 3 2\n",
          "A caller can opt into the stable probe stop code");
  const auto known = parse_output("stop_known 3 2\nend stop\n");
  require(known.end == HelperEnd::stop &&
              known.problem ==
                  PreviewError{3, fallback, MessageKey{"backend.previewEngine.stopProbe"}},
          "The coded stop has the same fallback and a localization key");
  const auto old = write_preview_stop(3, probe, {}, "");
  require(old == "stop 3 " + fallback + '\n' &&
              parse_output(old + "end stop\n").problem == PreviewError{3, fallback},
          "The unnegotiated helper keeps the original format");
  const PreviewStopSource source{"edge path.ngc", 5};
  require(write_preview_stop(3, probe, source, "2") ==
              "stop 3 edge path.ngc line 5: " + fallback + '\n',
          "Version 2 retains the original record for a subroutine");
  const auto at_source = write_preview_stop(3, probe, source, "3");
  const auto located = parse_output(at_source + "end stop\n");
  require(at_source == "stop_known 3 2 5 edge path.ngc\n" && located.end == HelperEnd::stop &&
              located.problem_source == source &&
              located.problem ==
                  PreviewError{3, fallback, MessageKey{"backend.previewEngine.stopProbe"}},
          "Version 3 keeps the main line, the file and its line even when the filename has spaces");
  require(write_preview_stop(3, probe, PreviewStopSource{}, "3") ==
              "stop 3 in a subroutine file: " + fallback + '\n',
          "An unknown subroutine location keeps the original message");
  const auto wcs = KnownPreviewStop::unknown_work_system;
  const std::string wcs_text =
      "moves in G59.3, whose offsets are not in the machine's parameter file";
  struct NumberedStop {
    KnownPreviewStop reason;
    unsigned id;
    int argument;
    std::string text;
    MessageKey key;
  };
  const std::array numbered{
      NumberedStop{
          wcs,
          8,
          0,
          "moves in another work system, whose offsets are not in the machine's parameter file",
          {"backend.previewEngine.stopOtherSystem"}},
      NumberedStop{wcs,
                   8,
                   1,
                   "moves in G54, whose offsets are not in the machine's parameter file",
                   {"backend.previewEngine.stopUnknownSystem", {{"wcs", "G54"}}}},
      NumberedStop{
          wcs, 8, 9, wcs_text, {"backend.previewEngine.stopUnknownSystem", {{"wcs", "G59.3"}}}},
      NumberedStop{
          KnownPreviewStop::machine_m_code,
          9,
          100,
          "M100 runs a program of the machine's own: what it does is not known to the preview",
          {"backend.previewEngine.stopMachineMCode", {{"code", "100"}}}},
      NumberedStop{
          KnownPreviewStop::machine_m_code,
          9,
          199,
          "M199 runs a program of the machine's own: what it does is not known to the preview",
          {"backend.previewEngine.stopMachineMCode", {{"code", "199"}}}}};
  for (const auto& row : numbered) {
    const auto wire = std::format("stop_known 4 {} {}\n", row.id, row.argument);
    require(write_preview_stop(4, row.reason, {}, "2", row.argument) == wire,
            std::format("Stop {} preserves its ID and number {}", row.id, row.argument));
    const auto parsed = parse_output(wire + "end stop\n");
    require(parsed.defect.empty() && parsed.end == HelperEnd::stop &&
                parsed.problem == PreviewError{4, row.text, row.key},
            std::format("Stop {} with number {} keeps its text and key", row.id, row.argument));
  }
  require(
      write_preview_stop(4, wcs, {}, "1", 9) == "stop 4 " + wcs_text + '\n' &&
          write_preview_stop(4, wcs, {}, "6", 9) == "stop 4 " + wcs_text + '\n' &&
          write_preview_stop(3, probe, {}, "2") == "stop_known 3 2\n",
      "Older or unknown versions get text for the numbered stop, and version 2 keeps fixed codes");
  const auto numbered_source =
      parse_output(write_preview_stop(4, wcs, source, "3", 9) + "end stop\n");
  require(numbered_source.end == HelperEnd::stop && numbered_source.problem_source == source &&
              numbered_source.problem ==
                  PreviewError{
                      4, wcs_text,
                      MessageKey{"backend.previewEngine.stopUnknownSystem", {{"wcs", "G59.3"}}}},
          "A numbered stop can also name its source file and line");
  require(!parse_output("stop_known 3 2 0 edge.ngc\nend stop\n").defect.empty() &&
              !parse_output("stop_known 3 2 5\nend stop\n").defect.empty(),
          "An incomplete source location is rejected");
  require(!parse_output("stop_known 3 8 10\nend stop\n").defect.empty() &&
              !parse_output("stop_known 3 9 99\nend stop\n").defect.empty() &&
              !parse_output("stop_known 3 9 200\nend stop\n").defect.empty() &&
              !parse_output("stop_known 3 8\nend stop\n").defect.empty(),
          "Codes that require a number reject missing or out-of-range values");
  const std::string kept = "seg 2 r 0 0 0 0 1 2 3\n";
  const auto unknown = parse_output(kept + "stop_known 3 99\nend stop\n");
  require(unknown.defect.localization == MessageKey{"backend.previewWire.invalidStopCode"} &&
              unknown.segments.size() == 1 && unknown.end == HelperEnd::missing,
          "An unknown ID retains the path before the rejected record");
}

void interpreter_stop_code_ids() {
  struct Case {
    KnownPreviewStop reason;
    unsigned id;
    std::string token;
    std::string text;
    MessageKey key;
  };
  const std::array cases{
      Case{KnownPreviewStop::computed_parameter,
           10,
           {},
           "reads a parameter whose number is computed (#[...], ##): which one it is, and "
           "whether the program has set it, is not known to the preview",
           {"backend.previewEngine.stopComputedParameter"}},
      Case{KnownPreviewStop::prior_return_value,
           11,
           {},
           "reads #<_value> or #<_value_returned> before a subroutine of the program has "
           "returned: its value from an earlier program or MDI block is not known to the preview",
           {"backend.previewEngine.stopReturnValue",
            {{"name", "#<_value>"}, {"other", "#<_value_returned>"}}}},
      Case{KnownPreviewStop::undefined_parameter,
           12,
           "#3000",
           "reads #3000, which the program does not set: its value from an earlier program "
           "or MDI block is not known to the preview",
           {"backend.previewEngine.stopUndefinedParameter", {{"name", "#3000"}}}},
      Case{KnownPreviewStop::parameter_exists,
           13,
           "#<depth>",
           "asks whether #<depth> exists outside a subroutine: on the machine it can exist "
           "from an earlier program or MDI block, which the preview does not know",
           {"backend.previewEngine.stopExistsNamed", {{"name", "#<depth>"}}}},
      Case{KnownPreviewStop::inherited_motion,
           14,
           "G84",
           "moves by the motion mode G84 that was active before the program, which the preview "
           "does not take over",
           {"backend.previewEngine.stopInheritedMotion", {{"mode", "G84"}}}},
      Case{KnownPreviewStop::remapped_code,
           15,
           "G88.1",
           "G88.1 is remapped on this machine ([RS274NGC] REMAP): what it does is not known to the "
           "preview",
           {"backend.previewEngine.stopRemapped", {{"code", "G88.1"}}}},
      Case{KnownPreviewStop::cross_scope_o_word,
           16,
           {},
           "an O word in a subroutine definition belongs to a block outside it: which lines "
           "run is not known to the preview",
           {"backend.previewEngine.stopDefinitionBlock"}}};
  for (const auto& row : cases) {
    const auto wire = row.token.empty() ? std::format("stop_known 5 {}\n", row.id)
                                        : std::format("stop_known 5 {} {}\n", row.id, row.token);
    const PreviewStopArgument argument = row.token.empty() ? PreviewStopArgument{} : row.token;
    require(write_preview_stop(5, row.reason, {}, "4", argument) == wire,
            std::format("Interpreter stop {} keeps its ID and argument", row.id));
    const auto parsed = parse_output(wire + "end stop\n");
    require(parsed.defect.empty() && parsed.end == HelperEnd::stop &&
                parsed.problem == PreviewError{5, row.text, row.key},
            std::format("Interpreter stop {} keeps its language key and fallback", row.id));
    require(write_preview_stop(5, row.reason, {}, "3", argument) == "stop 5 " + row.text + '\n',
            std::format("A version 3 reader gets the fallback for interpreter stop {}", row.id));
  }
  const auto location_wire =
      write_preview_stop(5, KnownPreviewStop::remapped_code, PreviewStopSource{"inner.ngc", 3}, "4",
                         std::string("G88.1"));
  require(location_wire == "stop_known 5 15 G88.1 3 inner.ngc\n" &&
              parse_output(location_wire + "end stop\n").problem_source ==
                  PreviewStopSource{"inner.ngc", 3},
          "A token argument can be followed by a source location");
  const auto spaced = write_preview_stop(5, KnownPreviewStop::undefined_parameter, {}, "4",
                                         std::string("#<a name>"));
  require(spaced.starts_with("stop 5 reads #<a name>,"),
          "Arguments that do not fit the coded record retain the original text");
}

void helper_problem_code_ids() {
  struct Case {
    KnownPreviewProblem reason;
    unsigned id;
    std::string_view text;
    std::string_view key;
  };
  const std::array errors{
      Case{KnownPreviewProblem::uvw_arc, 1, "an arc in a UVW plane cannot be shown",
           "helperUvwArc"},
      Case{KnownPreviewProblem::tapping_spindle, 2,
           "rigid tapping is not configured for this spindle on this machine", "rigidTapSpindle"},
      Case{KnownPreviewProblem::tapping_direction, 3,
           "rigid tapping requires clockwise spindle rotation (M3)", "rigidTapDirection"},
      Case{KnownPreviewProblem::tapping_override, 4,
           "rigid tapping requires spindle override off (M51 P0)", "rigidTapOverride"},
      Case{KnownPreviewProblem::tapping_rpm, 5,
           "rigid tapping exceeds the configured fixed spindle limit", "rigidTapRpm"},
      Case{KnownPreviewProblem::tapping_z_feed, 6,
           "rigid tapping exceeds the configured Z feed limit", "rigidTapZFeed"},
      Case{KnownPreviewProblem::tapping_z_only, 7, "rigid tapping requires a downward Z-only move",
           "rigidTapZOnly"},
      Case{KnownPreviewProblem::tapping_allowance, 8,
           "rigid tapping has no valid reversal allowance", "rigidTapAllowance"},
  };
  for (const auto& error : errors) {
    const auto wire = write_preview_problem(4, error.reason, {}, true);
    const auto output = parse_output(wire + "end error\n");
    require(wire == std::format("problem_known 4 {}\n", error.id) &&
                output.end == HelperEnd::error &&
                output.problem ==
                    PreviewError{4, std::string(error.text),
                                 MessageKey{std::format("backend.previewEngine.{}", error.key)}} &&
                write_preview_problem(4, error.reason, {}, false) ==
                    std::format("error 4 {}\n", error.text),
            std::format("Helper error {} keeps its ID, key, and legacy text", error.id));
  }
  const PreviewStopSource source{"inner part.ngc", 3};
  const auto cut = write_preview_problem(7, KnownPreviewProblem::point_limit, source, true, 1000);
  const auto output = parse_output(cut + "end cut\n");
  require(cut == "problem_known 7 9 1000 3 inner part.ngc\n" && output.end == HelperEnd::cut &&
              output.problem_source == source &&
              output.problem ==
                  PreviewError{
                      7, "the preview ends here: the path has more than 1000 points",
                      MessageKey{"backend.previewEngine.helperPointLimit", {{"limit", "1000"}}}} &&
              write_preview_problem(7, KnownPreviewProblem::point_limit, source, false, 1000) ==
                  "cut 7 inner part.ngc line 3: the preview ends here: the path has more than 1000 "
                  "points\n",
          "The point limit retains the numeric limit, source and fallback");
  require(parse_output(cut + "end error\n").end == HelperEnd::missing,
          "A cut record cannot end as an error");
  for (const auto malformed :
       {"problem_known 3 10\n", "problem_known 3 9\n", "problem_known 3 9 -1\n",
        "problem_known 3 1 0 inner.ngc\n", "problem_known 3 1 2\n"}) {
    const auto bad = parse_output(malformed);
    require(bad.end == HelperEnd::missing && !bad.problem &&
                bad.defect.localization == MessageKey{"backend.previewWire.invalidProblemCode"},
            "An invalid helper diagnostic is rejected");
  }
}

void block_refusal_code_ids() {
  struct Case {
    PreviewStopReason stop;
    unsigned id;
    std::string text;
    MessageKey key;
  };
  const std::array cases{
      Case{{KnownPreviewStop::task_parameter},
           17,
           "reads the _task parameter, which is 1 on the machine and 0 in the preview: "
           "the program can act differently in each",
           {"backend.previewEngine.stopTask"}},
      Case{
          {KnownPreviewStop::ini_or_hal_parameter},
          18,
          "reads an INI or HAL value (#<_ini[...]>, #<_hal[...]>), which the preview does not have",
          {"backend.previewEngine.stopIniHal", {{"names", "#<_ini[...]>, #<_hal[...]>"}}}},
      Case{{KnownPreviewStop::global_parameter_exists},
           19,
           "asks whether a global parameter (#<_...>) exists: on the machine it can exist from an "
           "earlier program or MDI block, which the preview does not know",
           {"backend.previewEngine.stopExists", {{"name", "#<_...>"}}}},
      Case{{KnownPreviewStop::computed_o_word},
           20,
           "a computed O word: programs with computed names are not previewed",
           {"backend.previewEngine.stopComputedOword"}},
      Case{{KnownPreviewStop::computed_g_word},
           21,
           "a computed G word: which code runs is not known before it does",
           {"backend.previewEngine.stopComputedGword"}},
      Case{{KnownPreviewStop::restore_g92},
           22,
           "G92.3 restores an offset saved in the parameter file",
           {"backend.previewEngine.stopG92Restore"}},
      Case{{KnownPreviewStop::python_line},
           23,
           "a `;py,` line runs Python in the interpreter",
           {"backend.previewEngine.stopPythonLine"}},
      Case{{KnownPreviewStop::python_comment, std::string("py,")},
           24,
           "a (py,...) comment runs Python in the interpreter",
           {"backend.previewEngine.stopPythonComment", {{"word", "py,"}}}},
      Case{{KnownPreviewStop::file_writing_comment, std::string("log,")},
           25,
           "a (log,...) comment writes a file",
           {"backend.previewEngine.stopFileWritingComment", {{"word", "log,"}}}},
      Case{{KnownPreviewStop::unsafe_o_word_name},
           26,
           "an O word whose name is computed or is not plainly a name "
           "(letters, digits, `_`, `-`, `.`): the interpreter would look for a file by it",
           {"backend.previewEngine.stopOwordName"}}};
  for (const auto& row : cases) {
    const auto* token = std::get_if<std::string>(&row.stop.argument);
    const auto wire = token ? std::format("stop_known 5 {} {}\n", row.id, *token)
                            : std::format("stop_known 5 {}\n", row.id);
    require(write_preview_stop(5, row.stop.reason, {}, "5", row.stop.argument) == wire,
            std::format("Block refusal {} keeps its wire ID", row.id));
    const auto parsed = parse_output(wire + "end stop\n");
    require(parsed.defect.empty() && parsed.end == HelperEnd::stop &&
                parsed.problem == PreviewError{5, row.text, row.key},
            std::format("Block refusal {} keeps its text and key", row.id));
    require(write_preview_stop(5, row.stop.reason, {}, "4", row.stop.argument) ==
                "stop 5 " + row.text + '\n',
            std::format("Version 4 keeps the text for block refusal {}", row.id));
  }
}

void fixed_preview_stop_code_ids() {
  struct Case {
    KnownPreviewStop reason;
    unsigned id;
    std::string text;
    MessageKey key;
  };
  const std::array cases{
      Case{KnownPreviewStop::nurbs,
           1,
           "a NURBS move (G5.2, G5.3): not computed by the preview",
           {"backend.previewEngine.stopNurbs"}},
      Case{KnownPreviewStop::probe_move,
           2,
           "a probe move (G38): where it ends depends on the machine",
           {"backend.previewEngine.stopProbe"}},
      Case{KnownPreviewStop::wait_input,
           3,
           "M66 waits for an input of the machine",
           {"backend.previewEngine.stopWaitInput"}},
      Case{KnownPreviewStop::read_input,
           4,
           "reads an input of the machine (M66)",
           {"backend.previewEngine.stopReadInput"}},
      Case{KnownPreviewStop::tool_change_g30,
           5,
           "a tool change that moves to the G30 position ([EMCIO] TOOL_CHANGE_AT_G30), "
           "which is not in the machine's parameter file",
           {"backend.previewEngine.stopG30ToolChange"}},
      Case{KnownPreviewStop::saved_g28,
           6,
           "G28 moves to a position that is not in the machine's parameter file",
           {"backend.previewEngine.stopSavedPosition", {{"code", "28"}}}},
      Case{KnownPreviewStop::saved_g30,
           7,
           "G30 moves to a position that is not in the machine's parameter file",
           {"backend.previewEngine.stopSavedPosition", {{"code", "30"}}}}};
  for (const auto& row : cases) {
    const auto wire = std::format("stop_known 3 {}\n", row.id);
    require(write_preview_stop(3, row.reason, {}, "1") == wire,
            std::format("Stop reason {} keeps its wire ID", row.id));
    const auto parsed = parse_output(wire + "end stop\n");
    require(parsed.defect.empty() && parsed.end == HelperEnd::stop &&
                parsed.problem == PreviewError{3, row.text, row.key},
            std::format("Stop reason {} keeps its fallback text, key and arguments", row.id));
  }
}

void restart_refusal_codes() {
  struct Row {
    unsigned code;
    RestartRefusal reason;
    std::string_view text;
    std::string_view key;
  };
  constexpr std::array<Row, 16> rows{{
      {1, RestartRefusal::cutter_compensation, "Cutter compensation is active at this line.",
       "backend.previewRestart.cutterCompensation"},
      {2, RestartRefusal::canned_cycle, "This line is inside a canned cycle.",
       "backend.previewRestart.cannedCycle"},
      {3, RestartRefusal::feed_spindle_mode,
       "This line requires a feed or spindle mode that cannot be restored for restart.",
       "backend.previewRestart.feedSpindleMode"},
      {4, RestartRefusal::no_motion, "Choose a line that moves the tool.",
       "backend.previewRestart.noMotion"},
      {5, RestartRefusal::move_kind, "Choose a rapid, feed, or arc move for restart.",
       "backend.previewRestart.moveKind"},
      {6, RestartRefusal::prior_motion, "The prior motion mode cannot be restored for restart.",
       "backend.previewRestart.priorMotion"},
      {7, RestartRefusal::explicit_arc, "Choose an arc block with an explicit G2 or G3.",
       "backend.previewRestart.explicitArc"},
      {8, RestartRefusal::skipped_effects,
       "The skipped blocks change tool data or machine outputs.",
       "backend.previewRestart.skippedEffects"},
      {9, RestartRefusal::frame_or_tool,
       "This line changes the coordinate frame or tool before moving.",
       "backend.previewRestart.frameOrTool"},
      {10, RestartRefusal::incomplete_state,
       "The spindle or motion plane at this line is incomplete.",
       "backend.previewRestart.incompleteState"},
      {11, RestartRefusal::length_inactive, "The required tool length is not active at this line.",
       "backend.previewRestart.lengthInactive"},
      {12, RestartRefusal::length_mismatch,
       "The tool length at this line does not match the loaded tool table.",
       "backend.previewRestart.lengthMismatch"},
      {13, RestartRefusal::subroutines,
       "LinuxCNC run from line cannot safely skip blocks in a program with subroutines.",
       "backend.previewRestart.subroutines"},
      {14, RestartRefusal::tool_preselection,
       "A later M6 depends on a tool selected before this restart line.",
       "backend.previewRestart.toolPreselection"},
      {15, RestartRefusal::rigid_tap,
       "Run this tapping program from the start to establish the spindle override state.",
       "backend.previewEngine.rigidTapRestart"},
      {16, RestartRefusal::not_executed, "The selected line is not executed in this program.",
       "backend.previewRestart.notExecuted"},
  }};
  for (const auto& row : rows) {
    const auto wire = std::format("restart_refused {}\n", row.code);
    require(write_restart_refusal(row.reason, true) == wire,
            std::format("Restart refusal {} has a stable wire code", row.key));
    const auto parsed = parse_output(wire + "end ok\n");
    require(parsed.end == HelperEnd::ok && !parsed.restart && parsed.restart_blocked == row.text &&
                parsed.restart_blocked_localization == MessageKey{std::string(row.key)},
            std::format("Restart refusal {} has the expected text and key", row.code));
    const auto old_wire = write_restart_refusal(row.reason, false);
    require(old_wire == std::format("restart_blocked {}\n", row.text),
            std::format("Restart refusal {} still writes the original text for older callers",
                        row.code));
    const auto legacy = parse_output(old_wire + "end ok\n");
    require(legacy.end == HelperEnd::ok && legacy.restart_blocked == row.text &&
                !legacy.restart_blocked_localization,
            std::format("Restart refusal {} still reads legacy text", row.code));
  }
  require(
      !parse_output("restart_refused 999\nend ok\n").defect.empty() &&
          !parse_output("restart_refused 257\nend ok\n").defect.empty() &&
          !parse_output("restart_refused 2 extra\nend ok\n").defect.empty() &&
          !parse_output("restart_refused 2\nrestart_blocked A refusal.\nend ok\n").defect.empty(),
      "Unknown, malformed, and duplicate restart refusals invalidate the helper result");
}

// The result is text of unknown origin: each case ends, soon, with the valid records before
// the bad one and nothing after.
void hostile_results() {
  const std::string good = "seg 2 r 0 0 0 0 1 2 3\n";
  const auto kept_one = [&](const std::string& bad, std::string_view what) -> void {
    const auto begun = std::chrono::steady_clock::now();
    const auto output = parse_output(good + bad);
    const auto took = std::chrono::steady_clock::now() - begun;
    require(took < 1s && output.end == HelperEnd::missing && output.segments.size() == 1 &&
                output.segments[0].line == 2,
            std::format("{}: the reading ends at once, with the record before it", what));
  };
  const auto defect = [&](const std::string& bad, std::string_view what) -> void {
    kept_one(bad, what);
    const auto reason = parse_output(good + bad).defect;
    require(!reason.empty() && reason.localization &&
                reason.localization->key.starts_with("backend.previewWire."),
            std::format("{}: and carries a localized defect", what));
  };
  // The parent once never came back from this one.
  defect("seg 1 f 0 0 bad 0 1 0 0\nend ok\n", "a word that is no number");
  defect("seg 1 f 0 0 0 0 1 0\nend ok\n", "a coordinate missing");
  defect("seg 1 f 0 0 0 0\nend ok\n", "one point");
  defect("seg 1 f 0\nend ok\n", "no point");
  defect("seg 1 f 0 0 0 nan 1 1 1\nend ok\n", "nan");
  defect("seg 1 f 0 inf 0 0 1 1 1\nend ok\n", "inf");
  defect("seg 1 f 0 0 0 0 1 1 -infinity\nend ok\n", "-infinity");
  defect("seg 1 f 0 0 0 0 1 1 1e999\nend ok\n", "a number out of range");
  defect("seg -1 f 0 0 0 0 1 1 1\nend ok\n", "a negative line");
  defect("seg 1 f -1 0 0 0 1 1 1\nend ok\n", "a negative tool");
  defect("seg 1 f 100000 0 0 0 1 1 1\nend ok\n", "a tool out of range");
  defect("seg 99999999999 f 0 0 0 0 1 1 1\nend ok\n", "a line that is no int");
  defect("seg 1 x 0 0 0 0 1 1 1\nend ok\n", "an unknown kind of move");
  defect("seg 1.5 f 0 0 0 0 1 1 1\nend ok\n", "a line that is a fraction");
  defect("segment 1 f 0 0 0 0 1 1 1\nend ok\n", "an unknown record");
  defect("\nend ok\n", "an empty line");
  defect("error -4 text\nend error\n", "a negative line of an error");
  defect("error 4 a\nerror 5 b\nend error\n", "two errors");
  defect("stop 4 a\nseg 5 f 0 0 0 0 1 1 1\nend stop\n", "a segment after a stop");
  defect("error 4 a\nend ok\n", "ok after an error");
  defect("end error\n", "an error end without an error");
  defect("stop 4 a\nend cut\n", "an end of another kind");
  defect("end ok\nend ok\n", "two ends");
  defect("end ok\nseg 5 f 0 0 0 0 1 1 1\n", "a record after the end");
  defect("end ok\ngarbage", "garbage after the end");
  defect("end ok extra\n", "words after the end's reason");
  defect("end\n", "an end without a reason");
  kept_one("seg 3 f 5 1 2 3 4 5", "a last record that was cut off");
  require(parse_output(good + "seg 3 f 5 1 2 3 4 5").defect.empty() &&
              parse_output(good).defect.empty() && parse_output(good).end == HelperEnd::missing,
          "A result that only stops short has no defect: its helper was ended");

  // Lines of the program: nothing beyond its last.
  auto output = parse_output("seg 7 f 0 0 0 0 1 1 1\nseg 8 f 0 1 1 1 2 2 2\nend ok\n", {100, 7});
  require(output.segments.size() == 1 && !output.defect.empty() &&
              parse_output("stop 8 x\nend stop\n", {100, 7}).problem == std::nullopt,
          "A line beyond the program's last is a defect");
  // Points: no more than the limit, in all segments together.
  std::string many;
  for (int i = 0; i < 40; ++i)
    many += "seg 1 f 0 0 0 0 1 1 1 2 2 2\n";
  output = parse_output(many + "end ok\n", {100, 10});
  require(output.segments.size() == 33 && !output.defect.empty() && output.end == HelperEnd::missing,
          std::format("More points than the limit: the segments before it ({})",
                      output.segments.size()));
  // A very long line, and very many bad ones: time is linear.
  std::string huge = "seg 1 f 0";
  for (int i = 0; i < 300000; ++i)
    huge += " 1.5 2.5 3.5";
  auto begun = std::chrono::steady_clock::now();
  output = parse_output(huge + "\nend ok\n", {400000, 10});
  require(output.end == HelperEnd::ok && output.segments.at(0).points.size() == 300000 &&
              std::chrono::steady_clock::now() - begun < 5s,
          "A segment of 300000 points is read in time");
  begun = std::chrono::steady_clock::now();
  output = parse_output(huge + " x\nend ok\n", {400000, 10});
  require(output.segments.empty() && !output.defect.empty() &&
              std::chrono::steady_clock::now() - begun < 5s,
          "The same with a bad word at its end is refused whole, in time");
  begun = std::chrono::steady_clock::now();
  output = parse_output(std::string(std::size_t{16} * 1024 * 1024, 'x'));
  require(output.segments.empty() && output.end == HelperEnd::missing &&
              std::chrono::steady_clock::now() - begun < 5s,
          "16 MiB without a newline");
}

void blocks_refused_by_their_text() {
  const auto refused = [](std::string_view line) -> bool { return refused_block(line).has_value(); };
  require(refused("o[1+2] call") && refused("N10 o[1] call") && refused("g[28]") &&
              refused("g#1 x1") && refused("G92.3") && refused("N10 G92.3") && refused("/G92.3") &&
              refused("G1 X#<_task>") && refused("o1 if [#<_task> EQ 0]"),
          "Computed words, G92.3 and #<_task>");
  require(!refused("G1 X1 Y2") && !refused("G28.1") && !refused("G30.1") && !refused("G92.1") &&
              !refused("G2 X8 I3") && !refused("G280") && !refused("o100 call") &&
              !refused("o<g28> call") && !refused("o100 if [1 GT 0]") && !refused("(#1) G1 X1") &&
              !refused("G1 X1 ; #1") && !refused("") && !refused("%") && !refused("M2") &&
              !refused("G3 X1 Y1 R[2+3]") && !refused("o1 while [2 GE 1]") &&
              !refused("#1=5") && !refused("g1 x#1") && !refused("G1 X#<a>") && !refused("x[##2]") &&
              !refused("O100 CALL [1]") && !refused("o<a b> call [1] [2]") && !refused("G28") &&
              !refused("G30 X1") && !refused("G1 X#<_tasks>"),
          "Blocks the interpreter is followed in: parameters, calls with arguments, G28, G30");
  require(saved_position_move("G28") == 28 && saved_position_move("g 2 8") == 28 &&
              saved_position_move("G28.0 X1") == 28 && saved_position_move("N10 G28") == 28 &&
              saved_position_move("/G28") == 28 && saved_position_move("G91 G30 Z0") == 30 &&
              saved_position_move("G280") == 0 && saved_position_move("o<g28> call") == 0 &&
              saved_position_move("G28.1") == 0 && saved_position_move("(G28) G1 X1") == 0 &&
              saved_position_move("G1 X[28]") == 0,
          "Moves to a saved position");
  const std::vector<std::string> remapped{"m6", "t", "g88.1"};
  require(remapped_use("T1 M6", remapped) == "M6" && remapped_use("M[2*3]", remapped) == "M6" &&
              remapped_use("T2", remapped) == "T" && remapped_use("G88.1 X1", remapped) == "G88.1" &&
              !remapped_use("M61 Q1", remapped) && !remapped_use("G88 X1", remapped) &&
              !remapped_use("o100 call", remapped) && !remapped_use("S100 M3", remapped) &&
              !remapped_use("(T1 M6)", remapped) && !remapped_use("G1 X[abs[-1]]", remapped) &&
              !remapped_use("T1 M6", {}),
          "Remapped codes in a block");
  require(uses_parameters("G1 X#1\n") && uses_parameters("G1 X1\n#<a>=1") &&
              !uses_parameters("G1 X1\n(#1) ; #2\n") && !uses_parameters(""),
          "Whether a text uses parameters");
  // Review of #276, findings 1, 3 and 4: what a line reads and sets, and what is refused.
  {
    const auto use = parameter_use("N10 #1 = [#2 + #3000] #4=#<x> (#5) ; #6");
    require(use.sets == std::vector<int>{1, 4} && use.reads == std::vector<int>{2, 3000} &&
                !use.computed && !use.returned && !use.control,
            "Settings and reads of a line, outside its comments");
    const auto branch = parameter_use("o100 if [#3000 LT 0]");
    require(branch.control && branch.name == "100" && branch.word == "if" &&
                branch.reads == std::vector<int>{3000} && branch.sets.empty(),
            "An O word reads its condition and sets nothing");
    require(parameter_use("o<a> elseif [1]").word == "elseif" &&
                parameter_use("o<a> endsub [#1]").word == "endsub" &&
                parameter_use("o<a> sub").word == "sub" &&
                parameter_use("o<a> call [#5]").reads == std::vector<int>{5},
            "The word of an O word");
    require(parameter_use("G1 X#[1+2]").computed && parameter_use("G1 X##1").computed &&
                parameter_use("G1 X#1.5").computed && parameter_use("G1 X#<_value>").returned &&
                parameter_use("o1 if [#<_value_returned>]").returned &&
                !parameter_use("G1 X#<_values>").returned,
            "A computed number and the return value");
    require(parameter_use("o1 if [EXISTS [#<Depth>] AND EXISTS[#<b>]]").exists == "#<depth>" &&
                parameter_use("#1 = exists[#<_a>]").exists.empty() &&
                parameter_use("G1 X#<depth>").exists.empty(),
            "The first name without `_` that EXISTS asks about");
    require(stored_parameters("5161\t1.5\n  31 2\nx 3\n0 1\n5220\n") ==
                std::map<int, double>{{31, 2}, {5161, 1.5}},
            "A parameter file as the interpreter reads it");
  }
  require(refused("o1 if [EXISTS[#<_depth>] EQ 0]") && refused("o1 if [exists [#< _depth >]]") &&
              refused("G1 X#<_ini[axis_x]max_limit>") && refused("G1 X#<_HAL[x.y]>") &&
              !refused("o1 if [EXISTS[#<depth>]]") && !refused("(exists[#<_a>])"),
          "EXISTS of a global, INI and HAL values");
  require(refused("(py,x)") && refused("(PYRUN,x)") && refused("( pyreload)") &&
              refused(";py,x") && refused("  ;PY,x") && refused("; py ,x") && refused("(Logopen,f)") &&
              refused("(LOG,x)") && refused("(logclose)") && refused("(probeopen f)") &&
              refused("(PROBECLOSE)") && refused("G1 X1 (comment) (py,x)"),
          "Comments the interpreter acts on");
  require(!refused("(msg, #1)") && !refused("(DEBUG,#<_x>)") && !refused("(print,#1)") &&
              !refused("(abort,#1)"),
          "Comments that expand parameters only print them");
  require(!refused("(msg, hello)") && !refused("(debug,x)") && !refused("(PRINT,x)") &&
              !refused("(pyramid)") && !refused("(logic)") && !refused("(py without a comma)") &&
              !refused("(the log, see below)") && !refused("; a python note") &&
              !refused(";python") && !refused("G1 X1 ;py,x"),
          "Comments it does not act on");
  require(o_word_name("O<My Sub> call") == "mysub" && o_word_name("N10 o100 sub") == "100" &&
              o_word_name("/o<a.b-c_d> if [1]") == "a.b-c_d" && !o_word_name("G1 X1") &&
              !o_word_name("o</etc/x> call") && !o_word_name("o<a..b> call") &&
              !o_word_name("o<> call") && !o_word_name("o[1] call") && !o_word_name("o<a") &&
              is_call("o<a> call") && is_call("O100 CALL [1]") && !is_call("o<a> sub") &&
              !is_call("G1 X1") && !is_call("o<call> sub"),
          "The name of an O word");
  const auto unsafe = first_forbidden_line("G1 X1\no<ok> call\no<~/x> sub\n");
  const auto last_line = first_forbidden_line("\n\n;py,x");
  require(unsafe && unsafe->line == 3 &&
              unsafe->stop == PreviewStopReason{KnownPreviewStop::unsafe_o_word_name} &&
              first_forbidden_line("o#1 call\n") && first_forbidden_line("(c) o<$a> endsub\n") &&
              !first_forbidden_line("o<ok> call\no100 if [1]\nG1 X1 (o</etc/x> call)\n"),
          "An O word that is not plainly a name is forbidden wherever it stands");
  const auto effect = first_forbidden_line("G1 X1\n(msg, #1)\n( LogOpen,f)\n;py,x\n");
  require(effect && effect->line == 3 && !first_forbidden_line("G1 X#1 (msg,#2)\n; py\n") &&
              last_line && last_line->line == 3,
          "The first line of a text with a side effect");
  require(bare_block(" G1 (c) X 1 ; tail") == "g1x1" && bare_block("(a)(b)") .empty(),
          "A bare block");
  require(motion_use("X1 Y2") == MotionUse::inherits && motion_use("N5 z[1+2]") == MotionUse::inherits &&
              motion_use("G53 X0") == MotionUse::inherits && motion_use("G1 X1") == MotionUse::sets &&
              motion_use("g0") == MotionUse::sets && motion_use("G81 R1 Z0") == MotionUse::sets &&
              motion_use("G38.2 Z-1") == MotionUse::sets && motion_use("G80") == MotionUse::sets &&
              motion_use("G21 G90 F100 S[fix[3.2]] M3") == MotionUse::none &&
              motion_use("o100 while [1 LT 2]") == MotionUse::none &&
              motion_use("(X1) T2 M6") == MotionUse::none && motion_use("G4 P1") == MotionUse::none,
          "What a block does about the motion mode");
}

// The collection of called names is bounded while it runs, not after.
void called_names_are_bounded() {
  std::string text = "G21 G90\n";
  std::size_t at_257 = 0;
  for (int i = 0; i < 100000; ++i) {
    if (i == 257)
      at_257 = text.size();
    text += std::format("o<missing{}> call\n", i);
  }
  auto begun = std::chrono::steady_clock::now();
  auto found = called_names(text, {}, 256, {});
  require(found.too_many && !found.cancelled && found.names.size() == 256 &&
              found.names.front() == "missing0" && found.names.back() == "missing255" &&
              found.examined <= at_257 && found.examined < text.size() / 100 &&
              std::chrono::steady_clock::now() - begun < 1s,
          std::format("The scan stops at the 257th name: examined {} of {} bytes, the 257th "
                      "ends at {}",
                      found.examined, text.size(), at_257));
  // Names already known do not count, a name twice counts once, and at the bound it is whole.
  found = called_names("o<a> call\nO<A> CALL\no<b> call\no<c> sub\nG1 X1\no<c> call\n", {"b"}, 2, {});
  require(!found.too_many && found.names == std::vector<std::string>{"a", "c"} &&
              found.examined == 55,
          "Distinct new names, in order, to the end of the text");
  found = called_names("o<a> call\no<b> call\no<c> call\n", {}, 2, {});
  require(found.too_many && found.names.size() == 2 && found.examined == 30, "One too many");
  // Cancelled inside one large text: it returns long before the end.
  int asked = 0;
  begun = std::chrono::steady_clock::now();
  found = called_names(text, {}, 1000000, [&]() -> bool { return ++asked > 3; });
  require(found.cancelled && !found.too_many && found.examined < text.size() / 10 &&
              found.names.size() == 3 * 1024 - 1 && asked == 4,
          std::format("Cancellation is asked every 1024 lines: stopped after {} of {} bytes",
                      found.examined, text.size()));
}

} // namespace

auto main() -> int {
  try {
    wire_formats();
    preview_stop_codes();
    fixed_preview_stop_code_ids();
    interpreter_stop_code_ids();
    helper_problem_code_ids();
    block_refusal_code_ids();
    restart_refusal_codes();
    hostile_results();
    blocks_refused_by_their_text();
    called_names_are_bounded();
    std::cout << checks << " preview wire checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
