// What crosses between the control thread and the others about the HAL component, without HAL:
// the control thread's readings are passed in.

#include <coffeecnc/linuxcnc/hal_control.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <latch>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

using namespace coffeecnc;
using namespace std::chrono_literals;

namespace {
int checks = 0;
void require(bool condition, const std::string& message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

auto at(std::chrono::milliseconds elapsed) -> HalShared::Clock::time_point {
  return HalShared::Clock::time_point{} + 1h + elapsed;
}

// The pins with LinuxCNC asking for `tool`, or for nothing.
auto asking(std::optional<int> tool) -> HalInputs {
  HalInputs inputs;
  inputs.tool_change_request = tool;
  inputs.watchdog_ok = true;
  return inputs;
}

// What the other threads are shown; the test fails where they would be shown nothing.
auto view(const HalShared& shared, std::chrono::milliseconds elapsed) -> HalView {
  const auto shown = shared.view(at(elapsed));
  if (!shown)
    throw std::runtime_error("No current copy of the pins");
  return *shown;
}
auto request(const HalShared& shared, std::chrono::milliseconds elapsed) -> ToolChangeRequest {
  const auto asked = view(shared, elapsed).tool_change_request;
  if (!asked)
    throw std::runtime_error("No tool change request is shown");
  return *asked;
}
auto asked(const HalShared& shared, std::chrono::milliseconds elapsed) -> bool {
  return view(shared, elapsed).tool_change_request.has_value();
}

void paused_z_count_baseline() {
  HalShared shared;
  auto inputs = asking({});
  inputs.paused_z = PausedZInputs{};
  static_cast<void>(shared.exchange(inputs, at(0ms)));
  require(shared.begin_paused_z(at(1ms)) && shared.move_paused_z(at(2ms), 300, false),
          "Enable paused Z and move from its count baseline");
  require(!shared.rebase_paused_z_counts() && shared.paused_z_outputs(at(3ms)).counts == 300,
          "The count does not reset while the move gate is open");
  shared.stop_paused_z();
  require(shared.rebase_paused_z_counts() && shared.paused_z_outputs(at(4ms)).counts == 0,
          "The next move starts with a fresh baseline after a confirmed clear");
}

void handshake_completes() {
  HalShared shared;
  require(!shared.view(at(0ms)), "Nothing is known before the control thread has read the pins");
  require(!shared.exchange(asking({}), at(0ms)), "Nothing to raise without a request");
  require(!asked(shared, 0ms), "No request is shown");
  require(!shared.exchange(asking(2), at(10ms)), "A request alone raises nothing");
  const auto first = request(shared, 10ms);
  require(first.tool == 2, "The request is shown with its tool");
  require(shared.confirm_tool_changed(first), "The operator confirms it");
  require(!asked(shared, 10ms), "It is answered: not shown any more");
  require(!shared.confirm_tool_changed(first), "It cannot be confirmed twice");
  require(shared.exchange(asking(2), at(20ms)),
          "The next period finds the request still on the pins: tool-changed is raised");
  require(!shared.exchange(asking(2), at(30ms)), "Once");
  require(!asked(shared, 30ms), "It stays answered while LinuxCNC reacts");
  require(!shared.exchange(asking({}), at(40ms)), "LinuxCNC drops its request");
  require(!shared.exchange(asking(2), at(50ms)), "It asks for the same tool again later");
  const auto again = request(shared, 50ms);
  require(again.tool == 2 && again.generation != first.generation, "That is another request");
}

void withdrawn_request_is_not_confirmed() {
  HalShared shared;
  static_cast<void>(shared.exchange(asking(2), at(0ms)));
  require(shared.confirm_tool_changed(request(shared, 0ms)), "Confirmed");
  // Abort or E-stop: the request is gone before the control thread comes to the confirmation.
  require(!shared.exchange(asking({}), at(10ms)), "Withdrawn: tool-changed is not raised");
  require(!shared.exchange(asking(2), at(20ms)),
          "Nor for the request that follows, although it asks for the same tool");
  require(asked(shared, 20ms), "The new request waits for its own confirmation");

  // The other order: withdrawn before the operator's confirmation arrives.
  HalShared late;
  static_cast<void>(late.exchange(asking(2), at(0ms)));
  const auto decided_on = request(late, 0ms);
  static_cast<void>(late.exchange(asking({}), at(10ms)));
  require(!late.confirm_tool_changed(decided_on), "Nothing is asked: the confirmation is refused");
  static_cast<void>(late.exchange(asking(2), at(20ms)));
  require(!late.confirm_tool_changed(decided_on),
          "A decision made on the first request does not answer a second one for the same tool");
  require(!late.exchange(asking(2), at(30ms)), "And nothing is raised");
}

void replaced_request_is_not_confirmed() {
  HalShared shared;
  static_cast<void>(shared.exchange(asking(2), at(0ms)));
  const auto first = request(shared, 0ms);
  require(shared.confirm_tool_changed(first), "Confirmed for tool 2");
  require(!shared.exchange(asking(3), at(10ms)),
          "LinuxCNC asks for tool 3 instead: the confirmation of tool 2 is not applied");
  const auto second = request(shared, 10ms);
  require(second.tool == 3 && second.generation != first.generation,
          "Request N + 1 is shown, unanswered");
  require(!shared.confirm_tool_changed(first), "The old request cannot be confirmed any more");
  require(!shared.confirm_tool_changed({second.generation, 2}) &&
              !shared.confirm_tool_changed({first.generation, 3}),
          "Generation and tool must both be those of the request");
  require(shared.confirm_tool_changed(second) && shared.exchange(asking(3), at(20ms)),
          "Its own confirmation is applied");
}

void old_copy_is_not_handed_out() {
  HalShared shared;
  auto inputs = asking(4);
  inputs.spindle_rpm = 1200.0;
  static_cast<void>(shared.exchange(inputs, at(0ms)));
  require(view(shared, 100ms).spindle_rpm == 1200.0, "A copy of max_age is still current");
  require(!shared.view(at(101ms)),
          "An older one is not handed out: the control thread has stalled");
  static_cast<void>(shared.exchange(inputs, at(500ms)));
  require(shared.view(at(500ms)).has_value(), "It is current again with the next reading");
}

void probe_cycle_needs_a_live_lease() {
  HalShared shared;
  require(!shared.begin_probe_cycle(at(0ms)), "Probe cannot arm before the first HAL sample");
  auto inputs = asking({});
  static_cast<void>(shared.exchange(inputs, at(0ms)));
  require(!shared.begin_probe_cycle(at(101ms)), "Probe cannot arm on stale HAL status");
  require(shared.begin_probe_cycle(at(10ms)), "Probe can arm on live HAL and watchdog status");
  require(!shared.begin_probe_cycle(at(11ms)), "A second probe cannot replace an active one");
  auto outputs = shared.probe_cycle_outputs(at(10ms));
  require(outputs.active && outputs.lease, "Armed probe selects the setter and permits motion");

  inputs.probe_setter_selected = true;
  inputs.probe_motion_permitted = true;
  inputs.probe_cycle_active = true;
  static_cast<void>(shared.exchange(inputs, at(90ms)));
  const auto armed = view(shared, 90ms);
  require(armed.probe_setter_selected && armed.probe_motion_permitted, "Worker can read the physical acknowledgement pins");
  require(shared.renew_probe_cycle(at(90ms)), "A live worker can renew before expiry");
  require(shared.probe_cycle_outputs(at(189ms)).lease, "Renewal extends the lease");
  outputs = shared.probe_cycle_outputs(at(190ms));
  require(outputs.active && !outputs.lease, "Expired motion stays guarded until explicitly released");
  static_cast<void>(shared.exchange(inputs, at(191ms)));
  require(!shared.renew_probe_cycle(at(191ms)), "A late worker cannot reinstate the expired lease");

  shared.finish_probe_cycle();
  require(!shared.probe_cycle_outputs(at(192ms)).active, "A finished probe releases the hardware guard");
  require(!shared.begin_probe_cycle(at(192ms)), "A finished cycle cannot supply the next cycle's permit");
  inputs.probe_cycle_active = false;
  static_cast<void>(shared.exchange(inputs, at(192ms)));
  require(shared.begin_probe_cycle(at(193ms)), "A fresh operation can arm");
  shared.stop_probe_cycle();
  outputs = shared.probe_cycle_outputs(at(194ms));
  require(outputs.active && !outputs.lease, "Stop revokes the lease immediately and keeps the guard active");
  require(!shared.renew_probe_cycle(at(195ms)), "Stop cannot be reversed by a renewal");
  shared.finish_probe_cycle();

  require(shared.begin_probe_cycle(at(196ms)), "Arm another operation before a watchdog trip");
  inputs.watchdog_ok = false;
  static_cast<void>(shared.exchange(inputs, at(197ms)));
  inputs.watchdog_ok = true;
  static_cast<void>(shared.exchange(inputs, at(198ms)));
  require(!shared.renew_probe_cycle(at(198ms)) && shared.probe_cycle_outputs(at(198ms)).active,
          "Even after watchdog recovery, the interrupted cycle stays guarded and stopped");
}

// For the thread sanitizer: readers and a confirming thread beside the control thread.
void threads_share_it() {
  HalShared shared;
  std::atomic<bool> stop{};
  const auto initial_time = HalShared::Clock::now();
  const auto initial_request = asking(1);
  static_cast<void>(shared.exchange(initial_request, initial_time));
  std::latch initial_attempt{1};
  std::thread reader{[&]() -> void {
    while (!stop)
      static_cast<void>(shared.view(HalShared::Clock::now()));
  }};
  std::thread confirmer{[&]() -> void {
    const auto confirm = [&](HalShared::Clock::time_point now) {
      if (const auto shown = shared.view(now))
        if (const auto wanted = shown->tool_change_request)
          static_cast<void>(shared.confirm_tool_changed(*wanted));
    };
    confirm(initial_time);
    initial_attempt.count_down();
    while (!stop)
      confirm(HalShared::Clock::now());
  }};
  // Make the first attempt while its request is still current. The stress loop
  // alone can exhaust all its periods before the confirming thread is scheduled.
  initial_attempt.wait();
  int raised = shared.exchange(initial_request, initial_time) ? 1 : 0;
  for (int period = 0; period < 20000; ++period) {
    // A request every other eight periods, each for another tool.
    const auto tool = (period / 8) % 2 == 0 ? std::optional<int>{period / 16 + 1} : std::nullopt;
    if (shared.exchange(asking(tool), HalShared::Clock::now()))
      ++raised;
  }
  stop = true;
  reader.join();
  confirmer.join();
  require(raised > 0, "Confirmations were applied");
}


// What the control thread noted about the jog-stop level and the watchdog.
void jog_stop_history_is_noted() {
  HalShared shared;
  auto none = shared.jog_stop_history();
  require(!none.own_level_high && !none.watchdog_tripped && !none.read_at,
          "nothing before a reading");
  auto inputs = asking(std::nullopt);
  static_cast<void>(shared.exchange(inputs, at(0ms)));
  auto history = shared.jog_stop_history();
  require(history.read_at == at(0ms) && !history.own_level_high && !history.watchdog_tripped,
          "a reading with the level low and the watchdog ok notes only the reading");
  shared.note_jog_stop_high(at(5ms));
  require(shared.jog_stop_history().own_level_high == at(5ms), "a raised level is noted");
  inputs.jog_stop_high = true;
  static_cast<void>(shared.exchange(inputs, at(10ms)));
  require(shared.jog_stop_history().own_level_high == at(10ms), "a level read high is noted");
  inputs.jog_stop_high = false;
  inputs.watchdog_ok = false;
  static_cast<void>(shared.exchange(inputs, at(20ms)));
  history = shared.jog_stop_history();
  require(history.own_level_high == at(10ms) && history.watchdog_tripped == at(20ms) &&
              history.read_at == at(20ms),
          "the last high level is kept; a tripped watchdog is noted");
  inputs.watchdog_ok = true;
  static_cast<void>(shared.exchange(inputs, at(30ms)));
  history = shared.jog_stop_history();
  require(history.watchdog_tripped == at(20ms) && history.read_at == at(30ms),
          "the last trip is kept once the watchdog is ok again");
}

} // namespace

auto main() -> int {
  try {
    handshake_completes();
    paused_z_count_baseline();
    withdrawn_request_is_not_confirmed();
    replaced_request_is_not_confirmed();
    old_copy_is_not_handed_out();
    probe_cycle_needs_a_live_lease();
    threads_share_it();
    jog_stop_history_is_noted();
    std::cout << checks << " HAL exchange checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Check " << checks << " failed: " << error.what() << '\n';
    return 1;
  }
}
