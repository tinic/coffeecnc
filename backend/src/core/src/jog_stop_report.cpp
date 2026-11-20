#include <coffeecnc/core/jog_stop_report.hpp>

namespace coffeecnc {

auto is_jog_stop_text(std::string_view text) -> bool {
  return text == "Jog aborted by jog-stop";
}

auto is_jog_inhibit_refusal(std::string_view reason) -> bool {
  constexpr std::string_view text = "Cannot jog while jog-inhibit is active";
  constexpr std::string_view separator = "; ";
  if (reason.empty())
    return false;
  for (;;) {
    const auto end = reason.find(separator);
    auto part = reason.substr(0, end);
    if (part.ends_with('.'))
      part.remove_suffix(1);
    if (part != text)
      return false;
    if (end == std::string_view::npos)
      return true;
    reason.remove_prefix(end + separator.size());
  }
}

auto classify_jog_stop(std::string_view text, std::chrono::steady_clock::time_point read_at,
                       std::optional<std::chrono::steady_clock::time_point> own_level_high,
                       std::optional<std::chrono::steady_clock::time_point> watchdog_tripped,
                       std::optional<std::chrono::steady_clock::time_point> contact_edge)
    -> JogStopCause {
  if (!is_jog_stop_text(text))
    return JogStopCause::not_jog_stop;
  const auto within = [&](std::optional<std::chrono::steady_clock::time_point> seen) -> bool {
    return seen && *seen >= read_at - jog_stop_window;
  };
  if (within(watchdog_tripped))
    return JogStopCause::watchdog;
  if (within(contact_edge))
    return JogStopCause::probe_contact;
  if (within(own_level_high))
    return JogStopCause::own;
  return JogStopCause::unattributed;
}

} // namespace coffeecnc
