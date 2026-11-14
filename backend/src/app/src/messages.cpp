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
          return {{"kind", "tool_change"}, {"tool", kind.tool}};
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
  return fields.finish(
      {{"type", "hello"},
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
       {"max_spindle_override", fields.real(hello.max_spindle_override, "max_spindle_override")}});
}

auto encode_status(const Status& status, std::uint64_t seq) -> Encoded {
  Fields fields;
  auto axes = Json::array();
  std::ranges::transform(status.axes, std::back_inserter(axes),
                         [&](const AxisStatus& axis) -> Json {
                           return {{"axis", name(axis.axis)},
                                   {"homed", axis.homed},
                                   {"homing", axis.homing},
                                   {"machine", fields.real(axis.machine, "axes.machine")},
                                   {"work", fields.real(axis.work, "axes.work")},
                                   {"dtg", fields.real(axis.dtg, "axes.dtg")},
                                   {"min_limit", fields.real(axis.min_limit, "axes.min_limit")},
                                   {"max_limit", fields.real(axis.max_limit, "axes.max_limit")},
                                   {"on_hard_limit", axis.on_hard_limit}};
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
  return fields.finish(
      {{"type", "status"},
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
       {"program",
        {{"file", nullable(program.file)},
         {"revision", fields.count(program.revision, "program.revision")},
         {"current_line", program.current_line},
         {"motion_line", program.motion_line},
         {"optional_stop", program.optional_stop},
         {"block_delete", program.block_delete}}},
       {"preview_context", fields.count(status.preview_context, "preview_context")},
       {"wait", wait(fields, status.wait)},
       {"active_codes", status.active_codes},
       {"program_units", name(status.program_units)},
       {"probe_input", nullable(status.probe_input)}});
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
         {"rotation_xy", fields.real(offsets.systems[i].rotation_xy, "systems.rotation_xy")}});
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
                        {"truncated", program.truncated}});
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
                        {"points", std::move(points)}});
  }
  Json error = nullptr;
  if (preview.error)
    error = {{"line", preview.error->line}, {"text", preview.error->text}};
  Json bounds = nullptr;
  if (preview.bounds)
    bounds = {{"min", fields.vec(preview.bounds->min, "bounds.min")},
              {"max", fields.vec(preview.bounds->max, "bounds.max")}};
  return fields.finish({{"type", "preview"},
                        {"context", fields.count(preview.context, "context")},
                        {"state", name(preview.state)},
                        {"error", std::move(error)},
                        {"segments", std::move(segments)},
                        {"bounds", std::move(bounds)}});
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
  return fields.finish({{"type", "directory"},
                        {"path", directory.path},
                        {"parent", nullable(directory.parent)},
                        {"entries", std::move(entries)},
                        {"truncated", directory.truncated}});
}

} // namespace coffeecnc
