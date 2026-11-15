// From LinuxCNC's view to the panel's messages, on constructed readings: to_hello,
// to_panel_status, to_work_systems, comments_of and NmlPanelSource on a mailbox, a loaded tool
// table and load counts the test fills, and files it writes. No LinuxCNC runs.
//   coffeecnc_panel_mapping_tests <scratch directory>

#include <coffeecnc/linuxcnc/nml_panel_source.hpp>
#include <coffeecnc/linuxcnc/panel_mapping.hpp>
#include <coffeecnc/linuxcnc/tool_table.hpp>

#include <chrono>
#include <cmath>
#include <map>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
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
std::filesystem::path scratch;

void require(bool condition, const std::string& message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

void write(const std::filesystem::path& file, std::string_view text) {
  std::ofstream out{file, std::ios::binary | std::ios::trunc};
  out << text;
}

// A mill with the G0759's values.
auto machine() -> MachineConfig {
  return MachineConfig{
      .ini_file = "/machine/mill.ini",
      .directory = "/machine",
      .machine_name = "G0759",
      .nml_file = "/machine/linuxcnc.nml",
      .machine_units = LengthUnit::mm,
      .millimetres_per_unit = 1,
      .axes = {Axis::x, Axis::y, Axis::z},
      .travel = {Travel{-1000, 1000}, Travel{-500, 500}, Travel{-300, 10}},
      .jog_increments = {5, 1, 0.5},
      .limits = Limits{1500, {1500, 1500, 750}, 2.0, 0.5, 1.0, 1500, false},
      .home_all_configured = false,
      .signals = {},
      .tool_table = (scratch / "tool.tbl").string(),
      .parameter_file = {},
      .postgui_halfile = {},
      .program_prefix = scratch.string(),
  };
}

// A reading as NmlStatusReader makes it: everything already in millimetres.
auto reading() -> NmlStatus {
  NmlStatus s;
  auto& c = s.controller;
  c.link = Link::live;
  c.machine = MachineState::on;
  c.mode = TaskMode::automatic;
  c.interp = InterpState::reading;
  c.axes[0] = {true, true, false, false, 12.5, -1000, 1000};
  c.axes[1] = {true, true, true, false, -3, -500, 500};
  c.axes[2] = {true, false, false, true, 7, -300, 10};
  c.in_position = false;
  c.tool_in_spindle = 1;
  c.program_units = LengthUnit::inch;
  s.work = {2.5, -13, 1};
  s.dtg = {0.5, 0, -0.25};
  s.g5x = {10, 10, 5};
  s.g92 = {0, 0, 1};
  s.tool_offset = {0, 0, 40};
  s.rotation_xy = 15;
  s.velocity = 300;
  s.feed_override = 1.2;
  s.rapid_override = 0.5;
  s.spindle_rpm = 1200;
  s.spindle_override = 0.9;
  s.programmed_feed = 254;
  s.feed_mode = FeedMode::per_revolution;
  s.active_codes = {"G1", "G55", "G59.1", "M3"};
  s.program_file = "/nc/part.ngc";
  s.prepared_pocket = 2;
  s.wcs = 2;
  s.current_line = 12;
  s.motion_line = 10;
  s.spindle_direction = SpindleDirection::reverse;
  s.flood = true;
  s.optional_stop = true;
  s.block_delete = true;
  return s;
}

// As LinuxCNC has it loaded: not in ascending order, with a T0 row, and an index without a row.
const std::vector<LoadedTool> table{{1, {5, 1, 50, 6, ""}},
                                    {2, {0, 0, 0, 0, ""}},
                                    {3, {9, 7, 40, 3, ""}},
                                    {5, {2, 3, 30, 1, ""}}};

void hello_from_the_configuration() {
  const auto hello = to_hello(machine());
  require(hello.machine_name == "G0759" && !hello.simulated &&
              hello.axes == std::vector<Axis>{Axis::x, Axis::y, Axis::z} &&
              hello.machine_units == LengthUnit::mm &&
              hello.jog_increments == std::vector<double>{5, 1, 0.5} &&
              hello.max_jog_velocity == 1500 && hello.axis_max_jog_velocity[2] == 750.0 &&
              hello.axis_max_jog_velocity[0] == 1500.0 && !hello.homing_required &&
              hello.max_feed_override == 2.0 && hello.min_spindle_override == 0.5 &&
              hello.max_spindle_override == 1.0,
          "hello carries the configuration's name, axes, units, increments, ceilings and limits");
  auto required = machine();
  required.limits.homing_required = true;
  required.machine_units = LengthUnit::inch;
  require(to_hello(required).homing_required &&
              to_hello(required).machine_units == LengthUnit::inch,
          "Homing policy and unit follow the configuration");
}

void status_fields() {
  const auto status = to_panel_status(reading(), machine(), table, 4);
  require(status.link == Link::live && status.machine == MachineState::on &&
              status.mode == TaskMode::automatic && status.interp == InterpState::reading,
          "Link, machine, mode and interpreter state are the controller's");
  require(status.axes.size() == 3 &&
              status.axes[0] == AxisStatus{Axis::x, true, false, 12.5, 2.5, 0.5, -1000, 1000, false} &&
              status.axes[1] == AxisStatus{Axis::y, true, true, -3, -13, 0, -500, 500, false} &&
              status.axes[2] == AxisStatus{Axis::z, false, false, 7, 1, -0.25, -300, 10, true},
          "Axes: homed, homing, machine, work, distance to go, limits, hard limit; no value is "
          "scaled again");
  require(status.offsets == Offsets{2, {10, 10, 5}, {0, 0, 1}, {0, 0, 40}, 15},
          "Offsets: active system, G5x, G92, tool offset, rotation");
  require(status.motion ==
              MotionStatus{300, FeedMode::per_revolution, 254, 1.2, 0.5, false},
          "Motion: velocity, feed mode, F word, overrides, in position");
  require(status.spindle.direction == SpindleDirection::reverse &&
              status.spindle.commanded_rpm == 1200 && status.spindle.override == 0.9 &&
              !status.spindle.actual_rpm && !status.spindle.at_speed && !status.probe_input,
          "Spindle as commanded; no measurement where the machine has no signal");
  require(status.coolant == CoolantStatus{true, false}, "Coolant");
  require(status.tool.in_spindle == 1 && status.tool.prepared == 0,
          "The prepared tool is the number of the loaded entry LinuxCNC's index names: here T0");
  require(status.program ==
              ProgramStatus{std::string("/nc/part.ngc"), 4, 12, 10, true, true},
          "Program: file, revision, lines, optional stop, block delete");
  require(std::holds_alternative<WaitNone>(status.wait) &&
              status.active_codes == std::vector<std::string>{"G1", "G55", "G59.1", "M3"} &&
              status.program_units == LengthUnit::inch && status.preview_context == 0,
          "Active codes and program unit; the preview context is the publisher's");
}

void optional_signals() {
  auto s = reading();
  s.spindle_actual_rpm = 0;
  s.spindle_at_speed = false;
  s.probe_input = true;
  const auto status = to_panel_status(s, machine(), table, 1);
  require(status.spindle.actual_rpm == 0.0 && status.spindle.at_speed == false &&
              status.probe_input == true,
          "A measured zero and a reported false are values, not absence");
}

void prepared_tool() {
  auto s = reading();
  const auto prepared = [&](std::optional<int> index) -> std::optional<int> {
    s.prepared_pocket = index;
    return to_panel_status(s, machine(), table, 1).tool.prepared;
  };
  require(prepared(1) == 5 && prepared(2) == 0 && prepared(3) == 9 && prepared(5) == 2,
          "The index names an entry of the loaded table, whatever the order of tool numbers; a "
          "T0 row is a tool like any other");
  require(!prepared({}) && !prepared(0) && !prepared(4) && !prepared(6) && !prepared(-1),
          "No prepared tool without an index, for the spindle's index 0, for an index without "
          "an entry and beyond the table");
  s.controller.requested_tool = 9;
  s.prepared_pocket = {};
  const auto status = to_panel_status(s, machine(), table, 1);
  require(status.tool.in_spindle == 1 && !status.tool.prepared,
          "A tool change request changes neither the loaded nor the prepared tool");
}

void waits() {
  auto s = reading();
  s.dwell_seconds_left = 1.5;
  auto wait = to_panel_status(s, machine(), table, 1).wait;
  require(wait == Wait{WaitDwell{1.5}}, "A dwell with time left");
  s.paused = true;
  wait = to_panel_status(s, machine(), table, 1).wait;
  require(wait == Wait{WaitPaused{}}, "Paused goes before a dwell");
  s.controller.requested_tool = 0;
  wait = to_panel_status(s, machine(), table, 1).wait;
  require(wait == Wait{WaitToolChange{0}}, "A tool change request goes first, also for T0");
}

void no_program_and_no_status() {
  auto s = reading();
  s.program_file.clear();
  require(!to_panel_status(s, machine(), table, 0).program.file, "No program: file is null");

  const auto down = to_panel_status(NmlStatus{}, machine(), {}, 0);
  require(down.link == Link::down && down.axes.size() == 3 && down.offsets.wcs == 1 &&
              down.axes[2].min_limit == -300 && down.axes[2].max_limit == 10 &&
              !down.program.file && !down.tool.prepared,
          "Without a status: placeholders, the configured travel as limits");
  for (const auto& axis : down.axes)
    require(std::isfinite(axis.machine) && std::isfinite(axis.min_limit) &&
                std::isfinite(axis.max_limit),
            "Every placeholder is a finite number");
  bool none = true;
  for (const auto& system : to_work_systems(NmlStatus{}))
    none = none && system == WorkSystem{{}, 0, false};
  require(none, "Without a status no work system is available");
}

void work_systems() {
  auto s = reading();
  const auto only = [&](std::size_t active, const WorkSystem& expected) -> bool {
    const auto systems = to_work_systems(s);
    bool right = true;
    for (std::size_t i = 0; i < systems.size(); ++i)
      right = right && systems.at(i) == (i == active ? expected : WorkSystem{{}, 0, false});
    return right;
  };
  require(only(1, WorkSystem{{10, 10, 5}, 15, true}),
          "The active system is available with the status's offset and rotation; the other "
          "eight are not available and hold zeros");
  s.wcs = 9;
  s.g5x = {1, 2, 3};
  s.rotation_xy = 0;
  require(only(8, WorkSystem{{1, 2, 3}, 0, true}), "G59.3 is system 9");
  s.wcs = 0;
  require(only(9, {}), "An index outside 1 to 9 names no system");
}

void tool_comments() {
  const auto comments = comments_of("; a comment line\n"
                                    "T1 P1 Z50 D6 ;6 mm end mill\n"
                                    "\n"
                                    "T0 P0 ;\n"
                                    "t7 p3 z+1.5 ;  spaced  \n"
                                    "T3 P4 Z-2\n"
                                    "T1 P9 ;second row for tool 1\n"
                                    "P9 Z1 ;no tool number\n");
  require(comments.size() == 2 && comments.at(1) == "6 mm end mill" && comments.at(7) == "spaced",
          "Comments by tool number: without the white space around them, the first row of a "
          "tool, none for an empty comment or a line without a T word");
  require(comments_of("").empty(), "An empty table has no comments");
}

// The source on a mailbox, a loaded table and load counts of the test's own.
struct Rig {
  MachineConfig config{machine()};
  NmlStatusMailbox mailbox;
  NmlLoads loads;
  std::optional<std::vector<LoadedTool>> loaded{std::vector<LoadedTool>{
      {1, {1, 1, 50, 6, ""}}, {2, {2, 2, 40, 3, ""}}}};
  NmlPanelSource::Clock::time_point now{NmlPanelSource::Clock::time_point{} + 1h};
  // How long reading one program text takes.
  std::chrono::milliseconds read_time{};
  int reads{};
  std::filesystem::path program{scratch / "part.ngc"};
  NmlPanelSource source{
      config, mailbox, [this]() -> std::optional<std::vector<LoadedTool>> { return loaded; },
      [this](std::string_view path,
             std::uint64_t revision) -> std::expected<ProgramText, std::string> {
        ++reads;
        now += read_time;
        if (path.ends_with("refused.ngc"))
          return std::unexpected(std::string("outside the program directory."));
        return ProgramText{revision, std::string(path), {"G0 X1", "M2"}, false};
      },
      &loads, [this]() -> NmlPanelSource::Clock::time_point { return now; }};

  Rig() { write(config.tool_table, "T1 P1 Z50 D6 ;end mill\nT2 P2 Z40 D3\n"); }
  // One publishing period: time passes, the status thread has read `s` again, the source reads.
  auto step(const NmlStatus& s) -> PanelReading {
    now += 50ms;
    mailbox.put(s, now);
    return source.read();
  }
  auto step() -> PanelReading { return step(status()); }
  // A live status with the program loaded, read just now.
  auto status() const -> NmlStatus {
    auto s = reading();
    s.program_file = program.string();
    s.prepared_pocket = 2;
    return s;
  }
};

void source_status_and_age() {
  Rig rig;
  require(rig.source.hello() == to_hello(rig.config), "hello is the configuration's");
  auto r = rig.source.read();
  require(r.status.link == Link::down && !r.program && r.messages.empty() &&
              r.tool_table.revision == 0,
          "Before the first reading: link down, and no tool table is shown yet");

  static_cast<void>(rig.step());
  r = rig.step();
  require(r.status.link == Link::live && r.status.tool.prepared == 2 &&
              r.status.axes[0].machine == 12.5,
          "A fresh reading is live and mapped with the loaded table");
  require(r.work_systems[1] == WorkSystem{{10, 10, 5}, 15, true} &&
              r.work_systems[0] == WorkSystem{{}, 0, false},
          "Only the active work system is available");

  rig.now += 100ms;
  require(rig.source.read().status.link == Link::live, "A reading 100 ms old is still live");
  rig.now += 1ms;
  r = rig.source.read();
  require(r.status.link == Link::stale && r.status.axes[0].machine == 12.5,
          "Older than that it is stale: the status thread has stopped delivering");
  auto stale = rig.status();
  stale.controller.link = Link::stale;
  require(rig.step(stale).status.link == Link::stale, "A stale reading stays stale when fresh");

  r = rig.step(NmlStatus{});
  require(r.status.link == Link::down && !r.status.program.file,
          "A reading without a status is link down");
}

// The age that counts is the one at the end of the call, after every read.
void source_ages_the_status_after_its_reads() {
  Rig rig;
  rig.read_time = 150ms; // longer than the bound
  rig.mailbox.put(rig.status(), rig.now);
  auto r = rig.source.read();
  require(rig.reads == 1 && r.program && r.status.link == Link::stale,
          "A status that was fresh before a slow file read goes out stale, not live");
  rig.mailbox.put(rig.status(), rig.now);
  require(rig.source.read().status.link == Link::live && rig.reads == 1,
          "Without a read in the way a fresh status is live");
}

void source_program_loads() {
  Rig rig;
  rig.mailbox.put(rig.status(), rig.now);
  auto r = rig.source.read();
  require(r.program && r.program->revision == 1 && r.status.program.revision == 1 &&
              r.program->file == rig.program.string() && rig.reads == 1,
          "A program task names is read, as revision 1");
  require(rig.source.read().program->revision == 1 && rig.reads == 1,
          "And kept while nothing is loaded");

  // The same path again, the same file on disk: LinuxCNC accepted another open_program.
  ++rig.loads.programs;
  r = rig.source.read();
  require(r.program && r.program->revision == 2 && r.status.program.revision == 2 &&
              r.program->file == rig.program.string() && rig.reads == 2,
          "A reload of the same unchanged file is a new revision, and its text is read again");
  require(rig.source.read().program->revision == 2 && rig.reads == 2, "Once per load");

  // Loaded by something else: the status names another file.
  auto s = rig.status();
  s.program_file = (scratch / "other.ngc").string();
  rig.mailbox.put(s, rig.now);
  r = rig.source.read();
  require(r.program && r.program->revision == 3 && r.program->file == s.program_file &&
              r.status.program.file == s.program_file && rig.reads == 3,
          "Another file named by task is a new revision without a counted load");

  s.program_file = (scratch / "refused.ngc").string();
  rig.mailbox.put(s, rig.now);
  r = rig.source.read();
  require(!r.program && r.status.program.revision == 4 &&
              r.status.program.file == s.program_file && r.messages.size() == 1 &&
              r.messages[0].error &&
              r.messages[0].text.find("outside the program directory") != std::string::npos,
          "A program whose text cannot be read: named in the status, no text, said once");
  require(rig.source.read().messages.empty() && rig.reads == 4, "And not asked for again");

  s.program_file.clear();
  rig.mailbox.put(s, rig.now);
  r = rig.source.read();
  require(!r.program && !r.status.program.file && r.status.program.revision == 0,
          "No program loaded");
  rig.mailbox.put(rig.status(), rig.now);
  r = rig.source.read();
  require(r.program && r.program->revision == 5, "Loaded again after none: a new revision");
}

// Task's line counters belong to task, not to a load: the status reports what was attributed
// to the loaded revision, never the counters themselves.
void source_lines_belong_to_the_revision() {
  Rig rig;
  const auto lines = [](const PanelReading& r) -> std::pair<int, int> {
    return {r.status.program.current_line, r.status.program.motion_line};
  };
  const auto at = [&](TaskMode mode, InterpState interp, int current, int motion,
                      bool moving = false) -> NmlStatus {
    auto s = rig.status();
    s.controller.mode = mode;
    s.controller.interp = interp;
    s.controller.in_position = !moving;
    s.current_line = current;
    s.motion_line = motion;
    return s;
  };
  const auto automatic = TaskMode::automatic;
  const auto reading = InterpState::reading;
  const auto idle = InterpState::idle;
  const std::pair none{0, 0};

  // The status that makes the source take the revision up was read before it did.
  auto r = rig.step(at(automatic, reading, 4, 4, true));
  require(r.status.program.revision == 1 && lines(r) == none,
          "A status read before the revision was taken up attributes nothing");
  require(lines(rig.step(at(automatic, idle, 0, 0))) == none, "Idle after the open");
  require(lines(rig.step(at(automatic, reading, 2, 0))) == std::pair{2, 0},
          "A counter that moved while the program runs is reported");
  require(lines(rig.step(at(automatic, InterpState::paused, 2, 0))) == std::pair{2, 0},
          "Paused in auto: the line is kept");
  require(lines(rig.step(at(automatic, reading, 7, 6))) == std::pair{7, 6}, "Running on");
  require(lines(rig.step(at(automatic, idle, 7, 7))) == std::pair{7, 7},
          "The first idle status after the run gives the lines it stopped at");
  require(lines(rig.step(at(automatic, idle, 9, 0))) == std::pair{7, 7},
          "An idle counter is not passed through: what was attributed stays");

  // A status that is not fresh changes nothing, whatever it shows.
  auto stale = at(automatic, reading, 8, 8, true);
  stale.controller.link = Link::stale;
  require(lines(rig.step(stale)) == std::pair{7, 7}, "A stale status attributes nothing");
  rig.mailbox.put(at(TaskMode::mdi, reading, 1, 1), rig.now - 60ms);
  require(lines(rig.source.read()) == std::pair{7, 7},
          "Nor does one read before the status judged last");

  // An MDI block after a valid run: the counters are no longer the program's.
  require(lines(rig.step(at(TaskMode::mdi, reading, 7, 7, true))) == none,
          "An MDI block executing: nothing is reported");
  require(lines(rig.step(at(TaskMode::mdi, idle, 5, 5))) == none &&
              lines(rig.step(at(TaskMode::manual, idle, 5, 5))) == none &&
              lines(rig.step(at(automatic, idle, 5, 5))) == none,
          "MDI finished and idle, manual, auto again: still nothing");
  require(lines(rig.step(at(automatic, reading, 5, 5))) == none,
          "The run has started but the counters still read what they read before it");
  require(lines(rig.step(at(automatic, reading, 1, 5))) == std::pair{1, 0},
          "Each counter is back from its first change in the run");
  require(lines(rig.step(at(automatic, reading, 3, 2))) == std::pair{3, 2} &&
              lines(rig.step(at(automatic, idle, 3, 3))) == std::pair{3, 3},
          "And follows task to the end of the run");

  // A run again whose first line seen is the one the last run ended at: motion shows that the
  // counters are of this run.
  require(lines(rig.step(at(automatic, reading, 3, 3))) == none &&
              lines(rig.step(at(automatic, reading, 3, 3, true))) == std::pair{3, 3} &&
              lines(rig.step(at(automatic, idle, 3, 3))) == std::pair{3, 3},
          "Equal counters are attributed once the machine moves in the run");
  auto dwell = at(automatic, reading, 3, 3);
  dwell.dwell_seconds_left = 0.5;
  require(lines(rig.step(dwell)) == std::pair{3, 0},
          "A dwell is a block of this run: the current line is attributed, the motion line "
          "only by motion");
  static_cast<void>(rig.step(at(automatic, idle, 3, 3)));

  // An MDI block too short for any status to show it: the port's count.
  ++rig.loads.mdi_blocks;
  require(lines(rig.step(at(automatic, idle, 6, 6))) == none,
          "An MDI block the port reports ends the attribution");
  require(lines(rig.step(at(automatic, reading, 6, 6))) == none &&
              lines(rig.step(at(automatic, reading, 7, 6))) == std::pair{7, 0},
          "A run right after it: its first status is what the counters must move from");
  static_cast<void>(rig.step(at(automatic, idle, 7, 7)));

  // B is opened: task zeroes the counters, and an E-stop brings A's current line back.
  ++rig.loads.programs;
  r = rig.step(at(automatic, idle, 0, 0));
  require(r.status.program.revision == 2 && lines(r) == none, "A new revision has no lines");
  require(lines(rig.step(at(automatic, idle, 7, 0))) == none,
          "A line the previous program left is not reported for the new revision");
  // A run too short for a status to show it.
  require(lines(rig.step(at(automatic, idle, 4, 3))) == none,
          "Counters that changed without a status showing the run are withheld");

  // A status of the run of the revision before, still in the mailbox when the load is seen.
  rig.now += 50ms;
  rig.mailbox.put(at(automatic, reading, 9, 9, true), rig.now - 1ms);
  ++rig.loads.programs;
  r = rig.source.read();
  require(r.status.program.revision == 3 && lines(r) == none,
          "A status read before the load says nothing about the new revision");
  require(lines(rig.step(at(automatic, reading, 9, 9, true))) == std::pair{9, 9},
          "The same read after it does");

  // The status thread was held up after it had acquired a status of the run before, and
  // left it in the mailbox only after the load: it is dated when its acquisition began.
  static_cast<void>(rig.step(at(automatic, idle, 9, 9)));
  ++rig.loads.programs;
  rig.now += 50ms;
  r = rig.source.read();
  require(r.status.program.revision == 4 && lines(r) == none, "Loaded again");
  rig.mailbox.put(at(automatic, reading, 6, 6, true), rig.now - 1ms);
  require(lines(rig.source.read()) == none,
          "A status put after the load but acquired before it is not evidence");
  rig.mailbox.put(at(automatic, reading, 6, 6, true), rig.now + 1ms);
  require(lines(rig.source.read()) == std::pair{6, 6}, "One acquired after it is");

  // Another file named by task while it runs (loaded and started by another client).
  auto other = at(automatic, reading, 9, 8, true);
  other.program_file = (scratch / "other.ngc").string();
  r = rig.step(other);
  require(r.status.program.revision == 5 && lines(r) == none, "First seen: a new revision");
  require(lines(rig.step(other)) == std::pair{9, 8}, "Seen executing after that: its lines");
}

void source_tool_table() {
  Rig rig;
  auto r = rig.step();
  require(r.tool_table.revision == 0 && !r.status.tool.prepared,
          "One pass over the loaded table is not shown, and resolves no prepared tool");
  r = rig.step();
  require(r.tool_table.revision == 1 &&
              r.tool_table.tools ==
                  std::vector<Tool>{{1, 1, 50, 6, "end mill"}, {2, 2, 40, 3, ""}} &&
              r.status.tool.prepared == 2,
          "Read the same a period later, with a status since that shows no load: the table "
          "LinuxCNC has loaded, with the file's comments by tool number");

  // The file changes on disk and LinuxCNC has not loaded it: nothing changes.
  write(rig.config.tool_table, "T1 P1 Z99 D9 ;edited\nT2 P2 Z40 D3 ;also\nT8 P3 ;new\n");
  r = rig.step();
  require(r.tool_table.revision == 1 && r.tool_table.tools[0] == Tool{1, 1, 50, 6, "end mill"} &&
              r.tool_table.tools.size() == 2,
          "A file edited on disk that LinuxCNC has not loaded changes nothing");

  // LinuxCNC loads it. While task has the load in execution, a table that reads the same twice
  // may still be half of it: it is not shown, and the prepared index is not resolved in the
  // old one. That holds whatever command task has taken up since: the status says only that
  // something is in execution, not what.
  auto loading = rig.status();
  loading.task_executing = true;
  loading.command_done = true; // of a later command, taken up and done while io still loads
  rig.loaded = std::vector<LoadedTool>{{1, {1, 1, 99, 9, ""}}};
  r = rig.step(loading);
  require(r.tool_table.revision == 1 && !r.status.tool.prepared,
          "A table that differs from the accepted one: the old one stays shown, and the "
          "prepared tool is unknown rather than a number from the old table");
  static_cast<void>(rig.step(loading));
  r = rig.step(loading);
  require(r.tool_table.revision == 1 && r.tool_table.tools.size() == 2 && !r.status.tool.prepared,
          "Two equal passes do not make a table while task is loading: io may stand still "
          "half way");
  rig.loaded = std::vector<LoadedTool>{
      {1, {1, 1, 99, 9, ""}}, {2, {2, 2, 40, 3, ""}}, {3, {8, 3, 0, 0, ""}}};
  static_cast<void>(rig.step(loading));
  r = rig.step(loading);
  require(r.tool_table.revision == 1, "Still loading: still the old table");
  r = rig.step();
  require(r.tool_table.revision == 2 &&
              r.tool_table.tools == std::vector<Tool>{{1, 1, 99, 9, "edited"},
                                                      {2, 2, 40, 3, "also"},
                                                      {8, 3, 0, 0, "new"}} &&
              r.status.tool.prepared == 2,
          "The load is over and the table reads the same again: it is shown, with the "
          "comments of the file of now, and the prepared tool resolves again");
  require(rig.step().tool_table.revision == 2, "And is not sent again unchanged");

  // The status that vouches must have been read after the first pass.
  rig.loaded = std::vector<LoadedTool>{{1, {7, 1, 1, 1, ""}}};
  static_cast<void>(rig.step());
  r = rig.source.read();
  require(r.tool_table.revision == 2,
          "A second equal pass without a status read since the first is not enough");
  r = rig.step();
  require(r.tool_table.revision == 3 && r.tool_table.tools[0].number == 7,
          "With one it is");
  rig.loaded = std::vector<LoadedTool>{
      {1, {1, 1, 99, 9, ""}}, {2, {2, 2, 40, 3, ""}}, {3, {8, 3, 0, 0, ""}}};
  static_cast<void>(rig.step());
  static_cast<void>(rig.step());

  // A load the port reports that changed only a comment.
  write(rig.config.tool_table, "T1 P1 Z99 D9 ;renamed\nT2 P2 Z40 D3 ;also\nT8 P3 ;new\n");
  require(rig.step().tool_table.tools[0].comment == "edited",
          "A comment is not taken from the file before a load");
  ++rig.loads.tool_tables;
  r = rig.step();
  require(r.tool_table.revision == 5 && r.tool_table.tools[0].comment == "renamed",
          "A load the port reports takes the comments of the file again");

  // The table cannot be read after the prepared index changed: no number from the old table.
  rig.loaded.reset();
  auto s = rig.status();
  s.prepared_pocket = 3;
  r = rig.step(s);
  require(r.tool_table.revision == 5 && r.tool_table.tools.size() == 3 &&
              !r.status.tool.prepared && r.messages.size() == 1 &&
              r.messages[0].text.find("tool table cannot be read") != std::string::npos,
          "While the table cannot be read the last one stays shown, the prepared tool is "
          "unknown, and it is said once");
  require(rig.step(s).messages.empty() && !rig.step(s).status.tool.prepared, "Once");
  rig.loaded = std::vector<LoadedTool>{
      {1, {1, 1, 99, 9, ""}}, {2, {2, 2, 40, 3, ""}}, {3, {8, 3, 0, 0, ""}}};
  r = rig.step(s);
  require(r.status.tool.prepared == 8, "Readable again and the same table: resolved again");

  // Non-ascending rows and a T0 row: the prepared index is resolved in the loaded table.
  rig.loaded = table;
  static_cast<void>(rig.step(s));
  r = rig.step(s);
  require(r.status.tool.prepared == 9 && r.tool_table.tools.size() == 4 &&
              r.tool_table.tools[1].number == 0,
          "The prepared index names the loaded entry; a T0 row is shown as a row");
}

// The table with every field, for the preview: accepted like the table shown, on every field.
void source_preview_tools() {
  Rig rig;
  std::vector<PreviewTool> raw{{0, 1, 1, {0, 0, 50}, 6, 0, 0, 0},
                               {1, 1, 1, {0, 0, 50}, 6, 0, 0, 0},
                               {2, 2, 2, {0, 0, 40}, 3, 0, 0, 0}};
  rig.source.raw_tools([&]() -> std::vector<PreviewTool> { return raw; });
  auto r = rig.step();
  require(r.preview_tools.empty() && !r.tools_settled, "One pass is not a table yet");
  r = rig.step();
  require(r.tool_table.revision == 1 && r.preview_tools == raw && r.tools_settled,
          "The second equal pass: the table shown and the table with every field, the "
          "spindle's entry too, are accepted together");
  // A difference only the full table has: an X offset.
  const auto accepted = raw;
  raw[1].offset[0] = 0.5;
  r = rig.step();
  require(r.tool_table.revision == 1 && r.preview_tools == accepted && !r.tools_settled,
          "A pass that differs only in an X offset is not accepted at once: the accepted "
          "table stays the preview's");
  raw[1].offset[0] = 0.7; // another value: caught while it changes
  r = rig.step();
  require(r.preview_tools == accepted && !r.tools_settled, "Nor one that differs again");
  r = rig.step();
  require(r.preview_tools == raw && r.tools_settled && r.tool_table.revision == 1,
          "The identical next pass is accepted");
  // While task executes, nothing is accepted.
  raw[0].number = 2;
  auto busy = rig.status();
  busy.task_executing = true;
  static_cast<void>(rig.step(busy));
  r = rig.step(busy);
  require(r.preview_tools[0].number == 1 && !r.tools_settled,
          "A changed spindle entry is not accepted while task has something in execution");
  static_cast<void>(rig.step());
  r = rig.step();
  require(r.preview_tools == raw && r.tools_settled, "Idle between two equal passes: accepted");
  raw = r.preview_tools;
  require(rig.step().tools_settled && rig.step().preview_tools == raw, "And kept");

  // Churn: the full table differs on every pass for a while, only in an X offset. Nothing
  // is accepted during it: the table accepted before stays shown and stays the preview\x27s.
  const auto before = raw;
  const auto shown = rig.step().tool_table;
  for (int i = 1; i <= 8; ++i) {
    raw[2].offset[0] = 0.1 * i;
    r = rig.step();
    require(r.preview_tools == before && !r.tools_settled && r.tool_table == shown,
            "While the table differs on every pass none is accepted");
  }
  // It stops changing, but task has something in execution between the equal passes.
  r = rig.step(busy);
  r = rig.step(busy);
  require(r.preview_tools == before && !r.tools_settled && r.tool_table == shown,
          "Equal passes with task executing between them are not accepted either");
  // Equal passes and a status between them that shows task idle: accepted then.
  static_cast<void>(rig.step());
  r = rig.step();
  require(r.preview_tools == raw && r.tools_settled && r.tool_table == shown,
          "Identical passes with task idle between: accepted, in one step");
}

void source_without_a_tool_table() {
  Rig rig;
  rig.loaded.reset();
  auto r = rig.step();
  require(r.tool_table.revision == 0 && r.tool_table.tools.empty() && r.messages.size() == 1 &&
              r.messages[0].text.find("tool table cannot be read") != std::string::npos &&
              !r.status.tool.prepared,
          "A table that was never read: none shown, no prepared tool, said once");
  require(rig.step().messages.empty(), "Once");
}

} // namespace

auto main(int argc, char** argv) -> int {
  try {
    if (argc != 2)
      throw std::runtime_error("usage: coffeecnc_panel_mapping_tests <scratch directory>");
    scratch = std::filesystem::path(argv[1]) / "panel_mapping";
    std::filesystem::create_directories(scratch);
    hello_from_the_configuration();
    status_fields();
    optional_signals();
    prepared_tool();
    waits();
    no_program_and_no_status();
    work_systems();
    tool_comments();
    source_status_and_age();
    source_ages_the_status_after_its_reads();
    source_program_loads();
    source_lines_belong_to_the_revision();
    source_tool_table();
    source_preview_tools();
    source_without_a_tool_table();
    std::cout << checks << " panel mapping checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
