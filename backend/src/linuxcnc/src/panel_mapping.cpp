#include <coffeecnc/linuxcnc/panel_mapping.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace coffeecnc {
auto to_hello(const MachineConfig& config) -> Hello {
  Hello hello;
  hello.machine_name = config.machine_name;
  hello.simulated = false;
  hello.axes.assign(config.axes.begin(), config.axes.end());
  hello.machine_units = config.machine_units;
  hello.jog_increments = config.jog_increments;
  hello.max_jog_velocity = config.limits.max_jog_velocity;
  for (const auto axis : config.axes)
    if (const auto index = axis_index(axis))
      hello.axis_max_jog_velocity.at(*index) = config.limits.max_axis_jog_velocity.at(*index);
  hello.homing_required = config.limits.homing_required;
  hello.max_feed_override = config.limits.max_feed_override;
  hello.min_spindle_override = config.limits.min_spindle_override;
  hello.max_spindle_override = config.limits.max_spindle_override;
  if (config.machine_id && valid_machine_id(*config.machine_id))
    hello.machine_id = config.machine_id;
  hello.coolant = config.coolant;
  hello.touch_probe_tool = config.touch_probe_tool;
  hello.max_spindle_rpm = config.limits.max_spindle_rpm;
  hello.fixed_setter = config.fixed_setter;
  hello.program_places = config.program_places;
  hello.macros = config.macros;
  return hello;
}

auto pin_signal(std::string_view shown, std::string_view pin) -> std::optional<std::string> {
  // `halcmd -s show pin <pin>`: one line per matching pin, `comp type dir value name [==> sig]`.
  std::istringstream lines{std::string(shown)};
  for (std::string line; std::getline(lines, line);) {
    std::istringstream words{line};
    std::vector<std::string> word{std::istream_iterator<std::string>{words}, {}};
    if (word.size() >= 5 && word[4] == pin) {
      if (word[1] != "bit" || word[2] != "OUT")
        return std::nullopt;
      if (word.size() == 5)
        return std::string{};
      if (word.size() == 7 && word[5] == "==>")
        return word[6];
      return std::nullopt;
    }
  }
  return std::nullopt;
}

auto signal_has_reader(std::string_view shown, std::string_view signal) -> std::optional<bool> {
  // `halcmd -s show sig <sig>`: one line per matching signal, `type value name` followed by
  // `<== writer` and `==> reader` pairs.
  std::istringstream lines{std::string(shown)};
  for (std::string line; std::getline(lines, line);) {
    std::istringstream words{line};
    std::vector<std::string> word{std::istream_iterator<std::string>{words}, {}};
    if (word.size() >= 3 && word[2] == signal) {
      if (word[0] != "bit" || word.size() % 2 != 1)
        return std::nullopt;
      bool reader = false;
      for (std::size_t i = 3; i < word.size(); i += 2) {
        if (word[i] == "==>" || word[i] == "<=>")
          reader = true;
        else if (word[i] != "<==")
          return std::nullopt;
      }
      return reader;
    }
  }
  return std::nullopt;
}

auto to_panel_status(const NmlStatus& reading, const MachineConfig& config,
                     std::span<const LoadedTool> tools, std::uint64_t program_revision) -> Status {
  const auto& controller = reading.controller;
  const bool known = controller.link != Link::down;
  Status status;
  status.link = controller.link;
  status.machine = controller.machine;
  status.mode = controller.mode;
  status.interp = controller.interp;
  for (std::size_t i = 0; i < config.axes.size(); ++i) {
    const auto& axis = controller.axes.at(i);
    status.axes.push_back({.axis = config.axes.at(i),
                           .homed = axis.homed,
                           .homing = axis.homing,
                           .machine = axis.machine,
                           .work = reading.work.at(i),
                           .dtg = reading.dtg.at(i),
                           .min_limit = known ? axis.min_limit : config.travel.at(i).min,
                           .max_limit = known ? axis.max_limit : config.travel.at(i).max,
                           .on_hard_limit = axis.on_hard_limit,
                           .reference_mark = axis.reference_mark,
                           .at_reference_mark = axis.at_reference_mark});
  }
  constexpr std::string_view rotary_letters = "abc";
  for (std::size_t i = 0; i < config.rotary_joints.size(); ++i) {
    if (config.rotary_joints[i].empty())
      continue;
    const auto& axis = controller.rotary_axes[i];
    status.rotary_axes.push_back({
        .axis = rotary_letters[i],
        .homed = axis.homed,
        .homing = axis.homing,
        .machine = known && std::isfinite(axis.machine) ? std::optional{axis.machine} : std::nullopt,
        .work = known ? reading.rotary_work[i] : std::nullopt,
        .dtg = known ? reading.rotary_dtg[i] : std::nullopt,
        .on_hard_limit = axis.on_hard_limit,
    });
  }
  status.offsets = {.wcs = known ? reading.wcs : 1,
                    .g5x = reading.g5x,
                    .g92 = reading.g92,
                    .tool = reading.tool_offset,
                    .rotation_xy = reading.rotation_xy};
  status.motion = {.velocity = reading.velocity,
                   .feed_mode = reading.feed_mode,
                   .programmed_feed = reading.programmed_feed,
                   .feed_override = reading.feed_override,
                   .rapid_override = reading.rapid_override,
                   .in_position = controller.in_position,
                   .velocity_limit = known ? reading.velocity_limit : std::nullopt};
  status.spindle = {.direction = reading.spindle_direction,
                    .commanded_rpm = reading.spindle_rpm,
                    .override = reading.spindle_override,
                    .actual_rpm = reading.spindle_actual_rpm,
                    .at_speed = reading.spindle_at_speed};
  status.coolant = {.flood = reading.flood, .mist = reading.mist};
  status.tool.in_spindle = controller.tool_in_spindle;
  if (const auto index = reading.prepared_pocket; index && *index >= 1) {
    const auto found = std::ranges::find(tools, *index, &LoadedTool::index);
    if (found != tools.end())
      status.tool.prepared = found->tool.number;
  }
  if (!reading.program_file.empty())
    status.program.file = reading.program_file;
  status.program.revision = program_revision;
  status.program.current_line = reading.current_line;
  status.program.motion_line = reading.motion_line;
  status.program.optional_stop = reading.optional_stop;
  status.program.block_delete = reading.block_delete;
  if (controller.requested_tool)
    status.wait = WaitToolChange{*controller.requested_tool};
  else if (reading.paused)
    status.wait = WaitPaused{};
  else if (reading.dwell_seconds_left > 0)
    status.wait = WaitDwell{reading.dwell_seconds_left};
  status.active_codes = reading.active_codes;
  status.program_units = controller.program_units;
  status.probe_input = reading.probe_input;
  status.paused_z = reading.controller.paused_z;
  return status;
}

auto parameter_work_systems(std::string_view text, double millimetres_per_unit)
    -> std::array<std::optional<WorkSystem>, 9> {
  std::array<std::optional<WorkSystem>, 9> systems{};
  if (text.size() > max_parameter_file_bytes)
    return systems;
  constexpr int first = 5221;
  constexpr int last = first + (20 * 8) + 9;
  const auto blank = [](char c) -> bool { return c == ' ' || c == '\t' || c == '\r'; };
  std::map<int, double> values;
  while (!text.empty()) {
    const auto end = text.find('\n');
    auto line = text.substr(0, end);
    text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    const auto skip = [&]() -> void {
      while (!line.empty() && blank(line.front()))
        line.remove_prefix(1);
    };
    skip();
    int number = 0;
    const auto [after_number, number_error] =
        std::from_chars(line.data(), line.data() + line.size(), number);
    if (number_error != std::errc{} || number < first || number > last)
      continue;
    line.remove_prefix(static_cast<std::size_t>(after_number - line.data()));
    if (line.empty() || !blank(line.front()))
      continue;
    skip();
    double value = 0;
    const auto [after_value, value_error] =
        std::from_chars(line.data(), line.data() + line.size(), value);
    if (value_error != std::errc{} || !std::isfinite(value))
      continue;
    values[number] = value;
  }
  for (std::size_t i = 0; i < systems.size(); ++i) {
    const int base = first + (20 * static_cast<int>(i));
    const std::array numbers{base, base + 1, base + 2, base + 9};
    if (!std::ranges::all_of(numbers, [&](int n) -> bool { return values.contains(n); }))
      continue;
    systems.at(i) = WorkSystem{.offset = {values.at(base) * millimetres_per_unit,
                                          values.at(base + 1) * millimetres_per_unit,
                                          values.at(base + 2) * millimetres_per_unit},
                               .rotation_xy = values.at(base + 9),
                               .available = true,
                               .current = false};
  }
  return systems;
}

auto to_work_systems(const NmlStatus& reading,
                     const std::array<std::optional<WorkSystem>, 9>& saved, bool saved_current)
    -> std::array<WorkSystem, 9> {
  std::array<WorkSystem, 9> systems{};
  if (reading.controller.link == Link::down)
    return systems;
  const bool live = reading.controller.link == Link::live;
  for (std::size_t i = 0; i < systems.size(); ++i)
    if (const auto& known = saved.at(i))
      systems.at(i) = {.offset = known->offset,
                       .rotation_xy = known->rotation_xy,
                       .available = true,
                       .current = live && saved_current};
  if (reading.wcs >= 1 && static_cast<std::size_t>(reading.wcs) <= systems.size())
    systems.at(static_cast<std::size_t>(reading.wcs) - 1) = {.offset = reading.g5x,
                                                             .rotation_xy = reading.rotation_xy,
                                                             .available = true,
                                                             .current = live};
  return systems;
}

} // namespace coffeecnc
