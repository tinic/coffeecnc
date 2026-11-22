#include <coffeecnc/app/messages.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace coffeecnc;

namespace {
using Json = nlohmann::json;

int checks = 0;
void require(bool condition, const std::string& message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

constexpr auto nan = std::numeric_limits<double>::quiet_NaN();
constexpr auto infinity = std::numeric_limits<double>::infinity();
constexpr std::uint64_t max_safe_integer = 9007199254740991;

auto parsed(const Encoded& encoded) -> Json {
  require(encoded.has_value(), "Message encodes: " + encoded.error_or(""));
  return Json::parse(encoded.value_or("null"));
}

// The message is refused and the reason names the field.
void refused(const Encoded& encoded, const std::string& field) {
  require(!encoded.has_value(), "Refused, not sent: " + field);
  require(encoded.error_or("").starts_with(field + ' '),
          "The reason names " + field + ": " + encoded.error_or(""));
}

auto valid_hello() -> Hello {
  Hello hello;
  hello.machine_name = "Mill";
  hello.axes = {Axis::x, Axis::y, Axis::z};
  hello.jog_increments = {0.1, 1};
  hello.max_jog_velocity = 1500;
  hello.axis_max_jog_velocity = {1500, 1500, 750};
  hello.max_feed_override = 1.2;
  hello.min_spindle_override = 0.5;
  hello.max_spindle_override = 1.5;
  return hello;
}

auto valid_status() -> Status {
  Status status;
  status.link = Link::live;
  status.axes = {AxisStatus{Axis::x, true, false, 1, 2, 3, -10, 10, false}};
  status.wait = WaitDwell{2};
  return status;
}

void hello_rules() {
  const auto hello = parsed(encode_hello(valid_hello()));
  require(hello.at("protocol") == 1 && hello.at("protocol").is_number_integer(),
          "Hello carries protocol version 1");
  require(hello.at("axis_max_jog_velocity") == Json{{"x", 1500}, {"y", 1500}, {"z", 750}},
          "Per-axis jog ceilings are keyed by lowercase axis letter");
  require(hello.at("homing_required") == false, "homing_required is sent when false");
  require(hello.size() == 17, "Hello has the fields of protocol v1 and no other");
  require(hello.at("quit_supported") == true, "quit is supported");
  require(hello.at("machine_id").is_null() && hello.at("tool_model_storage") == 1 &&
              hello.at("tool_model_storage").is_number_integer(),
          "Without a machine ID it is sent as null; the storage capability is always 1");
  require(hello.at("operator_settings_storage") == 1,
          "Operator settings storage capability is advertised");
  auto limited = valid_hello();
  limited.max_spindle_rpm = 3000;
  require(parsed(encode_hello(limited)).at("max_spindle_rpm") == 3000,
          "Spindle preset ceiling is sent");
  for (const auto bad : {0.0, -1.0, nan, infinity}) {
    limited.max_spindle_rpm = bad;
    refused(encode_hello(limited), "max_spindle_rpm");
  }
  require(!hello.contains("coolant"), "Unknown coolant outputs are not sent");
  auto cooled = valid_hello();
  cooled.coolant = CoolantOutputs{.flood = true, .mist = false};
  require(!hello.contains("touch_probe_tool"), "No touch probe tool is sent unless configured");
  auto probed = valid_hello();
  probed.touch_probe_tool = 99;
  require(parsed(encode_hello(probed)).at("touch_probe_tool") == 99,
          "The configured touch probe tool is sent as touch_probe_tool");
  require(parsed(encode_hello(cooled)).at("coolant") == Json{{"flood", true}, {"mist", false}},
          "Known coolant outputs are sent as coolant.flood and coolant.mist");
  auto named = valid_hello();
  named.machine_id = "mill-1";
  require(parsed(encode_hello(named)).at("machine_id") == "mill-1" &&
              parsed(encode_hello(named)).at("tool_model_storage") == 1,
          "A machine ID is sent as configured");

  auto two_axes = valid_hello();
  two_axes.axes = {Axis::x, Axis::z};
  two_axes.axis_max_jog_velocity = {1500, std::nullopt, 750};
  require(parsed(encode_hello(two_axes)).at("axis_max_jog_velocity") ==
              Json{{"x", 1500}, {"z", 750}},
          "Only configured axes are in the map");

  auto missing = valid_hello();
  missing.axis_max_jog_velocity.at(2).reset();
  refused(encode_hello(missing), "axis_max_jog_velocity");
  for (const auto bad : {0.0, -1.0, nan, infinity}) {
    auto invalid = valid_hello();
    invalid.axis_max_jog_velocity.at(1) = bad;
    refused(encode_hello(invalid), "axis_max_jog_velocity");
  }
  const std::vector<std::pair<std::string, std::function<void(Hello&)>>> numbers{
      {"jog_increments", [](Hello& h) -> void { h.jog_increments.back() = nan; }},
      {"max_jog_velocity", [](Hello& h) -> void { h.max_jog_velocity = infinity; }},
      {"max_feed_override", [](Hello& h) -> void { h.max_feed_override = nan; }},
      {"min_spindle_override", [](Hello& h) -> void { h.min_spindle_override = -infinity; }},
      {"max_spindle_override", [](Hello& h) -> void { h.max_spindle_override = nan; }},
  };
  for (const auto& [field, spoil] : numbers) {
    auto invalid = valid_hello();
    spoil(invalid);
    refused(encode_hello(invalid), field);
  }

  auto probing = valid_hello();
  probing.fixed_setter = FixedSetterLimits{90, 300, 98, 0.5, 80, 25, 1};
  probing.fixed_setter->material_geometry = {8, 4, 6, 3};
  probing.touch_probe_tool = 99;
  const auto limits = parsed(encode_hello(probing)).at("fixed_setter");
  require(limits.at("latch_feed_mm_min") == 25 && limits.at("latch_backoff_mm") == 1 &&
              limits.at("check_tolerance_mm") == 0.1 &&
              limits.at("material_travel_mm") == 20 &&
              limits.at("material_geometry") == Json{{"reach_mm", 8}, {"margin_mm", 4},
                                                    {"clearance_mm", 6}, {"depth_mm", 3}},
          "Probe setup guides receive the configured limits in millimetres");
  auto invalid_probe = probing;
  invalid_probe.fixed_setter->latch_feed_mm_min = nan;
  refused(encode_hello(invalid_probe), "fixed_setter.latch_feed_mm_min");
  invalid_probe = probing;
  invalid_probe.fixed_setter->material_travel_mm = infinity;
  refused(encode_hello(invalid_probe), "fixed_setter.material_travel_mm");
  probing.fixed_setter->latch_feed_mm_min = 0;
  const auto single_pass = parsed(encode_hello(probing)).at("fixed_setter");
  require(!single_pass.contains("latch_feed_mm_min") && !single_pass.contains("latch_backoff_mm") &&
              !single_pass.contains("material_travel_mm"),
          "A single-pass setter advertises no latch or material cycle");
}

void status_rules() {
  const auto status = parsed(encode_status(valid_status(), 7));
  require(status.at("seq") == 7 && status.at("seq").is_number_integer(),
          "The caller's seq is sent as an integer");
  require(status.size() == 18, "Status has the fields of protocol v1 and no other");
  const auto& spindle = status.at("spindle");
  require(spindle.contains("actual_rpm") && spindle.at("actual_rpm").is_null() &&
              spindle.contains("at_speed") && spindle.at("at_speed").is_null() &&
              status.contains("probe_input") && status.at("probe_input").is_null(),
          "A signal the machine does not have is an explicit null, not absent");
  require(status.at("tool").at("prepared").is_null() && status.at("program").at("file").is_null(),
          "No prepared tool and no program are null");
  require(!status.at("axes").at(0).contains("reference_mark"),
          "An axis of a machine without reference marks sends no such field");
  auto unmarked = valid_status();
  unmarked.axes.front().reference_mark = false;
  unmarked.axes.front().at_reference_mark = false;
  const auto unmarked_axis = parsed(encode_status(unmarked, 1)).at("axes").at(0);
  require(unmarked_axis.at("reference_mark") == false &&
              unmarked_axis.at("at_reference_mark") == false,
          "A reference mark not found yet is sent as false, not left out");

  auto rotary = valid_status();
  rotary.rotary_axes.push_back({'b', true, false, 45.0, {}, 0.0, true});
  const auto angles = parsed(encode_status(rotary, 1)).at("rotary_axes");
  require(angles == Json::array({Json{{"axis", "b"}, {"homed", true}, {"homing", false},
                                     {"machine", 45.0}, {"work", nullptr}, {"dtg", 0.0},
                                     {"on_hard_limit", true}}}),
          "Rotary angles use degrees and missing positions are sent as null");

  auto measured = valid_status();
  measured.spindle.commanded_rpm = 6000;
  measured.spindle.actual_rpm = 0;
  measured.spindle.at_speed = false;
  measured.probe_input = false;
  const auto zero = parsed(encode_status(measured, 1));
  require(zero.at("spindle").at("actual_rpm").is_number() &&
              zero.at("spindle").at("actual_rpm") == 0 &&
              zero.at("spindle").at("at_speed") == false && zero.at("probe_input") == false,
          "A measured zero and a reported false are not null");
  require(zero.at("spindle").at("commanded_rpm") == 6000,
          "The measured speed is never taken from the commanded speed");

  const std::vector<std::pair<std::string, std::function<void(Status&)>>> numbers{
      {"axes.machine", [](Status& s) -> void { s.axes.front().machine = nan; }},
      {"axes.work", [](Status& s) -> void { s.axes.front().work = infinity; }},
      {"axes.dtg", [](Status& s) -> void { s.axes.front().dtg = nan; }},
      {"axes.min_limit", [](Status& s) -> void { s.axes.front().min_limit = -infinity; }},
      {"axes.max_limit", [](Status& s) -> void { s.axes.front().max_limit = nan; }},
      {"offsets.g5x", [](Status& s) -> void { s.offsets.g5x[0] = nan; }},
      {"offsets.g92", [](Status& s) -> void { s.offsets.g92[1] = nan; }},
      {"offsets.tool", [](Status& s) -> void { s.offsets.tool[2] = infinity; }},
      {"offsets.rotation_xy", [](Status& s) -> void { s.offsets.rotation_xy = nan; }},
      {"motion.velocity", [](Status& s) -> void { s.motion.velocity = nan; }},
      {"motion.programmed_feed", [](Status& s) -> void { s.motion.programmed_feed = nan; }},
      {"motion.feed_override", [](Status& s) -> void { s.motion.feed_override = infinity; }},
      {"motion.rapid_override", [](Status& s) -> void { s.motion.rapid_override = nan; }},
      {"spindle.commanded_rpm", [](Status& s) -> void { s.spindle.commanded_rpm = nan; }},
      {"spindle.override", [](Status& s) -> void { s.spindle.override = nan; }},
      {"spindle.actual_rpm", [](Status& s) -> void { s.spindle.actual_rpm = nan; }},
      {"spindle.actual_rpm", [](Status& s) -> void { s.spindle.actual_rpm = -0.5; }},
      {"wait.seconds_left", [](Status& s) -> void { s.wait = WaitDwell{nan}; }},
      {"program.revision", [](Status& s) -> void { s.program.revision = max_safe_integer + 1; }},
      {"preview_context", [](Status& s) -> void { s.preview_context = max_safe_integer + 1; }},
  };
  for (const auto& [field, spoil] : numbers) {
    auto invalid = valid_status();
    spoil(invalid);
    refused(encode_status(invalid, 1), field);
  }
  refused(encode_status(valid_status(), max_safe_integer + 1), "seq");
  auto largest = valid_status();
  largest.program.revision = max_safe_integer;
  largest.preview_context = max_safe_integer;
  const auto exact = parsed(encode_status(largest, max_safe_integer));
  require(exact.at("seq") == max_safe_integer && exact.at("preview_context") == max_safe_integer,
          "2^53 - 1 is the largest counter sent");

  // The default AxisState limits of the policy input are NaN: a placeholder status must not
  // copy them.
  auto placeholder = valid_status();
  placeholder.link = Link::down;
  placeholder.axes.front().min_limit = AxisState{}.min_limit;
  refused(encode_status(placeholder, 1), "axes.min_limit");
}

void table_rules() {
  ToolTable table;
  table.tools.emplace_back(); // Tool's default length and diameter are NaN.
  refused(encode_tool_table(table), "tools.length");
  table.tools.back().length = 10;
  refused(encode_tool_table(table), "tools.diameter");
  table.tools.back().diameter = 3;
  table.revision = max_safe_integer + 1;
  refused(encode_tool_table(table), "revision");
  table.revision = 2;
  table.tools.back().comment = "Fr\xE4ser";
  require(parsed(encode_tool_table(table)).at("tools").at(0).at("comment") == "Fr\xEF\xBF\xBDser",
          "A comment that is not UTF-8 is replaced, and the table is still sent");

  WorkOffsets offsets;
  const auto systems = parsed(encode_work_offsets(offsets)).at("systems");
  require(systems.size() == 9, "All nine work systems are sent");
  for (std::size_t i = 0; i < systems.size(); ++i)
    require(systems.at(i).at("wcs") == i + 1 && systems.at(i).at("offset") == Json{0, 0, 0},
            "Systems are numbered 1 to 9 in order, offsets as three numbers");
  require(systems.at(0).at("available") == false, "A system is not available unless it says so");
  offsets.systems.at(1) = {{1, 2, 3}, 15, true};
  const auto marked = parsed(encode_work_offsets(offsets)).at("systems");
  require(marked.at(1).at("available") == true && marked.at(2).at("available") == false &&
              marked.at(2).at("offset") == Json{0, 0, 0},
          "Available and not available are both said; a system that is not keeps finite zeros");
  offsets.systems.at(4).offset[1] = nan;
  refused(encode_work_offsets(offsets), "systems.offset");
  offsets.systems.at(4).offset[1] = 0;
  offsets.systems.at(8).rotation_xy = infinity;
  refused(encode_work_offsets(offsets), "systems.rotation_xy");
  offsets.systems.at(8).rotation_xy = 0;
  offsets.revision = max_safe_integer + 1;
  refused(encode_work_offsets(offsets), "revision");

  OffsetHistory history{1, true, "", {}};
  history.entries.push_back({3, 1700000000000, HistorySource::touch_off, HistoryTarget::wcs, 1,
                             {{HistoryField::x, std::nullopt, 2.5}}, std::nullopt});
  const auto entry = parsed(encode_offset_history(history)).at("entries").at(0);
  require(entry.at("changes").at(0).at("before").is_null() &&
              entry.at("changes").at(0).at("after") == 2.5 && entry.at("restores").is_null(),
          "An unknown value and no restored entry are null, never zero");
  history.entries[0].changes[0].after = nan;
  refused(encode_offset_history(history), "entries.changes.after");
  history.entries[0].changes[0].after = 2.5;
  history.entries[0].changes[0].before = infinity;
  refused(encode_offset_history(history), "entries.changes.before");
  history.entries[0].changes[0].before = 0;
  history.entries[0].time = -1;
  refused(encode_offset_history(history), "entries.time");
  history.entries[0].time = 0;
  history.entries[0].restores = max_safe_integer + 1;
  refused(encode_offset_history(history), "entries.restores");
}

void program_and_preview_rules() {
  ProgramText program{1, "/nc/part.ngc", {"G0 X1", "(Fr\xE4ser)"}, true};
  const auto text = parsed(encode_program(program));
  require(text.size() == 5 && text.at("truncated") == true,
          "A program that was cut says so on the wire");
  program.truncated = false;
  require(parsed(encode_program(program)).at("truncated") == false,
          "A whole program says so explicitly");
  require(text.at("lines") == Json{"G0 X1", "(Fr\xEF\xBF\xBDser)"},
          "A line that is not UTF-8 is replaced, and the program is still sent");
  program.revision = max_safe_integer + 1;
  refused(encode_program(program), "revision");

  Preview preview;
  preview.state = PreviewState::ready;
  preview.segments = {{3, MoveKind::feed, 2, {{1, 2, 3}, {4, 5, 6}}}, {4, MoveKind::rapid, 2, {}}};
  preview.bounds = Bounds{{1, 2, 3}, {4, 5, 6}};
  const auto sent = parsed(encode_preview(preview));
  require(sent.at("segments").at(0).at("points") == Json{1, 2, 3, 4, 5, 6},
          "Points are flattened x, y, z");
  require(sent.at("segments").at(1).at("points") == Json::array(),
          "A segment without points has an empty array");
  require(sent.at("bounds") == Json{{"min", {1, 2, 3}}, {"max", {4, 5, 6}}}, "Bounds are two Vec3");
  auto invalid = preview;
  invalid.segments.front().points.back()[2] = nan;
  refused(encode_preview(invalid), "segments.points");
  invalid = preview;
  invalid.bounds = Bounds{{-infinity, 2, 3}, {4, 5, 6}};
  refused(encode_preview(invalid), "bounds.min");
  invalid = preview;
  invalid.bounds = Bounds{{1, 2, 3}, {4, 5, nan}};
  refused(encode_preview(invalid), "bounds.max");
  preview.context = max_safe_integer + 1;
  refused(encode_preview(preview), "context");
}

void directory_rules() {
  Directory directory;
  directory.path = "/nc";
  directory.truncated = true;
  directory.entries = {{"caf\xE9.ngc", EntryKind::file, 10, -1}};
  const auto sent = parsed(encode_directory(directory));
  require(sent.size() == 5 && sent.at("truncated") == true,
          "A listing that was cut says so on the wire");
  require(sent.at("parent").is_null(), "The top of the program directory has a null parent");
  const auto& entry = sent.at("entries").at(0);
  require(entry.at("bytes").is_number_integer() && entry.at("modified").is_number_integer() &&
              entry.at("modified") == -1,
          "Size and time are integers; a time before 1970 is negative");
  directory.entries.front().bytes = max_safe_integer + 1;
  refused(encode_directory(directory), "entries.bytes");
  directory.entries.front().bytes = max_safe_integer;
  directory.entries.front().modified = std::numeric_limits<std::int64_t>::max();
  refused(encode_directory(directory), "entries.modified");
  directory.entries.front().modified = std::numeric_limits<std::int64_t>::min();
  refused(encode_directory(directory), "entries.modified");
}
} // namespace

auto main() -> int {
  try {
    hello_rules();
    status_rules();
    table_rules();
    program_and_preview_rules();
    directory_rules();
    std::cout << checks << " message encoding checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
