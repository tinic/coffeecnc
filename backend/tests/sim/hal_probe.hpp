#ifndef COFFEECNC_TESTS_SIM_HAL_PROBE_HPP
#define COFFEECNC_TESTS_SIM_HAL_PROBE_HPP

#include <expected>
#include <string>

namespace coffeecnc {

// The simulation test's own HAL component: input pins the test nets to the signals it watches,
// so that it reads what the servo thread sees, every fraction of a millisecond and without
// asking the executable's component or its threads.
//
//   coffeecnc-test.jog-stop      bit in    the level the executable drives
//   coffeecnc-test.watchdog-ok   bit in
//   coffeecnc-test.tool-change   bit in
//   coffeecnc-test.tool-changed  bit in
//   coffeecnc-test.x-pos         float in  position of joint 0, machine units
//
// One thread, the test's, reads it.
class HalProbe {
public:
  [[nodiscard]] static auto create() -> std::expected<HalProbe, std::string>;
  HalProbe(HalProbe&& other) noexcept;
  HalProbe(const HalProbe&) = delete;
  auto operator=(const HalProbe&) -> HalProbe& = delete;
  auto operator=(HalProbe&&) -> HalProbe& = delete;
  ~HalProbe();

  [[nodiscard]] auto jog_stop() const -> bool;
  [[nodiscard]] auto watchdog_ok() const -> bool;
  [[nodiscard]] auto tool_change() const -> bool;
  [[nodiscard]] auto tool_changed() const -> bool;
  [[nodiscard]] auto x() const -> double;

private:
  struct Pins;
  HalProbe(int id, Pins* pins);
  int id_{-1};
  Pins* pins_{};
};

} // namespace coffeecnc

#endif // COFFEECNC_TESTS_SIM_HAL_PROBE_HPP
