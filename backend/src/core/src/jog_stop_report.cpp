#include <coffeecnc/core/jog_stop_report.hpp>

namespace coffeecnc {

auto is_jog_stop_text(std::string_view text) -> bool {
  return text == "Jog aborted by jog-stop" || text == "Jog aborted by jog-stop-immediate";
}

auto classify_jog_stop(std::string_view text, std::chrono::steady_clock::time_point read_at,
                       std::optional<std::chrono::steady_clock::time_point> own_level_high,
                       std::optional<std::chrono::steady_clock::time_point> watchdog_tripped)
    -> JogStopCause {
  if (!is_jog_stop_text(text))
    return JogStopCause::not_jog_stop;
  const auto within = [&](std::optional<std::chrono::steady_clock::time_point> seen) -> bool {
    return seen && *seen >= read_at - jog_stop_window;
  };
  if (within(watchdog_tripped))
    return JogStopCause::watchdog;
  if (within(own_level_high))
    return JogStopCause::own;
  return JogStopCause::unattributed;
}

} // namespace coffeecnc
