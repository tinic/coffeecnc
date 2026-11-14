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
               inputs_.jog_stop_high};
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
  // Withdrawn or replaced since it was confirmed: the confirmation answers nothing.
  const bool raise = confirmed_ && inputs.tool_change_request &&
                     *confirmed_ == ToolChangeRequest{generation_, *inputs.tool_change_request};
  confirmed_.reset();
  return raise;
}

HalControl::HalControl(HalComponent& hal, HalShared& shared) : hal_(hal), shared_(shared) {}

void HalControl::beat() {
  const HalInputs inputs{hal_.tool_change_request(), hal_.spindle_rpm(), hal_.spindle_at_speed(),
                         hal_.probe_input(),         hal_.watchdog_ok(), hal_.jog_stop_high()};
  if (shared_.exchange(inputs, HalShared::Clock::now()))
    hal_.confirm_tool_changed();
  hal_.beat();
}

void HalControl::hold_jog_stop(bool on) { hal_.hold_jog_stop(on); }
auto HalControl::jog_stop_high() const -> bool { return hal_.jog_stop_high(); }
auto HalControl::watchdog_ok() const -> bool { return hal_.watchdog_ok(); }

} // namespace coffeecnc
