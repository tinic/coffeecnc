#ifndef COFFEECNC_CORE_PROBE_MODAL_HPP
#define COFFEECNC_CORE_PROBE_MODAL_HPP

#include <coffeecnc/core/controller_port.hpp>

#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace coffeecnc {

// The modal state a probe cycle changes while it runs: distance mode and feed, M50, M52.
struct ProbeModalState {
  bool incremental{};  // G91
  double feed_mm_min{};
  bool feed_override{}; // M50 P1
  bool adaptive_feed{}; // M52 P1
};

// What to restore after a probe cycle stopped early. Each item (distance mode with F, M50, M52)
// is restored on its own, and only while it is still what the cycle left: an item somebody set
// since stays as it is and is reported in `changed`.
struct ProbeModalPlan {
  bool unknown{};       // nothing is known about what the cycle left: restore nothing, tell
  bool distance{};      // send G90/G91 and F
  bool feed_override{}; // send M50 P1
  bool adaptive_feed{}; // send M52 P1
  // What the cycle left of the items not restored, as G-code words ("G90 F240", "M50 P0"),
  // joined by ", ". Empty when every item it changed is restored.
  std::string changed{};
};

// `left` is the state after the stop, empty when status was unavailable then. `commands` is
// whether a G-code command (MDI, a program) ran since the stop: then nothing is restored.
inline auto plan_probe_restore(const ProbeModalState& operator_state,
                               const std::optional<ProbeModalState>& left,
                               const ProbeModalState& now, bool commands) -> ProbeModalPlan {
  ProbeModalPlan plan;
  if (!left) {
    plan.unknown = true;
    return plan;
  }
  const auto same_feed = [](double a, double b) { return std::abs(a - b) <= 1e-6; };
  const auto note = [&](std::string words) {
    plan.changed += (plan.changed.empty() ? "" : ", ") + std::move(words);
  };
  // The cycle changed an item when it left it other than the operator had it.
  if (left->incremental != operator_state.incremental ||
      !same_feed(left->feed_mm_min, operator_state.feed_mm_min)) {
    if (!commands && now.incremental == left->incremental &&
        same_feed(now.feed_mm_min, left->feed_mm_min))
      plan.distance = true;
    else
      note(std::format("{} F{:g}", left->incremental ? "G91" : "G90", left->feed_mm_min));
  }
  if (left->feed_override != operator_state.feed_override) {
    if (!commands && now.feed_override == left->feed_override)
      plan.feed_override = true;
    else
      note(std::format("M50 P{}", left->feed_override ? 1 : 0));
  }
  if (left->adaptive_feed != operator_state.adaptive_feed) {
    if (!commands && now.adaptive_feed == left->adaptive_feed)
      plan.adaptive_feed = true;
    else
      note(std::format("M52 P{}", left->adaptive_feed ? 1 : 0));
  }
  return plan;
}

// A new probe cycle while a restore is pending. It takes over the operator's state only while
// every item is what the stopped cycle left and no command ran since. Otherwise nothing of the
// stopped cycle is restored, so `report` names every item that cycle changed.
struct ProbeModalTakeover {
  bool take_over{};
  ProbeModalPlan report{};
};

inline auto plan_probe_takeover(const ProbeModalState& operator_state,
                                const std::optional<ProbeModalState>& left,
                                const ProbeModalState& now, bool commands) -> ProbeModalTakeover {
  auto plan = plan_probe_restore(operator_state, left, now, commands);
  if (plan.unknown)
    return {.take_over = false, .report = std::move(plan)};
  if (plan.changed.empty())
    return {.take_over = true, .report = {}};
  // As after a command: every item the cycle changed is named, also one still restorable.
  return {.take_over = false, .report = plan_probe_restore(operator_state, left, now, true)};
}

// A stop the port could not confirm keeps the probe guard set until a backend restart, so
// idle_maintenance never restores: report "not restored" now. Empty for a confirmed stop, where
// idle_maintenance decides later.
inline auto plan_probe_stop(bool stop_confirmed) -> std::optional<ProbeModalPlan> {
  if (stop_confirmed)
    return std::nullopt;
  return ProbeModalPlan{.unknown = true};
}

// The keyed panel messages for what a restore did not do.
inline auto probe_modal_messages(const ProbeModalPlan& plan) -> std::vector<ControllerMessage> {
  std::vector<ControllerMessage> messages;
  if (plan.unknown)
    messages.push_back(
        {true,
         "Could not restore the distance mode and feed controls after the stopped probe cycle. "
         "Check G90 or G91 and the feed override.",
         MessageKey{"backend.nmlPort.probeModalNotRestored"}});
  if (!plan.changed.empty())
    messages.push_back(
        {false,
         std::format("The stopped probe cycle left {}. CoffeeCNC did not restore it: it changed, "
                     "or a command ran, after the stop.",
                     plan.changed),
         MessageKey{"backend.nmlPort.probeModalChanged", {{"items", plan.changed}}}});
  return messages;
}

} // namespace coffeecnc

#endif
