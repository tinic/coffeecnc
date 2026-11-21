#ifndef COFFEECNC_LINUXCNC_HAL_CONTROL_HPP
#define COFFEECNC_LINUXCNC_HAL_CONTROL_HPP

#include <coffeecnc/core/command_policy.hpp>
#include <coffeecnc/core/safety_outputs.hpp>
#include <coffeecnc/linuxcnc/hal_component.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>

namespace coffeecnc {

// One request of LinuxCNC for a manual tool change, as the control thread saw it appear.
struct ToolChangeRequest {
  // Counts the requests seen: goes up when a request appears and when its tool number changes.
  std::uint64_t generation{};
  int tool{};
  friend auto operator==(const ToolChangeRequest&, const ToolChangeRequest&) -> bool = default;
};

// What the HAL component's pins read in one control period.
struct HalInputs {
  // The tool LinuxCNC is waiting for the operator to load, until it is confirmed.
  std::optional<int> tool_change_request;
  // Nothing where the machine does not connect the pin.
  std::optional<double> spindle_rpm; // magnitude
  std::optional<bool> spindle_at_speed;
  std::optional<bool> probe_input;
  bool watchdog_ok{};
  bool jog_stop_high{};
  bool probe_setter_selected{};
  bool probe_motion_permitted{};
  bool probe_cycle_active{};
  bool jog_contact{};
  std::optional<PausedZInputs> paused_z{};
};

// The same for the other threads.
struct HalView {
  std::optional<ToolChangeRequest> tool_change_request; // not yet confirmed
  std::optional<double> spindle_rpm;
  std::optional<bool> spindle_at_speed;
  std::optional<bool> probe_input;
  bool watchdog_ok{};
  bool jog_stop_high{};
  bool probe_setter_selected{};
  bool probe_motion_permitted{};
  bool probe_cycle_active{};
  std::optional<PausedZInputs> paused_z{};
};

// What crosses between the control thread, which alone touches the HAL component, and every
// other thread: a copy of the pins one way, the tool change confirmation the other. Thread-safe;
// its mutex is held for a copy only. No HAL in it: the control thread passes in what it read.
//
// A confirmation answers one request, named by its generation and tool. The control thread
// raises tool-changed for it only while exactly that request is still on the pins. A
// confirmation whose request was withdrawn (abort, E-stop) or replaced by another before the
// control thread came to it is dropped.
class HalShared {
public:
  using Clock = std::chrono::steady_clock;
  // A copy the control thread has not renewed for this long is not handed out: that thread has
  // stalled, and the pins may read anything by now.
  static constexpr auto max_age = std::chrono::milliseconds{100};

  // Any thread: the pins as the control thread read them last. Nothing before the first reading
  // and when the last is older than max_age.
  [[nodiscard]] auto view(Clock::time_point now) const -> std::optional<HalView>;
  // Any thread: the operator has loaded the tool of this request. It is gone from view() at
  // once. False, and nothing is noted, when it is not the request in the last reading.
  [[nodiscard]] auto confirm_tool_changed(const ToolChangeRequest& request) -> bool;

  // Control thread, once per period, with the pins as just read. True when tool-changed must be
  // raised now: a confirmation is waiting and it answers the request in this reading.
  [[nodiscard]] auto exchange(const HalInputs& inputs, Clock::time_point now) -> bool;

  // Control thread: CoffeeCNC's own jog-stop level is high at `now`, or is about to be raised
  // or lowered. Called before every write of the level, so that a jog-stop text LinuxCNC
  // publishes about it is always read after the note.
  void note_jog_stop_high(Clock::time_point now);

  // When the control thread last found or set CoffeeCNC's level high, when it last read the
  // watchdog not ok, and when it last read the pins. Nothing for what never happened.
  struct JogStopHistory {
    std::optional<Clock::time_point> own_level_high;
    std::optional<Clock::time_point> watchdog_tripped;
    std::optional<Clock::time_point> read_at;
    std::optional<Clock::time_point> contact_edge;
  };
  [[nodiscard]] auto jog_stop_history() const -> JogStopHistory;

  // The worker must renew from a live status loop. Expiry holds the permit low while active,
  // and a late renewal cannot restart it. Finish only after confirming motion has stopped.
  [[nodiscard]] auto begin_probe_cycle(Clock::time_point now, bool setter = true) -> bool;
  [[nodiscard]] auto renew_probe_cycle(Clock::time_point now) -> bool;
  void stop_probe_cycle();
  void finish_probe_cycle();
  [[nodiscard]] auto probe_cycle_outputs(Clock::time_point now) const -> ProbeCycleOutputs;

  // Worker-owned spindle hold state, reported with controller status.
  std::atomic<PausedSpindle> paused_spindle{PausedSpindle::none};

  [[nodiscard]] auto begin_paused_z(Clock::time_point now) -> bool;
  [[nodiscard]] auto move_paused_z(Clock::time_point now, std::int32_t delta, bool clear) -> bool;
  [[nodiscard]] auto renew_paused_z(Clock::time_point now) -> bool;
  void stop_paused_z();
  // Called after the controller has seen the gate closed and the applied offset is zero.
  [[nodiscard]] auto rebase_paused_z_counts() -> bool;
  [[nodiscard]] auto paused_z_outputs(Clock::time_point now) const -> PausedZOutputs;

private:
  mutable std::mutex mutex_;
  HalInputs inputs_;
  Clock::time_point read_at_{};
  bool have_inputs_{};
  // Of the request in inputs_, or of the last one seen.
  std::uint64_t generation_{};
  // The request in inputs_ has been confirmed.
  bool answered_{};
  std::optional<ToolChangeRequest> confirmed_;
  JogStopHistory history_;
  std::optional<Clock::time_point> probe_deadline_;
  bool probe_setter_{true};
  std::optional<Clock::time_point> paused_z_deadline_;
  std::int32_t paused_z_counts_{}; // Hold the count while disabled. LinuxCNC acts on deltas.
  bool paused_z_clear_{};
};

// The control thread's use of the HAL component, as the runtime sees it. Every member is called
// by that thread only.
class HalControl final : public SafetyOutputs {
public:
  // Both must outlive this.
  HalControl(HalComponent& hal, HalShared& shared);

  // Reads the pins and hands them to the other threads, raises tool-changed when one of them
  // has confirmed the request the pins show, then HalComponent::beat. In that order: a
  // confirmation is checked against the pins of this period and is on its pin before beat()
  // looks at the handshake, and beat() takes it back only once LinuxCNC has dropped tool-change.
  void beat() override;
  void hold_jog_stop(bool on) override;
  [[nodiscard]] auto jog_stop_high() const -> bool override;
  [[nodiscard]] auto watchdog_ok() const -> bool override;

private:
  HalComponent& hal_;
  HalShared& shared_;
};

} // namespace coffeecnc

#endif // COFFEECNC_LINUXCNC_HAL_CONTROL_HPP
