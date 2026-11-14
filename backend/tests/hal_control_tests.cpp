// What crosses between the control thread and the others about the HAL component, without HAL:
// the control thread's readings are passed in.

#include <coffeecnc/linuxcnc/hal_control.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
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

void handshake_completes() {
  HalShared shared;
  require(!shared.view(at(0ms)), "Nothing is known before the control thread has read the pins");
  require(!shared.exchange(asking({}), at(0ms)), "Nothing to raise without a request");
  require(shared.view(at(0ms)).has_value() && !shared.view(at(0ms))->tool_change_request,
          "No request is shown");
  require(!shared.exchange(asking(2), at(10ms)), "A request alone raises nothing");
  const auto request = shared.view(at(10ms))->tool_change_request;
  require(request.has_value() && request->tool == 2, "The request is shown with its tool");
  require(shared.confirm_tool_changed(*request), "The operator confirms it");
  require(!shared.view(at(10ms))->tool_change_request, "It is answered: not shown any more");
  require(!shared.confirm_tool_changed(*request), "It cannot be confirmed twice");
  require(shared.exchange(asking(2), at(20ms)),
          "The next period finds the request still on the pins: tool-changed is raised");
  require(!shared.exchange(asking(2), at(30ms)), "Once");
  require(!shared.view(at(30ms))->tool_change_request, "It stays answered while LinuxCNC reacts");
  require(!shared.exchange(asking({}), at(40ms)), "LinuxCNC drops its request");
  require(!shared.exchange(asking(2), at(50ms)), "It asks for the same tool again later");
  const auto again = shared.view(at(50ms))->tool_change_request;
  require(again.has_value() && again->tool == 2 && again->generation != request->generation,
          "That is another request");
}

void withdrawn_request_is_not_confirmed() {
  HalShared shared;
  static_cast<void>(shared.exchange(asking(2), at(0ms)));
  const auto request = *shared.view(at(0ms))->tool_change_request;
  require(shared.confirm_tool_changed(request), "Confirmed");
  // Abort or E-stop: the request is gone before the control thread comes to the confirmation.
  require(!shared.exchange(asking({}), at(10ms)), "Withdrawn: tool-changed is not raised");
  require(!shared.exchange(asking(2), at(20ms)),
          "Nor for the request that follows, although it asks for the same tool");
  require(shared.view(at(20ms))->tool_change_request.has_value(),
          "The new request waits for its own confirmation");

  // The other order: withdrawn before the operator's confirmation arrives.
  HalShared late;
  static_cast<void>(late.exchange(asking(2), at(0ms)));
  const auto decided_on = *late.view(at(0ms))->tool_change_request;
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
  const auto first = *shared.view(at(0ms))->tool_change_request;
  require(shared.confirm_tool_changed(first), "Confirmed for tool 2");
  require(!shared.exchange(asking(3), at(10ms)),
          "LinuxCNC asks for tool 3 instead: the confirmation of tool 2 is not applied");
  const auto second = shared.view(at(10ms))->tool_change_request;
  require(second.has_value() && second->tool == 3 && second->generation != first.generation,
          "Request N + 1 is shown, unanswered");
  require(!shared.confirm_tool_changed(first), "The old request cannot be confirmed any more");
  require(!shared.confirm_tool_changed({second->generation, 2}) &&
              !shared.confirm_tool_changed({first.generation, 3}),
          "Generation and tool must both be those of the request");
  require(shared.confirm_tool_changed(*second) && shared.exchange(asking(3), at(20ms)),
          "Its own confirmation is applied");
}

void old_copy_is_not_handed_out() {
  HalShared shared;
  auto inputs = asking(4);
  inputs.spindle_rpm = 1200.0;
  static_cast<void>(shared.exchange(inputs, at(0ms)));
  require(shared.view(at(100ms)).has_value() && shared.view(at(100ms))->spindle_rpm == 1200.0,
          "A copy of max_age is still current");
  require(!shared.view(at(101ms)),
          "An older one is not handed out: the control thread has stalled");
  static_cast<void>(shared.exchange(inputs, at(500ms)));
  require(shared.view(at(500ms)).has_value(), "It is current again with the next reading");
}

// For the thread sanitizer: readers and a confirming thread beside the control thread.
void threads_share_it() {
  HalShared shared;
  std::atomic<bool> stop{};
  std::atomic<int> raised{};
  std::thread reader{[&]() -> void {
    while (!stop)
      static_cast<void>(shared.view(HalShared::Clock::now()));
  }};
  std::thread confirmer{[&]() -> void {
    while (!stop)
      if (const auto view = shared.view(HalShared::Clock::now());
          view && view->tool_change_request)
        static_cast<void>(shared.confirm_tool_changed(*view->tool_change_request));
  }};
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

} // namespace

auto main() -> int {
  try {
    handshake_completes();
    withdrawn_request_is_not_confirmed();
    replaced_request_is_not_confirmed();
    old_copy_is_not_handed_out();
    threads_share_it();
    std::cout << checks << " HAL exchange checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Check " << checks << " failed: " << error.what() << '\n';
    return 1;
  }
}
