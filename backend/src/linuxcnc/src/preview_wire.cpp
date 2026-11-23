#include "preview_wire.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <iterator>
#include <system_error>
#include <utility>

namespace coffeecnc {
namespace {

constexpr std::string_view request_header = "coffeecnc-preview-request 1";

// The next blank-separated word of `text`, which is advanced past it.
auto word(std::string_view& text) -> std::string_view {
  while (!text.empty() && text.front() == ' ')
    text.remove_prefix(1);
  const auto end = text.find(' ');
  const auto found = text.substr(0, end);
  text.remove_prefix(end == std::string_view::npos ? text.size() : end);
  return found;
}

template <typename Number> auto number(std::string_view& text, Number& value) -> bool {
  const auto found = word(text);
  if (found.empty())
    return false;
  const auto [end, error] = std::from_chars(found.data(), found.data() + found.size(), value);
  return error == std::errc{} && end == found.data() + found.size();
}

auto vector(std::string_view& text, Vec3& value) -> bool {
  return number(text, value[0]) && number(text, value[1]) && number(text, value[2]);
}

auto finite_number(std::string_view& text, double& value) -> bool {
  return number(text, value) && std::isfinite(value);
}

auto finite_vector(std::string_view& text, Vec3& value) -> bool {
  return finite_number(text, value[0]) && finite_number(text, value[1]) &&
         finite_number(text, value[2]);
}

auto flag(std::string_view& text, bool& value) -> bool {
  int given = 0;
  if (!number(text, given) || (given != 0 && given != 1))
    return false;
  value = given == 1;
  return true;
}

// The rest of a line as it is: a path or a text.
auto rest(std::string_view text) -> std::string {
  while (!text.empty() && text.front() == ' ')
    text.remove_prefix(1);
  return std::string(text);
}

auto vec(const Vec3& v) -> std::string { return std::format("{} {} {}", v[0], v[1], v[2]); }

auto bounds(std::string_view& line, Bounds& result) -> bool {
  if (!finite_vector(line, result.min) || !finite_vector(line, result.max))
    return false;
  return std::ranges::equal(result.min, result.max, std::less_equal<>{});
}

auto read_check(std::string_view line, std::optional<HelperOutput::ProgramFacts>& check) -> bool {
  const auto type = word(line);
  if (type == "units") {
    HelperOutput::ProgramFacts facts;
    if (check || !flag(line, facts.mm) || !flag(line, facts.inch) ||
        !flag(line, facts.variable_spindle) || !flag(line, facts.feed_per_revolution) ||
        !word(line).empty())
      return false;
    check = std::move(facts);
    return true;
  }
  if (!check)
    return false;
  if (type == "tool") {
    int tool = 0;
    if (!number(line, tool) || tool < 1 || tool > OutputLimits::max_tool ||
        check->tools.size() >= 1000 || std::ranges::find(check->tools, tool) != check->tools.end())
      return false;
    check->tools.push_back(tool);
  } else if (type == "wcs") {
    ProgramFrameCheck frame;
    if (!number(line, frame.wcs) || frame.wcs < 1 || frame.wcs > 9 ||
        !flag(line, frame.available) ||
        std::ranges::find(check->frames, frame.wcs, &ProgramFrameCheck::wcs) != check->frames.end())
      return false;
    if (frame.available && !bounds(line, frame.bounds.emplace()))
      return false;
    check->frames.push_back(frame);
  } else if (type == "machine") {
    if (check->machine || !bounds(line, check->machine.emplace()))
      return false;
  } else if (type == "feed" || type == "spindle") {
    auto& range = type == "feed" ? check->feed : check->spindle;
    if (range)
      return false;
    auto& value = range.emplace();
    if (!finite_number(line, value.min) || !finite_number(line, value.max) || value.min < 0 ||
        value.min > value.max)
      return false;
  } else {
    return false;
  }
  return word(line).empty();
}

auto parse_restart(std::string_view line, const OutputLimits& limits, RestartPoint& p) -> bool {
  int spindle = 0;
  bool inch = false;
  if (!number(line, p.line) || p.line < 1 || p.line > limits.max_line || !number(line, p.tool) ||
      p.tool < 0 || p.tool > OutputLimits::max_tool || !number(line, p.wcs) || p.wcs < 1 ||
      p.wcs > 9 || !number(line, p.h) || (p.h != 0 && p.h != p.tool) || !number(line, spindle) ||
      spindle < -1 || spindle > 1 || !finite_number(line, p.spindle_rpm) || p.spindle_rpm < 0 ||
      !flag(line, p.flood) || !flag(line, p.mist) || !finite_number(line, p.feed) || p.feed < 0 ||
      !flag(line, inch) || !number(line, p.plane) || p.plane < 17 || p.plane > 19 ||
      !flag(line, p.incremental) || !flag(line, p.absolute_arc) || !number(line, p.motion) ||
      (p.motion != 0 && p.motion != 10 && p.motion != 20 && p.motion != 30 && p.motion != 800) ||
      !number(line, p.path_control) ||
      (p.path_control != 610 && p.path_control != 611 && p.path_control != 640) ||
      !finite_number(line, p.path_tolerance) || p.path_tolerance < 0 ||
      !finite_number(line, p.naivecam_tolerance) || p.naivecam_tolerance < 0 ||
      !finite_number(line, p.clearance_z) || !finite_vector(line, p.start) ||
      p.clearance_z < p.start[2] || !finite_vector(line, p.g5x) || !finite_vector(line, p.g92) ||
      !finite_vector(line, p.tool_offset) || !finite_number(line, p.rotation) ||
      !word(line).empty())
    return false;
  p.units = inch ? LengthUnit::inch : LengthUnit::mm;
  p.spindle_direction = spindle == 1    ? SpindleDirection::forward
                        : spindle == -1 ? SpindleDirection::reverse
                                        : SpindleDirection::stopped;
  return true;
}

} // namespace

auto write_request(const HelperRequest& request) -> std::string {
  std::string out{request_header};
  out += '\n';
  const auto line = [&out](std::string_view key, const std::string& value) -> void {
    out += key;
    out += ' ';
    out += value;
    out += '\n';
  };
  line("program", request.program);
  if (!request.subroutines.empty())
    line("subroutines", request.subroutines);
  line("parameters", request.parameters);
  line("units_per_mm", std::format("{}", request.units_per_mm));
  line("position", vec(request.position));
  line("tool_in_spindle", std::format("{}", request.tool_in_spindle));
  line("tool_prepared", std::format("{}", request.tool_prepared));
  line("tool_offset", vec(request.tool_offset));
  line("feed", std::format("{}", request.feed));
  line("block_delete", request.block_delete ? "1" : "0");
  line("wcs", std::format("{}", request.wcs));
  line("known_systems", std::format("{}", request.known_systems));
  line("saved_g28", request.saved_g28 ? "1" : "0");
  line("saved_g30", request.saved_g30 ? "1" : "0");
  for (const auto& code : request.remapped)
    line("remapped", code);
  line("tool_change_at_g30", request.tool_change_at_g30 ? "1" : "0");
  if (request.tool_change_position)
    line("tool_change_position", vec(*request.tool_change_position));
  line("max_points", std::format("{}", request.max_points));
  line("chord_tolerance", std::format("{}", request.chord_tolerance));
  if (request.rigid_tap)
    line("rigid_tap", std::format("{} {}", request.rigid_tap->max_rpm,
                                   request.rigid_tap->reversal_turns));
  if (request.restart_line)
    line("restart_line", std::format("{}", *request.restart_line));
  for (const auto& code : request.codes)
    line("code", code);
  if (!request.inherited_motion.empty())
    line("inherited_motion", request.inherited_motion);
  for (const auto& tool : request.tools)
    line("tool", std::format("{} {} {} {} {} {} {} {} {}", tool.index, tool.number, tool.pocket,
                             vec(tool.offset), tool.diameter, tool.front_angle, tool.back_angle,
                             tool.orientation, 0));
  return out;
}

auto parse_request(std::string_view text) -> std::expected<HelperRequest, std::string> {
  HelperRequest request;
  bool first = true;
  while (!text.empty()) {
    const auto end = text.find('\n');
    if (end == std::string_view::npos)
      return std::unexpected(std::string("the request ends in an incomplete line"));
    auto line = text.substr(0, end);
    text.remove_prefix(end + 1);
    if (std::exchange(first, false)) {
      if (line != request_header)
        return std::unexpected(std::string("not a preview request of this version"));
      continue;
    }
    const auto key = word(line);
    bool good = true;
    if (key == "program")
      request.program = rest(line);
    else if (key == "subroutines")
      request.subroutines = rest(line);
    else if (key == "parameters")
      request.parameters = rest(line);
    else if (key == "units_per_mm")
      good = number(line, request.units_per_mm) && request.units_per_mm > 0;
    else if (key == "position")
      good = vector(line, request.position);
    else if (key == "tool_in_spindle")
      good = number(line, request.tool_in_spindle);
    else if (key == "tool_prepared")
      good = number(line, request.tool_prepared);
    else if (key == "tool_offset")
      good = vector(line, request.tool_offset);
    else if (key == "feed")
      good = number(line, request.feed);
    else if (key == "block_delete")
      good = flag(line, request.block_delete);
    else if (key == "wcs")
      good = number(line, request.wcs) && request.wcs >= 1 && request.wcs <= 9;
    else if (key == "known_systems")
      good = number(line, request.known_systems) && request.known_systems < (1U << 9);
    else if (key == "saved_g28")
      good = flag(line, request.saved_g28);
    else if (key == "saved_g30")
      good = flag(line, request.saved_g30);
    else if (key == "remapped")
      request.remapped.push_back(rest(line));
    else if (key == "tool_change_at_g30")
      good = flag(line, request.tool_change_at_g30);
    else if (key == "tool_change_position") {
      Vec3 position{};
      good = vector(line, position);
      request.tool_change_position = position;
    } else if (key == "max_points")
      good = number(line, request.max_points);
    else if (key == "chord_tolerance")
      good = number(line, request.chord_tolerance) && request.chord_tolerance > 0;
    else if (key == "rigid_tap") {
      RigidTapLimits tapping{};
      good = !request.rigid_tap && number(line, tapping.max_rpm) && tapping.max_rpm >= 1 &&
             number(line, tapping.reversal_turns) && tapping.reversal_turns > 0 && word(line).empty();
      request.rigid_tap = tapping;
    } else if (key == "restart_line") {
      int target = 0;
      good = !request.restart_line && number(line, target) && target > 0 && word(line).empty();
      request.restart_line = target;
    } else if (key == "code")
      request.codes.push_back(rest(line));
    else if (key == "inherited_motion")
      request.inherited_motion = rest(line);
    else if (key == "tool") {
      HelperTool tool;
      int unused = 0;
      good = number(line, tool.index) && number(line, tool.number) && number(line, tool.pocket) &&
             vector(line, tool.offset) && number(line, tool.diameter) &&
             number(line, tool.front_angle) && number(line, tool.back_angle) &&
             number(line, tool.orientation) && number(line, unused);
      request.tools.push_back(tool);
    } else
      good = false;
    if (!good)
      return std::unexpected(std::format("the request line `{}` is not understood", key));
  }
  if (first || request.program.empty() || request.parameters.empty())
    return std::unexpected(std::string("the request names no program or no parameter file"));
  return request;
}

auto write_restart(const RestartPoint& p) -> std::string {
  const int spindle = p.spindle_direction == SpindleDirection::forward    ? 1
                      : p.spindle_direction == SpindleDirection::reverse ? -1
                                                                         : 0;
  return std::format(
      "restart {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {}\n", p.line,
      p.tool, p.wcs, p.h, spindle, p.spindle_rpm, int{p.flood}, int{p.mist}, p.feed,
      int{p.units == LengthUnit::inch}, p.plane, int{p.incremental}, int{p.absolute_arc}, p.motion,
      p.path_control, p.path_tolerance, p.naivecam_tolerance, p.clearance_z, vec(p.start),
      vec(p.g5x), vec(p.g92), vec(p.tool_offset), p.rotation);
}

auto write_check(const HelperOutput::ProgramFacts& facts) -> std::string {
  auto out = std::format("check units {} {} {} {}\n", int{facts.mm}, int{facts.inch},
                         int{facts.variable_spindle}, int{facts.feed_per_revolution});
  for (const int tool : facts.tools)
    std::format_to(std::back_inserter(out), "check tool {}\n", tool);
  for (const auto& frame : facts.frames) {
    if (frame.bounds)
      std::format_to(std::back_inserter(out), "check wcs {} 1 {} {}\n", frame.wcs,
                     vec(frame.bounds->min), vec(frame.bounds->max));
    else
      std::format_to(std::back_inserter(out), "check wcs {} 0\n", frame.wcs);
  }
  if (const auto& machine = facts.machine)
    out += std::format("check machine {} {}\n", vec(machine->min), vec(machine->max));
  if (const auto& feed = facts.feed)
    out += std::format("check feed {} {}\n", feed->min, feed->max);
  if (const auto& spindle = facts.spindle)
    out += std::format("check spindle {} {}\n", spindle->min, spindle->max);
  return out;
}

auto parse_output(std::string_view text, const OutputLimits& limits) -> HelperOutput {
  HelperOutput output;
  std::size_t points = 0;
  std::string_view problem_kind;
  const auto defect = [&output](std::string why) -> HelperOutput {
    output.defect = std::move(why);
    output.end = HelperEnd::missing;
    return output;
  };
  const auto line_number = [&limits](std::string_view& line, int& value) -> bool {
    return number(line, value) && value >= 0 && value <= limits.max_line;
  };
  // Every pass of this loop removes one line from `text`, or returns.
  while (!text.empty()) {
    const auto newline = text.find('\n');
    if (newline == std::string_view::npos)
      return output; // cut off in a record: what came before stands
    auto line = text.substr(0, newline);
    text.remove_prefix(newline + 1);
    const auto key = word(line);
    // `seg line r|f tool points…`, or `segf line tool feed points…` for a feed move whose
    // programmed feed (mm/min) is known.
    if (key == "seg" || key == "segf") {
      PreviewSegment segment;
      if (output.problem)
        return defect("a segment after the record that ends the path");
      if (!line_number(line, segment.line))
        return defect("a segment with a line that is none of the program");
      const auto kind = key == "segf" ? std::string_view{"f"} : word(line);
      if (kind != "r" && kind != "f")
        return defect("a segment of an unknown kind");
      if (!number(line, segment.tool) || segment.tool < 0 || segment.tool > OutputLimits::max_tool)
        return defect("a segment with a tool number out of range");
      if (key == "segf" && (!finite_number(line, segment.feed) || segment.feed < 0))
        return defect("a segment with a feed that is not a finite, positive number");
      segment.kind = kind == "r" ? MoveKind::rapid : MoveKind::feed;
      // Every pass removes one word from `line`, or returns.
      for (;;) {
        auto rest_of_line = line;
        if (word(rest_of_line).empty())
          break;
        Vec3 point{};
        if (!vector(line, point) || !std::isfinite(point[0]) || !std::isfinite(point[1]) ||
            !std::isfinite(point[2]))
          return defect("a segment with a coordinate that is missing or not a finite number");
        if (++points > limits.max_points)
          return defect("more points than the preview allows");
        segment.points.push_back(point);
      }
      if (segment.points.size() < 2)
        return defect("a segment of fewer than two points");
      output.segments.push_back(std::move(segment));
    } else if (key == "dwell") {
      PreviewDwell dwell;
      if (output.problem)
        return defect("a dwell after the record that ends the path");
      if (!line_number(line, dwell.line) || !finite_number(line, dwell.seconds) ||
          dwell.seconds < 0)
        return defect("a dwell with a line or duration that is not valid");
      if (output.dwells.size() >= limits.max_points)
        return defect("more dwells than the preview allows");
      output.dwells.push_back(dwell);
    } else if (key == "error" || key == "stop" || key == "cut") {
      PreviewError problem;
      if (output.problem)
        return defect("a second record that ends the path");
      if (!line_number(line, problem.line))
        return defect("a record with a line that is none of the program");
      problem.text = rest(line);
      output.problem = std::move(problem);
      problem_kind = key;
    } else if (key == "check") {
      if (!read_check(line, output.check))
        return defect("a malformed program check record");
    } else if (key == "restart" || key == "restart_blocked") {
      if (output.problem || output.restart || !output.restart_blocked.empty())
        return defect("a duplicate or misplaced restart record");
      if (key == "restart") {
        RestartPoint point;
        if (!parse_restart(line, limits, point))
          return defect("a malformed restart point");
        output.restart = point;
      } else {
        output.restart_blocked = rest(line);
        if (output.restart_blocked.empty())
          return defect("a restart rejection without a reason");
      }
    } else if (key == "end") {
      const auto how = word(line);
      if (!word(line).empty() || !text.empty())
        return defect("text after the end of the result");
      if (how == "ok" && !output.problem)
        output.end = HelperEnd::ok;
      else if (how == "error" && problem_kind == "error")
        output.end = HelperEnd::error;
      else if (how == "stop" && problem_kind == "stop")
        output.end = HelperEnd::stop;
      else if (how == "cut" && problem_kind == "cut")
        output.end = HelperEnd::cut;
      else
        return defect("an end that does not fit the records before it");
      return output;
    } else {
      return defect("a record of an unknown kind");
    }
  }
  return output;
}

} // namespace coffeecnc
