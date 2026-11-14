#include <coffeecnc/app/status_publisher.hpp>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace coffeecnc;

namespace {
using Json = nlohmann::json;
using Types = std::vector<std::string>;

int checks = 0;
void require(bool condition, const std::string& message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

// What the adapter holds between readings, and the publisher for one panel.
struct Rig {
  Hello hello;
  Status status;
  ToolTable tool_table;
  WorkOffsets work_offsets;
  ProgramText program;
  bool loaded{}; // LinuxCNC has a program
  Preview preview;
  StatusPublisher publisher;
  std::vector<Json> sent;

  Rig() {
    hello.machine_name = "Mill";
    hello.axes = {Axis::x};
    hello.axis_max_jog_velocity = {1500, std::nullopt, std::nullopt};
    tool_table.tools = {{1, 1, 40, 6, "End mill"}};
  }

  auto publish(bool snapshots = false) -> std::expected<std::vector<std::string>, std::string> {
    return publisher.publish(
        {hello, status, tool_table, work_offsets, loaded ? &program : nullptr, preview}, snapshots);
  }
  // The types of the messages one successful call sends, in order.
  auto types(bool snapshots = false) -> Types {
    const auto messages = publish(snapshots);
    require(messages.has_value(), "Publishing succeeds: " + messages.error_or(""));
    Types found;
    sent.clear();
    if (!messages)
      return found;
    for (const auto& text : *messages) {
      sent.push_back(Json::parse(text));
      found.push_back(sent.back().at("type").get<std::string>());
    }
    return found;
  }
  auto seq() -> std::uint64_t { return sent.back().at("seq").get<std::uint64_t>(); }
};

void snapshots_and_sequence() {
  Rig rig;
  require(rig.types(true) == Types{"hello", "tool_table", "work_offsets", "preview", "status"},
          "After ready: hello and every snapshot, the status last; no program without one");
  require(rig.seq() == 1, "The first status is number 1");
  for (std::uint64_t expected = 2; expected < 6; ++expected) {
    require(rig.types() == Types{"status"}, "Nothing but a status while nothing changed");
    require(rig.seq() == expected, "seq increases by one with every status");
  }
  rig.status.motion.velocity = 100;
  require(rig.types() == Types{"status"} && rig.sent.back().at("motion").at("velocity") == 100,
          "A status change needs no other message");

  rig.program = ProgramText{1, "/nc/part.ngc", {"G0 X1"}, false};
  rig.loaded = true;
  require(rig.types(true) ==
              Types{"hello", "tool_table", "work_offsets", "program", "preview", "status"},
          "Another ready sends everything again, unchanged or not, in this order");
  require(rig.seq() == 7, "seq continues across pages");
}

void changes_only() {
  Rig rig;
  static_cast<void>(rig.types(true));

  rig.tool_table.tools.front().length = 41;
  require(rig.types() == Types{"tool_table", "status"} &&
              rig.sent.front().at("tools").at(0).at("length") == 41,
          "Changed tool table content is sent, with the same revision too");
  require(rig.types() == Types{"status"}, "And sent once");
  rig.tool_table.revision = 2;
  require(rig.types() == Types{"tool_table", "status"}, "A new tool table revision is sent");

  rig.work_offsets.systems.at(3).offset[2] = -5;
  require(rig.types() == Types{"work_offsets", "status"}, "Changed work offsets are sent");
  rig.work_offsets.systems.at(3).rotation_xy = 30;
  require(rig.types() == Types{"work_offsets", "status"}, "A changed rotation is sent");
  rig.work_offsets.revision = 9;
  require(rig.types() == Types{"work_offsets", "status"}, "A new work offset revision is sent");

  rig.program = ProgramText{1, "/nc/part.ngc", {"G0 X1"}, false};
  rig.loaded = true;
  require(rig.types() == Types{"program", "status"}, "A loaded program is sent");
  require(rig.types() == Types{"status"}, "And sent once");
  rig.program.revision = 2;
  require(rig.types() == Types{"program", "status"}, "A reloaded program is sent");
  rig.program.lines.emplace_back("M2");
  require(rig.types() == Types{"program", "status"}, "Changed program text is sent");
  rig.program.file = "/nc/other.ngc";
  require(rig.types() == Types{"program", "status"}, "A program under another name is sent");
  rig.loaded = false;
  require(rig.types() == Types{"status"}, "Protocol v1 has no message for no program");
  rig.loaded = true;
  require(rig.types() == Types{"program", "status"},
          "The same program loaded again after none is sent again");

  rig.preview.state = PreviewState::pending;
  rig.preview.context = 5;
  require(rig.types() == Types{"preview", "status"}, "A new preview state is sent");
  rig.preview.state = PreviewState::ready;
  rig.preview.segments = {{1, MoveKind::feed, 1, {{0, 0, 0}, {1, 0, 0}}}};
  rig.preview.bounds = Bounds{{0, 0, 0}, {1, 0, 0}};
  require(rig.types() == Types{"preview", "status"}, "A computed preview is sent");
  require(rig.types() == Types{"status"}, "And sent once");
  rig.preview.segments.front().points.back()[0] = 2;
  require(rig.types() == Types{"preview", "status"}, "A changed preview point is sent");
  rig.preview.context = 6;
  require(rig.types() == Types{"preview", "status"}, "A preview for another context is sent");

  rig.tool_table.revision = 3;
  rig.work_offsets.revision = 10;
  rig.program.revision = 3;
  rig.preview.context = 7;
  require(rig.types() == Types{"tool_table", "work_offsets", "program", "preview", "status"},
          "Several changes in one reading keep the order, the status last");
  rig.hello.machine_name = "Other";
  require(rig.types() == Types{"status"}, "Hello is sent only after ready");
}

void first_call_without_snapshots() {
  Rig rig;
  require(rig.types() == Types{"tool_table", "work_offsets", "preview", "status"},
          "Nothing was sent before: everything but hello is new");
}

void failures_send_nothing() {
  constexpr auto nan = std::numeric_limits<double>::quiet_NaN();
  Rig rig;
  static_cast<void>(rig.types(true));

  rig.tool_table.revision = 2;
  rig.status.motion.velocity = nan;
  auto failed = rig.publish();
  require(!failed.has_value() &&
              failed.error_or("") == "status: motion.velocity is not a finite number.",
          "A status that cannot be encoded sends nothing and names the message and field");
  rig.status.motion.velocity = 0;
  require(rig.types() == Types{"tool_table", "status"},
          "What was due with the failed status is still due");
  require(rig.seq() == 2, "A failed call uses no seq");

  rig.tool_table.tools.front().diameter = nan;
  rig.work_offsets.revision = 4;
  failed = rig.publish();
  require(!failed.has_value() && failed.error_or("").starts_with("tool_table: tools.diameter "),
          "A snapshot that cannot be encoded sends nothing, the status neither");
  rig.tool_table.tools.front().diameter = 6;
  require(rig.types() == Types{"work_offsets", "status"} && rig.seq() == 3,
          "After the repair: the pending change and the next seq; the table equals the one sent");

  rig.hello.max_jog_velocity = nan;
  failed = rig.publish(true);
  require(!failed.has_value() && failed.error_or("").starts_with("hello: max_jog_velocity "),
          "A hello that cannot be encoded sends no snapshot");
}

void preview_contexts() {
  PreviewInputs inputs;
  inputs.program_file = "/nc/part.ngc";
  inputs.program_revision = 3;
  inputs.systems.at(1) = {{100, 100, -100}, 15, true};
  inputs.g92 = {0, 0, 3.5};
  inputs.tools = {{3, 3, 48.2, 6, "End mill"}, {4, 4, 61.75, 3.175, ""}};

  inputs.start = {1,     LengthUnit::mm, {10, 20, 30}, 3,    std::nullopt, {0, 0, 48.2},
                  {"G1", "G17", "G21", "G54", "G90"},  600,  6000,         7};

  PreviewContext counter;
  const auto context = counter.update(inputs, false);
  require(context == 1, "The first inputs are context 1");
  require(counter.update(PreviewInputs{inputs}, false) == context,
          "Equal inputs keep the context");

  const std::vector<std::pair<std::string, std::function<void(PreviewInputs&)>>> changes{
      {"the program is reloaded", [](PreviewInputs& i) -> void { ++i.program_revision; }},
      {"another program", [](PreviewInputs& i) -> void { i.program_file = "/nc/part2.ngc"; }},
      {"no program", [](PreviewInputs& i) -> void { i.program_file.reset(); }},
      {"an empty program name", [](PreviewInputs& i) -> void { i.program_file = ""; }},
      {"a work offset", [](PreviewInputs& i) -> void { i.systems.at(1).offset[0] = 100.001; }},
      {"the offset of an inactive system",
       [](PreviewInputs& i) -> void { i.systems.at(8).offset[2] = 1; }},
      {"a rotation", [](PreviewInputs& i) -> void { i.systems.at(1).rotation_xy = 16; }},
      {"the rotation of another system",
       [](PreviewInputs& i) -> void { i.systems.at(0).rotation_xy = 15; }},
      {"G92", [](PreviewInputs& i) -> void { i.g92[2] = 0; }},
      {"G92 on another axis", [](PreviewInputs& i) -> void { std::swap(i.g92[0], i.g92[2]); }},
      {"a tool length", [](PreviewInputs& i) -> void { i.tools.at(0).length = 48.3; }},
      {"a tool diameter", [](PreviewInputs& i) -> void { i.tools.at(1).diameter = 3; }},
      {"a tool number", [](PreviewInputs& i) -> void { i.tools.at(1).number = 5; }},
      {"a tool pocket", [](PreviewInputs& i) -> void { i.tools.at(1).pocket = 5; }},
      {"a removed tool", [](PreviewInputs& i) -> void { i.tools.pop_back(); }},
      {"an added tool", [](PreviewInputs& i) -> void { i.tools.push_back({9, 9, 1, 1, ""}); }},
      {"tools in another order",
       [](PreviewInputs& i) -> void { std::swap(i.tools.at(0), i.tools.at(1)); }},
      {"a value moved between neighbouring fields",
       [](PreviewInputs& i) -> void { std::swap(i.tools.at(0).length, i.tools.at(0).diameter); }},
      {"the active work system (G54 to G55)", [](PreviewInputs& i) -> void { i.start.wcs = 2; }},
      {"the program unit",
       [](PreviewInputs& i) -> void { i.start.program_units = LengthUnit::inch; }},
      {"block delete", [](PreviewInputs& i) -> void { i.block_delete = true; }},
      {"the start position (a jog)", [](PreviewInputs& i) -> void { i.start.position[0] = 11; }},
      {"the prepared tool", [](PreviewInputs& i) -> void { i.start.tool_prepared = 4; }},
      {"the feed in effect", [](PreviewInputs& i) -> void { i.start.feed = 900; }},
      {"the spindle speed in effect",
       [](PreviewInputs& i) -> void { i.start.spindle_speed = 8000; }},
      {"the interpreter parameters",
       [](PreviewInputs& i) -> void { ++i.start.parameters_revision; }},
      {"the loaded tool", [](PreviewInputs& i) -> void { i.start.tool_in_spindle = 4; }},
      {"the tool offset in effect", [](PreviewInputs& i) -> void { i.start.tool_offset[2] = 0; }},
      {"the distance mode",
       [](PreviewInputs& i) -> void { i.start.active_codes.back() = "G91"; }},
  };
  // Each change gives the next context, and going back to the first inputs gives a new one too:
  // a context is never used for two different sets of inputs.
  auto expected = context;
  for (const auto& [what, change] : changes) {
    auto changed = inputs;
    change(changed);
    require(counter.update(changed, false) == ++expected, "The next context for: " + what);
    require(counter.update(changed, false) == expected, "Kept while unchanged: " + what);
    require(counter.update(inputs, false) == ++expected, "And the next one going back: " + what);
  }

  auto renamed = inputs;
  renamed.tools.at(0).comment = "Roughing";
  require(counter.update(renamed, false) == expected, "A tool comment does not change the path");
  // Availability is an input of its own: the placeholder zeros of a system that is not
  // available can equal the numbers it had.
  auto zero_offset = inputs;
  zero_offset.systems.at(0) = {{0, 0, 0}, 0, true};
  const auto with_zero = counter.update(zero_offset, false);
  auto lost = zero_offset;
  lost.systems.at(0).available = false;
  require(counter.update(lost, false) == with_zero + 1,
          "A system that is no longer available changes the context, also with the same numbers");
  require(counter.update(zero_offset, false) == with_zero + 2, "And so does one that is again");
  expected = counter.update(inputs, false);
  auto negative_zero = inputs;
  negative_zero.systems.at(0).offset[0] = -0.0;
  require(counter.update(negative_zero, false) == expected, "-0.0 is the same offset as 0.0");

  // While the program runs its own G55, tool change and G91 are not a new start state.
  auto during = inputs;
  during.start = {2, LengthUnit::inch, {55, 5, -2}, 4, 5, {0, 0, 61.75}, {"G1", "G55", "G91"},
                  150, 12000, 8};
  require(counter.update(during, true) == expected,
          "The start state is frozen while the program runs");
  during.block_delete = true;
  require(counter.update(during, true) == ++expected,
          "Block delete switched during a run or a pause counts at once");
  auto offset_changed = during;
  offset_changed.systems.at(1).offset[1] = 55;
  require(counter.update(offset_changed, true) == ++expected,
          "A work offset written during the run still counts at once");
  require(counter.update(offset_changed, false) == ++expected,
          "After the run the state it left is a new start state");
  require(counter.update(offset_changed, false) == expected, "And then it is kept");

  PreviewContext fresh;
  require(fresh.update(inputs, true) == 1,
          "A first reading during a run takes the state it finds");
}
} // namespace

auto main() -> int {
  try {
    snapshots_and_sequence();
    changes_only();
    first_call_without_snapshots();
    failures_send_nothing();
    preview_contexts();
    std::cout << checks << " status publishing checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
