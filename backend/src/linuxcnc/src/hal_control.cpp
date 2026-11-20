#include <coffeecnc/linuxcnc/hal_control.hpp>

namespace coffeecnc {

auto HalShared::view(Clock::time_point now) const -> std::optional<HalView> {
  const std::scoped_lock lock{mutex_};
  if (!have_inputs_ || now - read_at_ > max_age)
    return {};
  HalView view{{},
               inputs_.spindle_rpm,
               inputs_.spindle_at_speed,
               inputs_.probe_input,
               inputs_.watchdog_ok,
               inputs_.jog_stop_high,
               inputs_.probe_setter_selected,
               inputs_.probe_motion_permitted,
               inputs_.probe_cycle_active};
  if (inputs_.tool_change_request && !answered_)
    view.tool_change_request = ToolChangeRequest{generation_, *inputs_.tool_change_request};
  return view;
}

auto HalShared::confirm_tool_changed(const ToolChangeRequest& request) -> bool {
  const std::scoped_lock lock{mutex_};
  if (!have_inputs_ || answered_ || !inputs_.tool_change_request ||
      request != ToolChangeRequest{generation_, *inputs_.tool_change_request})
    return false;
  confirmed_ = request;
  answered_ = true;
  return true;
}

auto HalShared::exchange(const HalInputs& inputs, Clock::time_point now) -> bool {
  const std::scoped_lock lock{mutex_};
  // A request that appears, or asks for another tool, is a new one.
  if (inputs.tool_change_request &&
      (!have_inputs_ || inputs.tool_change_request != inputs_.tool_change_request)) {
    ++generation_;
    answered_ = false;
  } else if (!inputs.tool_change_request) {
    answered_ = false;
  }
  inputs_ = inputs;
  read_at_ = now;
  have_inputs_ = true;
  history_.read_at = now;
  if (inputs.jog_stop_high)
    history_.own_level_high = now;
  if (!inputs.watchdog_ok)
    history_.watchdog_tripped = now;
  if (probe_deadline_ && !inputs.watchdog_ok)
    probe_deadline_ = Clock::time_point::min();
  // Withdrawn or replaced since it was confirmed: the confirmation answers nothing.
  const bool raise = confirmed_ && inputs.tool_change_request &&
                     *confirmed_ == ToolChangeRequest{generation_, *inputs.tool_change_request};
  confirmed_.reset();
  return raise;
}

void HalShared::note_jog_stop_high(Clock::time_point now) {
  const std::scoped_lock lock{mutex_};
  history_.own_level_high = now;
}

auto HalShared::jog_stop_history() const -> JogStopHistory {
  const std::scoped_lock lock{mutex_};
  return history_;
}

auto HalShared::begin_probe_cycle(Clock::time_point now) -> bool {
  const std::scoped_lock lock{mutex_};
  if (probe_deadline_ || !have_inputs_ || now - read_at_ > max_age || !inputs_.watchdog_ok)
    return false;
  probe_deadline_ = now + max_age;
  return true;
}

auto HalShared::renew_probe_cycle(Clock::time_point now) -> bool {
  const std::scoped_lock lock{mutex_};
  if (!probe_deadline_ || now >= *probe_deadline_ || now - read_at_ > max_age)
    return false;
  probe_deadline_ = now + max_age;
  return true;
}

void HalShared::stop_probe_cycle() {
  const std::scoped_lock lock{mutex_};
  if (probe_deadline_)
    probe_deadline_ = Clock::time_point::min();
}

void HalShared::finish_probe_cycle() {
  const std::scoped_lock lock{mutex_};
  probe_deadline_.reset();
}

auto HalShared::probe_cycle_outputs(Clock::time_point now) const -> ProbeCycleOutputs {
  const std::scoped_lock lock{mutex_};
  return {probe_deadline_.has_value(), probe_deadline_ && now < *probe_deadline_};
}

HalControl::HalControl(HalComponent& hal, HalShared& shared) : hal_(hal), shared_(shared) {}

void HalControl::beat() {
  const auto now = HalShared::Clock::now();
  const HalInputs inputs{hal_.tool_change_request(), hal_.spindle_rpm(), hal_.spindle_at_speed(),
                         hal_.probe_input(),         hal_.watchdog_ok(), hal_.jog_stop_high(),
                         hal_.probe_setter_selected(), hal_.probe_motion_permitted(),
                         hal_.probe_cycle_active()};
  // The exchange notes a high level before beat() may lower it.
  if (shared_.exchange(inputs, now))
    hal_.confirm_tool_changed();
  hal_.set_probe_cycle(shared_.probe_cycle_outputs(now));
  hal_.beat();
}

void HalControl::hold_jog_stop(bool on) {
  // Noted before the pin changes: raised, held, or lowered from high.
  if (on || hal_.jog_stop_high())
    shared_.note_jog_stop_high(HalShared::Clock::now());
  hal_.hold_jog_stop(on);
}
auto HalControl::jog_stop_high() const -> bool { return hal_.jog_stop_high(); }
auto HalControl::watchdog_ok() const -> bool { return hal_.watchdog_ok(); }

} // namespace coffeecnc
