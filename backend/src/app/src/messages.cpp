#include <coffeecnc/app/messages.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace coffeecnc {
namespace {
using Json = nlohmann::json;

constexpr std::uint64_t max_safe_integer = 9007199254740991; // 2^53 - 1
// Hello.tool_model_storage: the `tool_models` message and its two commands, as of
// docs/design/native-tool-model-storage.md.
constexpr int tool_model_storage_version = 1;

auto name(Axis axis) -> std::string_view {
  switch (axis) {
  case Axis::x:
    return "x";
  case Axis::y:
    return "y";
  case Axis::z:
    return "z";
  }
  return "x";
}

auto name(Link link) -> std::string_view {
  switch (link) {
  case Link::live:
    return "live";
  case Link::stale:
    return "stale";
  case Link::down:
    return "down";
  }
  return "down";
}

auto name(MachineState state) -> std::string_view {
  switch (state) {
  case MachineState::estop:
    return "estop";
  case MachineState::off:
    return "off";
  case MachineState::on:
    return "on";
  }
  return "estop";
}

auto name(TaskMode mode) -> std::string_view {
  switch (mode) {
  case TaskMode::manual:
    return "manual";
  case TaskMode::mdi:
    return "mdi";
  case TaskMode::automatic:
    return "auto";
  }
  return "manual";
}

auto name(InterpState state) -> std::string_view {
  switch (state) {
  case InterpState::idle:
    return "idle";
  case InterpState::reading:
    return "reading";
  case InterpState::paused:
    return "paused";
  case InterpState::waiting:
    return "waiting";
  }
  return "idle";
}

auto name(ProgramCycle cycle) -> std::string_view {
  switch (cycle) {
  case ProgramCycle::idle:
    return "idle";
  case ProgramCycle::running:
    return "running";
  case ProgramCycle::completed:
    return "completed";
  }
  return "idle";
}

auto name(LengthUnit unit) -> std::string_view { return unit == LengthUnit::inch ? "inch" : "mm"; }

auto name(SpindleDirection direction) -> std::string_view {
  switch (direction) {
  case SpindleDirection::stopped:
    return "stopped";
  case SpindleDirection::forward:
    return "forward";
  case SpindleDirection::reverse:
    return "reverse";
  }
  return "stopped";
}

auto name(FeedMode mode) -> std::string_view {
  switch (mode) {
  case FeedMode::per_minute:
    return "per_minute";
  case FeedMode::inverse_time:
    return "inverse_time";
  case FeedMode::per_revolution:
    return "per_revolution";
  }
  return "per_minute";
}

auto name(PreviewState state) -> std::string_view {
  switch (state) {
  case PreviewState::pending:
    return "pending";
  case PreviewState::ready:
    return "ready";
  case PreviewState::failed:
    return "failed";
  case PreviewState::unavailable:
    return "unavailable";
  }
  return "unavailable";
}

// Builds the values of one message and keeps the first field that cannot go on the wire.
class Fields {
public:
  auto real(double value, std::string_view field) -> Json {
    if (!std::isfinite(value)) {
      fail(field, "is not a finite number");
      return 0;
    }
    return value;
  }
  auto count(std::uint64_t value, std::string_view field) -> Json {
    if (value > max_safe_integer) {
      fail(field, "is above 2^53 - 1");
      return 0;
    }
    return value;
  }
  auto vec(const Vec3& value, std::string_view field) -> Json {
    return Json::array({real(value[0], field), real(value[1], field), real(value[2], field)});
  }
  void fail(std::string_view field, std::string_view problem) {
    if (error_.empty())
      error_ = std::string(field) + ' ' + std::string(problem) + '.';
  }
  auto finish(const Json& message) -> Encoded {
    if (!error_.empty())
      return std::unexpected(std::move(error_));
    // File names and controller text are not guaranteed to be UTF-8; dump() must not throw.
    return message.dump(-1, ' ', false, Json::error_handler_t::replace);
  }

private:
  std::string error_;
};

template <typename T> auto nullable(const std::optional<T>& value) -> Json {
  return value ? Json(*value) : Json(nullptr);
}

auto tool(Fields& fields, const Tool& t) -> Json {
  return {{"number", t.number},
          {"pocket", t.pocket},
          {"length", fields.real(t.length, "tools.length")},
          {"diameter", fields.real(t.diameter, "tools.diameter")},
          {"comment", t.comment}};
}

auto wait(Fields& fields, const Wait& w) -> Json {
  return std::visit(
      [&fields](const auto& kind) -> Json {
        using T = std::decay_t<decltype(kind)>;
        if constexpr (std::is_same_v<T, WaitNone>) {
          return {{"kind", "none"}};
        } else if constexpr (std::is_same_v<T, WaitPaused>) {
          return {{"kind", "paused"}};
        } else if constexpr (std::is_same_v<T, WaitToolChange>) {
          Json result{{"kind", "tool_change"}, {"tool", kind.tool}};
          if (kind.measure_request)
            result["measure_request"] = *kind.measure_request;
          return result;
        } else {
          static_assert(std::is_same_v<T, WaitDwell>, "Every wait needs a wire form.");
          return {{"kind", "dwell"},
                  {"seconds_left", fields.real(kind.seconds_left, "wait.seconds_left")}};
        }
      },
      w);
}

} // namespace

auto encode_hello(const Hello& hello) -> Encoded {
  Fields fields;
  auto axes = Json::array();
  auto axis_velocity = Json::object();
  for (const auto axis : hello.axes) {
    axes.push_back(name(axis));
    const auto index = axis_index(axis);
    const auto velocity = index ? hello.axis_max_jog_velocity.at(*index) : std::nullopt;
    if (!velocity) {
      fields.fail("axis_max_jog_velocity", "lacks a configured axis");
      continue;
    }
    // The panel disables jogging an axis whose ceiling is not positive.
    if (*velocity <= 0)
      fields.fail("axis_max_jog_velocity", "is not positive");
    axis_velocity[std::string(name(axis))] = fields.real(*velocity, "axis_max_jog_velocity");
  }
  auto increments = Json::array();
  std::ranges::transform(
      hello.jog_increments, std::back_inserter(increments),
      [&](double increment) -> Json { return fields.real(increment, "jog_increments"); });
  Json message{
      {"type", "hello"},
      {"protocol", protocol_version},
      {"machine_name", hello.machine_name},
      {"simulated", hello.simulated},
      {"axes", std::move(axes)},
      {"machine_units", name(hello.machine_units)},
      {"jog_increments", std::move(increments)},
      {"max_jog_velocity", fields.real(hello.max_jog_velocity, "max_jog_velocity")},
      {"axis_max_jog_velocity", std::move(axis_velocity)},
      {"homing_required", hello.homing_required},
      {"max_feed_override", fields.real(hello.max_feed_override, "max_feed_override")},
      {"min_spindle_override", fields.real(hello.min_spindle_override, "min_spindle_override")},
      {"max_spindle_override", fields.real(hello.max_spindle_override, "max_spindle_override")},
      {"machine_id", nullable(hello.machine_id)},
      {"tool_model_storage", tool_model_storage_version},
      {"operator_settings_storage", 1},
      {"quit_supported", true}};
  if (hello.max_spindle_rpm) {
    if (*hello.max_spindle_rpm <= 0)
      fields.fail("max_spindle_rpm", "is not positive");
    message["max_spindle_rpm"] = fields.real(*hello.max_spindle_rpm, "max_spindle_rpm");
  }
  if (hello.rigid_tap)
    message["rigid_tap"] = {
        {"max_rpm", fields.real(hello.rigid_tap->max_rpm, "rigid_tap.max_rpm")},
        {"reversal_turns", fields.real(hello.rigid_tap->reversal_turns, "rigid_tap.reversal_turns")}};
  if (hello.touch_probe_tool)
    message["touch_probe_tool"] = *hello.touch_probe_tool;
  if (!hello.program_places.empty()) {
    auto places = Json::array();
    std::ranges::transform(hello.program_places, std::back_inserter(places), [](const auto& place) -> Json {
      return {{"name", place.name}, {"path", place.path}};
    });
    message["program_places"] = std::move(places);
  }
  if (!hello.macros.empty()) {
    auto macros = Json::array();
    std::ranges::transform(hello.macros, std::back_inserter(macros), [](const auto& macro) -> Json {
      return {{"label", macro.label}, {"mdi", macro.mdi}};
    });
    message["macros"] = std::move(macros);
  }
  if (hello.diagnostics)
    message["diagnostics"] = true;
  if (hello.coolant)
    message["coolant"] = {{"flood", hello.coolant->flood}, {"mist", hello.coolant->mist}};
  if (const auto& setter = hello.fixed_setter)
    message["fixed_setter"] = {
        {"reference_tool", setter->reference_tool},
        {"travel_mm", fields.real(setter->travel_mm, "fixed_setter.travel_mm")},
        {"feed_mm_min", fields.real(setter->feed_mm_min, "fixed_setter.feed_mm_min")},
        {"xy_tolerance_mm", fields.real(setter->xy_tolerance_mm, "fixed_setter.xy_tolerance_mm")},
        {"clearance_mm", fields.real(setter->clearance_mm, "fixed_setter.clearance_mm")},
        {"check_tolerance_mm", fields.real(setter->check_tolerance_mm, "fixed_setter.check_tolerance_mm")}};
  if (hello.fixed_setter) {
    const auto& setter = *hello.fixed_setter;
    const auto latch_feed = fields.real(setter.latch_feed_mm_min, "fixed_setter.latch_feed_mm_min");
    const auto latch_backoff = fields.real(setter.latch_backoff_mm, "fixed_setter.latch_backoff_mm");
    const auto travel = fields.real(setter.material_travel_mm, "fixed_setter.material_travel_mm");
    const auto& geometry = setter.material_geometry;
    const Json material_geometry{
        {"reach_mm", fields.real(geometry.reach_mm, "fixed_setter.material_geometry.reach_mm")},
        {"margin_mm", fields.real(geometry.margin_mm, "fixed_setter.material_geometry.margin_mm")},
        {"clearance_mm", fields.real(geometry.clearance_mm, "fixed_setter.material_geometry.clearance_mm")},
        {"depth_mm", fields.real(geometry.depth_mm, "fixed_setter.material_geometry.depth_mm")}};
    if (latch_feed > 0) {
      message["fixed_setter"]["latch_feed_mm_min"] = latch_feed;
      message["fixed_setter"]["latch_backoff_mm"] = latch_backoff;
    }
    if (hello.touch_probe_tool && latch_feed > 0 && setter.feed_mm_min > 0 &&
        travel <= 3 * setter.feed_mm_min) {
      message["fixed_setter"]["material_travel_mm"] = travel;
      message["fixed_setter"]["material_features"] = true;
      message["fixed_setter"]["material_geometry"] = material_geometry;
    }
  }
  return fields.finish(message);
}

auto encode_status(const Status& status, std::uint64_t seq) -> Encoded {
  Fields fields;
  auto axes = Json::array();
  std::ranges::transform(status.axes, std::back_inserter(axes),
                         [&](const AxisStatus& axis) -> Json {
                           Json sent = {{"axis", name(axis.axis)},
                                        {"homed", axis.homed},
                                        {"homing", axis.homing},
                                        {"machine", fields.real(axis.machine, "axes.machine")},
                                        {"work", fields.real(axis.work, "axes.work")},
                                        {"dtg", fields.real(axis.dtg, "axes.dtg")},
                                        {"min_limit", fields.real(axis.min_limit, "axes.min_limit")},
                                        {"max_limit", fields.real(axis.max_limit, "axes.max_limit")},
                                        {"on_hard_limit", axis.on_hard_limit}};
                           if (axis.reference_mark)
                             sent["reference_mark"] = *axis.reference_mark;
                           if (axis.at_reference_mark)
                             sent["at_reference_mark"] = *axis.at_reference_mark;
                           return sent;
                         });
  const auto& spindle = status.spindle;
  Json actual_rpm = nullptr;
  if (spindle.actual_rpm) {
    actual_rpm = fields.real(*spindle.actual_rpm, "spindle.actual_rpm");
    if (*spindle.actual_rpm < 0)
      fields.fail("spindle.actual_rpm", "is negative");
  }
  const auto& motion = status.motion;
  const auto& program = status.program;
  Json program_fields{{"file", nullable(program.file)},
                      {"revision", fields.count(program.revision, "program.revision")},
                      {"current_line", program.current_line},
                      {"motion_line", program.motion_line},
                      {"optional_stop", program.optional_stop},
                      {"block_delete", program.block_delete}};
  if (const auto hint = program.restart_hint)
    program_fields["restart_hint"] = {{"line", hint->line}, {"tool", hint->tool}};
  if (const auto breakpoint = program.breakpoint)
    program_fields["breakpoint"] = {{"line", breakpoint->line}, {"reached", breakpoint->reached}};
  if (const auto cycle = program.cycle)
    program_fields["cycle"] = name(*cycle);
  Json message = {
      {"type", "status"},
      {"seq", fields.count(seq, "seq")},
      {"link", name(status.link)},
      {"machine", name(status.machine)},
      {"mode", name(status.mode)},
      {"interp", name(status.interp)},
      {"axes", std::move(axes)},
      {"offsets",
       {{"wcs", status.offsets.wcs},
        {"g5x", fields.vec(status.offsets.g5x, "offsets.g5x")},
        {"g92", fields.vec(status.offsets.g92, "offsets.g92")},
        {"tool", fields.vec(status.offsets.tool, "offsets.tool")},
        {"rotation_xy", fields.real(status.offsets.rotation_xy, "offsets.rotation_xy")}}},
      {"motion",
       {{"velocity", fields.real(motion.velocity, "motion.velocity")},
        {"feed_mode", name(motion.feed_mode)},
        {"programmed_feed", fields.real(motion.programmed_feed, "motion.programmed_feed")},
        {"feed_override", fields.real(motion.feed_override, "motion.feed_override")},
        {"rapid_override", fields.real(motion.rapid_override, "motion.rapid_override")},
        {"in_position", motion.in_position}}},
      {"spindle",
       {{"direction", name(spindle.direction)},
        {"commanded_rpm", fields.real(spindle.commanded_rpm, "spindle.commanded_rpm")},
        {"override", fields.real(spindle.override, "spindle.override")},
        {"actual_rpm", std::move(actual_rpm)},
        {"at_speed", nullable(spindle.at_speed)}}},
      {"coolant", {{"flood", status.coolant.flood}, {"mist", status.coolant.mist}}},
      {"tool",
       {{"in_spindle", status.tool.in_spindle}, {"prepared", nullable(status.tool.prepared)}}},
      {"program", std::move(program_fields)},
      {"preview_context", fields.count(status.preview_context, "preview_context")},
      {"wait", wait(fields, status.wait)},
      {"active_codes", status.active_codes},
      {"program_units", name(status.program_units)},
      {"probe_input", nullable(status.probe_input)}};
  if (!status.rotary_axes.empty()) {
    auto rotary = Json::array();
    for (const auto& axis : status.rotary_axes) {
      const auto angle = [&](const std::optional<double>& value, std::string_view field) -> Json {
        return value ? fields.real(*value, field) : Json(nullptr);
      };
      rotary.push_back({{"axis", std::string(1, axis.axis)},
                        {"homed", axis.homed},
                        {"homing", axis.homing},
                        {"machine", angle(axis.machine, "rotary_axes.machine")},
                        {"work", angle(axis.work, "rotary_axes.work")},
                        {"dtg", angle(axis.dtg, "rotary_axes.dtg")},
                        {"on_hard_limit", axis.on_hard_limit}});
    }
    message["rotary_axes"] = std::move(rotary);
  }
  if (motion.velocity_limit)
    message["motion"]["velocity_limit"] =
        fields.real(*motion.velocity_limit, "motion.velocity_limit");
  if (const auto& z = status.paused_z) {
    message["paused_z"] = {{"offset", fields.real(z->offset, "paused_z.offset")},
                           {"max_lift", fields.real(z->max_lift, "paused_z.max_lift")},
                           {"displaced", z->displaced},
                           {"reason", z->reason}};
    switch (z->spindle) {
    case PausedSpindle::can_stop:
      message["paused_z"]["spindle"] = "can_stop";
      break;
    case PausedSpindle::stopping:
      message["paused_z"]["spindle"] = "stopping";
      break;
    case PausedSpindle::restore:
      message["paused_z"]["spindle"] = "restore";
      break;
    case PausedSpindle::recover:
      message["paused_z"]["spindle"] = "recover";
      break;
    case PausedSpindle::none:
      break;
    }
  }
  return fields.finish(message);
}

auto encode_tool_table(const ToolTable& table) -> Encoded {
  Fields fields;
  auto tools = Json::array();
  std::ranges::transform(table.tools, std::back_inserter(tools),
                         [&](const Tool& t) -> Json { return tool(fields, t); });
  return fields.finish({{"type", "tool_table"},
                        {"revision", fields.count(table.revision, "revision")},
                        {"tools", std::move(tools)}});
}

auto encode_work_offsets(const WorkOffsets& offsets) -> Encoded {
  Fields fields;
  auto systems = Json::array();
  for (std::size_t i = 0; i < offsets.systems.size(); ++i)
    systems.push_back(
        {{"wcs", i + 1},
         {"offset", fields.vec(offsets.systems[i].offset, "systems.offset")},
         {"rotation_xy", fields.real(offsets.systems[i].rotation_xy, "systems.rotation_xy")},
         {"available", offsets.systems[i].available},
         {"current", offsets.systems[i].available && offsets.systems[i].current}});
  return fields.finish({{"type", "work_offsets"},
                        {"revision", fields.count(offsets.revision, "revision")},
                        {"systems", std::move(systems)}});
}

auto encode_program(const ProgramText& program) -> Encoded {
  Fields fields;
  return fields.finish({{"type", "program"},
                        {"revision", fields.count(program.revision, "revision")},
                        {"file", program.file},
                        {"lines", program.lines},
                        {"truncated", program.truncated},
                        {"stamp", program.stamp}});
}

auto encode_preview(const Preview& preview) -> Encoded {
  Fields fields;
  auto segments = Json::array();
  for (const auto& segment : preview.segments) {
    auto points = Json::array();
    for (const auto& point : segment.points)
      std::ranges::transform(point, std::back_inserter(points), [&](double coordinate) -> Json {
        return fields.real(coordinate, "segments.points");
      });
    segments.push_back({{"line", segment.line},
                        {"kind", segment.kind == MoveKind::feed ? "feed" : "rapid"},
                        {"tool", segment.tool},
                        {"points", std::move(points)},
                        {"feed", fields.real(segment.feed, "segments.feed")}});
  }
  auto dwells = Json::array();
  std::ranges::transform(preview.dwells, std::back_inserter(dwells), [&](const auto& dwell) -> Json {
    return {{"line", dwell.line}, {"seconds", fields.real(dwell.seconds, "dwells.seconds")}};
  });
  Json error = nullptr;
  if (preview.error)
    error = {{"line", preview.error->line}, {"text", preview.error->text}};
  Json bounds = nullptr;
  if (preview.bounds)
    bounds = {{"min", fields.vec(preview.bounds->min, "bounds.min")},
              {"max", fields.vec(preview.bounds->max, "bounds.max")}};
  Json message{{"type", "preview"},
               {"context", fields.count(preview.context, "context")},
               {"state", name(preview.state)},
               {"error", std::move(error)},
               {"segments", std::move(segments)},
               {"bounds", std::move(bounds)},
               {"dwells", std::move(dwells)}};
  if (const auto& check = preview.check) {
    auto tools = Json::array();
    std::ranges::transform(check->tools, std::back_inserter(tools), [](const auto& row) -> Json {
      return {{"tool", row.tool},
              {"length", row.length == ToolLengthCheck::touched_off ? "touched_off"
                         : row.length == ToolLengthCheck::table    ? "table"
                                                                   : "missing"}};
    });
    auto frames = Json::array();
    for (const auto& frame : check->frames) {
      Json extent = nullptr;
      if (frame.bounds)
        extent = {{"min", fields.vec(frame.bounds->min, "check.frames.min")},
                  {"max", fields.vec(frame.bounds->max, "check.frames.max")}};
      frames.push_back(
          {{"wcs", frame.wcs}, {"available", frame.available}, {"bounds", std::move(extent)}});
    }
    const auto range = [&](const std::optional<ProgramRange>& value) -> Json {
      return value ? Json{{"min", fields.real(value->min, "check.range.min")},
                          {"max", fields.real(value->max, "check.range.max")}}
                   : Json(nullptr);
    };
    auto units = Json::array();
    std::ranges::transform(check->units, std::back_inserter(units),
                           [](LengthUnit unit) { return name(unit); });
    message["check"] = {{"tools", std::move(tools)},
                        {"frames", std::move(frames)},
                        {"feed", range(check->feed)},
                        {"spindle", range(check->spindle)},
                        {"units", std::move(units)},
                        {"errors", check->errors},
                        {"warnings", check->warnings},
                        {"travel", check->travel == TravelCheck::fits      ? "fits"
                                   : check->travel == TravelCheck::outside ? "outside"
                                                                           : "unknown"}};
  }
  return fields.finish(message);
}

auto encode_directory(const Directory& directory) -> Encoded {
  Fields fields;
  auto entries = Json::array();
  for (const auto& entry : directory.entries) {
    if (entry.modified > static_cast<std::int64_t>(max_safe_integer) ||
        entry.modified < -static_cast<std::int64_t>(max_safe_integer))
      fields.fail("entries.modified", "is outside ±(2^53 - 1)");
    entries.push_back({{"name", entry.name},
                       {"kind", entry.kind == EntryKind::directory ? "directory" : "file"},
                       {"bytes", fields.count(entry.bytes, "entries.bytes")},
                       {"modified", entry.modified}});
  }
  Json message{{"type", "directory"},
               {"path", directory.path},
               {"parent", nullable(directory.parent)},
               {"entries", std::move(entries)},
               {"truncated", directory.truncated}};
  if (directory.writable.has_value())
    message["writable"] = directory.writable.value_or(false);
  return fields.finish(message);
}

auto history_name(HistorySource source) -> std::string_view {
  switch (source) {
  case HistorySource::touch_off:
    return "touch_off";
  case HistorySource::zero:
    return "zero";
  case HistorySource::half:
    return "half";
  case HistorySource::probe:
    return "probe";
  case HistorySource::tool_setter:
    return "tool_setter";
  case HistorySource::tool_touch_off:
    return "tool_touch_off";
  case HistorySource::tool_table:
    return "tool_table";
  case HistorySource::work_offset_table:
    return "work_offset_table";
  case HistorySource::mdi:
    return "mdi";
  case HistorySource::program:
    return "program";
  case HistorySource::restore:
    return "restore";
  case HistorySource::outside:
    return "outside";
  }
  return "outside";
}

auto history_name(HistoryTarget target) -> std::string_view {
  switch (target) {
  case HistoryTarget::wcs:
    return "wcs";
  case HistoryTarget::g92:
    return "g92";
  case HistoryTarget::tool:
    return "tool";
  }
  return "wcs";
}

auto history_name(HistoryField field) -> std::string_view {
  switch (field) {
  case HistoryField::x:
    return "x";
  case HistoryField::y:
    return "y";
  case HistoryField::z:
    return "z";
  case HistoryField::rotation_xy:
    return "rotation_xy";
  case HistoryField::length:
    return "length";
  case HistoryField::diameter:
    return "diameter";
  }
  return "x";
}

auto encode_offset_history(const OffsetHistory& history) -> Encoded {
  Fields fields;
  const auto value = [&](const std::optional<double>& number, std::string_view field) -> Json {
    return number ? fields.real(*number, field) : Json(nullptr);
  };
  auto entries = Json::array();
  for (const auto& entry : history.entries) {
    if (entry.time > static_cast<std::int64_t>(max_safe_integer) || entry.time < 0)
      fields.fail("entries.time", "is outside 0 to 2^53 - 1");
    auto changes = Json::array();
    std::ranges::transform(entry.changes, std::back_inserter(changes), [&](const auto& change) -> Json {
      return {{"field", history_name(change.field)},
              {"before", value(change.before, "entries.changes.before")},
              {"after", value(change.after, "entries.changes.after")}};
    });
    entries.push_back(
        {{"id", fields.count(entry.id, "entries.id")},
         {"time", entry.time},
         {"source", history_name(entry.source)},
         {"target", history_name(entry.target)},
         {"number", entry.number},
         {"changes", std::move(changes)},
         {"restores", entry.restores ? fields.count(*entry.restores, "entries.restores")
                                     : Json(nullptr)}});
  }
  return fields.finish({{"type", "offset_history"},
                        {"revision", fields.count(history.revision, "revision")},
                        {"storage", history.saved ? "saved" : "session_only"},
                        {"reason", history.reason},
                        {"entries", std::move(entries)}});
}

auto diagnostic_name(DiagnosticRole role) -> std::string_view {
  switch (role) {
  case DiagnosticRole::min_limit:
    return "min_limit";
  case DiagnosticRole::max_limit:
    return "max_limit";
  case DiagnosticRole::home_switch:
    return "home_switch";
  case DiagnosticRole::amp_fault:
    return "amp_fault";
  case DiagnosticRole::estop_chain:
    return "estop_chain";
  case DiagnosticRole::estop_request:
    return "estop_request";
  case DiagnosticRole::probe:
    return "probe";
  case DiagnosticRole::spindle_on:
    return "spindle_on";
  case DiagnosticRole::spindle_at_speed:
    return "spindle_at_speed";
  case DiagnosticRole::spindle_speed_out:
    return "spindle_speed_out";
  case DiagnosticRole::spindle_speed_in:
    return "spindle_speed_in";
  case DiagnosticRole::spindle_fault:
    return "spindle_fault";
  case DiagnosticRole::configured:
    return "configured";
  }
  return "configured";
}

auto diagnostic_name(DiagnosticState state) -> std::string_view {
  switch (state) {
  case DiagnosticState::missing:
    return "missing";
  case DiagnosticState::unwired:
    return "unwired";
  case DiagnosticState::undriven:
    return "undriven";
  case DiagnosticState::live:
    return "live";
  }
  return "missing";
}

auto encode_diagnostics(const Diagnostics& diagnostics) -> Encoded {
  Fields fields;
  auto inputs = Json::array();
  for (const auto& reading : diagnostics.inputs) {
    const auto& input = reading.input;
    // A float pin can hold anything: what is not a number is sent as no value, never refused.
    const auto value = std::visit(
        [](const auto& given) -> Json {
          using T = std::decay_t<decltype(given)>;
          if constexpr (std::is_same_v<T, bool>)
            return given;
          else if constexpr (std::is_same_v<T, double>)
            return std::isfinite(given) ? Json(given) : Json(nullptr);
          else
            return nullptr;
        },
        reading.value);
    Json row{{"role", diagnostic_name(input.role)},
             {"name", input.name},
             {"kind", reading.signal ? "signal" : "pin"},
             {"state", diagnostic_name(reading.state)},
             {"value", value}};
    if (input.joint)
      row["joint"] = *input.joint;
    if (input.axis)
      row["axis"] = std::string(1, *input.axis);
    if (input.role == DiagnosticRole::configured)
      row["label"] = input.label;
    inputs.push_back(std::move(row));
  }
  auto tools = Json::array();
  std::ranges::transform(diagnostics.tools, std::back_inserter(tools),
                         [](const HalToolState& tool) -> Json {
                           return {{"tool", hal_tool_name(tool.tool)},
                                   {"available", tool.available},
                                   {"open", tool.open}};
                         });
  return fields.finish(
      {{"type", "diagnostics"}, {"inputs", std::move(inputs)}, {"tools", std::move(tools)}});
}

} // namespace coffeecnc
