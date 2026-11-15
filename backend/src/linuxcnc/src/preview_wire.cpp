#include "preview_wire.hpp"

#include <charconv>
#include <format>
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
  const auto vec = [](const Vec3& v) -> std::string {
    return std::format("{} {} {}", v[0], v[1], v[2]);
  };
  line("program", request.program);
  line("parameters", request.parameters);
  line("units_per_mm", std::format("{}", request.units_per_mm));
  line("position", vec(request.position));
  line("tool_in_spindle", std::format("{}", request.tool_in_spindle));
  line("tool_prepared", std::format("{}", request.tool_prepared));
  line("tool_offset", vec(request.tool_offset));
  line("feed", std::format("{}", request.feed));
  line("block_delete", request.block_delete ? "1" : "0");
  line("wcs", std::format("{}", request.wcs));
  line("tool_change_at_g30", request.tool_change_at_g30 ? "1" : "0");
  if (request.tool_change_position)
    line("tool_change_position", vec(*request.tool_change_position));
  line("max_points", std::format("{}", request.max_points));
  line("chord_tolerance", std::format("{}", request.chord_tolerance));
  for (const auto& code : request.codes)
    line("code", code);
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
    else if (key == "code")
      request.codes.push_back(rest(line));
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

auto parse_output(std::string_view text) -> HelperOutput {
  HelperOutput output;
  while (!text.empty() && output.end == HelperEnd::missing) {
    const auto end = text.find('\n');
    if (end == std::string_view::npos)
      break;
    auto line = text.substr(0, end);
    text.remove_prefix(end + 1);
    const auto key = word(line);
    if (key == "seg") {
      PreviewSegment segment;
      if (!number(line, segment.line) || segment.line < 0)
        break;
      const auto kind = word(line);
      if ((kind != "r" && kind != "f") || !number(line, segment.tool))
        break;
      segment.kind = kind == "r" ? MoveKind::rapid : MoveKind::feed;
      std::vector<double> values;
      bool numbers = true;
      for (auto next = line; !word(next).empty();) {
        double value = 0;
        numbers = numbers && number(line, value);
        values.push_back(value);
        next = line;
      }
      if (!numbers || values.size() < 6 || values.size() % 3 != 0)
        break;
      for (std::size_t i = 0; i < values.size(); i += 3)
        segment.points.push_back({values[i], values[i + 1], values[i + 2]});
      output.last_line = segment.line;
      output.segments.push_back(std::move(segment));
    } else if (key == "error" || key == "stop" || key == "cut") {
      PreviewError problem;
      if (!number(line, problem.line) || output.problem)
        break;
      problem.text = rest(line);
      output.last_line = problem.line;
      output.problem = std::move(problem);
    } else if (key == "end") {
      const auto how = word(line);
      output.end = how == "ok"      ? HelperEnd::ok
                   : how == "error" ? HelperEnd::error
                   : how == "stop"  ? HelperEnd::stop
                   : how == "cut"   ? HelperEnd::cut
                                    : HelperEnd::missing;
      break;
    } else {
      break;
    }
  }
  return output;
}

} // namespace coffeecnc
