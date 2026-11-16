#ifndef COFFEECNC_CORE_JOG_STOP_REPORT_HPP
#define COFFEECNC_CORE_JOG_STOP_REPORT_HPP

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

namespace coffeecnc {

// Who stopped a jog that LinuxCNC reports as "Jog aborted by jog-stop". Motion reports it in
// every servo period in which that pin is high while a jog is active (emc/motion/control.c:542),
// so every such text was produced while the pin was high. In the shipped post-GUI file
// motion.jog-stop is driven by CoffeeCNC's own level OR the HAL watchdog, nothing else.
// "Jog aborted by jog-stop-immediate" comes from motion.jog-stop-immediate, which neither
// drives: it is never attributed and always reaches the operator unchanged.
//
// The watchdog is asked first. It trips only after CoffeeCNC's control thread has not run for
// the watchdog's timeout, and CoffeeCNC raises its own level as soon as that thread runs again
// and sees the trip: a level high next to a trip is a reaction to the same stall.
enum class JogStopCause : std::uint8_t {
  not_jog_stop, // another text: passed on unchanged
  watchdog,     // watchdog-ok read false within the window or after: watchdog_stop_text
  own,          // else CoffeeCNC's level was high within the window: debug log only
  unattributed, // neither: passed on unchanged
};

// How long after CoffeeCNC's level was last high, or the watchdog last read tripped, the text
// may still be read. Motion publishes it while the pin is high; task passes it on a few ms later
// (docs/linuxcnc-observed.md, error channel); the worker reads the error channel in every 1 ms
// poll of its waits and at least once per 10 ms control period when idle. 200 ms is ten times
// that. It is below the watchdog's 300 ms timeout, which the port relies on: a control thread
// that read the pins less than 200 ms before the text has seen any trip behind it.
inline constexpr auto jog_stop_window = std::chrono::milliseconds{200};

// What the operator is told in place of LinuxCNC's text when the watchdog stopped the jog.
inline constexpr std::string_view watchdog_stop_text =
    "Jog stopped by the HAL watchdog: CoffeeCNC's heartbeat stalled for longer than the "
    "watchdog timeout.";

[[nodiscard]] auto is_jog_stop_text(std::string_view text) -> bool;

// Motion refuses a jog start while motion.jog-inhibit is high with "Cannot jog while jog-inhibit
// is active." (emc/motion/command.c; the text in motmod.so of LinuxCNC 2.9). In the shipped
// post-GUI file that pin is driven by CoffeeCNC's own level and the HAL watchdog. True for a
// failure reason that is this text alone, with or without its period, once or several times
// joined by "; ".
[[nodiscard]] auto is_jog_inhibit_refusal(std::string_view reason) -> bool;

// `read_at`: when the text was read from LinuxCNC's error channel. `own_level_high`: the last
// moment CoffeeCNC's control thread found or set its level high; it notes the level before each
// write, so a level behind the text is known at `read_at`. `watchdog_tripped`: the last reading
// of the pins with watchdog-ok false, from a history the control thread has renewed after the
// text was read where it could.
[[nodiscard]] auto
classify_jog_stop(std::string_view text, std::chrono::steady_clock::time_point read_at,
                  std::optional<std::chrono::steady_clock::time_point> own_level_high,
                  std::optional<std::chrono::steady_clock::time_point> watchdog_tripped)
    -> JogStopCause;

} // namespace coffeecnc

#endif // COFFEECNC_CORE_JOG_STOP_REPORT_HPP
