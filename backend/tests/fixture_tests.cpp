#include <coffeecnc/app/alarm_log.hpp>
#include <coffeecnc/app/machine_data.hpp>
#include <coffeecnc/app/messages.hpp>
#include <coffeecnc/app/operator_settings.hpp>
#include <coffeecnc/app/request.hpp>
#include <coffeecnc/app/response.hpp>
#include <coffeecnc/app/tool_models.hpp>

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace coffeecnc;
using namespace std::chrono_literals;

namespace {
using Json = nlohmann::json;

int checks = 0;
void require(bool condition, const std::string& message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

// Protocol v1 `type` of every Command alternative, in the variant's order. A command added to
// the variant without a name here, or without an example in the fixture file, fails below; so
// does an example the decoder does not know.
constexpr std::array<std::string_view, std::variant_size_v<Command>> wire_types{
    "ready",
    "set_machine",
    "home",
    "jog",
    "jog_step",
    "jog_stop_all",
    "mdi",
    "list_directory",
    "open_program",
    "run",
    "pause",
    "resume",
    "step",
    "abort",
    "set_override",
    "set_spindle",
    "set_coolant",
    "set_program_option",
    "select_wcs",
    "touch_off",
    "tool_touch_off",
    "set_tool",
    "remove_tool",
    "tool_changed",
    "set_tool_model_defaults",
    "set_tool_model_assignment",
    "quit",
    "set_operator_settings",
    "set_work_offset",
    "fixed_setter",
    "plan_restart",
    "start_restart",
    "edit_program",
    "jog_paused_z",
    "save_program",
    "restore_offset",
    "set_work_rotation",
    "clear_g92",
    "watch_diagnostics",
    "launch_hal_tool",
    "backup",
    "check_restore",
    "restore",
    "manage_program_file"};

void every_panel_command_decodes() {
  std::ifstream file{COFFEECNC_COMMAND_FIXTURES};
  require(file.is_open(), std::string("Fixture file opens: ") + COFFEECNC_COMMAND_FIXTURES);
  const auto fixtures = Json::parse(file);
  require(fixtures.is_array() && !fixtures.empty(), "Fixture file is a nonempty array");

  std::array<bool, wire_types.size()> covered{};
  for (const auto& fixture : fixtures) {
    const auto wire = fixture.dump();
    const auto decoded = decode_request(wire);
    const auto* request = std::get_if<Request>(&decoded);
    require(request != nullptr, "Panel command decodes: " + wire);
    if (request == nullptr)
      continue;
    require(request->id == fixture.at("id").get<RequestId>(), "Request ID preserved: " + wire);
    const auto alternative = request->command.index();
    require(wire_types.at(alternative) == fixture.at("type").get<std::string>(),
            "Decodes to the alternative of its type: " + wire);
    require(!covered.at(alternative), "One example per command: " + wire);
    covered.at(alternative) = true;
  }
  for (std::size_t i = 0; i < covered.size(); ++i) {
    require(!wire_types.at(i).empty(),
            "Command alternative " + std::to_string(i) + " has a wire type");
    require(covered.at(i), "Fixture file has an example of " + std::string(wire_types.at(i)));
  }
}

auto axis(Axis letter, double machine, double work, double dtg, double min_limit, double max_limit)
    -> AxisStatus {
  return {letter, true, false, machine, work, dtg, min_limit, max_limit, false};
}

// A machine that is on, homed, idle and at its origin, without the optional signals.
auto idle_status() -> Status {
  Status status;
  status.link = Link::live;
  status.machine = MachineState::on;
  status.axes = {axis(Axis::x, 0, 0, 0, 0, 300), axis(Axis::y, 0, 0, 0, 0, 200),
                 axis(Axis::z, 0, 0, 0, -120, 0)};
  status.motion.feed_override = 1;
  status.motion.rapid_override = 1;
  status.spindle.override = 1;
  return status;
}

constexpr auto part = "/home/cnc/nc_files/part.ngc";

// The messages of protocol/fixtures/messages.json, in its order, built the way the adapter will:
// from the core structs through the encoders.
auto encoded_messages() -> std::vector<EncodedMessage> {
  std::vector<EncodedMessage> messages;

  Hello g0759;
  g0759.machine_name = "G0759";
  g0759.axes = {Axis::x, Axis::y, Axis::z};
  g0759.jog_increments = {0.01, 0.1, 1, 10};
  g0759.max_jog_velocity = 1500;
  g0759.max_spindle_rpm = 5000;
  g0759.axis_max_jog_velocity = {1500, 1500, 750};
  g0759.homing_required = false;
  g0759.max_feed_override = 1.2;
  g0759.min_spindle_override = 0.5;
  g0759.max_spindle_override = 1.5;
  g0759.machine_id = "G0759";
  g0759.coolant = CoolantOutputs{.flood = false, .mist = false};
  g0759.macros = {{"Park", "G53 G0 Z0"}, {"Clamp check", "O<clamp> call"}};
  g0759.diagnostics = true;
  messages.push_back(encode_hello(g0759));

  Hello demo;
  demo.machine_name = "Demo mill";
  demo.simulated = true;
  demo.axes = {Axis::x, Axis::y, Axis::z};
  demo.machine_units = LengthUnit::inch;
  demo.jog_increments = {0.0254, 0.254, 2.54};
  demo.max_jog_velocity = 3048;
  demo.axis_max_jog_velocity = {3048, 3048, 3048};
  demo.homing_required = true;
  demo.max_feed_override = 2;
  demo.min_spindle_override = 0.5;
  demo.max_spindle_override = 1;
  messages.push_back(encode_hello(demo));

  const ProgramStatus loaded{part, 3, 12, 10, true, false};

  // Every optional signal present, and true or nonzero.
  auto running = idle_status();
  running.mode = TaskMode::automatic;
  running.interp = InterpState::reading;
  running.axes = {axis(Axis::x, 112.5, 12.5, 3.25, 0, 300), axis(Axis::y, 80, -20, 0, 0, 200),
                  axis(Axis::z, -42.75, 5.45, 0, -120, 0)};
  running.offsets = {2, {100, 100, -100}, {0, 0, 3.6}, {0, 0, 48.2}, 15};
  running.motion = {540, FeedMode::per_minute, 600, 0.9, 0.5, false};
  running.spindle = {SpindleDirection::forward, 6000, 1.1, 6587.5, true};
  running.coolant = {true, false};
  running.tool = {3, 4};
  running.program = loaded;
  running.preview_context = 4821337;
  running.active_codes = {"G1", "G55", "G90", "G94", "M3", "M8"};
  running.probe_input = true;
  for (auto& a : running.axes) {
    a.reference_mark = true;
    a.at_reference_mark = true;
  }
  messages.push_back(encode_status(running, 1));

  // The optional signals present with their zero and false values: not the same as unknown.
  auto paused = idle_status();
  paused.link = Link::stale;
  paused.mode = TaskMode::automatic;
  paused.interp = InterpState::paused;
  paused.motion = {0, FeedMode::inverse_time, 2.5, 1, 1, true};
  paused.spindle = {SpindleDirection::reverse, 800, 1, 0.0, false};
  paused.tool = {3, std::nullopt};
  paused.program = loaded;
  paused.preview_context = 4821337;
  paused.wait = WaitPaused{};
  paused.active_codes = {"G1", "G54", "G93", "M4"};
  paused.probe_input = false;
  paused.paused_z = PausedZStatus{0,
                                  20,
                                  false,
                                  true,
                                  "Pause a running program before jogging Z.",
                                  PausedSpindle::none,
                                  MessageKey{"backend.nmlStatus.pauseFirst"}};
  for (auto& a : paused.axes) {
    a.reference_mark = false;
    a.at_reference_mark = false;
  }
  messages.push_back(encode_status(paused, 2));

  // No connection: placeholders, and a machine without the optional signals.
  auto down = idle_status();
  down.link = Link::down;
  down.machine = MachineState::estop;
  for (auto& a : down.axes) {
    a.homed = false;
    a.min_limit = 0;
    a.max_limit = 0;
  }
  messages.push_back(encode_status(down, 3));

  auto changing = idle_status();
  changing.mode = TaskMode::automatic;
  changing.interp = InterpState::waiting;
  changing.axes = {axis(Axis::x, 10, 10, 0, 0, 300), axis(Axis::y, 20, 20, 0, 0, 200),
                   axis(Axis::z, -1.5, -1.5, 0, -120, 0)};
  changing.axes.back().on_hard_limit = true;
  changing.motion = {0, FeedMode::per_revolution, 0.05, 1, 1, true};
  changing.tool = {3, 4};
  changing.program = {part, 3, 8, 7, false, true};
  changing.preview_context = 4821337;
  changing.wait = WaitToolChange{4};
  changing.active_codes = {"G0", "G20", "G95"};
  changing.program_units = LengthUnit::inch;
  messages.push_back(encode_status(changing, 4));

  auto dwelling = idle_status();
  dwelling.mode = TaskMode::mdi;
  dwelling.interp = InterpState::waiting;
  dwelling.axes.at(0).homed = false;
  dwelling.axes.at(0).homing = true;
  dwelling.axes.at(1).homed = false;
  dwelling.wait = WaitDwell{1.5};
  dwelling.active_codes = {"G4"};
  messages.push_back(encode_status(dwelling, 5));

  ToolTable tools;
  tools.revision = 7;
  tools.tools = {{3, 3, 48.2, 6, "End mill 6 mm"}, {4, 4, 61.75, 3.175, ""}};
  messages.push_back(encode_tool_table(tools));
  messages.push_back(encode_tool_table({}));

  WorkOffsets offsets;
  offsets.revision = 5;
  offsets.systems.fill({{0, 0, 0}, 0, true, true});
  offsets.systems.at(1) = {{100, 100, -100}, 15, true, true};
  // Read before the interpreter ran something: possibly changed since.
  offsets.systems.at(2) = {{-12.5, 0.125, 0}, -90, true, false};
  offsets.systems.at(8) = {{1, 2, 3}, 0, true, true};
  messages.push_back(encode_work_offsets(offsets));
  // Only the active system is known: no parameter file.
  WorkOffsets active_only;
  active_only.revision = 6;
  active_only.systems.at(1) = {{100, 100, -100}, 15, true, true};
  messages.push_back(encode_work_offsets(active_only));

  messages.push_back(encode_program(
      {3, part, {"G21 G90 G54", "", "(pièce)", "G0 X0 Y0 Z10", "M2"}, false, "9f6b1c2e04d3a587"}));
  messages.push_back(encode_program(
      {4, "/home/cnc/nc_files/huge.ngc", {"G21", "G0 X0"}, true, "0d41e5a3b2c79f10"}));

  Preview blocked;
  blocked.context = 4821340;
  blocked.state = PreviewState::failed;
  blocked.error = PreviewError{
      3, "The preview ends at line 3: a (log,...) comment writes a file.",
      MessageKey{
          "backend.previewEngine.endsAtLine",
          {{"line", "3"}, {"detail", "a (log,...) comment writes a file"}},
          {{"detail", {"backend.previewEngine.stopFileWritingComment", {{"word", "log,"}}}}}}};
  messages.push_back(encode_preview(blocked));

  const PreviewSegment approach{4, MoveKind::rapid, 3, {{0, 0, 10}, {0, 0, 2}}};
  const PreviewSegment cut{
      5, MoveKind::feed, 3, {{0, 0, 2}, {0, 0, -1.5}, {30, 0, -1.5}, {30, 20.5, -1.5}}, 450};
  Preview preview;
  preview.context = 4821337;
  preview.state = PreviewState::pending;
  messages.push_back(encode_preview(preview));
  preview.state = PreviewState::ready;
  preview.segments = {approach, cut};
  preview.bounds = Bounds{{0, 0, -1.5}, {30, 20.5, 2}};
  preview.dwells = {{6, 2}};
  auto& check = preview.check.emplace();
  check.errors = {{"T3 has no verified length in the tool table.",
                   MessageKey{"backend.previewEngine.noVerifiedLength", {{"tool", "3"}}}}};
  check.warnings = {{"Additional controller check is required."},
                    {"Feed per revolution is excluded from the feed range.",
                     MessageKey{"backend.previewEngine.feedPerRevolution"}}};
  messages.push_back(encode_preview(preview));
  preview.check.reset();
  preview.dwells.clear();
  preview.context = 4821338;
  preview.state = PreviewState::failed;
  preview.error = PreviewError{6, "Unknown G code used"};
  preview.segments = {approach};
  preview.bounds.reset();
  messages.push_back(encode_preview(preview));
  messages.push_back(encode_preview({}));
  Preview unavailable;
  unavailable.context = 4821339;
  unavailable.error =
      PreviewError{0, "Cutter compensation (G41) is active: no preview.",
                   MessageKey{"backend.previewEngine.cutterCompensation", {{"code", "G41"}}}};
  messages.push_back(encode_preview(unavailable));

  messages.emplace_back(
      encode_message(MessageLevel::error, "Joint 0 on limit switch error", 1700000000123ms));
  messages.emplace_back(encode_message(MessageLevel::info, "T1 restored in the spindle.",
                                       1700000001000ms,
                                       MessageKey{"backend.nmlPort.restored", {{"tool", "1"}}}));
  messages.emplace_back(encode_message(MessageLevel::info, "Jog stopped: probe contact",
                                       1700000001100ms,
                                       MessageKey{"backend.jogStop.probeContact"}));
  messages.emplace_back(encode_message(
      MessageLevel::error, "Panel request ignored: Missing required field: id.", 1700000001200ms,
      MessageKey{"messages.session.requestIgnored",
                 {{"detail", "Missing required field: id."}},
                 {{"detail", {"messages.request.missingField", {{"field", "id"}}}}}}));
  messages.emplace_back(encode_message(
      MessageLevel::error,
      "The machine status cannot be shown: status: motion.velocity is not a finite number.",
      1700000001300ms,
      MessageKey{
          "messages.panelFeed.statusCannotShow",
          {{"detail", "status: motion.velocity is not a finite number."}},
          {{"detail", {"messages.wire.notFinite", {{"field", "status: motion.velocity"}}}}}}));
  messages.emplace_back(encode_message(
      MessageLevel::error, "Jog stop failed on X: LinuxCNC did not answer.", 1700000001400ms,
      MessageKey{"messages.session.jogStopFailed",
                 {{"axis", "X"}, {"detail", "LinuxCNC did not answer."}},
                 {{"detail", {"messages.session.linuxcncNoAnswer"}}}}));
  messages.emplace_back(encode_result(
      53, CommandOutcome::failed,
      "Saved on disk, but not reopened. LinuxCNC did not open the program.", {}, {}, {},
      MessageKey{"messages.session.savedNotReopened",
                 {{"detail", "LinuxCNC did not open the program."}},
                 {{"detail", {"backend.nmlPort.notOpened"}}}}));
  messages.emplace_back(encode_result(41, CommandOutcome::accepted, ""));
  messages.emplace_back(encode_result(58, CommandOutcome::rejected,
                                      "This line is inside a canned cycle.", {}, {},
                                      RefusalMessage{"backend.previewRestart.cannedCycle"}));
  messages.emplace_back(
      encode_result(42, CommandOutcome::rejected, "Home all configured axes first."));
  messages.emplace_back(encode_result(
      51, CommandOutcome::rejected, "The interpreter modes changed. Review a fresh program check.",
      {}, {}, RefusalMessage{"backend.nmlPort.modesChanged"}));
  messages.emplace_back(encode_result(
      46, CommandOutcome::rejected, "Jog velocity is above the Z axis limit of 750 mm/min.", {}, {},
      RefusalMessage{"messages.policy.jogVelocityAxisLimit", {{"axis", "Z"}, {"limit", "750"}}}));
  messages.emplace_back(encode_result(43, CommandOutcome::failed, "LinuxCNC reported an error.", {},
                                      {}, {}, MessageKey{"backend.nmlPort.reportedError"}));
  messages.emplace_back(encode_result(44, CommandOutcome::timed_out, "LinuxCNC did not answer."));
  messages.emplace_back(encode_result(
      56, CommandOutcome::timed_out,
      "The previous jog stop on X is not confirmed: LinuxCNC did not answer.", {}, {}, {},
      MessageKey{"messages.session.previousStopAxis",
                 {{"axis", "X"}, {"detail", "LinuxCNC did not answer."}}}));
  messages.emplace_back(encode_result(47, CommandOutcome::timed_out, "LinuxCNC did not answer.", {},
                                      {}, {}, MessageKey{"messages.session.linuxcncNoAnswer"}));
  messages.emplace_back(
      encode_result(48, CommandOutcome::failed, "A program named part.ngc exists already.", {}, {},
                    {}, MessageKey{"backend.programEdit.exists", {{"name", "part.ngc"}}}));
  messages.emplace_back(
      encode_result(57, CommandOutcome::rejected,
                    "This line runs in G55. Select G55, then choose the line again.", {}, {},
                    RefusalMessage{"backend.nmlPort.restartSelectWcs", {{"wcs", "G55"}}}));
  const RestartPoint restart{.line = 25,
                             .tool = 98,
                             .h = 98,
                             .feed = 600,
                             .clearance_z = 30,
                             .approach_feed = 300,
                             .start = {-16, 12, 30}};
  messages.emplace_back(encode_result(45, CommandOutcome::accepted, "", {},
                                      RestartPlan{3, true, restart}));

  messages.emplace_back(
      encode_result(49, CommandOutcome::rejected, "Missing required field: path.", {}, {},
                    RefusalMessage{"messages.request.missingField", {{"field", "path"}}}));
  messages.emplace_back(encode_result(
      52, CommandOutcome::failed, "Tool measured. The updated program check is unavailable.", {},
      {}, {}, MessageKey{"backend.toolChangeMeasurement.measuredCheckUnavailable"}));

  Directory top;
  top.path = "/home/cnc/nc_files";
  top.entries = {{"fixtures", EntryKind::directory, 0, 1700000000000},
                 {"part.ngc", EntryKind::file, 1834, 1700000123456}};
  messages.push_back(encode_directory(top));
  Directory below;
  below.path = "/home/cnc/nc_files/fixtures";
  below.parent = "/home/cnc/nc_files";
  messages.push_back(encode_directory(below));
  Directory many;
  many.path = "/home/cnc/nc_files/many";
  many.parent = "/home/cnc/nc_files";
  many.entries = {{"0001.ngc", EntryKind::file, 12, 1700000200000}};
  many.truncated = true;
  messages.push_back(encode_directory(many));

  ToolModels saved;
  saved.machine_id = "G0759";
  saved.revision = 2;
  saved.availability = ToolModelAvailability::available;
  ToolModelAssignment end_mill{{3, 3, 6, "End mill 6 mm"}, ToolFamily::end_mill, {}};
  end_mill.overrides.stickout = 30;
  saved.document = ToolModelDocument{factory_tool_defaults(), {end_mill}};
  messages.push_back(encode_tool_models(saved));
  ToolModels session_only;
  session_only.reason = "No machine identity configured";
  messages.push_back(encode_tool_models(session_only));
  ToolModels in_use;
  in_use.machine_id = "G0759";
  in_use.availability = ToolModelAvailability::unavailable;
  in_use.reason = "Tool model settings in /data/G0759 are already in use by another CoffeeCNC. "
                  "Tool models are not saved.";
  in_use.reason_localization =
      MessageKey{"backend.toolModelStore.inUse", {{"path", "/data/G0759"}}};
  messages.push_back(encode_tool_models(in_use));
  if (auto localized = encode_tool_models_reason(in_use))
    messages.emplace_back(std::move(*localized));
  else
    messages.emplace_back(std::unexpected(LocalizedReason{"Missing tool-model reason"}));
  const auto settings_message = [](const OperatorSettings& settings) -> EncodedMessage {
    return encode_operator_settings(settings).transform_error(
        [](std::string detail) { return LocalizedReason{std::move(detail)}; });
  };
  messages.push_back(settings_message({"G0759", 2, ToolModelAvailability::available, "",
                                       OperatorPreferences{StartupUnits::inch,
                                                           {1, 2, 3},
                                                           1800,
                                                           600,
                                                           6.35,
                                                           ProgramLayout::split,
                                                           PanelTheme::dark,
                                                           PanelLanguage::ja,
                                                           {"/home/cnc/nc_files/air/03-pocket.ngc"},
                                                           {{"Vise jaw", {-120.5, 33.25, -10}}},
                                                           40.0}}));
  messages.push_back(settings_message(
      {{}, 0, ToolModelAvailability::session_only, "No machine identity configured", {}}));
  messages.push_back(
      settings_message({"G0759",
                        0,
                        ToolModelAvailability::unavailable,
                        "/home/cnc/operator-settings.json exceeds 16 KiB. The file was left alone.",
                        {},
                        MessageKey{"backend.operatorSettingsStore.fileTooLarge",
                                   {{"file", "/home/cnc/operator-settings.json"}}}}));

  messages.emplace_back(encode_result(
      53, CommandOutcome::rejected, "Default jog speed exceeds the configured machine limit.", {},
      {}, RefusalMessage{"backend.operatorSettings.jogSpeedLimit"}));
  messages.emplace_back(
      encode_result(54, CommandOutcome::rejected, "Invalid panel presentation.", {}, {},
                    RefusalMessage{"backend.operatorSettings.invalidPresentation"}));

  OffsetHistory history{4, true, "", {}};
  const auto change = [](HistoryField field, std::optional<double> before,
                         std::optional<double> after) -> HistoryChange {
    return {field, before, after};
  };
  history.entries = {
      {12, 1700000003000, HistorySource::restore, HistoryTarget::wcs, 1,
       {change(HistoryField::x, 0, 112.5)}, 10},
      {11, 1700000002000, HistorySource::tool_table, HistoryTarget::tool, 3,
       {change(HistoryField::length, 48.2, 50), change(HistoryField::diameter, 6, 6.35)}, {}},
      {10, 1700000001000, HistorySource::zero, HistoryTarget::wcs, 1,
       {change(HistoryField::x, 112.5, 0)}, {}},
      {9, 1700000000500, HistorySource::outside, HistoryTarget::g92, 0,
       {change(HistoryField::z, 0, 2)}, {}},
      {8, 1700000000250, HistorySource::probe, HistoryTarget::wcs, 2,
       {change(HistoryField::rotation_xy, {}, 15)}, {}},
      {7, 1700000000000, HistorySource::tool_table, HistoryTarget::tool, 4,
       {change(HistoryField::length, 61.75, {}), change(HistoryField::diameter, 3.175, {})}, {}}};
  messages.push_back(encode_offset_history(history));
  messages.push_back(
      encode_offset_history({1, false, "No machine identity configured", {}}));
  messages.push_back(
      encode_offset_history({2,
                             true,
                             "The saved offset history was damaged and starts empty. The damaged "
                             "file is kept as offset-history.json.corrupt.",
                             {},
                             MessageKey{"backend.offsetHistory.damagedKept",
                                        {{"file", "offset-history.json.corrupt"}}}}));
  AlarmLogView alarms;
  alarms.saved = true;
  alarms.session = 3;
  alarms.skipped = 1;
  alarms.entries = {
      {1, 4, 1700000000000, MessageLevel::error, "Joint 2 following error", "", ""},
      {2, 1, 1700000100000, MessageLevel::info, "Program loaded", "", ""},
      {2, 2, 1700000105000, MessageLevel::error, "The machine is off.", "rejected",
       R"({"axis":"x","type":"home"})"}};
  messages.emplace_back(encode_alarm_log(alarms));
  messages.emplace_back(encode_alarm_log(
      {true,
       "The alarm log could not be saved: No space left on device",
       3,
       0,
       {},
       MessageKey{"backend.alarmLog.notSaved", {{"detail", "No space left on device"}}}}));
  messages.emplace_back(encode_alarm_log({false, "No machine identity configured", 1, 0, {}}));
  messages.emplace_back(
      encode_backup_written({"/media/cnc/STICK/coffeecnc-G0759-20261123-140200.tar.gz",
                             8,
                             5321,
                             {{"LIB:basic_sim.tcl", "a HAL file of LinuxCNC's own library",
                               MessageKey{"backend.machineData.libHal"}}}}));
  messages.emplace_back(
      encode_backup_written({"/home/cnc/coffeecnc-machine-20261123-140200.tar.gz", 3, 1024, {}}));
  messages.emplace_back(encode_result(31, CommandOutcome::accepted,
                                      "Nothing to restore: every file is the same.", {}, {}, {},
                                      MessageKey{"backend.machineData.nothing"}));
  constexpr auto configs = "/home/cnc/linuxcnc/configs/g0759/";
  messages.emplace_back(encode_restore_summary(
      {"/media/cnc/STICK/coffeecnc-G0759-20261123-140200.tar.gz",
       "3f1c8e0a9b2d4c75",
       "G0759",
       1795960920000,
       {{"ini", "config/g0759.ini", std::string(configs) + "g0759.ini", "same", 4012, false},
        {"hal", "config/g0759.hal", std::string(configs) + "g0759.hal", "changed", 2210, true},
        {"tool_table", "tool-table/tool.tbl", std::string(configs) + "tool.tbl", "changed", 310,
         false},
        {"parameters", "parameters/g0759.var", std::string(configs) + "g0759.var", "at_exit", 1882,
         false},
        {"data", "data/tool-models.json",
         "/home/cnc/.local/share/coffeecnc/G0759/tool-models.json", "new", 512, false}}}));
  messages.emplace_back(encode_restore_summary(
      {"/home/cnc/old.tar.gz",
       "0000000000000001",
       std::nullopt,
       0,
       {{"ini", "config/mill.ini", "/home/cnc/linuxcnc/configs/mill/mill.ini", "same", 10, false}}}));

  Diagnostics diagnostics;
  const auto row = [](DiagnosticRole role, std::optional<int> joint, std::string name,
                      DiagnosticState state, DiagnosticValue value, std::string label = {},
                      bool signal = false) -> DiagnosticReading {
    return {{.role = role,
             .joint = joint,
             .axis = joint ? std::optional<char>{'x'} : std::nullopt,
             .label = std::move(label),
             .name = std::move(name)},
            state,
            value,
            signal};
  };
  diagnostics.inputs = {
      row(DiagnosticRole::min_limit, 0, "joint.0.neg-lim-sw-in", DiagnosticState::undriven, false),
      row(DiagnosticRole::home_switch, 0, "joint.0.home-sw-in", DiagnosticState::live, true),
      row(DiagnosticRole::amp_fault, 0, "joint.0.amp-fault-in", DiagnosticState::unwired, false),
      row(DiagnosticRole::estop_chain, {}, "iocontrol.0.emc-enable-in", DiagnosticState::live,
          true),
      row(DiagnosticRole::spindle_speed_out, {}, "spindle.0.speed-out", DiagnosticState::live,
          1200.0),
      row(DiagnosticRole::spindle_speed_in, {}, "spindle.0.speed-in", DiagnosticState::live,
          std::numeric_limits<double>::infinity()),
      row(DiagnosticRole::configured, {}, "setter-tripped", DiagnosticState::live, false,
          "Setter contact", true),
      row(DiagnosticRole::configured, {}, "vfd.modbus-errors", DiagnosticState::missing, {},
          "VFD errors")};
  diagnostics.tools = {{HalTool::halshow, true, false},
                       {HalTool::halmeter, true, true},
                       {HalTool::halscope, false, false}};
  messages.push_back(encode_diagnostics(diagnostics));
  messages.emplace_back(encode_result(
      50, CommandOutcome::rejected,
      "halscope is not installed: it is not in the search path (/usr/bin:/bin).", {}, {},
      RefusalMessage{"backend.halDiagnostics.notInstalled",
                     {{"name", "halscope"}, {"path", "/usr/bin:/bin"}}}));
  messages.emplace_back(encode_result(55, CommandOutcome::failed,
                                      "LinuxCNC is not waiting for that tool change any more.", {},
                                      {}, {}, MessageKey{"backend.nmlPort.toolChangeGone"}));
  return messages;
}

// `null`, `false`, `true`, `zero` or `nonzero`: what the panel must tell apart.
auto shape(const Json& value) -> std::string {
  if (value.is_number())
    return value == 0 ? "zero" : "nonzero";
  return value.dump();
}

// What one fixture is an example of.
auto variants(const Json& message) -> std::set<std::string> {
  const auto type = message.at("type").get<std::string>();
  std::set<std::string> found{type};
  const auto add = [&found, &type](const std::string& field, const std::string& value) -> void {
    found.insert(type + ' ' + field + ' ' + value);
  };
  if (type == "hello") {
    add("homing_required", shape(message.at("homing_required")));
    add("simulated", shape(message.at("simulated")));
    add("machine_units", message.at("machine_units").get<std::string>());
    const auto& caps = message.at("axis_max_jog_velocity");
    add("axis_max_jog_velocity", caps.at("x") == caps.at("z") ? "equal" : "per axis");
    add("machine_id", message.at("machine_id").is_null() ? "null" : "given");
    add("tool_model_storage", message.at("tool_model_storage").dump());
    add("macros", message.contains("macros") ? "some" : "absent");
    add("diagnostics", message.contains("diagnostics") ? shape(message.at("diagnostics")) : "absent");
  } else if (type == "status") {
    for (const auto* field : {"link", "machine", "mode", "interp", "program_units"})
      add(field, message.at(field).get<std::string>());
    add("wait", message.at("wait").at("kind").get<std::string>());
    add("feed_mode", message.at("motion").at("feed_mode").get<std::string>());
    add("direction", message.at("spindle").at("direction").get<std::string>());
    add("actual_rpm", shape(message.at("spindle").at("actual_rpm")));
    add("at_speed", shape(message.at("spindle").at("at_speed")));
    add("probe_input", shape(message.at("probe_input")));
    const auto& first_axis = message.at("axes").at(0);
    add("reference_mark",
        first_axis.contains("reference_mark") ? shape(first_axis.at("reference_mark")) : "absent");
    add("prepared", message.at("tool").at("prepared").is_null() ? "null" : "tool");
    add("file", message.at("program").at("file").is_null() ? "null" : "path");
  } else if (type == "tool_table") {
    add("tools", message.at("tools").empty() ? "empty" : "some");
  } else if (type == "tool_models") {
    add("availability", message.at("availability").get<std::string>());
    add("machine_id", message.at("machine_id").is_null() ? "null" : "given");
    add("document", message.at("document").is_null() ? "null" : "given");
    if (!message.at("document").is_null()) {
      add("assignments", message.at("document").at("assignments").empty() ? "empty" : "some");
      for (const auto& assignment : message.at("document").at("assignments"))
        add("overrides", assignment.at("overrides").empty() ? "empty" : "some");
    }
  } else if (type == "work_offsets") {
    for (const auto& system : message.at("systems")) {
      add("available", system.at("available").get<bool>() ? "true" : "false");
      add("current", system.at("current").get<bool>() ? "true" : "false");
    }
  } else if (type == "program") {
    add("truncated", message.at("truncated").get<bool>() ? "true" : "false");
  } else if (type == "preview") {
    add("state", message.at("state").get<std::string>());
    add("error", message.at("error").is_null() ? "null" : "given");
    add("bounds", message.at("bounds").is_null() ? "null" : "given");
    for (const auto& segment : message.at("segments"))
      add("kind", segment.at("kind").get<std::string>());
  } else if (type == "message") {
    add("level", message.at("level").get<std::string>());
  } else if (type == "result") {
    add("outcome", message.at("outcome").get<std::string>());
  } else if (type == "offset_history") {
    add("storage", message.at("storage").get<std::string>());
    add("entries", message.at("entries").empty() ? "empty" : "some");
    for (const auto& entry : message.at("entries")) {
      add("target", entry.at("target").get<std::string>());
      add("restores", entry.at("restores").is_null() ? "null" : "given");
      for (const auto& change : entry.at("changes")) {
        add("before", change.at("before").is_null() ? "null" : "given");
        add("after", change.at("after").is_null() ? "null" : "given");
      }
    }
  } else if (type == "alarm_log") {
    add("storage", message.at("storage").get<std::string>());
    add("entries", message.at("entries").empty() ? "empty" : "some");
    for (const auto& entry : message.at("entries")) {
      add("outcome", entry.at("outcome").is_null() ? "null" : "given");
      add("command", entry.at("command").is_null() ? "null" : "given");
    }
  } else if (type == "backup_written") {
    add("not_included", message.at("not_included").empty() ? "empty" : "some");
  } else if (type == "restore_summary") {
    add("machine_id", message.at("machine_id").is_null() ? "null" : "given");
    for (const auto& file : message.at("files")) {
      add("role", file.at("role").get<std::string>());
      add("change", file.at("change").get<std::string>());
      add("symlink", file.at("symlink").get<bool>() ? "true" : "false");
    }
  } else if (type == "diagnostics") {
    for (const auto& input : message.at("inputs")) {
      add("state", input.at("state").get<std::string>());
      add("kind", input.at("kind").get<std::string>());
      const auto& value = input.at("value");
      add("value", value.is_boolean() ? "bit" : value.is_number() ? "number" : shape(value));
      add("joint", input.contains("joint") ? "given" : "absent");
      add("label", input.contains("label") ? "given" : "absent");
    }
    for (const auto& tool : message.at("tools")) {
      add("available", shape(tool.at("available")));
      add("open", shape(tool.at("open")));
    }
  } else if (type == "directory") {
    add("parent", message.at("parent").is_null() ? "null" : "path");
    add("truncated", message.at("truncated").get<bool>() ? "true" : "false");
    add("entries", message.at("entries").empty() ? "empty" : "some");
    for (const auto& entry : message.at("entries"))
      add("kind", entry.at("kind").get<std::string>());
  }
  return found;
}

// Every FromMachine type of protocol v1 and every variant a panel handles differently. A type or
// variant added to the protocol is added here and to the file.
constexpr auto required_variants = std::to_array<std::string_view>({
    "hello",
    "hello homing_required true",
    "hello homing_required false",
    "hello simulated true",
    "hello simulated false",
    "hello machine_units mm",
    "hello machine_units inch",
    "hello axis_max_jog_velocity equal",
    "hello axis_max_jog_velocity per axis",
    "status",
    "status link live",
    "status link stale",
    "status link down",
    "status machine estop",
    "status machine on",
    "status mode manual",
    "status mode mdi",
    "status mode auto",
    "status interp idle",
    "hello machine_id null",
    "hello machine_id given",
    "hello tool_model_storage 1",
    "hello macros some",
    "hello macros absent",
    "tool_models",
    "tool_models availability available",
    "tool_models availability session_only",
    "tool_models availability unavailable",
    "tool_models machine_id null",
    "tool_models machine_id given",
    "tool_models document null",
    "tool_models document given",
    "tool_models assignments some",
    "tool_models overrides some",
    "status interp reading",
    "status interp paused",
    "status interp waiting",
    "status program_units mm",
    "status program_units inch",
    "status wait none",
    "status wait paused",
    "status wait tool_change",
    "status wait dwell",
    "status feed_mode per_minute",
    "status feed_mode inverse_time",
    "status feed_mode per_revolution",
    "status direction stopped",
    "status direction forward",
    "status direction reverse",
    "status actual_rpm null",
    "status actual_rpm zero",
    "status actual_rpm nonzero",
    "status at_speed null",
    "status at_speed false",
    "status at_speed true",
    "status probe_input null",
    "status probe_input false",
    "status probe_input true",
    "status reference_mark absent",
    "status reference_mark false",
    "status reference_mark true",
    "status prepared null",
    "status prepared tool",
    "status file null",
    "status file path",
    "tool_table",
    "tool_table tools empty",
    "tool_table tools some",
    "work_offsets",
    "work_offsets available true",
    "work_offsets available false",
    "program",
    "program truncated false",
    "program truncated true",
    "preview",
    "preview state pending",
    "preview state ready",
    "preview state failed",
    "preview state unavailable",
    "preview error null",
    "preview error given",
    "preview bounds null",
    "preview bounds given",
    "preview kind rapid",
    "preview kind feed",
    "message",
    "message level error",
    "message level info",
    "result",
    "result outcome accepted",
    "result outcome rejected",
    "result outcome failed",
    "result outcome timed_out",
    "directory",
    "directory parent null",
    "directory parent path",
    "directory truncated false",
    "directory truncated true",
    "directory entries empty",
    "directory entries some",
    "directory kind directory",
    "directory kind file",
    "offset_history",
    "offset_history storage saved",
    "offset_history storage session_only",
    "offset_history entries empty",
    "offset_history entries some",
    "offset_history target wcs",
    "offset_history target g92",
    "offset_history target tool",
    "offset_history restores null",
    "offset_history restores given",
    "offset_history before null",
    "offset_history before given",
    "offset_history after null",
    "offset_history after given",
    "tool_models_reason",
    "backup_written",
    "backup_written not_included empty",
    "backup_written not_included some",
    "restore_summary",
    "restore_summary machine_id given",
    "restore_summary machine_id null",
    "restore_summary role ini",
    "restore_summary role hal",
    "restore_summary role tool_table",
    "restore_summary role parameters",
    "restore_summary role data",
    "restore_summary change same",
    "restore_summary change changed",
    "restore_summary change new",
    "restore_summary change at_exit",
    "restore_summary symlink true",
    "restore_summary symlink false",
    "alarm_log",
    "alarm_log storage saved",
    "alarm_log storage session_only",
    "alarm_log entries empty",
    "alarm_log entries some",
    "alarm_log outcome null",
    "alarm_log outcome given",
    "alarm_log command null",
    "alarm_log command given",
    "hello diagnostics true",
    "hello diagnostics absent",
    "diagnostics",
    "diagnostics state missing",
    "diagnostics state unwired",
    "diagnostics state undriven",
    "diagnostics state live",
    "diagnostics kind pin",
    "diagnostics kind signal",
    "diagnostics value bit",
    "diagnostics value number",
    "diagnostics value null",
    "diagnostics joint given",
    "diagnostics joint absent",
    "diagnostics label given",
    "diagnostics label absent",
    "diagnostics available true",
    "diagnostics available false",
    "diagnostics open true",
    "diagnostics open false",
});

void every_machine_message_encodes() {
  std::ifstream file{COFFEECNC_MESSAGE_FIXTURES};
  require(file.is_open(), std::string("Fixture file opens: ") + COFFEECNC_MESSAGE_FIXTURES);
  const auto fixtures = Json::parse(file);
  require(fixtures.is_array() && !fixtures.empty(), "Message fixture file is a nonempty array");

  const auto messages = encoded_messages();
  require(messages.size() == fixtures.size(),
          "The file holds " + std::to_string(messages.size()) + " messages");
  std::set<std::string> covered;
  for (std::size_t i = 0; i < messages.size(); ++i) {
    const auto& fixture = fixtures.at(i);
    const auto label = "Message " + std::to_string(i) + " (" + fixture.value("type", "?") + ")";
    const auto& message = messages.at(i);
    require(message.has_value(), label + " encodes: " + message.error_or(LocalizedReason{}).text);
    if (!message)
      continue;
    require(Json::parse(*message) == fixture, label + " is what the encoder sends: " + *message);
    covered.merge(variants(fixture));
  }
  for (const auto variant : required_variants)
    require(covered.contains(std::string(variant)),
            "Fixture file has an example of: " + std::string(variant));
}
} // namespace

auto main() -> int {
  try {
    every_panel_command_decodes();
    every_machine_message_encodes();
    std::cout << checks << " protocol fixture checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
