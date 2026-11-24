#include <coffeecnc/app/response.hpp>

#include "message_key_json.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace coffeecnc {
namespace {
using Json = nlohmann::json;

auto name(CommandOutcome outcome) -> std::string_view {
  switch (outcome) {
  case CommandOutcome::accepted:
    return "accepted";
  case CommandOutcome::rejected:
    return "rejected";
  case CommandOutcome::failed:
    return "failed";
  case CommandOutcome::timed_out:
    return "timed_out";
  }
  return "failed";
}

auto finite_numbers(const Json& value) -> bool {
  if (value.is_number_float())
    return std::isfinite(value.get<double>());
  return !value.is_structured() || std::all_of(value.begin(), value.end(), finite_numbers);
}

auto text(const Json& message) -> std::string {
  // Controller text is not guaranteed to be UTF-8; dump() must not throw on it.
  return message.dump(-1, ' ', false, Json::error_handler_t::replace);
}
} // namespace

auto encode_result(RequestId id, CommandOutcome outcome, std::string_view reason,
                   const std::optional<SetterState>& setter,
                   const std::optional<RestartPlan>& restart,
                   const std::optional<RefusalMessage>& refusal,
                   const std::optional<MessageKey>& localization) -> std::string {
  Json message{{"type", "result"}, {"id", id}, {"outcome", name(outcome)}, {"reason", reason}};
  if (outcome == CommandOutcome::rejected && refusal)
    message["refusal"] = message_key_json(*refusal);
  // A refusal following a command already sent (for example a mode change) is a failure.
  const auto& key =
      !localization && outcome != CommandOutcome::rejected && outcome != CommandOutcome::accepted
          ? refusal
          : localization;
  if (key && (outcome != CommandOutcome::accepted || !reason.empty()))
    message["localization"] = message_key_json(*key);
  if (setter) {
    auto measurement = Json(nullptr);
    if (const auto& r = setter->measurement) {
      measurement = {{"calibration", r->calibration},
                     {"tool", r->tool},
                     {"contact", r->contact},
                     {"value", r->value},
                     {"previous_length", r->previous_length},
                     {"stopping_distance", r->stopping_distance}};
      if (r->check_tolerance_mm)
        measurement["check_tolerance_mm"] = *r->check_tolerance_mm;
    }
    message["setter"] = {
        {"calibration", setter->calibration ? Json(*setter->calibration) : Json(nullptr)},
        {"measurement", std::move(measurement)},
        {"measured_tools", setter->measured_tools}};
    if (setter->ball_diameter)
      message["setter"]["ball_diameter"] = *setter->ball_diameter;
    if (setter->center_diameter)
      message["setter"]["center_diameter"] = *setter->center_diameter;
    if (setter->arc_span_deg)
      message["setter"]["arc_span"] = *setter->arc_span_deg;
    if (setter->center_uncertainty_mm)
      message["setter"]["center_uncertainty"] = *setter->center_uncertainty_mm;
    if (setter->edge_angle_deg)
      message["setter"]["edge_angle"] = *setter->edge_angle_deg;
    if (const auto& edge = setter->edge_rotation)
      message["setter"]["edge_rotation"] = {
          {"measurement_id", edge->id}, {"wcs", edge->wcs}, {"rotation", edge->rotation_deg}};
    if (const auto width = setter->feature_size[0])
      message["setter"]["feature_size"]["x"] = *width;
    if (const auto width = setter->feature_size[1])
      message["setter"]["feature_size"]["y"] = *width;
    if (const auto& measured = setter->measured) {
      const auto direction = [](ProbeDirection d) {
        switch (d) {
        case ProbeDirection::z_top:
          return "z_top";
        case ProbeDirection::x_minus:
          return "x_minus";
        case ProbeDirection::x_plus:
          return "x_plus";
        case ProbeDirection::y_minus:
          return "y_minus";
        case ProbeDirection::y_plus:
          return "y_plus";
        }
        return "z_top";
      };
      auto faces = Json::array();
      std::ranges::transform(measured->faces, std::back_inserter(faces), [&](const auto& face) {
        return Json{
            {"direction", direction(face.direction)}, {"work", face.work}, {"machine", face.machine}};
      });
      auto work = Json::object();
      for (std::size_t axis = 0; axis < measured->work.size(); ++axis)
        if (const auto value = measured->work[axis])
          work[std::string(1, "xyz"[axis])] = *value;
      auto& out = message["setter"]["measured"];
      out = {{"wcs", measured->wcs}, {"axes", measured->axes}, {"work", std::move(work)},
             {"faces", std::move(faces)}};
      if (measured->id != 0)
        out["measurement_id"] = measured->id;
      if (measured->machine)
        out["machine"] = *measured->machine;
    }
    if (const auto& material = setter->material)
      message["setter"]["material"] = {{"wcs", material->wcs},
                                       {"axis", material->corner ? std::string{"XY"}
                                                                 : std::string(1, material->axis)}};
  }
  if (restart) {
    const auto& p = restart->point;
    const auto direction = p.spindle_direction == SpindleDirection::forward   ? "forward"
                           : p.spindle_direction == SpindleDirection::reverse ? "reverse"
                                                                             : "stopped";
    message["restart"] = {{"plan_id", restart->plan_id},
                          {"line", p.line},
                          {"tool", p.tool},
                          {"needs_tool_change", restart->needs_tool_change},
                          {"wcs", p.wcs},
                          {"h", p.h},
                          {"spindle_direction", direction},
                          {"spindle_rpm", p.spindle_rpm},
                          {"flood", p.flood},
                          {"mist", p.mist},
                          {"feed", p.feed},
                          {"units", p.units == LengthUnit::mm ? "mm" : "inch"},
                          {"plane", p.plane == 17   ? "XY"
                                    : p.plane == 18 ? "XZ"
                                                    : "YZ"},
                          {"distance_mode", p.incremental ? "incremental" : "absolute"},
                          {"clearance_z", p.clearance_z},
                          {"approach_feed", p.approach_feed},
                          {"start", p.start}};
  }
  if (!finite_numbers(message))
    return encode_result(id, CommandOutcome::failed, "The controller returned a non-finite result.",
                         {}, {}, {}, MessageKey{"messages.response.nonFinite"});
  return text(message);
}

auto encode_message(MessageLevel level, std::string_view text_for_operator,
                    std::chrono::milliseconds unix_time,
                    const std::optional<MessageKey>& localization) -> std::string {
  Json message{{"type", "message"},
               {"level", level == MessageLevel::info ? "info" : "error"},
               {"text", text_for_operator},
               {"time", unix_time.count()}};
  if (localization)
    message["localization"] = message_key_json(*localization);
  return text(message);
}

} // namespace coffeecnc
