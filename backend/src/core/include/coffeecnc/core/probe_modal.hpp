#ifndef COFFEECNC_CORE_PROBE_MODAL_HPP
#define COFFEECNC_CORE_PROBE_MODAL_HPP

#include <cmath>
#include <format>
#include <optional>
#include <string>

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
  std::string changed;
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

} // namespace coffeecnc

#endif
