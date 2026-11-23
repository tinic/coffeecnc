// The diagnostics view's app layer (#251): the wire text of a `diagnostics` message and the feed
// that reads only while a page watches. No HAL: the readings are the test's own.

#include <coffeecnc/app/diagnostics_feed.hpp>
#include <coffeecnc/app/messages.hpp>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace coffeecnc;
using namespace std::chrono_literals;
using Json = nlohmann::json;

namespace {
int checks = 0;
void require(bool condition, const std::string& message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

auto sample() -> Diagnostics {
  Diagnostics diagnostics;
  diagnostics.inputs = {
      {.input = {.role = DiagnosticRole::min_limit, .joint = 0, .name = "joint.0.neg-lim-sw-in"},
       .state = DiagnosticState::unwired,
       .value = false},
      {.input = {.role = DiagnosticRole::estop_chain, .name = "iocontrol.0.emc-enable-in"},
       .state = DiagnosticState::live,
       .value = true},
      {.input = {.role = DiagnosticRole::spindle_speed_in, .name = "spindle.0.speed-in"},
       .state = DiagnosticState::live,
       .value = std::numeric_limits<double>::quiet_NaN()},
      {.input = {.role = DiagnosticRole::configured, .label = "Setter contact",
                 .name = "setter-tripped"},
       .state = DiagnosticState::undriven,
       .value = true,
       .signal = true},
      {.input = {.role = DiagnosticRole::configured, .label = "VFD errors",
                 .name = "vfd.modbus-errors"},
       .state = DiagnosticState::live,
       .value = 3.0},
      {.input = {.role = DiagnosticRole::configured, .label = "Gone", .name = "no-such-signal"}},
  };
  diagnostics.tools = {{HalTool::halshow, true, false},
                       {HalTool::halmeter, true, true},
                       {HalTool::halscope, false, false}};
  return diagnostics;
}

void wire_text() {
  const auto encoded = encode_diagnostics(sample());
  require(encoded.has_value(), "A diagnostics message encodes, with a NaN among its values");
  const auto message = Json::parse(*encoded);
  const auto expected = Json::parse(R"({"type":"diagnostics","inputs":[
    {"role":"min_limit","joint":0,"name":"joint.0.neg-lim-sw-in","kind":"pin","state":"unwired","value":false},
    {"role":"estop_chain","name":"iocontrol.0.emc-enable-in","kind":"pin","state":"live","value":true},
    {"role":"spindle_speed_in","name":"spindle.0.speed-in","kind":"pin","state":"live","value":null},
    {"role":"configured","label":"Setter contact","name":"setter-tripped","kind":"signal","state":"undriven","value":true},
    {"role":"configured","label":"VFD errors","name":"vfd.modbus-errors","kind":"pin","state":"live","value":3.0},
    {"role":"configured","label":"Gone","name":"no-such-signal","kind":"pin","state":"missing","value":null}],
    "tools":[{"tool":"halshow","available":true,"open":false},
             {"tool":"halmeter","available":true,"open":true},
             {"tool":"halscope","available":false,"open":false}]})");
  require(message == expected, "Every row, its state and value, and the HAL tools: " + *encoded);

  Hello hello;
  hello.axes = {Axis::x};
  hello.axis_max_jog_velocity = {1000.0, std::nullopt, std::nullopt};
  const auto without = encode_hello(hello);
  hello.diagnostics = true;
  const auto with = encode_hello(hello);
  require(without && !Json::parse(*without).contains("diagnostics") && with &&
              Json::parse(*with).at("diagnostics") == true,
          "hello.diagnostics is sent only when served");
}

// The test's sink: every message posted, with its page.
struct Posted {
  std::mutex mutex;
  std::vector<std::pair<std::uint64_t, std::string>> messages;
  auto count() -> std::size_t {
    const std::scoped_lock lock{mutex};
    return messages.size();
  }
  auto first() -> std::string {
    const std::scoped_lock lock{mutex};
    return messages.empty() ? std::string("{}") : messages.front().second;
  }
  auto last_page() -> std::uint64_t {
    const std::scoped_lock lock{mutex};
    return messages.empty() ? 0 : messages.back().first;
  }
};

auto wait_for(const std::function<bool()>& done, std::chrono::milliseconds timeout = 5s) -> bool {
  for (const auto end = std::chrono::steady_clock::now() + timeout;
       std::chrono::steady_clock::now() < end;) {
    if (done())
      return true;
    std::this_thread::sleep_for(1ms);
  }
  return done();
}

void feed_reads_only_while_watched() {
  Posted posted;
  std::atomic<int> reads{0};
  std::atomic<int> begins{0};
  std::vector<HalTool> launched;
  constexpr auto period = 20ms;
  {
    DiagnosticsFeed feed{
        {.begin = [&]() -> void { ++begins; },
         .read = [&]() -> Diagnostics {
           ++reads;
           return sample();
         },
         .launch = [&](HalTool tool) -> std::optional<std::string> {
           launched.push_back(tool);
           return std::nullopt;
         }},
        [&](PageToken page, std::vector<std::string> messages) -> void {
          const std::scoped_lock lock{posted.mutex};
          for (auto& message : messages)
            posted.messages.emplace_back(page.value, std::move(message));
        },
        period};
    std::this_thread::sleep_for(5 * period);
    require(reads == 0 && posted.count() == 0, "Nothing is read while no page watches");

    feed.watch(PageToken{1}, true);
    require(wait_for([&]() -> bool { return posted.count() >= 5; }), "Readings arrive");
    require(begins == 1, "The wiring is asked once for a watch");
    require(posted.last_page() == 1 && Json::parse(posted.first()).at("type") == "diagnostics",
            "A `diagnostics` message for the watching page");

    // About one reading per period: neither a busy loop nor a stall.
    const auto before = posted.count();
    std::this_thread::sleep_for(10 * period);
    const auto during = posted.count() - before;
    require(during >= 3 && during <= 13,
            "About ten readings in ten periods: " + std::to_string(during));

    feed.watch(PageToken{1}, false);
    const auto after_off = posted.count();
    std::this_thread::sleep_for(5 * period);
    require(posted.count() == after_off, "Nothing is posted after the watch has ended");

    feed.watch(PageToken{2}, true);
    require(wait_for([&]() -> bool { return posted.last_page() == 2; }) && begins == 2,
            "A new watch asks for the wiring again and posts to its page");
    feed.watch(PageToken{3}, true);
    require(wait_for([&]() -> bool { return posted.last_page() == 3; }),
            "A watch of a later page replaces the one before");
    feed.watch(PageToken{2}, false);
    const auto replaced = posted.count();
    require(wait_for([&]() -> bool { return posted.count() > replaced; }),
            "Ending a watch that was replaced does not end the current one");
    feed.page_ended(PageToken{3});
    const auto after_end = posted.count();
    std::this_thread::sleep_for(5 * period);
    require(posted.count() == after_end, "The end of the page ends its watch");

    require(!feed.launch(HalTool::halmeter) && launched == std::vector<HalTool>{HalTool::halmeter},
            "Launch goes to the source");
    feed.watch(PageToken{4}, true);
    require(wait_for([&]() -> bool { return posted.last_page() == 4; }), "Watched at close");
  }
  // The destructor joined the thread while a page watched.
  const auto at_close = posted.count();
  std::this_thread::sleep_for(3 * period);
  require(posted.count() == at_close, "Nothing after the feed is gone");

  DiagnosticsFeed bare{{}, [](PageToken, std::vector<std::string>) -> void {}, period};
  require(bare.launch(HalTool::halscope).has_value(), "No launcher: a refusal, never a start");
}

} // namespace

auto main() -> int {
  try {
    wire_text();
    feed_reads_only_while_watched();
    std::cout << checks << " diagnostics checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Check " << checks << " failed: " << error.what() << '\n';
    return 1;
  }
}
