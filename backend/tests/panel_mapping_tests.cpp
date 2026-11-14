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
              r.tool_table.revision == 1,
          "Before the first reading: link down; the loaded tool table is already there");

  rig.mailbox.put(rig.status(), rig.now);
  r = rig.source.read();
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
  rig.mailbox.put(stale, rig.now);
  require(rig.source.read().status.link == Link::stale, "A stale reading stays stale when fresh");

  rig.mailbox.put(NmlStatus{}, rig.now);
  r = rig.source.read();
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

void source_tool_table() {
  Rig rig;
  rig.mailbox.put(rig.status(), rig.now);
  auto r = rig.source.read();
  require(r.tool_table.revision == 1 &&
              r.tool_table.tools ==
                  std::vector<Tool>{{1, 1, 50, 6, "end mill"}, {2, 2, 40, 3, ""}},
          "The table LinuxCNC has loaded, with the file's comments by tool number");

  // The file changes on disk and LinuxCNC has not loaded it: nothing changes.
  write(rig.config.tool_table, "T1 P1 Z99 D9 ;edited\nT2 P2 Z40 D3 ;also\nT8 P3 ;new\n");
  r = rig.source.read();
  require(r.tool_table.revision == 1 && r.tool_table.tools[0] == Tool{1, 1, 50, 6, "end mill"} &&
              r.tool_table.tools.size() == 2,
          "A file edited on disk that LinuxCNC has not loaded changes nothing");

  // LinuxCNC loads it: the loaded table changes, and the comments are the file's of now.
  rig.loaded = std::vector<LoadedTool>{
      {1, {1, 1, 99, 9, ""}}, {2, {2, 2, 40, 3, ""}}, {3, {8, 3, 0, 0, ""}}};
  r = rig.source.read();
  require(r.tool_table.revision == 2 &&
              r.tool_table.tools == std::vector<Tool>{{1, 1, 99, 9, "edited"},
                                                      {2, 2, 40, 3, "also"},
                                                      {8, 3, 0, 0, "new"}},
          "After the load the panel's table is the loaded one");
  require(rig.source.read().tool_table.revision == 2, "And is not sent again unchanged");

  // A load the port reports that changed only a comment.
  write(rig.config.tool_table, "T1 P1 Z99 D9 ;renamed\nT2 P2 Z40 D3 ;also\nT8 P3 ;new\n");
  require(rig.source.read().tool_table.tools[0].comment == "edited",
          "A comment is not taken from the file before a load");
  ++rig.loads.tool_tables;
  r = rig.source.read();
  require(r.tool_table.revision == 3 && r.tool_table.tools[0].comment == "renamed",
          "A load the port reports takes the comments of the file again");

  // A reload in progress reads as nothing: the last table stays, and the mapping with it.
  rig.loaded.reset();
  r = rig.source.read();
  require(r.tool_table.revision == 3 && r.tool_table.tools.size() == 3 &&
              r.status.tool.prepared == 2 && r.messages.empty(),
          "While the table cannot be read the last one stays");

  // Non-ascending rows and a T0 row: the prepared index is resolved in the loaded table.
  rig.loaded = table;
  auto s = rig.status();
  s.prepared_pocket = 3;
  rig.mailbox.put(s, rig.now);
  r = rig.source.read();
  require(r.status.tool.prepared == 9 && r.tool_table.tools.size() == 4 &&
              r.tool_table.tools[1].number == 0,
          "The prepared index names the loaded entry; a T0 row is shown as a row");
}

void source_without_a_tool_table() {
  Rig rig;
  rig.loaded.reset();
  rig.mailbox.put(rig.status(), rig.now);
  auto r = rig.source.read();
  require(r.tool_table.revision == 0 && r.tool_table.tools.empty() && r.messages.size() == 1 &&
              r.messages[0].text.find("tool table cannot be read") != std::string::npos &&
              !r.status.tool.prepared,
          "A table that was never read: none shown, no prepared tool, said once");
  require(rig.source.read().messages.empty(), "Once");
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
    source_tool_table();
    source_without_a_tool_table();
    std::cout << checks << " panel mapping checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
