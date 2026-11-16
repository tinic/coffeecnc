// The application with the real panel in its window, as LinuxCNC's DISPLAY program in
// simulation: start it with tests/sim/run.sh through tests/shell/under_xvfb.sh, never by hand.
// The test runs the application (run_application) on its main thread and drives it from a
// second one: it runs script in the page to read what the panel received and shows and to
// operate its controls, presses the mouse through GDK, and watches LinuxCNC through a status
// channel and HAL pins of its own.
//
// Output: one `check=<name> result=pass|fail detail=<text>` line per check, `fact=` lines with
// observations for the record, and `exit=<code>` last. Exit code 0 only when every check passed.
// Screenshots go to COFFEECNC_EVIDENCE_DIRECTORY.

#include "application.hpp"

#include <coffeecnc/linuxcnc/nml_status_reader.hpp>
#include <coffeecnc/core/text_file.hpp>
#include <coffeecnc/shell/test_hooks.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <signal.h> // kill: POSIX, not in <csignal>
#include <sys/types.h>
#include <unistd.h>

using namespace coffeecnc;
using namespace std::chrono_literals;

namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

int failures = 0;

auto word(std::string_view given) -> std::string {
  std::string text(given);
  std::ranges::replace(text, ' ', '_');
  std::ranges::replace(text, '\n', '_');
  return text.empty() ? std::string("-") : text;
}

void check(std::string_view name, bool passed, const std::string& detail = {}) {
  if (!passed)
    ++failures;
  std::cout << "check=" << name << " result=" << (passed ? "pass" : "fail")
            << " detail=" << word(detail) << '\n'
            << std::flush;
}

void fact(std::string_view name, const std::string& detail) {
  std::cout << "fact=" << name << ' ' << detail << '\n' << std::flush;
}

auto near(double value, double expected, double tolerance = 0.01) -> bool {
  return std::fabs(value - expected) <= tolerance;
}

// First line a shell command prints.
auto output_of(const std::string& command) -> std::string {
  std::string line;
  if (FILE* pipe = popen(command.c_str(), "r"); pipe != nullptr) {
    std::array<char, 512> buffer{};
    if (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
      line = buffer.data();
    static_cast<void>(pclose(pipe));
  }
  while (!line.empty() && (line.back() == '\n' || line.back() == ' '))
    line.pop_back();
  return line;
}

auto contents(const std::filesystem::path& file) -> std::string {
  return read_text_file(file.string(), std::size_t{64} << 20).value_or(std::string());
}

void write(const std::filesystem::path& file, std::string_view text) {
  std::ofstream out{file, std::ios::binary | std::ios::trunc};
  out << text;
}

// Process IDs of this process's descendants with this name. The kernel keeps 15 characters of
// a name.
auto descendants(std::string_view name) -> std::vector<pid_t> {
  name = name.substr(0, 15);
  const auto parent_of = [](pid_t pid) -> pid_t {
    std::ifstream in{std::format("/proc/{}/status", pid)};
    for (std::string line; std::getline(in, line);)
      if (line.starts_with("PPid:"))
        return static_cast<pid_t>(std::atoi(line.c_str() + 5));
    return 0;
  };
  std::vector<pid_t> found;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator("/proc", error)) {
    const auto text = entry.path().filename().string();
    if (text.empty() || !std::ranges::all_of(text, [](unsigned char c) -> bool {
          return c >= '0' && c <= '9';
        }))
      continue;
    const auto pid = static_cast<pid_t>(std::atoi(text.c_str()));
    std::string command = contents(entry.path() / "comm");
    while (!command.empty() && command.back() == '\n')
      command.pop_back();
    if (command != name)
      continue;
    for (pid_t ancestor = parent_of(pid); ancestor > 1; ancestor = parent_of(ancestor))
      if (ancestor == getpid()) {
        found.push_back(pid);
        break;
      }
  }
  return found;
}

// Runs in every document before the page's own scripts: records what the page is sent and
// what goes wrong in it, and helps finding its controls. A test's own; the panel has none of it.
constexpr std::string_view instrumentation = R"js((() => {
  const t = { errors: [], received: [], counts: {}, last: {}, firstSeq: null, lastSeq: null,
              seqBreaks: 0, sent: [], resultIds: [], lastStatusAt: null, maxGapMs: 0, gaps: [],
              previews: [], contextMismatches: [] };
  window.__t = t;
  const bridge = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.coffeecnc;
  t.handlerAtStart = !!(bridge && typeof bridge.postMessage === 'function');
  window.addEventListener('error', (e) => t.errors.push('error: ' + e.message + ' ' + (e.filename || '') + ':' + (e.lineno || '')));
  window.addEventListener('unhandledrejection', (e) => t.errors.push('rejection: ' + String((e.reason && e.reason.stack) || e.reason)));
  for (const level of ['error', 'warn']) {
    const original = console[level].bind(console);
    console[level] = (...parts) => { t.errors.push(level + ': ' + parts.map(String).join(' ')); original(...parts); };
  }
  document.addEventListener('securitypolicyviolation', (e) => t.errors.push('csp: ' + e.violatedDirective + ' ' + e.blockedURI));
  let inner;
  const record = (m) => {
    t.counts[m.type] = (t.counts[m.type] || 0) + 1;
    t.last[m.type] = m;
    // The order of the previews as they arrive, and every status whose context is not the
    // context of the newest preview: the page must never be told a context it has no preview
    // message for.
    if (m.type === 'preview') t.previews.push([m.context, m.state]);
    if (m.type === 'status' && t.last.preview && m.preview_context !== t.last.preview.context)
      t.contextMismatches.push([m.seq, m.preview_context, t.last.preview.context]);
    if (m.type === 'status') {
      if (t.firstSeq === null) t.firstSeq = m.seq;
      else if (m.seq !== t.lastSeq + 1) t.seqBreaks++;
      t.lastSeq = m.seq;
      const now = performance.now();
      if (t.lastStatusAt !== null) {
        const gap = now - t.lastStatusAt;
        t.maxGapMs = Math.max(t.maxGapMs, gap);
        // The statuses come every 50 ms; the panel takes 1000 ms without one for a stale link.
        if (gap > 150) t.gaps.push(Math.round(gap));
      }
      t.lastStatusAt = now;
    } else {
      t.received.push(m);
      if (m.type === 'result') t.resultIds.push(m.id);
    }
    inner(m);
  };
  Object.defineProperty(window, 'coffeecncReceive', {
    configurable: true,
    get() { return inner ? record : undefined; },
    set(value) { inner = value; },
  });
  if (t.handlerAtStart) {
    const post = bridge.postMessage.bind(bridge);
    try {
      bridge.postMessage = (text) => { try { t.sent.push(JSON.parse(text)); } catch (e) { t.sent.push(null); } post(text); };
    } catch (e) { t.sentUnavailable = String(e); }
  }
  t.all = (selector) => [...document.querySelectorAll(selector)];
  t.text = (element) => (element ? element.textContent.replace(/\s+/g, ' ').trim() : '');
  t.find = (selector, text) => t.all(selector).find((e) => text === undefined || t.text(e).includes(text) || (e.title || '').includes(text) || (e.getAttribute('aria-label') || '').includes(text)) || null;
  t.click = (selector, text) => {
    const element = t.find(selector, text);
    if (!element) return 'missing';
    if (element.disabled) return 'disabled: ' + (element.title || '');
    element.click();
    return 'ok';
  };
  t.point = (selector, text) => {
    const element = t.find(selector, text);
    if (!element) return null;
    element.scrollIntoView({ block: 'center' });
    const box = element.getBoundingClientRect();
    return [box.left + box.width / 2, box.top + box.height / 2];
  };
  // Whether `last` (the newest message of each type) shows work system `wcs` with a finished
  // preview of that very status. What a wait after a work-system change by another client
  // goes by: the status and preview from before the change are consistent with each other
  // too, and must not satisfy it.
  t.settledIn = (last, wcs) => !!(last.status && last.preview && last.status.offsets.wcs === wcs &&
    last.preview.state !== 'pending' && last.preview.context === last.status.preview_context);
  t.axis = (letter) => (t.last.status ? t.last.status.axes.find((a) => a.axis === letter) : null);
})();)js";

struct Failure : std::runtime_error {
  using std::runtime_error::runtime_error;
};
// Ends a scenario that is not the whole test.
struct ScenarioDone {};

// The test's side: the page through the hooks, LinuxCNC through its own channel and halcmd.
class Rig {
public:
  Rig(ShellTestHooks& test_hooks, MachineConfig machine, NmlStatusReader& observer)
      : hooks(test_hooks), config(std::move(machine)), view(observer) {}

  ShellTestHooks& hooks;
  MachineConfig config;
  NmlStatusReader& view;

  // What the script returns. Throws when it cannot run or throws itself.
  auto js(const std::string& body, std::chrono::milliseconds timeout = 10s) const -> Json {
    const auto result = hooks.evaluate(body, timeout);
    if (!result)
      throw Failure(std::format("script failed: {} in: {}", result.error(), body.substr(0, 120)));
    return Json::parse(*result);
  }
  // Until the expression is true in the page.
  auto wait_js(const std::string& expression, std::chrono::milliseconds timeout = 10s) const
      -> bool {
    for (const auto end = Clock::now() + timeout; Clock::now() < end;) {
      const auto result = hooks.evaluate("return !!(" + expression + ");", 5s);
      if (result && *result == "true")
        return true;
      std::this_thread::sleep_for(50ms);
    }
    return false;
  }
  auto wait(std::chrono::milliseconds timeout, const std::function<bool()>& done) const -> bool {
    for (const auto end = Clock::now() + timeout; Clock::now() < end;) {
      if (done())
        return true;
      std::this_thread::sleep_for(20ms);
    }
    return done();
  }
  auto click(const std::string& selector, const std::string& text) const -> std::string {
    return js(std::format("return __t.click({}, {});", Json(selector).dump(), Json(text).dump()))
        .get<std::string>();
  }
  // The middle of a control, in the view's coordinates.
  auto point(const std::string& selector, const std::string& text) const
      -> std::optional<std::pair<double, double>> {
    const auto at =
        js(std::format("return __t.point({}, {});", Json(selector).dump(), Json(text).dump()));
    if (!at.is_array())
      return {};
    return std::pair{at.at(0).get<double>(), at.at(1).get<double>()};
  }
  // A click the page takes as the operator's own (a user gesture), through GDK.
  auto real_click(const std::string& selector, const std::string& text) const -> bool {
    const auto at = point(selector, text);
    if (!at)
      return false;
    hooks.mouse_move(at->first, at->second);
    hooks.mouse_press(at->first, at->second);
    hooks.mouse_release(at->first, at->second);
    return true;
  }
  auto status() const -> NmlStatus { return view.status(); }
  // Joint position from HAL, in millimetres: not through NML and not through the application.
  auto hal_position(int joint) const -> double {
    return std::atof(output_of(std::format("halcmd getp joint.{}.pos-fb", joint)).c_str()) *
           config.millimetres_per_unit;
  }
  auto panel_status() const -> Json { return js("return __t.last.status || null;"); }
  auto errors() const -> std::string { return js("return __t.errors;").dump(); }
  void snapshot(const std::string& name) const {
    const auto directory = std::filesystem::path(COFFEECNC_EVIDENCE_DIRECTORY);
    std::filesystem::create_directories(directory);
    const auto file =
        directory / std::format("{}-{}.png", name,
                                config.machine_units == LengthUnit::inch ? "inch" : "mm");
    const auto taken = hooks.snapshot(file.string());
    check("snapshot_" + name, taken.has_value(), taken ? file.string() : taken.error());
  }
  // Waits for a page that has its hello and a live status.
  auto page_live(std::chrono::milliseconds timeout = 20s) const -> bool {
    return wait_js("window.__t && __t.last.hello && __t.last.status && "
                   "__t.last.status.link === 'live'",
                   timeout);
  }
};

// Where the page went without a status for longer than three periods, and for how long: said
// after each part of the test, for the part it happened in.
void status_gaps(const Rig& rig, std::string_view part) {
  const auto gaps = rig.hooks.evaluate(
      "const g = __t.gaps.slice(); __t.gaps.length = 0; return g;", 10s);
  fact("status_gaps", std::format("part={} gaps_ms={}", part, gaps ? *gaps : gaps.error()));
}

void first_page(const Rig& rig) {
  check("page_loads", rig.page_live(30s), rig.hooks.state().uri);
  const auto state = rig.hooks.state();
  check("page_url", state.uri == "coffeecnc://panel/index.html", state.uri);
  check("window_size", state.width == 1280 && state.height == 800,
        std::format("{}x{}", state.width, state.height));
  // The handler was there when the document's first script ran, before any script of the page.
  check("handler_before_page_scripts",
        state.handler_at_start == true && rig.js("return __t.handlerAtStart;") == true,
        std::format("shell_script={} test_script={}",
                    state.handler_at_start ? (*state.handler_at_start ? "yes" : "no") : "unknown",
                    rig.js("return __t.handlerAtStart;").dump()));
  check("one_document", state.commits == 1 && state.page != 0,
        std::format("commits={} page={}", state.commits, state.page));
  check("no_console_errors", rig.js("return __t.errors.length;") == 0, rig.errors());
  check("no_request_off_the_scheme", state.foreign_resources.empty(),
        state.foreign_resources.empty() ? std::format("resources={}", state.resources.size())
                                        : state.foreign_resources.front());
  for (const auto& resource : state.resources)
    fact("resource", resource);

  const auto hello = rig.js("return __t.last.hello;");
  fact("hello", hello.dump());
  check("hello_machine", hello.at("machine_name") == "CoffeeCNC-sim" &&
                             hello.at("simulated") == false && hello.at("protocol") == 1 &&
                             hello.at("axes") == Json::array({"x", "y", "z"}),
        hello.at("machine_name").dump());
  check("hello_units",
        hello.at("machine_units") ==
            (rig.config.machine_units == LengthUnit::inch ? "inch" : "mm"),
        hello.at("machine_units").dump());
  // The same millimetre values on the millimetre and the inch machine.
  check("hello_limits_in_mm",
        near(hello.at("max_jog_velocity").get<double>(), 1800) &&
            near(hello.at("axis_max_jog_velocity").at("z").get<double>(), 1800) &&
            hello.at("homing_required") == true &&
            near(hello.at("max_feed_override").get<double>(), 1.5) &&
            near(hello.at("min_spindle_override").get<double>(), 0.5) &&
            near(hello.at("max_spindle_override").get<double>(), 1.2),
        hello.dump());
  check("panel_is_not_the_simulator",
        rig.js("return __t.text(document.querySelector('footer.foot'));")
                .get<std::string>()
                .find("SIMULATOR") == std::string::npos,
        rig.js("return __t.text(document.querySelector('footer.foot'));").dump());

  const auto status = rig.panel_status();
  fact("first_status", status.dump());
  check("status_estop", status.at("machine") == "estop" && status.at("link") == "live" &&
                            status.at("axes").size() == 3,
        status.at("machine").dump());
  check("status_limits_in_mm",
        near(status.at("axes").at(0).at("min_limit").get<double>(), -250) &&
            near(status.at("axes").at(0).at("max_limit").get<double>(), 250) &&
            near(status.at("axes").at(2).at("min_limit").get<double>(), -50) &&
            near(status.at("axes").at(2).at("max_limit").get<double>(), 100),
        status.at("axes").dump());
  check("status_feedback_unavailable",
        status.at("spindle").at("actual_rpm").is_null() &&
            status.at("spindle").at("at_speed").is_null() && status.at("probe_input").is_null(),
        status.at("spindle").dump());
  // 20 statuses a second, numbered from 1 without a gap.
  const auto before = rig.js("return __t.counts.status;").get<int>();
  std::this_thread::sleep_for(1s);
  const auto after = rig.js("return __t.counts.status;").get<int>();
  check("status_rate", after - before >= 17 && after - before <= 23,
        std::format("per_second={}", after - before));
  check("status_numbering",
        rig.js("return __t.firstSeq === 1 && __t.seqBreaks === 0;") == true,
        rig.js("return [__t.firstSeq, __t.lastSeq, __t.seqBreaks];").dump());

  const auto offsets = rig.js("return __t.last.work_offsets;");
  bool rows = offsets.is_object() && offsets.at("systems").size() == 9;
  for (std::size_t i = 0; rows && i < 9; ++i)
    rows = offsets.at("systems").at(i).at("available") == (i == 0);
  check("work_offsets_only_active_available", rows, offsets.dump());
  const auto preview = rig.js("return __t.last.preview;");
  check("preview_unavailable_without_a_program",
        preview.is_object() && preview.at("state") == "unavailable" &&
            preview.at("context") == status.at("preview_context") &&
            preview.at("error").is_null() && preview.at("segments").empty(),
        preview.dump());
  const auto tools = rig.js("return __t.last.tool_table;");
  fact("tool_table", tools.dump());
  check("tool_table_loaded_in_mm",
        tools.at("tools").size() == 2 && tools.at("tools").at(0).at("number") == 1 &&
            near(tools.at("tools").at(0).at("length").get<double>(),
                 rig.config.machine_units == LengthUnit::inch ? 50.8 : 50) &&
            near(tools.at("tools").at(1).at("diameter").get<double>(),
                 rig.config.machine_units == LengthUnit::inch ? 3.175 : 3) &&
            !tools.at("tools").at(0).at("comment").get<std::string>().empty(),
        tools.dump());
  rig.snapshot("estop");
}

void power_on(const Rig& rig) {
  // Power is directly accessible without opening a menu. Reset does not power on.
  check("panel_power_outside_menu", rig.js(R"(
    const b = document.querySelector('button.power-control');
    return !!b && !b.closest('details') && !document.querySelector('details.state-menu').open;
  )") == true);
  check("panel_reset_estop", rig.click("button.power-control", "Reset E-stop") == "ok");
  check("linuxcnc_estop_reset",
        rig.wait(5s, [&]() -> bool { return rig.status().controller.machine == MachineState::off; }));
  check("panel_shows_off", rig.wait_js(R"((() => {
    const b = __t.find('button.power-control', 'Machine on');
    return __t.last.status.machine === 'off' && b && !b.disabled;
  })())"));
  check("panel_off_offers_only_power_on", rig.js(R"(
    return !__t.find('button', 'Reset E-stop') && !!__t.find('button.power-control', 'Machine on');
  )") == true);
  bool stayed_off = true;
  for (int sample = 0; sample < 25; ++sample) {
    stayed_off = stayed_off && rig.status().controller.machine == MachineState::off;
    std::this_thread::sleep_for(20ms);
  }
  check("linuxcnc_stays_off_until_separate_power_press", stayed_off);
  check("panel_machine_on", rig.click("button.power-control", "Machine on") == "ok");
  check("linuxcnc_machine_on",
        rig.wait(5s, [&]() -> bool { return rig.status().controller.machine == MachineState::on; }));
  check("panel_shows_on", rig.wait_js(R"((() => {
    const b = __t.find('button.power-control', 'Machine off');
    return __t.last.status.machine === 'on' && b && !b.disabled;
  })())"), rig.js("return __t.text(document.querySelector('.state-pill'));").dump());
}

void home(const Rig& rig) {
  check("panel_manual_tab", rig.click("nav.em-nav button", "Manual") == "ok");
  check("panel_home_all", rig.wait_js("!!__t.find('button', 'Home all axes')") &&
                              rig.click("button", "Home all axes") == "ok",
        rig.js("const b = __t.find('button', 'Home all axes'); return b ? b.title : null;").dump());
  const bool homed = rig.wait(40s, [&]() -> bool {
    const auto status = rig.status();
    return std::ranges::all_of(status.controller.axes,
                               [](const auto& axis) -> bool { return axis.homed; }) &&
           status.controller.in_position;
  });
  check("linuxcnc_homed", homed);
  check("panel_shows_homed",
        rig.wait_js("__t.last.status.axes.every((a) => a.homed && !a.homing) && "
                    "__t.all('.homestate.homed').length === 3"),
        rig.js("return __t.all('.homestate').map(__t.text);").dump());
  rig.snapshot("machine-on-idle");
}

// The panel's numbers are LinuxCNC's, once in millimetres: against the joint feedback pins,
// which are in machine units.
void positions_in_mm(const Rig& rig) {
  // The unit first, in a block of its own: an F word in the block that switches to G21 is
  // still read in the unit before it, and LinuxCNC converts it (F1200 becomes 30480 mm/min).
  rig.js(R"(
      const input = __t.find('input[aria-label="MDI command"]');
      input.value = 'G21';
      input.dispatchEvent(new Event('input', { bubbles: true }));
      await new Promise((r) => setTimeout(r, 50));
      return __t.click('section.mdi button[type="submit"]');)");
  check("panel_mdi_units", rig.wait_js("__t.last.status.program_units === 'mm' && "
                                       "__t.last.status.interp === 'idle' && "
                                       "__t.all('section.mdi .history .entry.accepted').length === 1"));
  check("panel_mdi_units_move", rig.js(R"(
      const input = __t.find('input[aria-label="MDI command"]');
      input.value = 'G90 G1 X25.4 Y-12.7 Z5 F1200';
      input.dispatchEvent(new Event('input', { bubbles: true }));
      await new Promise((r) => setTimeout(r, 50));
      return __t.click('section.mdi button[type="submit"]');)") == "ok");
  const bool arrived = rig.wait(15s, [&]() -> bool {
    return near(rig.hal_position(0), 25.4, 0.005) && near(rig.hal_position(1), -12.7, 0.005) &&
           near(rig.hal_position(2), 5, 0.005);
  });
  check("mdi_from_panel_moves_machine", arrived,
        std::format("hal_mm={},{},{}", rig.hal_position(0), rig.hal_position(1),
                    rig.hal_position(2)));
  check("panel_mdi_history_accepted",
        rig.wait_js("__t.all('section.mdi .history .entry.accepted').length >= 2"),
        rig.js("return __t.all('section.mdi .history .entry').map(__t.text);").dump());
  const bool at_rest =
      rig.wait_js("__t.last.status.interp === 'idle' && __t.last.status.motion.in_position", 5s);
  check("panel_shows_machine_at_rest", at_rest, rig.js("return __t.last.status.motion;").dump());
  const auto status = rig.panel_status();
  check("panel_machine_position_mm",
        near(status.at("axes").at(0).at("machine").get<double>(), 25.4, 0.005) &&
            near(status.at("axes").at(1).at("machine").get<double>(), -12.7, 0.005) &&
            near(status.at("axes").at(2).at("machine").get<double>(), 5, 0.005),
        status.at("axes").dump());
  check("panel_programmed_feed_mm_per_min",
        near(status.at("motion").at("programmed_feed").get<double>(), 1200) &&
            status.at("motion").at("feed_mode") == "per_minute" &&
            status.at("program_units") == "mm",
        status.at("motion").dump());
  check("panel_active_codes",
        std::ranges::count(status.at("active_codes"), Json("G21")) == 1 &&
            std::ranges::count(status.at("active_codes"), Json("G54")) == 1 &&
            std::ranges::count(status.at("active_codes"), Json("G90")) == 1,
        status.at("active_codes").dump());
  // A work offset: the DRO's work position and the offsets follow, in millimetres.
  const auto revision = rig.js("return __t.last.work_offsets.revision;").get<std::uint64_t>();
  const auto context = status.at("preview_context").get<std::uint64_t>();
  rig.js(R"(
      const input = __t.find('input[aria-label="MDI command"]');
      input.value = 'G10 L2 P1 X5.4 Y0 Z0';
      input.dispatchEvent(new Event('input', { bubbles: true }));
      await new Promise((r) => setTimeout(r, 50));
      return __t.click('section.mdi button[type="submit"]');)");
  check("work_offset_follows",
        rig.wait_js("Math.abs(__t.last.status.offsets.g5x[0] - 5.4) < 1e-6 && "
                    "Math.abs(__t.axis('x').work - 20) < 0.005 && "
                    "Math.abs(__t.last.work_offsets.systems[0].offset[0] - 5.4) < 1e-6"),
        rig.js("return [__t.last.status.offsets, __t.axis('x')];").dump());
  const auto after = rig.panel_status();
  check("offset_change_is_a_new_revision_and_context",
        rig.js("return __t.last.work_offsets.revision;").get<std::uint64_t>() > revision &&
            after.at("preview_context").get<std::uint64_t>() > context &&
            rig.js("return __t.last.preview.context === __t.last.status.preview_context;") == true,
        std::format("revision={}->{} context={}->{}", revision,
                    rig.js("return __t.last.work_offsets.revision;").dump(), context,
                    after.at("preview_context").dump()));
  // G55 becomes the active system: it is the available one, G54 is not any more.
  rig.js(R"(
      const input = __t.find('input[aria-label="MDI command"]');
      input.value = 'G55';
      input.dispatchEvent(new Event('input', { bubbles: true }));
      await new Promise((r) => setTimeout(r, 50));
      return __t.click('section.mdi button[type="submit"]');)");
  check("active_system_changes_with_g55",
        rig.wait_js("__t.last.status.offsets.wcs === 2 && "
                    "__t.last.work_offsets.systems[1].available === true && "
                    "__t.last.work_offsets.systems[0].available === false && "
                    "__t.last.work_offsets.systems[0].offset[0] === 0"),
        rig.js("return __t.last.work_offsets;").dump());
  rig.js(R"(
      const input = __t.find('input[aria-label="MDI command"]');
      input.value = 'G54 G10 L2 P1 X0 Y0 Z0';
      input.dispatchEvent(new Event('input', { bubbles: true }));
      await new Promise((r) => setTimeout(r, 50));
      return __t.click('section.mdi button[type="submit"]');)");
  const bool back = rig.wait_js("__t.last.status.offsets.wcs === 1 && "
                                "__t.last.status.offsets.g5x[0] === 0 && "
                                "__t.last.status.interp === 'idle' && "
                                "__t.last.work_offsets.systems[0].available === true && "
                                "__t.last.work_offsets.systems[1].available === false");
  check("back_to_g54", back, rig.js("return __t.last.status.offsets;").dump());
}

struct Jogging {
  std::pair<double, double> at;
  double start{};
};

// Presses the X+ jog control with the mouse and waits until the axis moves.
auto press_jog(const Rig& rig, const std::string& name) -> std::optional<Jogging> {
  const bool enabled =
      rig.wait_js("__t.last.status.interp === 'idle' && !!__t.find('button.jogbtn', 'Jog X+') && "
                  "!__t.find('button.jogbtn', 'Jog X+').disabled",
                  10s);
  const auto at = enabled ? rig.point("button.jogbtn", "Jog X+") : std::nullopt;
  check(name + "_control_enabled", at.has_value(),
        rig.js("return __t.all('button.jogbtn').map((b) => b.title);").dump());
  if (!at)
    return {};
  Jogging jog{*at, rig.hal_position(0)};
  rig.hooks.mouse_move(at->first, at->second);
  rig.hooks.mouse_press(at->first, at->second);
  const bool moving =
      rig.wait(3s, [&]() -> bool { return rig.hal_position(0) > jog.start + 0.5; });
  check(name + "_moves_while_held", moving,
        std::format("x_mm={}->{}", jog.start, rig.hal_position(0)));
  return jog;
}

// The axis has stopped: its position is the same over 300 ms. Returns when it was seen to.
auto stopped(const Rig& rig, std::chrono::milliseconds timeout) -> std::optional<double> {
  const auto asked = Clock::now();
  for (const auto end = asked + timeout; Clock::now() < end;) {
    const double first = rig.hal_position(0);
    const auto seen = Clock::now();
    std::this_thread::sleep_for(300ms);
    if (rig.hal_position(0) == first)
      return std::chrono::duration<double, std::milli>(seen - asked).count();
  }
  return {};
}

void jog_by_held_press(const Rig& rig) {
  rig.js("document.querySelector('input[aria-label=\"Jog speed\"]').value = 600; "
         "document.querySelector('input[aria-label=\"Jog speed\"]')"
         ".dispatchEvent(new Event('input', { bubbles: true }));");
  const auto jog = press_jog(rig, "jog");
  if (!jog)
    return;
  check("panel_shows_jog",
        rig.wait_js("__t.find('button.jogbtn', 'Jog X+').classList.contains('held') && "
                    "__t.last.status.motion.velocity > 0 && !__t.last.status.motion.in_position"),
        rig.js("return __t.last.status.motion;").dump());
  std::this_thread::sleep_for(700ms);
  rig.snapshot("jog-in-progress");
  const double held = rig.hal_position(0);
  check("jog_keeps_moving_while_held", held > jog->start + 4,
        std::format("x_mm={}->{}", jog->start, held));
  rig.hooks.mouse_release(jog->at.first, jog->at.second);
  const auto time = stopped(rig, 3s);
  const double end = rig.hal_position(0);
  check("jog_stops_on_release", time.has_value() && *time < 500 && end - held < 3,
        std::format("stopped_after_ms={} x_mm={} travelled_after_release_mm={}",
                    time.value_or(-1), end, end - held));
  check("panel_shows_new_position",
        rig.wait_js(std::format("Math.abs(__t.axis('x').machine - {}) < 0.01 && "
                                "__t.last.status.motion.in_position",
                                end)),
        rig.js("return __t.axis('x');").dump());
  rig.snapshot("jog-finished");
  check("no_console_errors_after_jog", rig.js("return __t.errors.length;") == 0, rig.errors());
}

// A reload while the control is held: the dead-man stops the axis, the next page starts clean,
// and nothing of the old page reaches it.
void reload_mid_jog(const Rig& rig) {
  const auto before = rig.hooks.state();
  const auto sent_before = rig.js("return __t.sent.length;").get<int>();
  const auto jog = press_jog(rig, "reload_jog");
  if (!jog)
    return;
  rig.hooks.reload();
  const auto asked = Clock::now();
  const auto time = stopped(rig, 3s);
  const double end = rig.hal_position(0);
  check("reload_stops_jog", time.has_value() && *time < 700,
        std::format("stopped_after_ms={} x_mm={}", time.value_or(-1), end));
  rig.hooks.mouse_release(jog->at.first, jog->at.second);
  const bool replaced = rig.wait(15s, [&]() -> bool {
    const auto state = rig.hooks.state();
    return state.commits == before.commits + 1 && state.page != before.page && state.identified;
  });
  const auto state = rig.hooks.state();
  check("reload_is_a_new_page", replaced && state.page > before.page,
        std::format("page={}->{} commits={}->{} after_ms={}", before.page, state.page,
                    before.commits, state.commits,
                    std::chrono::duration<double, std::milli>(Clock::now() - asked).count()));
  check("new_page_live", rig.page_live());
  check("new_page_handler_before_scripts", rig.hooks.state().handler_at_start == true);
  // A message for the old page that arrives now, as a late reply of the worker would.
  rig.hooks.post(before.page,
                 {R"({"type":"message","level":"error","text":"stale-marker","time":1})"});
  rig.hooks.post(state.page,
                 {R"({"type":"message","level":"info","text":"fresh-marker","time":2})"});
  check("new_page_gets_its_own_messages",
        rig.wait_js("__t.received.some((m) => m.text === 'fresh-marker')"));
  const auto record = rig.js(R"(return {
      stale: __t.received.some((m) => m.text === 'stale-marker'),
      firstSeq: __t.firstSeq, breaks: __t.seqBreaks,
      results: __t.resultIds, sent: __t.sent.map((m) => m && m.id),
      hello: __t.counts.hello, errors: __t.errors };)");
  check("old_page_messages_not_delivered", record.at("stale") == false &&
                                               rig.hooks.state().dropped > before.dropped,
        std::format("dropped={}->{}", before.dropped, rig.hooks.state().dropped));
  check("new_page_starts_clean",
        record.at("firstSeq") == 1 && record.at("breaks") == 0 && record.at("hello") == 1 &&
            record.at("errors").empty(),
        record.dump());
  // Every result the new page got answers a request it sent itself.
  bool own = true;
  for (const auto& id : record.at("results"))
    own = own && std::ranges::count(record.at("sent"), id) == 1;
  check("new_page_results_are_its_own", own && !record.at("results").empty(),
        std::format("old_page_sent={} new_page={}", sent_before, record.dump()));
  check("machine_still_on_after_reload", rig.status().controller.machine == MachineState::on);
  check("axis_did_not_move_after_reload", rig.hal_position(0) == end,
        std::format("x_mm={}->{}", end, rig.hal_position(0)));
  check("panel_manual_tab_again", rig.click("nav.em-nav button", "Manual") == "ok");
}

// The web process dies while the control is held: end of the page, the axis stops, and the
// shell loads the panel again.
void web_process_killed_mid_jog(const Rig& rig) {
  const auto before = rig.hooks.state();
  const auto jog = press_jog(rig, "crash_jog");
  if (!jog)
    return;
  const auto processes = descendants("WebKitWebProcess");
  check("web_process_found", processes.size() == 1, std::format("count={}", processes.size()));
  if (processes.empty()) {
    rig.hooks.mouse_release(jog->at.first, jog->at.second);
    return;
  }
  ::kill(processes.front(), SIGKILL);
  const auto time = stopped(rig, 3s);
  check("web_process_death_stops_jog", time.has_value() && *time < 700,
        std::format("stopped_after_ms={} x_mm={}", time.value_or(-1), rig.hal_position(0)));
  rig.hooks.mouse_release(jog->at.first, jog->at.second);
  const bool noticed =
      rig.wait(5s, [&]() -> bool { return rig.hooks.state().process_terminations == 1; });
  check("web_process_termination_seen", noticed);
  const bool replaced = rig.wait(20s, [&]() -> bool {
    const auto state = rig.hooks.state();
    return state.commits == before.commits + 1 && state.page > before.page && state.identified;
  });
  check("panel_loaded_again_after_crash", replaced && rig.page_live(),
        std::format("page={}->{}", before.page, rig.hooks.state().page));
  check("new_web_process", descendants("WebKitWebProcess").size() == 1 &&
                               descendants("WebKitWebProcess").front() != processes.front());
  check("machine_still_on_after_crash", rig.status().controller.machine == MachineState::on);
  check("page_after_crash_is_clean",
        rig.js("return __t.firstSeq === 1 && __t.errors.length === 0;") == true, rig.errors());
  check("panel_manual_tab_after_crash", rig.click("nav.em-nav button", "Manual") == "ok");
}

// What the scheme serves, asked from the page itself.
void scheme(const Rig& rig) {
  const auto answers = rig.js(R"(
      const ask = async (url) => {
        try {
          const response = await fetch(url);
          const body = await response.text();
          return { status: response.status, type: response.headers.get('content-type'), bytes: body.length, html: body.includes('<div id="app">') };
        } catch (error) { return { failed: String(error) }; }
      };
      const out = {};
      out.index = await ask('/index.html');
      out.missing = await ask('/assets/missing-file.js');
      out.missingPage = await ask('/no/such/page');
      out.directory = await ask('/assets/');
      out.traversal = await ask('/assets/..%2f..%2f..%2fetc/passwd');
      out.absolute = await ask('coffeecnc://panel//etc/passwd');
      out.otherHost = await ask('coffeecnc://other/index.html');
      out.http = await ask('http://127.0.0.1:9/');
      out.post = await (async () => { try { return (await fetch('/index.html', { method: 'POST', body: 'x' })).status; } catch (e) { return String(e); } })();
      return out;)", 20s);
  fact("scheme", answers.dump());
  check("scheme_serves_index", answers.at("index").value("status", 0) == 200 &&
                                   answers.at("index").value("type", "") == "text/html",
        answers.at("index").dump());
  check("scheme_missing_file_is_404_not_the_entry_page",
        answers.at("missing").value("status", 0) == 404 &&
            answers.at("missing").value("html", true) == false &&
            answers.at("missingPage").value("status", 0) == 404 &&
            answers.at("missingPage").value("html", true) == false,
        answers.at("missing").dump());
  check("scheme_lists_no_directory", answers.at("directory").value("status", 0) == 404,
        answers.at("directory").dump());
  check("scheme_refuses_traversal",
        answers.at("traversal").value("status", 200) >= 400 || answers.at("traversal").contains("failed"),
        answers.at("traversal").dump());
  check("scheme_refuses_absolute_path",
        answers.at("absolute").value("status", 200) >= 400 || answers.at("absolute").contains("failed"),
        answers.at("absolute").dump());
  check("scheme_has_one_host", answers.at("otherHost").contains("failed") ||
                                   answers.at("otherHost").value("status", 200) == 404,
        answers.at("otherHost").dump());
  check("network_is_refused", answers.at("http").contains("failed"), answers.at("http").dump());
  check("scheme_is_read_only", answers.at("post") != 200, answers.at("post").dump());

  // Navigation away from the panel is refused: the document stays.
  const auto before = rig.hooks.state();
  rig.js("setTimeout(() => { window.location.href = 'https://example.org/'; }, 0); return 1;");
  std::this_thread::sleep_for(700ms);
  rig.hooks.load("coffeecnc://other/index.html");
  std::this_thread::sleep_for(700ms);
  const auto after = rig.hooks.state();
  check("navigation_away_is_refused",
        after.commits == before.commits && after.page == before.page &&
            after.uri == "coffeecnc://panel/index.html" &&
            rig.js("return __t.last.status.link;") == "live",
        std::format("uri={} commits={}->{}", after.uri, before.commits, after.commits));
  check("no_new_window", rig.js("const w = window.open('coffeecnc://panel/index.html'); "
                                "return w === null || w === undefined;") == true);
  // The refusals above are reported in the page as policy violations: expected, and cleared.
  fact("expected_refusals", rig.errors());
  rig.js("__t.errors.length = 0;");
}

void operator_error(const Rig& rig, const std::filesystem::path& programs) {
  // A probe move that ends without contact: LinuxCNC reports it on its error channel a second
  // after the run command was answered.
  write(programs / "probe.ngc", "G21 G91\nG38.2 X3 F200\nG90\nM2\n");
  check("panel_program_tab", rig.click("nav.em-nav button", "Machine") == "ok");
  check("panel_open_browser", rig.wait_js("!!__t.find('button[aria-label=\"Open program\"]')") &&
                                  rig.click("button[aria-label=\"Open program\"]", "") == "ok");
  const bool listed = rig.wait_js("!!__t.find('.entries button.entry', 'probe.ngc')");
  check("browser_lists_program_directory", listed,
        rig.js("return [__t.text(document.querySelector('.path .where')), "
               "__t.all('.entries button.entry .name').map(__t.text)];")
            .dump());
  check("browser_opens_program", rig.click(".entries button.entry", "probe.ngc") == "ok");
  const bool loaded = rig.wait(5s, [&]() -> bool {
    return rig.status().program_file == (programs / "probe.ngc").string();
  });
  check("linuxcnc_program_loaded", loaded, rig.status().program_file);
  const bool ready =
      rig.wait_js("__t.last.status.program.file && !__t.find('button.main-action').disabled");
  check("panel_run_probe_program", ready && rig.click("button.main-action", "") == "ok",
        rig.js("const b = __t.find('button.main-action'); return [__t.text(b), b.title];").dump());
  const bool shown = rig.wait_js(
      "__t.received.some((m) => m.type === 'message' && m.level === 'error' && "
      "/contact/i.test(m.text)) && "
      "/contact/i.test(__t.text(document.querySelector('.message-area .error')))",
      15s);
  check("operator_error_shown_in_panel", shown,
        rig.js("return [__t.received.filter((m) => m.type === 'message'), "
               "__t.text(document.querySelector('.message-area .error'))];")
            .dump());
  rig.snapshot("operator-error");
  // The error stays in the bar until the operator dismisses it, and stays in the log after.
  check("panel_dismisses_error", rig.click(".message-area button.dismiss", "") == "ok");
  const bool dismissed = rig.wait_js("!document.querySelector('.message-area .error')");
  check("dismissed_error_leaves_the_bar", dismissed,
        rig.js("return __t.text(document.querySelector('.message-area'));").dump());
  rig.js("document.querySelector('details.state-menu').open = true;");
  check("panel_opens_message_log", rig.click("details.state-menu button", "Message log") == "ok");
  const bool logged = rig.wait_js("/contact/i.test(__t.text(document.querySelector('section.dialog')))");
  check("dismissed_error_stays_in_the_log", logged,
        rig.js("return __t.text(document.querySelector('section.dialog')).slice(0, 300);").dump());
  check("panel_closes_message_log", rig.click("section.dialog button[title=\"Close\"]", "") == "ok");
  rig.js("document.querySelector('details.state-menu').open = false;");
  const bool idle = rig.wait(5s, [&]() -> bool {
    return rig.status().controller.interp == InterpState::idle;
  });
  check("probe_program_ended", idle);
  check("no_console_errors_after_operator_error", rig.js("return __t.errors.length;") == 0,
        rig.errors());
}

// The table the panel shows is the one LinuxCNC has loaded, not the file.
void tool_table(const Rig& rig) {
  const bool inch = rig.config.machine_units == LengthUnit::inch;
  const auto before = rig.js("return __t.last.tool_table;");
  // Rows in no order, with a T0 row. Lengths in the file are machine units.
  write(rig.config.tool_table, inch ? "T9 P1 Z0.5 D0.25 ;nine\nT0 P4 ;none\nT4 P2 Z1 D0.5\n"
                                      "T2 P3 Z2 D0.125 ;eighth inch\n"
                                    : "T9 P1 Z12.7 D6.35 ;nine\nT0 P4 ;none\nT4 P2 Z25.4 D12.7\n"
                                      "T2 P3 Z50.8 D3.175 ;eighth inch\n");
  std::this_thread::sleep_for(1s);
  check("file_edit_without_load_changes_nothing",
        rig.js("return __t.last.tool_table;") == before, rig.js("return __t.last.tool_table;").dump());
  // LinuxCNC is told to load it, by another client than the panel.
  const auto loaded = output_of(
      "python3 -c 'import linuxcnc; c = linuxcnc.command(); c.load_tool_table(); "
      "print(c.wait_complete(5))'");
  fact("load_tool_table", "rcs=" + loaded);
  const bool followed = rig.wait_js(
      std::format("__t.last.tool_table.tools.length === 3 && "
                  "__t.last.tool_table.tools[0].number === 9 && __t.last.tool_table.revision > {}",
                  before.at("revision").dump()),
      5s);
  const auto table = rig.js("return __t.last.tool_table;");
  fact("loaded_tool_table", table.dump());
  check("loaded_table_is_shown", followed, table.dump());
  if (!followed)
    return;
  const auto& tools = table.at("tools");
  check("loaded_table_rows_in_mm",
        tools.at(1).at("number") == 4 && tools.at(2).at("number") == 2 &&
            near(tools.at(0).at("length").get<double>(), 12.7) &&
            near(tools.at(0).at("diameter").get<double>(), 6.35) &&
            near(tools.at(1).at("length").get<double>(), 25.4) &&
            near(tools.at(2).at("diameter").get<double>(), 3.175) &&
            tools.at(0).at("comment") == "nine" && tools.at(2).at("comment") == "eighth inch" &&
            tools.at(1).at("comment") == "",
        tools.dump());
  // A prepared tool is an index into that table: the panel is told the tool number.
  check("panel_manual_tab_for_tools", rig.click("nav.em-nav button", "Manual") == "ok");
  for (const int tool : {4, 2, 9}) {
    rig.js(std::format(R"(
      const input = __t.find('input[aria-label="MDI command"]');
      input.value = 'T{}';
      input.dispatchEvent(new Event('input', {{ bubbles: true }}));
      await new Promise((r) => setTimeout(r, 50));
      return __t.click('section.mdi button[type="submit"]');)", tool));
    check(std::format("prepared_tool_T{}", tool),
          rig.wait_js(std::format("__t.last.status.tool.prepared === {}", tool), 5s),
          std::format("panel={} linuxcnc_index={}",
                      rig.js("return __t.last.status.tool.prepared;").dump(),
                      rig.status().prepared_pocket.value_or(-1)));
  }
  check("loaded_tool_unchanged_by_prepare", rig.panel_status().at("tool").at("in_spindle") == 0);

  // A reload followed at once by another command, the table read all the while: the panel is
  // never shown a table that is neither the old nor the new one.
  rig.js("__t.received = __t.received.filter((m) => m.type !== 'tool_table'); return 1;");
  for (int round = 0; round < 6; ++round) {
    const bool three = round % 2 == 0;
    const auto revision = rig.js("return __t.last.tool_table.revision;");
    write(rig.config.tool_table, three ? "T9 P1 ;nine\nT4 P2\nT2 P3 ;two\n"
                                       : "T9 P1 ;nine\nT0 P4\nT4 P2\nT2 P3 ;two\nT5 P5\nT6 P6\n");
    const auto done = output_of(
        "python3 -c 'import linuxcnc; c = linuxcnc.command(); c.load_tool_table(); "
        "c.set_optional_stop(True); c.set_block_delete(True); print(c.wait_complete(5))'");
    const bool shown = rig.wait_js(
        std::format("__t.last.tool_table.tools.length === {} && "
                    "__t.last.tool_table.revision > {}", three ? 3 : 5, revision.dump()), 5s);
    check(std::format("reload_then_command_round_{}", round), shown,
          std::format("rcs={} tools={}", done,
                      rig.js("return __t.last.tool_table.tools.map((t) => t.number);").dump()));
  }
  const auto tables = rig.js(
      "return __t.received.filter((m) => m.type === 'tool_table').map((m) => m.tools.map((t) => t.number));");
  bool whole = true;
  for (const auto& numbers : tables)
    whole = whole && (numbers == Json::array({9, 4, 2}) ||
                      numbers == Json::array({9, 4, 2, 5, 6}));
  check("no_half_loaded_table_shown", whole && tables.size() >= 6, tables.dump());
  // Back to the table the program below uses.
  write(rig.config.tool_table, inch ? "T9 P1 Z0.5 D0.25 ;nine\nT0 P4 ;none\nT4 P2 Z1 D0.5\n"
                                      "T2 P3 Z2 D0.125 ;eighth inch\n"
                                    : "T9 P1 Z12.7 D6.35 ;nine\nT0 P4 ;none\nT4 P2 Z25.4 D12.7\n"
                                      "T2 P3 Z50.8 D3.175 ;eighth inch\n");
  static_cast<void>(output_of("python3 -c 'import linuxcnc; c = linuxcnc.command(); "
                              "c.load_tool_table(); print(c.wait_complete(5))'"));
  check("table_restored", rig.wait_js("__t.last.tool_table.tools.length === 3 && "
                                      "__t.last.tool_table.tools[2].comment === 'eighth inch'", 5s),
        rig.js("return __t.last.tool_table;").dump());
}

// The preview of part.ngc as the page receives it: computed by LinuxCNC's interpreter in the
// helper for the status's context, stale when a work offset changes, and computed again.
void preview_of_the_loaded_program(const Rig& rig) {
  const std::string current = "__t.last.preview && __t.last.preview.state === 'ready' && "
                              "__t.last.preview.context === __t.last.status.preview_context";
  const bool ready = rig.wait_js(current, 20s);
  const auto first = rig.js("return [__t.last.preview, __t.last.status.preview_context, "
                            "__t.last.status.offsets, __t.counts.preview];");
  const auto& preview = first.at(0);
  bool path = ready && preview.is_object() && preview.at("segments").size() == 3 &&
              preview.at("error").is_null() && preview.at("bounds").is_object();
  const std::array lines{3, 5, 7};
  for (std::size_t i = 0; path && i < lines.size(); ++i) {
    const auto& segment = preview.at("segments").at(i);
    path = segment.at("line") == lines.at(i) && segment.at("kind") == "feed" &&
           segment.at("tool") == (i == 0 ? 0 : 2) && segment.at("points").size() == 6;
  }
  // G1 X10 Y0, X14, X16 in G54: machine X is the work offset further.
  const double offset = first.at(2).at("g5x").at(0).get<double>();
  const auto x_of = [](const Json& of, std::size_t segment) -> double {
    return of.at("segments").at(segment).at("points").at(3).get<double>();
  };
  path = path && near(x_of(preview, 0), 10 + offset, 0.001) &&
         near(x_of(preview, 1), 14 + offset, 0.001) && near(x_of(preview, 2), 16 + offset, 0.001);
  check("preview_of_loaded_program", path,
        std::format("ready={} context={} status_context={} g5x_x={} preview={}", ready,
                    preview.is_object() ? preview.at("context").dump() : "-", first.at(1).dump(),
                    offset, preview.dump().substr(0, 700)));

  // Another client moves the work offset of G54 by 1.5 machine units. The block says G21: a
  // G10 value is in the interpreter's active unit, not the machine's.
  const double unit = rig.config.millimetres_per_unit;
  const auto mdi = [&](double x_offset_mm) -> bool {
    return std::system(std::format(
                           "python3 -c \"import linuxcnc; c = linuxcnc.command(); "
                           "c.mode(linuxcnc.MODE_MDI); c.wait_complete(); "
                           "c.mdi('G21 G10 L2 P1 X{:.6f}'); c.wait_complete()\"",
                           x_offset_mm)
                           .c_str()) == 0;
  };
  const auto before = rig.js("return __t.previews.length;").get<std::size_t>();
  const bool moved = mdi(offset + 1.5 * unit);
  const bool again = rig.wait_js(std::format("{} && __t.last.preview.context > {}", current,
                                             first.at(1).dump()), 20s);
  const auto second = rig.js("return [__t.last.preview, __t.last.status.preview_context, "
                             "__t.last.status.offsets.g5x[0], __t.previews, "
                             "__t.contextMismatches];");
  // From the page's own record of what arrived, in order: after the ready preview of the old
  // context comes a pending one of the next context (the old path is stale from then on, by
  // the protocol's rule), then the ready one of that context; and no status ever named a
  // context other than the newest preview's.
  const auto& trail = second.at(3);
  const auto old_context = first.at(1).get<std::uint64_t>();
  bool ordered = trail.size() >= before + 2 && before >= 1 &&
                 trail.at(before - 1) == Json::array({old_context, "ready"});
  if (ordered) {
    const auto next = trail.at(before).at(0).get<std::uint64_t>();
    ordered = next > old_context && trail.at(before) == Json::array({next, "pending"}) &&
              trail.back() == Json::array({second.at(1).get<std::uint64_t>(), "ready"});
    for (std::size_t i = before; ordered && i < trail.size(); ++i)
      ordered = trail.at(i).at(0).get<std::uint64_t>() > old_context;
  }
  check("preview_stale_and_recomputed_after_work_offset_change",
        moved && again && ordered && second.at(0).at("context") == second.at(1) &&
            near(second.at(2).get<double>(), offset + 1.5 * unit, 0.001) &&
            near(x_of(second.at(0), 0), 10 + offset + 1.5 * unit, 0.001),
        std::format("mdi={} ready_again={} ordered={} g5x_x={} x={} previews_since={}", moved,
                    again, ordered, second.at(2).dump(), again ? x_of(second.at(0), 0) : 0.0,
                    Json(std::vector<Json>(trail.begin() + static_cast<std::ptrdiff_t>(
                                                               std::min(before - 1, trail.size())),
                                           trail.end()))
                        .dump()));
  check("status_context_is_always_the_newest_previews", second.at(4).empty(),
        second.at(4).dump().substr(0, 300));
  // Back, for what follows.
  const bool back = mdi(offset) && rig.wait_js(std::format(
                        "Math.abs(__t.last.status.offsets.g5x[0] - {}) < 0.001 && {}", offset,
                        current), 20s);
  check("preview_follows_the_offset_back", back &&
            near(x_of(rig.js("return __t.last.preview;"), 0), 10 + offset, 0.001));
}

void program_with_tool_change(const Rig& rig, const std::filesystem::path& programs) {
  write(programs / "part.ngc",
        "G21 G90 G54\nF400\nG1 X10 Y0\nT2 M6\nG1 X14\nG4 P1.5\nG1 X16\nM2\n");
  check("panel_program_tab_again", rig.click("nav.em-nav button", "Machine") == "ok");
  const auto open = [&]() -> bool {
    return rig.click("button[aria-label=\"Open program\"]", "") == "ok" &&
           rig.wait_js("!!__t.find('.entries button.entry', 'part.ngc')") &&
           rig.click(".entries button.entry", "part.ngc") == "ok";
  };
  check("panel_opens_part_program", open());
  check("panel_gets_program_text",
        rig.wait_js("__t.last.program && __t.last.program.file.endsWith('/part.ngc') && "
                    "__t.last.status.program.file === __t.last.program.file && "
                    "__t.last.status.program.revision === __t.last.program.revision && "
                    "__t.last.program.lines.length === 8 && !__t.last.program.truncated"),
        rig.js("return [__t.last.program, __t.last.status.program];").dump());
  check("panel_code_view", rig.click(".view-switch button", "Code") == "ok");
  const bool text_shown = rig.wait_js("/G4 P1.5/.test(__t.text(document.querySelector('.views')))");
  check("panel_shows_program_text", text_shown,
        rig.js("return __t.text(document.querySelector('.views')).slice(0, 300);").dump());
  // Opening the same unchanged file again is a new load: a new revision, the text again, and
  // a new preview context.
  const auto first = rig.js("return [__t.last.program.revision, __t.last.status.preview_context, "
                            "__t.counts.program];");
  check("panel_opens_same_program_again", open());
  const bool again = rig.wait_js(std::format(
      "__t.last.program.revision > {} && __t.last.status.program.revision === "
      "__t.last.program.revision && __t.last.status.preview_context > {} && __t.counts.program > {}",
      first.at(0).dump(), first.at(1).dump(), first.at(2).dump()));
  check("reload_of_same_file_is_a_new_revision", again,
        std::format("before={} after={}", first.dump(),
                    rig.js("return [__t.last.program.revision, __t.last.status.preview_context, "
                           "__t.counts.program];")
                        .dump()));

  preview_of_the_loaded_program(rig);

  check("panel_runs_program",
        rig.wait_js("__t.last.status.interp === 'idle' && !__t.find('button.main-action').disabled") &&
            rig.click("button.main-action", "") == "ok",
        rig.js("const b = __t.find('button.main-action'); return [__t.text(b), b.title];").dump());
  check("program_runs", rig.wait_js("__t.last.status.mode === 'auto' && "
                                    "__t.last.status.interp !== 'idle' && "
                                    "__t.last.status.program.motion_line >= 3", 10s),
        rig.js("return [__t.last.status.mode, __t.last.status.interp, __t.last.status.program];")
            .dump());
  rig.snapshot("program-running");
  const bool prompted = rig.wait_js("__t.last.status.wait.kind === 'tool_change' && "
                                    "__t.last.status.wait.tool === 2 && "
                                    "!!document.querySelector('section.prompt[role=\"alertdialog\"]')",
                                    20s);
  check("tool_change_prompt", prompted,
        rig.js("return [__t.last.status.wait, __t.text(document.querySelector('section.prompt'))];")
            .dump());
  // The request as LinuxCNC's io raises it on its pins, and the machine waiting where it was.
  const auto asked = output_of("halcmd getp iocontrol.0.tool-change") + "/" +
                     output_of("halcmd getp iocontrol.0.tool-prep-number");
  check("linuxcnc_waits_for_tool",
        asked == "TRUE/2" && rig.status().controller.tool_in_spindle == 0 &&
            near(rig.hal_position(0), 10, 0.01),
        std::format("tool_change/number={} in_spindle={} x_mm={}", asked,
                    rig.status().controller.tool_in_spindle, rig.hal_position(0)));
  rig.snapshot("tool-change-prompt");
  fact("tool_change_prompt_buttons",
       rig.js("return __t.all('section.prompt button').map(__t.text);").dump());
  check("panel_confirms_tool_change",
        rig.js("const buttons = __t.all('section.prompt button').filter((b) => !b.disabled); "
               "if (buttons.length === 0) return 'missing'; "
               "buttons[buttons.length - 1].click(); return 'ok';") == "ok");
  check("tool_change_completes", rig.wait(10s, [&]() -> bool {
    return rig.status().controller.tool_in_spindle == 2 && !rig.status().controller.requested_tool;
  }), std::format("in_spindle={}", rig.status().controller.tool_in_spindle));
  check("prompt_closes", rig.wait_js("__t.last.status.wait.kind !== 'tool_change' && "
                                     "!document.querySelector('section.prompt') && "
                                     "__t.last.status.tool.in_spindle === 2"));
  check("panel_shows_dwell",
        rig.wait_js("__t.last.status.wait.kind === 'dwell' && __t.last.status.wait.seconds_left > 0",
                    10s),
        rig.js("return __t.last.status.wait;").dump());
  check("program_finishes", rig.wait(15s, [&]() -> bool {
    return rig.status().controller.interp == InterpState::idle && near(rig.hal_position(0), 16, 0.01);
  }), std::format("x_mm={}", rig.hal_position(0)));
  check("panel_shows_program_done",
        rig.wait_js("__t.last.status.interp === 'idle' && Math.abs(__t.axis('x').machine - 16) < 0.01"));
  check("no_console_errors_after_program", rig.js("return __t.errors.length;") == 0, rig.errors());
}

// The paired sample of the repository: EngiWorld task 17, its two programs as they are and
// its STEP model, in the real panel. What the preview is for each program and each active
// work system, and what of the model is on the screen before and after its placement is
// confirmed. Facts and screenshots; the panel's part of it belongs to the panel.
void paired_sample(const Rig& rig, const std::filesystem::path& programs) {
  const auto samples = std::filesystem::path(COFFEECNC_SAMPLES_DIRECTORY);
  const auto task = samples / "upstream/engiworld-cam/task/single-software-execution/cli/"
                              "freecad-path/task-17";
  const auto model = task / "init_file/two_sided_part.step";
  if (!std::filesystem::exists(task / "ground_truth/task-17_A.nc") ||
      !std::filesystem::exists(model)) {
    fact("paired_sample", "skipped=the-sample-corpus-is-not-in-this-checkout");
    return;
  }
  power_on(rig);
  for (const auto* name : {"task-17_A.nc", "task-17_B.nc"})
    std::filesystem::copy_file(task / "ground_truth" / name, programs / name,
                               std::filesystem::copy_options::overwrite_existing);
  const std::string current = "__t.last.preview && __t.last.preview.state !== 'pending' && "
                              "__t.last.preview.context === __t.last.status.preview_context";
  const auto open = [&](const std::string& name) -> bool {
    return rig.click("nav.em-nav button", "Machine") == "ok" &&
           rig.click("button[aria-label=\"Open program\"]", "") == "ok" &&
           rig.wait_js(std::format("!!__t.find('.entries button.entry', '{}')", name)) &&
           rig.click(".entries button.entry", name) == "ok" &&
           rig.wait_js(std::format("__t.last.status.program.file && "
                                   "__t.last.status.program.file.endsWith('/{}') && {}",
                                   name, current), 30s);
  };
  // Another client makes a work system active. Done when the panel has a status that shows
  // that system and a finished preview of that status's context. The preview of the system
  // before satisfies `current` too, until the first status after the change arrives (one
  // publisher period): without the system in the condition this returned on it.
  const auto select = [&](int wcs) -> bool {
    const bool sent =
        std::system(std::format("python3 -c \"import linuxcnc; c = linuxcnc.command(); "
                                "c.mode(linuxcnc.MODE_MDI); c.wait_complete(); "
                                "c.mdi('G{}'); c.wait_complete()\"",
                                53 + wcs)
                        .c_str()) == 0;
    return sent &&
           rig.wait_js(std::format("__t.settledIn(__t.last, {})", wcs), 30s);
  };
  // What a preview check judges with: the status the panel has now.
  const auto judged = [&](const Json& preview) -> std::string {
    return std::format("status_wcs={} status_context={} {}",
                       rig.js("return __t.last.status.offsets.wcs;").dump(),
                       rig.js("return __t.last.status.preview_context;").dump(),
                       preview.dump().substr(0, 300));
  };
  const auto of_status = [&](const Json& preview, int wcs) -> bool {
    return rig.js(std::format("return __t.last.status.offsets.wcs === {} && "
                              "__t.last.status.preview_context === {};",
                              wcs, preview.at("context").dump())) == true;
  };
  const auto report = [&](std::string_view sample, std::string_view active) -> Json {
    const auto preview = rig.js("return __t.last.preview;");
    const auto& segments = preview.at("segments");
    const auto points =
        std::accumulate(segments.begin(), segments.end(), std::size_t{0},
                        [](std::size_t sum, const Json& segment) -> std::size_t {
                          return sum + segment.at("points").size() / 3;
                        });
    fact("paired_sample_preview",
         std::format("sample={} active={} state={} error={} segments={} points={} bounds={}",
                     sample, active, preview.at("state").get<std::string>(),
                     word(preview.at("error").dump()), preview.at("segments").size(), points,
                     word(preview.at("bounds").dump())));
    return preview;
  };
  // The wait of select() on recorded states around a change from G54 to G55, in the order the
  // page gets them: only the last one, a G55 status with a finished preview of its context,
  // ends the wait. The first is what the wait returned on at 9892a42.
  const auto sequence = rig.js(R"(
      const state = (wcs, status_context, preview_context, preview_state) => ({
        status: { offsets: { wcs: wcs }, preview_context: status_context },
        preview: { context: preview_context, state: preview_state } });
      return [
        state(1, 36, 36, 'failed'),   // before the change: consistent, but G54
        state(2, 37, 36, 'failed'),   // G55 status, the preview still the G54 one
        state(2, 37, 37, 'pending'),  // the G55 preview is being computed
        state(2, 37, 37, 'ready'),    // settled
        { status: { offsets: { wcs: 2 }, preview_context: 37 } },  // no preview at all
      ].map((last) => __t.settledIn(last, 2));)");
  check("select_wait_refuses_the_state_from_before_the_change",
        sequence == Json::array({false, false, false, true, false}), sequence.dump());
  // The programs use tools 1, 2 and 3: the machine's table gets them, loaded by another
  // client as an operator's own editor would. Lengths in machine units.
  const double unit = rig.config.millimetres_per_unit;
  write(rig.config.tool_table,
        std::format("T1 P1 Z{:.4f} D{:.4f} ;10 mm end mill\nT2 P2 Z{:.4f} D{:.4f} ;5 mm drill\n"
                    "T3 P3 Z{:.4f} D{:.4f} ;8 mm end mill\n",
                    50 / unit, 10 / unit, 40 / unit, 5 / unit, 45 / unit, 8 / unit));
  const bool tools =
      std::system("python3 -c \"import linuxcnc; c = linuxcnc.command(); "
                  "c.load_tool_table(); c.wait_complete()\"") == 0 &&
      rig.wait_js("__t.last.tool_table.tools.length === 3 && "
                  "__t.last.tool_table.tools[2].number === 3", 20s);
  check("paired_sample_tools_loaded", tools, rig.js("return __t.last.tool_table;").dump());
  // Side A is G54 throughout.
  const bool opened_a = select(1) && open("task-17_A.nc");
  auto preview = report("task-17_A.nc", "G54");
  check("paired_sample_a_previewed_in_g54",
        opened_a && preview.at("state") == "ready" && preview.at("segments").size() > 20,
        preview.dump().substr(0, 300));
  rig.snapshot("task17a-preview");

  // Its STEP model through the panel's own dialog.
  const auto summary = [&]() -> std::string {
    return rig.js("return __t.text(document.querySelector('.model-summary'));").get<std::string>();
  };
  const auto scene = [&](std::string_view what) -> std::uint64_t {
    std::this_thread::sleep_for(700ms);
    const auto box = rig.js(R"(
        const canvas = document.querySelector('canvas.toolpath-canvas');
        if (!canvas) return null;
        const b = canvas.getBoundingClientRect();
        return [Math.ceil(b.left), Math.ceil(b.top), Math.floor(b.width), Math.floor(b.height)];)");
    if (!box.is_array())
      return 0;
    const auto drawn = rig.hooks.pixels(box.at(0).get<int>(), box.at(1).get<int>(),
                                        box.at(2).get<int>(), box.at(3).get<int>());
    fact("paired_sample_scene",
         std::format("at={} drawn_pixels={} of={} summary={}", what, drawn ? drawn->drawn : 0,
                     drawn ? drawn->total : 0, word(summary())));
    return drawn ? drawn->drawn : 0;
  };
  check("panel_split_view_for_sample", rig.click(".view-switch button", "Split") == "ok");
  const auto path_only = scene("toolpath_without_model");
  const bool dialog = rig.click("button.attach", "") == "ok" &&
                      rig.wait_js("!!__t.find('button', 'Choose STEP file')");
  const auto choosers = rig.hooks.state().file_choosers;
  rig.hooks.answer_file_chooser({model.string()});
  const bool chosen = dialog && rig.real_click("button", "Choose STEP file") &&
                      rig.wait(5s, [&]() -> bool {
                        return rig.hooks.state().file_choosers == choosers + 1;
                      });
  const bool imported = chosen &&
                        rig.wait_js("__t.text(document.querySelector('.identity'))"
                                    ".startsWith('two_sided_part.step')", 60s);
  fact("paired_sample_model",
       std::format("imported={} identity={} size={}", imported,
                   word(rig.js("return __t.text(document.querySelector('.identity'));").dump()),
                   word(rig.js("return __t.text(__t.find('p.hint', 'CAD size'));").dump())));
  check("paired_sample_model_imported", imported);
  check("panel_closes_sample_dialog",
        rig.click("section.dialog button[title=\"Close\"]", "") == "ok" &&
            rig.wait_js("!document.querySelector('section.dialog')"));
  const auto unplaced_text = summary();
  const auto unplaced = scene("model_attached_placement_unconfirmed");
  rig.snapshot("task17a-model-unplaced");
  // The operator confirms the placement: zero offsets, the CAD origin at the work origin.
  const bool placed = rig.click("button.attach", "") == "ok" &&
                      rig.wait_js("!!__t.find('button', 'Apply placement')") &&
                      rig.click("button", "Apply placement") == "ok" &&
                      rig.wait_js("!document.querySelector('section.dialog')");
  const auto placed_text = summary();
  const auto with_model = scene("placement_set");
  rig.snapshot("task17a-model-placed");
  // And hides the part again: what is left is the path.
  const bool hidden = rig.click("button.attach", "") == "ok" &&
                      rig.wait_js("!!document.querySelector('section.dialog .display-row input')") &&
                      rig.js("const box = document.querySelector('section.dialog .display-row "
                             "input[type=checkbox]'); if (box.checked) box.click(); "
                             "return !box.checked;") == true &&
                      rig.click("section.dialog button[title=\"Close\"]", "") == "ok" &&
                      rig.wait_js("!document.querySelector('section.dialog')");
  const auto without_model = scene("part_hidden_by_the_operator");
  check("paired_sample_part_is_drawn_over_the_path",
        placed && hidden && placed_text.find("Placement set by operator") != std::string::npos &&
            with_model > without_model + 500 && path_only > 100 &&
            without_model * 2 > path_only && without_model < path_only * 2,
        std::format("unplaced_summary={} placed_summary={} pixels_path_only={} unplaced={} "
                    "placed={} hidden={}",
                    word(unplaced_text), word(placed_text), path_only, unplaced, with_model,
                    without_model));
  check("no_console_errors_with_paired_sample", rig.js("return __t.errors.length;") == 0,
        rig.errors());

  // Side B cuts in G55 and returns to G54 in its last block.
  const bool opened_b = open("task-17_B.nc");
  preview = report("task-17_B.nc", "G54");
  const auto context_g54 = preview.at("context").get<std::uint64_t>();
  check("paired_sample_b_stops_at_g55_while_g54_is_active",
        opened_b && of_status(preview, 1) && preview.at("state") == "failed" &&
            preview.at("error").is_object() && preview.at("error").at("line") == 24 &&
            preview.at("error").at("text").get<std::string>().find("G55") != std::string::npos,
        judged(preview));
  rig.snapshot("task17b-preview-g54-active");
  const bool g55 = select(2);
  preview = report("task-17_B.nc", "G55");
  // Judged on the preview of a status that shows G55, never on the one from before the change.
  check("paired_sample_g55_preview_is_of_the_g55_status",
        g55 && of_status(preview, 2) && preview.at("context").get<std::uint64_t>() > context_g54,
        std::format("selected={} context_g54={} {}", g55, context_g54, judged(preview)));
  // The simulated mill has tools 1 and 2; the program changes to tool 3 in line 43.
  check("paired_sample_b_previewed_while_g55_is_active",
        g55 && of_status(preview, 2) && !preview.at("segments").empty() &&
            (preview.at("state") == "ready" ||
             (preview.at("error").is_object() && preview.at("error").at("line") == 43)),
        judged(preview));
  rig.snapshot("task17b-preview-g55-active");
  check("paired_sample_back_to_g54", select(1));
  // As this part of the test found things: probe.ngc loaded, the machine in E-stop.
  const bool back = open("probe.ngc") &&
                    std::system("python3 -c \"import linuxcnc; c = linuxcnc.command(); "
                                "c.state(linuxcnc.STATE_ESTOP); c.wait_complete()\"") == 0 &&
                    rig.wait(5s, [&]() -> bool {
                      return rig.status().controller.machine == MachineState::estop;
                    });
  check("paired_sample_leaves_the_machine_as_it_was", back);
}

// WebKitGTK as the panel needs it: module worker, WebAssembly, WebGL2, fonts, and the STEP
// chain through the panel's own file input.
void platform(const Rig& rig, const std::filesystem::path& ui) {
  std::string wasm;
  std::string worker;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(ui / "assets", error)) {
    const auto name = entry.path().filename().string();
    if (name.ends_with(".wasm"))
      wasm = name;
    else if (name.starts_with("step.worker"))
      worker = name;
  }
  fact("assets", std::format("wasm={} worker={}", wasm, worker));
  fact("versions", rig.js("return navigator.userAgent;").dump());

  const auto compiled = rig.js(std::format(R"(
      const url = '/assets/{}';
      const out = {{}};
      const head = await fetch(url);
      out.status = head.status;
      out.type = head.headers.get('content-type');
      out.bytes = (await head.arrayBuffer()).byteLength;
      try {{
        const module = await WebAssembly.compileStreaming(fetch(url));
        out.streaming = true;
        out.imports = WebAssembly.Module.imports(module).length;
        out.exports = WebAssembly.Module.exports(module).length;
      }} catch (e) {{ out.streaming = false; out.streamingError = String(e); }}
      out.secureContext = window.isSecureContext;
      out.origin = window.location.origin;
      return out;)", wasm), 60s);
  fact("wasm", compiled.dump());
  check("wasm_served_as_application_wasm",
        compiled.value("status", 0) == 200 && compiled.value("type", "") == "application/wasm",
        compiled.dump());
  check("wasm_streaming_compilation", compiled.value("streaming", false), compiled.dump());
  check("secure_context", compiled.value("secureContext", false), compiled.dump());
  status_gaps(rig, "platform.wasm_compile");

  const auto gl = rig.js(R"(
      const out = {};
      const canvas = document.createElement('canvas');
      const context = canvas.getContext('webgl2');
      out.webgl2 = !!context;
      if (context) {
        out.version = context.getParameter(context.VERSION);
        out.renderer = context.getParameter(context.RENDERER);
        out.vendor = context.getParameter(context.VENDOR);
        const info = context.getExtension('WEBGL_debug_renderer_info');
        if (info) {
          out.unmaskedRenderer = context.getParameter(info.UNMASKED_RENDERER_WEBGL);
          out.unmaskedVendor = context.getParameter(info.UNMASKED_VENDOR_WEBGL);
        }
        out.maxTexture = context.getParameter(context.MAX_TEXTURE_SIZE);
      }
      out.webgl1 = !!document.createElement('canvas').getContext('webgl');
      return out;)");
  fact("webgl", gl.dump());
  status_gaps(rig, "platform.webgl_context");
  // The graphics libraries the web process has loaded: WebKit hides the renderer's name from
  // the page.
  for (const auto pid : descendants("WebKitWebProcess"))
    fact("web_process_graphics_libraries",
         output_of(std::format("grep -oE '[^/ ]*(dri|gallium|LLVM|EGL|GLES|GLX|vulkan|nvidia)[^/ ]*\\.so[^ ]*' "
                               "/proc/{}/maps | sort -u | tr '\\n' ' '",
                               pid)));
  const auto graphics = rig.hooks.graphics_report();
  status_gaps(rig, "platform.graphics_report");
  fact("webkit_gpu", graphics ? word(*graphics).substr(0, 3000) : "unavailable: " + graphics.error());
  fact("graphics_environment",
       std::format("WEBKIT_DISABLE_DMABUF_RENDERER={} WEBKIT_DISABLE_COMPOSITING_MODE={} "
                   "LIBGL_ALWAYS_SOFTWARE={} GDK_BACKEND={}",
                   output_of("printenv WEBKIT_DISABLE_DMABUF_RENDERER"),
                   output_of("printenv WEBKIT_DISABLE_COMPOSITING_MODE"),
                   output_of("printenv LIBGL_ALWAYS_SOFTWARE"), output_of("printenv GDK_BACKEND")));
  check("webgl2_context", gl.value("webgl2", false), gl.dump());

  const auto fonts = rig.js(R"(
      await document.fonts.ready;
      const faces = [...document.fonts].map((f) => ({ family: f.family, weight: f.weight, status: f.status }));
      const used = getComputedStyle(document.body).fontFamily;
      const loads = [];
      for (const face of [...document.fonts]) { try { await face.load(); loads.push(face.family + ' ' + face.weight + ': ' + face.status); } catch (e) { loads.push(face.family + ': ' + String(e)); } }
      return { faces, used, loads, loaded: [...document.fonts].filter((f) => f.status === 'loaded').length };)", 30s);
  fact("fonts", fonts.dump());
  status_gaps(rig, "platform.fonts");
  check("bundled_fonts_load", fonts.at("faces").empty()
                                  ? false
                                  : fonts.at("loaded").get<std::size_t>() == fonts.at("faces").size(),
        fonts.at("loads").dump());

  // A module worker of the panel's own, started directly: it answers, so it has started and
  // its WebAssembly module was instantiated (an empty file is refused by the importer).
  const auto direct = rig.js(std::format(R"(
      return await new Promise((resolve) => {{
        const worker = new Worker('/assets/{}', {{ type: 'module' }});
        const timer = setTimeout(() => {{ worker.terminate(); resolve({{ timeout: true }}); }}, 40000);
        worker.onmessage = (event) => {{ clearTimeout(timer); worker.terminate(); resolve({{ reply: {{ ok: event.data.ok, error: event.data.error || null }} }}); }};
        worker.onerror = (event) => {{ clearTimeout(timer); resolve({{ error: String(event.message || event) }}); }};
        const bytes = new TextEncoder().encode('not a STEP file').buffer;
        worker.postMessage(bytes, [bytes]);
      }});)", worker), 60s);
  fact("module_worker", direct.dump());
  status_gaps(rig, "platform.module_worker");
  check("module_worker_starts_and_instantiates_wasm",
        direct.contains("reply") && direct.at("reply").at("ok") == false, direct.dump());

  // The whole chain through the panel: the file input's chooser, the worker, the import, the
  // WebGL2 layer. A program must be loaded (part.ngc is).
  const auto samples = std::filesystem::path(COFFEECNC_SAMPLES_DIRECTORY);
  const auto sm101 = samples / "upstream/instant-machinist/instant-machinist/parts/sm-101/part.step";
  check("step_sample_exists", std::filesystem::exists(sm101), sm101.string());
  check("panel_program_tab_for_model", rig.click("nav.em-nav button", "Machine") == "ok");
  check("panel_split_view", rig.click(".view-switch button", "Split") == "ok");
  check("panel_opens_model_dialog", rig.wait_js("!!__t.find('button.attach')") &&
                                        rig.click("button.attach", "") == "ok" &&
                                        rig.wait_js("!!__t.find('button', 'Choose STEP file')"),
        rig.js("return __t.all('button.attach').map(__t.text);").dump());
  check("sample_shortcut_hidden_in_native_panel",
        rig.js("return !__t.find('button', 'Load SM-101 sample');") == true);
  const auto import = [&](const std::filesystem::path& file,
                          std::chrono::milliseconds timeout) -> Json {
    // What the panel itself requires of an import: a live link and a loaded program. Seen
    // here, not assumed; and how long the page went without a status so far.
    const bool panel_ready =
        rig.wait_js("__t.last.status.link === 'live' && !!__t.last.status.program.file && "
                    "!document.querySelector('.connection.disconnected')",
                    5s);
    const auto choosers = rig.hooks.state().file_choosers;
    rig.hooks.answer_file_chooser(file.empty() ? std::vector<std::string>{}
                                               : std::vector<std::string>{file.string()});
    const bool clicked = rig.real_click("button", "Choose STEP file");
    const bool asked = rig.wait(5s, [&]() -> bool {
      return rig.hooks.state().file_choosers == choosers + 1;
    });
    Json result{{"clicked", clicked}, {"chooser", asked}, {"panel_ready", panel_ready}};
    result["longest_status_gap_ms"] = rig.js("return Math.round(__t.maxGapMs);");
    result["connection"] = rig.js("return __t.text(document.querySelector('.connection'));");
    // The import has ended, one way or the other, with this file's name or an error to show.
    if (!file.empty())
      result["finished"] = rig.wait_js(
          std::format("!__t.find('p.hint', 'Reading and tessellating') && "
                      "(__t.text(document.querySelector('.identity')).startsWith({}) || "
                      "!!__t.find('section.dialog p.error'))",
                      Json(file.filename().string()).dump()),
          timeout);
    else
      std::this_thread::sleep_for(500ms);
    result["identity"] = rig.js("return __t.text(document.querySelector('.identity'));");
    result["error"] = rig.js("return __t.text(__t.find('section.dialog p.error'));");
    result["size"] = rig.js("return __t.text(__t.find('p.hint', 'CAD size'));");
    result["summary"] = rig.js("return __t.text(document.querySelector('.model-summary'));");
    return result;
  };
  auto result = import(sm101, 60s);
  fact("step_sm101", result.dump());
  status_gaps(rig, "platform.import_sm101");
  // (a) Import only: the panel's file input and WebKit's chooser request, the module worker,
  // the WebAssembly importer, and the geometry it returns. Nothing here draws the part.
  check("step_import_panel_ready", result.at("panel_ready") == true, result.dump());
  check("step_import_file_chooser_opens",
        result.at("clicked") == true && result.at("chooser") == true, result.dump());
  check("step_import_sm101_finished", result.value("finished", false), result.dump());
  // WebKit's C locale can omit grouping separators; the geometric count is the assertion.
  auto geometry_text = result.at("identity").get<std::string>();
  std::erase(geometry_text, ',');
  check("step_import_sm101_geometry",
        geometry_text.find("1 parts") != std::string::npos &&
            geometry_text.find("1132 triangles") != std::string::npos,
        result.at("identity").dump());
  check("step_import_sm101_size", result.at("size").get<std::string>().find("120") != std::string::npos &&
                               result.at("size").get<std::string>().find("80") != std::string::npos &&
                               result.at("size").get<std::string>().find("20") != std::string::npos,
        result.at("size").dump());
  // (b) Overlay rendering: with a current toolpath preview (part.ngc is loaded and previewed)
  // the panel attaches the model and waits for its placement to be confirmed. No check here
  // sees a part drawn.
  check("step_overlay_waits_for_placement",
        result.at("summary").get<std::string>().find("Placement unconfirmed") != std::string::npos,
        result.at("summary").dump());
  // Cancelling the chooser leaves the attachment alone; choosing the same file again works.
  const auto cancelled = import({}, 1s);
  status_gaps(rig, "platform.import_cancel");
  check("step_import_chooser_cancel_keeps_attachment",
        cancelled.at("chooser") == true && cancelled.at("identity") == result.at("identity"),
        cancelled.dump());
  const auto same = import(sm101, 60s);
  status_gaps(rig, "platform.import_same");
  check("step_import_same_file_again", same.value("finished", false) &&
                                           same.at("panel_ready") == true &&
                                           same.at("chooser") == true &&
                                    same.at("identity") == result.at("identity") &&
                                    same.at("error") == "",
        same.dump());
  const auto block = import(samples / "upstream/camcraft/fixtures/step/ops_block.step", 60s);
  fact("step_ops_block", block.dump());
  status_gaps(rig, "platform.import_ops_block");
  check("step_import_centimetre_source_units",
        block.value("finished", false) &&
            block.at("identity").get<std::string>().starts_with("ops_block.step") &&
            block.at("size").get<std::string>().find("100") != std::string::npos &&
            block.at("size").get<std::string>().find("70") != std::string::npos,
        block.at("size").dump());
  const auto lego = import(samples / "upstream/camcraft/fixtures/step/lego_4x2_01.step", 60s);
  fact("step_lego", lego.dump());
  status_gaps(rig, "platform.import_lego");
  check("step_import_inch_source_units",
        lego.value("finished", false) &&
            lego.at("identity").get<std::string>().starts_with("lego_4x2_01.step") &&
            lego.at("size").get<std::string>().find("100.584") != std::string::npos &&
            lego.at("size").get<std::string>().find("49.784") != std::string::npos &&
            lego.at("size").get<std::string>().find("38.1") != std::string::npos,
        lego.at("size").dump());
  // Not a STEP file: refused, and the attachment before it stays.
  const auto invalid_file =
      std::filesystem::path(rig.config.directory) / "invalid.step";
  write(invalid_file, "ISO-10303-21;\nthis is not a model\n");
  const auto invalid = import(invalid_file, 60s);
  fact("step_invalid", invalid.dump());
  status_gaps(rig, "platform.import_invalid");
  check("step_import_invalid_file_keeps_attachment",
        invalid.value("finished", false) && invalid.at("error") != "" &&
            invalid.at("identity") == lego.at("identity"),
        invalid.dump());
  check("panel_closes_model_dialog", rig.click("section.dialog button[title=\"Close\"]", "") == "ok" &&
                                         rig.wait_js("!document.querySelector('section.dialog')"));
  // (b) again: the scene's WebGL2 canvas exists and its context is alive. That is the layer a
  // part would be drawn in; with the part hidden, nothing of a part is in it.
  const auto layer = rig.js(R"(
      const out = { canvases: [] };
      for (const canvas of document.querySelectorAll('canvas')) {
        const entry = { width: canvas.width, height: canvas.height, class: canvas.className };
        const gl = canvas.getContext('webgl2');
        entry.webgl2 = !!gl;
        if (gl) { entry.lost = gl.isContextLost(); entry.renderer = gl.getParameter(gl.RENDERER); }
        out.canvases.push(entry);
      }
      out.sceneError = __t.all('.scene [role="status"], .scene .error').map(__t.text);
      return out;)");
  fact("model_layer", layer.dump());
  bool rendering = false;
  for (const auto& canvas : layer.at("canvases"))
    rendering = rendering || (canvas.value("webgl2", false) && !canvas.value("lost", true));
  check("step_overlay_scene_context_alive", rendering, layer.dump());
  rig.snapshot("step-model");
  status_gaps(rig, "platform.snapshot");
  check("no_console_errors_after_step", rig.js("return __t.errors.length;") == 0, rig.errors());
  fact("page_errors_after_step", rig.errors());

  // Tool model rendering: the combined tool editor contains its own WebGL2
  // canvas of its own. The context must be alive and the model must be on the screen: pixels
  // of the window where the canvas is, read from a snapshot, that are not background.
  check("panel_tools_tab", rig.click("nav.em-nav button", "Tools") == "ok");
  check("panel_opens_tool_row", rig.wait_js("!!document.querySelector('.tool-row')") &&
                                   rig.click(".tool-row", "") == "ok");
  check("panel_opens_tool_model", rig.wait_js("!!document.querySelector('.tool-editor .preview')"));
  const bool canvas_shown = rig.wait_js(
      "!!document.querySelector('.model-canvas canvas') && "
      "document.querySelector('.model-canvas canvas').getBoundingClientRect().width > 50",
      5s);
  check("tool_model_canvas_shown", canvas_shown,
        rig.js("return __t.text(document.querySelector('section.dialog')).slice(0, 200);").dump());
  std::this_thread::sleep_for(700ms); // a frame or two for the first drawing
  const auto model = rig.js(R"(
      const canvas = document.querySelector('.model-canvas canvas');
      if (!canvas) return null;
      const gl = canvas.getContext('webgl2');
      const box = canvas.getBoundingClientRect();
      return { webgl2: !!gl, lost: gl ? gl.isContextLost() : null, width: canvas.width, height: canvas.height,
               box: [Math.ceil(box.left), Math.ceil(box.top), Math.floor(box.width), Math.floor(box.height)],
               error: __t.text(document.querySelector('.model-canvas .error')) };)");
  fact("tool_model_canvas", model.dump());
  check("tool_model_webgl2_context_alive",
        model.is_object() && model.value("webgl2", false) && model.value("lost", true) == false &&
            model.value("error", "x").empty(),
        model.dump());
  if (model.is_object()) {
    const auto& box = model.at("box");
    const auto drawn = rig.hooks.pixels(box.at(0).get<int>(), box.at(1).get<int>(),
                                        box.at(2).get<int>(), box.at(3).get<int>());
    // A tool envelope covers a good part of its canvas, and never all of it.
    check("tool_model_geometry_visible",
          drawn.has_value() && drawn->drawn * 100 > drawn->total * 3 &&
              drawn->drawn * 100 < drawn->total * 90,
          drawn ? std::format("drawn_pixels={}_of_{}", drawn->drawn, drawn->total) : drawn.error());
  }
  rig.snapshot("tool-model");
  status_gaps(rig, "platform.tool_model");
  check("panel_closes_tool_model", rig.click("section.dialog button[title=\"Close\"]", "") == "ok");
  check("panel_closes_combined_editor", rig.wait_js("!document.querySelector('.tool-fields')"));
  rig.snapshot("tools-tab");
  // The work offset table of the Tools tab: only the active system has numbers.
  const auto offsets_text = rig.js("return __t.text(document.querySelector('main.middle'));");
  check("tools_tab_shows_unavailable_work_systems",
        offsets_text.get<std::string>().find("Unavailable") != std::string::npos ||
            offsets_text.get<std::string>().find("unavailable") != std::string::npos,
        offsets_text.get<std::string>().substr(0, 600));
  check("no_console_errors_in_tools_tab", rig.js("return __t.errors.length;") == 0, rig.errors());

  // Another program: the attachment goes with the one it was bound to.
  check("panel_machine_tab_after_tools", rig.click("nav.em-nav button", "Machine") == "ok");
  const bool reopened = rig.click("button[aria-label=\"Open program\"]", "") == "ok" &&
                        rig.wait_js("!!__t.find('.entries button.entry', 'probe.ngc')") &&
                        rig.click(".entries button.entry", "probe.ngc") == "ok";
  check("panel_opens_other_program", reopened);
  const bool removed =
      rig.wait_js("/Part model removed because the program changed/.test("
                  "__t.text(document.querySelector('.model-summary')))");
  check("step_attachment_removed_with_program", removed,
        rig.js("return __t.text(document.querySelector('.model-summary'));").dump());

  // The STEP imports ran on this page: the panel takes a second without a status for a stale
  // link, so the longest time between two statuses says whether it ever did.
  // Serving the 7.6 MB WebAssembly file once kept the GTK thread, and with it every status,
  // for 350 ms in a debug build and 2.5 s under ThreadSanitizer: the file was read there.
  const auto gap = rig.js("return __t.maxGapMs;").get<double>();
  check("status_never_late_on_import_page", gap < 1000, std::format("longest_gap_ms={:.0f}", gap));

  // A new page starts without an attachment.
  const auto before = rig.hooks.state();
  rig.hooks.reload();
  const bool replaced = rig.wait(15s, [&]() -> bool {
    return rig.hooks.state().commits == before.commits + 1 && rig.hooks.state().identified;
  });
  check("reload_after_import", replaced && rig.page_live());
  check("panel_machine_tab_after_reload", rig.click("nav.em-nav button", "Machine") == "ok");
  const bool none = rig.wait_js("/Attach STEP model/.test(__t.text(document.querySelector('.model-summary')))");
  check("new_page_has_no_attachment", none,
        rig.js("return __t.text(document.querySelector('.model-summary'));").dump());
  check("no_console_errors_after_reload", rig.js("return __t.errors.length;") == 0, rig.errors());

  // A request of the old document that arrives after the new one has committed: held here,
  // released now. It carries the old document's identity, is not executed and gets no result.
  const auto now = rig.hooks.state();
  check("document_identities_differ",
        before.identity.size() == 32 && now.identity.size() == 32 &&
            before.identity != now.identity &&
            rig.js("return window.__coffeecncPage;") == now.identity,
        std::format("old={} new={}", before.identity, now.identity));
  const std::string late = R"({"id":2,"type":"set_machine","state":"estop"})";
  rig.hooks.deliver_from_page(before.identity + ":" + late);
  rig.hooks.deliver_from_page(late); // and one without any identity
  std::this_thread::sleep_for(700ms);
  const auto after_late = rig.hooks.state();
  check("late_request_of_old_document_is_dropped",
        after_late.rejected == now.rejected + 2 &&
            rig.status().controller.machine == MachineState::on &&
            rig.js("return __t.resultIds.includes(2);") == false,
        std::format("rejected={}->{} machine_on={} results={}", now.rejected, after_late.rejected,
                    rig.status().controller.machine == MachineState::on,
                    rig.js("return __t.resultIds;").dump()));
  // The same request with the present document's identity is the page's own: executed.
  rig.hooks.deliver_from_page(now.identity + ":" + late);
  const bool executed = rig.wait(5s, [&]() -> bool {
    return rig.status().controller.machine == MachineState::estop;
  });
  check("request_of_present_document_is_executed",
        executed && rig.wait_js("__t.resultIds.includes(2)"),
        rig.js("return __t.resultIds;").dump());
  // The page's own request numbering is now behind the session's: a fresh page for what follows.
  const auto last = rig.hooks.state();
  rig.hooks.reload();
  const bool fresh = rig.wait(15s, [&]() -> bool {
    return rig.hooks.state().commits == last.commits + 1 && rig.hooks.state().identified;
  });
  // With what the shell and the page say about that load, should it not come up: it did not
  // in one ThreadSanitizer run (3186b99), and that was not reproduced after the panel's files
  // were served as streams; its cause is not established.
  const bool up = fresh && rig.page_live();
  const auto shell = rig.hooks.state();
  const auto page = rig.hooks.evaluate(
      "return window.__t ? { counts: __t.counts, sent: __t.sent, results: __t.resultIds, "
      "errors: __t.errors, link: __t.last.status ? __t.last.status.link : null } : null;", 10s);
  check("fresh_page_after_injected_request", up,
        std::format("commits={}->{} page={} identified={} handler_at_start={} delivered={} "
                    "dropped={} rejected={} page_state={}",
                    last.commits, shell.commits, shell.page, shell.identified,
                    shell.handler_at_start.value_or(false), shell.delivered, shell.dropped,
                    shell.rejected, page ? *page : page.error()));
}

void estop(const Rig& rig) {
  power_on(rig);
  check("panel_estop", rig.click("button.estop", "") == "ok");
  check("linuxcnc_estop", rig.wait(5s, [&]() -> bool {
    return rig.status().controller.machine == MachineState::estop;
  }));
  check("panel_shows_estop", rig.wait_js("__t.last.status.machine === 'estop'"),
        rig.js("return __t.text(document.querySelector('.state-pill'));").dump());
  // probe.ngc was opened after part.ngc had run to its end and has not been run since. The
  // E-stop brings task's current line of part.ngc back (2.9.4): it is not a line of probe.ngc
  // and must not be shown beside it.
  std::this_thread::sleep_for(300ms);
  const auto raw = rig.status();
  const auto shown = rig.js("return __t.last.status.program;");
  check("lines_of_an_earlier_program_are_not_shown",
        shown.value("file", "").ends_with("/probe.ngc") && shown.value("current_line", -1) == 0 &&
            shown.value("motion_line", -1) == 0,
        std::format("task_current_line={} task_motion_line={} shown={}", raw.current_line,
                    raw.motion_line, shown.dump()));
  rig.snapshot("estop-after-run");
}

// The paths of this machine's tool models: [COFFEECNC] DATA_DIRECTORY/MACHINE_ID.
auto tool_model_file(const MachineConfig& config) -> std::filesystem::path {
  return std::filesystem::path(config.data_directory.value_or("")) /
         config.machine_id.value_or("") / "tool-models.json";
}

// Waits for a control and clicks it.
auto click_when_ready(const Rig& rig, const std::string& selector, const std::string& text)
    -> std::string {
  rig.wait_js(std::format("(() => {{ const e = __t.find({}, {}); return e && !e.disabled; }})()",
                          Json(selector).dump(), Json(text).dump()),
              10s);
  return rig.click(selector, text);
}

auto open_shape_templates(const Rig& rig) -> bool {
  return click_when_ready(rig, "summary.app-menu-toggle", "") == "ok" &&
         click_when_ready(rig, "button.settings", "Settings") == "ok" &&
         click_when_ready(rig, "button.templates", "Tool shape templates") == "ok" &&
         rig.wait_js("__t.find('section.dialog label.entry', 'Stick-out')", 10s);
}

// The positions, among the messages the page received, of the first snapshot at `revision`
// and of the result of the last command of `type` the page sent: [snapshot, result, outcome].
auto save_order(const Rig& rig, std::string_view type, int revision) -> Json {
  return rig.js(std::format(R"(
    const sent = __t.sent.filter((m) => m && m.type === '{}').pop();
    if (!sent) return [-1, -1, 'not sent', null];
    const snapshot = __t.received.findIndex((m) => m.type === 'tool_models' && m.revision === {});
    const result = __t.received.findIndex((m) => m.type === 'result' && m.id === sent.id);
    return [snapshot, result, result < 0 ? 'no result' : __t.received[result].outcome, sent];)",
                            type, revision));
}

// Run one of the tool-model scenario (tests/shell/tool_models_restart.sh): through the real
// editor a defaults save and an assignment save reach the disk, each shown by a snapshot
// before its accepted result. LinuxCNC's tool table and offsets stay what they were.
void tool_models_saved(const Rig& rig) {
  const auto file = tool_model_file(rig.config);
  const auto hello = rig.js("return __t.last.hello;");
  check("tool_models_hello",
        hello.value("machine_id", Json()) == "shell-mill" &&
            hello.value("tool_model_storage", Json()) == 1,
        std::format("machine_id={} tool_model_storage={}", hello.value("machine_id", Json()).dump(),
                    hello.value("tool_model_storage", Json()).dump()));
  const bool shown = rig.wait_js("__t.last.tool_models && __t.last.tool_table", 10s);
  const auto first = rig.js("return __t.last.tool_models || null;");
  check("tool_models_first_snapshot",
        shown && first.value("availability", "") == "available" && first.value("revision", -1) == 0 &&
            first.value("machine_id", "") == "shell-mill" && !std::filesystem::exists(file) &&
            rig.js("return __t.received.findIndex((m) => m.type === 'hello') < "
                   "__t.received.findIndex((m) => m.type === 'tool_models');") == true,
        first.dump().substr(0, 200));
  if (!shown)
    throw Failure("no tool model snapshot: nothing to save against");
  const auto table_file = contents(rig.config.tool_table);
  const auto tools = rig.js("return __t.last.tool_table.tools;");
  const auto offsets = rig.panel_status().at("offsets");

  check("tool_models_tools_tab", rig.click("nav.em-nav button", "Tools") == "ok");
  // The family defaults: the end mill's stick-out.
  check("tool_models_defaults_editor", open_shape_templates(rig));
  const auto typed = rig.js(R"(
    const input = __t.find('section.dialog label.entry', 'Stick-out').querySelector('input');
    const before = input.value;
    input.value = '36.5';
    input.dispatchEvent(new Event('input', { bubbles: true }));
    await new Promise((r) => setTimeout(r, 100));
    return [before, input.value];)");
  const auto saving = click_when_ready(rig, "section.dialog button.primary", "Save defaults");
  const bool saved = rig.wait_js("__t.last.tool_models.revision === 1 && "
                                 "!!document.querySelector('.settings-grid')",
                                 15s);
  auto order = save_order(rig, "set_tool_model_defaults", 1);
  check("tool_models_defaults_saved_by_the_editor",
        typed == Json::array({"35", "36.5"}) && saving == "ok" && saved &&
            order.at(0).get<int>() >= 0 && order.at(0).get<int>() < order.at(1).get<int>() &&
            order.at(2) == "accepted" && order.at(3).at("base_revision") == 0 &&
            order.at(3).at("machine_id") == "shell-mill",
        std::format("typed={} click={} snapshot_at={} result_at={} outcome={} errors={}",
                    typed.dump(), saving, order.at(0).dump(), order.at(1).dump(),
                    order.at(2).dump(), rig.errors()));
  auto on_disk = Json::parse(contents(file), nullptr, false);
  check("tool_models_defaults_on_disk",
        on_disk.is_object() && on_disk.value("revision", 0) == 1 &&
            on_disk.value("machine_id", "") == "shell-mill" &&
            on_disk.at("document").at("defaults").at("end-mill").at("stickout") == 36.5 &&
            on_disk.at("document").at("defaults").at("drill").at("tipAngle") == 118 &&
            on_disk.at("document").at("assignments").empty(),
        std::format("file={} bytes={}", file.string(), contents(file).size()));

  // A model for T1, as the tool table shows that row.
  check("tool_models_close_settings", rig.click("section.dialog button", "Cancel") == "ok");
  check("tool_models_assignment_editor",
        click_when_ready(rig, ".tool-row", "Edit tool 1:") == "ok" &&
            rig.wait_js("__t.find('section.dialog select')", 10s));
  const auto chosen = rig.js(R"(
    const select = __t.find('section.dialog select');
    select.value = 'touch-probe';
    select.dispatchEvent(new Event('change', { bubbles: true }));
    await new Promise((r) => setTimeout(r, 100));
    return select.value;)");
  const auto assigning =
      click_when_ready(rig, "section.dialog button.primary", "Save tool");
  const bool assigned =
      rig.wait_js("__t.last.tool_models.revision === 2 && !document.querySelector('.tool-fields')",
                  15s);
  order = save_order(rig, "set_tool_model_assignment", 2);
  const auto row = *std::ranges::find_if(
      tools, [](const Json& tool) -> bool { return tool.at("number") == 1; });
  const Json binding{{"number", 1},
                     {"pocket", row.at("pocket")},
                     {"diameter", row.at("diameter")},
                     {"comment", row.at("comment")}};
  check("tool_models_assignment_saved_by_the_editor",
        chosen == "touch-probe" && assigning == "ok" && assigned && order.at(0).get<int>() >= 0 &&
            order.at(0).get<int>() < order.at(1).get<int>() && order.at(2) == "accepted" &&
            order.at(3).at("base_revision") == 1 && order.at(3).at("tool") == binding,
        std::format("click={} snapshot_at={} result_at={} outcome={} sent={} errors={}", assigning,
                    order.at(0).dump(), order.at(1).dump(), order.at(2).dump(),
                    word(order.at(3).dump()), rig.errors()));
  on_disk = Json::parse(contents(file), nullptr, false);
  check("tool_models_assignment_on_disk",
        on_disk.is_object() && on_disk.value("revision", 0) == 2 &&
            on_disk.at("document").at("assignments") ==
                Json::array({{{"tool", binding}, {"family", "touch-probe"}, {"overrides", order.at(3).at("overrides")}}}) &&
            on_disk.at("document").at("defaults").at("end-mill").at("stickout") == 36.5,
        word(on_disk.is_object() ? on_disk.at("document").at("assignments").dump() : "unreadable"));
  check("tool_models_reopen_assignment", click_when_ready(rig, ".tool-row", "Edit tool 1:") == "ok");
  check("tool_models_assignment_shown",
        rig.wait_js("__t.find('.type-entry select').value === 'touch-probe'", 5s),
        rig.js("return __t.find('.type-entry select').value;").dump());
  check("tool_models_probe_ball_preview",
        rig.wait_js("__t.find('.model-canvas .ruby') && __t.find('.model-canvas canvas')", 10s),
        rig.errors());
  rig.snapshot("tool-models-saved");
  check("tool_models_linuxcnc_unchanged",
        contents(rig.config.tool_table) == table_file &&
            rig.js("return __t.last.tool_table.tools;") == tools &&
            rig.panel_status().at("offsets") == offsets && !table_file.empty(),
        std::format("tools={} offsets={}", word(tools.dump()), word(offsets.dump())));
  check("tool_models_no_console_errors", rig.js("return __t.errors.length;") == 0, rig.errors());
}

// Run two, LinuxCNC and the executable started again: the page is sent what run one saved, and
// the panel shows it, in the table and in the editor.
void tool_models_restored(const Rig& rig) {
  const auto file = tool_model_file(rig.config);
  const auto on_disk = Json::parse(contents(file), nullptr, false);
  check("tool_models_file_from_first_run",
        on_disk.is_object() && on_disk.value("revision", 0) == 2, file.string());
  if (!on_disk.is_object())
    throw Failure("the first run left no tool model file");
  const bool shown = rig.wait_js("__t.last.tool_models && __t.last.tool_models.revision === 2", 10s);
  const auto snapshot = rig.js("return __t.last.tool_models || null;");
  check("tool_models_restored_snapshot",
        shown && snapshot.value("availability", "") == "available" &&
            snapshot.value("machine_id", "") == "shell-mill" &&
            snapshot.at("document") == on_disk.at("document") &&
            rig.js("return __t.received.findIndex((m) => m.type === 'hello') < "
                   "__t.received.findIndex((m) => m.type === 'tool_models');") == true &&
            rig.js("return __t.sent.filter((m) => m && m.type.startsWith('set_tool_model')).length;") ==
                0,
        word(snapshot.dump().substr(0, 300)));
  check("tool_models_tools_tab", rig.click("nav.em-nav button", "Tools") == "ok");
  check("tool_models_restored_tool_row", click_when_ready(rig, ".tool-row", "Edit tool 1:") == "ok");
  check("tool_models_restored_assignment_shown",
        rig.wait_js("__t.find('.type-entry select').value === 'touch-probe'", 10s),
        rig.js("return __t.find('.type-entry select').value;").dump());
  check("tool_models_close_first_row", rig.click("section.dialog button", "Cancel") == "ok");
  check("tool_models_restored_second_row", click_when_ready(rig, ".tool-row", "Edit tool 2:") == "ok");
  check("tool_models_unassigned_row_shown",
        rig.wait_js("__t.find('.type-entry select').value === 'end-mill'", 5s));
  check("tool_models_close_second_row", rig.click("section.dialog button", "Cancel") == "ok");
  const bool editor = open_shape_templates(rig);
  const auto value = editor ? rig.js("return __t.find('section.dialog label.entry', 'Stick-out')"
                                     ".querySelector('input').value;")
                            : Json();
  check("tool_models_restored_defaults_in_editor", value == "36.5", value.dump());
  rig.snapshot("tool-models-restored");
  check("tool_models_editor_closes", rig.click("section.dialog button", "Cancel") == "ok");
  check("tool_models_file_untouched_by_restart",
        Json::parse(contents(file), nullptr, false) == on_disk &&
            !std::filesystem::exists(file.string() + ".tmp"));
  check("tool_models_no_console_errors", rig.js("return __t.errors.length;") == 0, rig.errors());
}

// What a run of this program does: everything, or one half of the tool-model scenario.
enum class Scenario : std::uint8_t { everything, tool_models_first_run, tool_models_second_run };

void drive(ShellTestHooks& hooks, const MachineConfig& config, Scenario scenario) {
  try {
    auto observer = NmlStatusReader::connect(config, nullptr);
    check("observer_connected", observer.has_value(), observer ? "status" : observer.error());
    if (!observer)
      throw Failure("no status channel for the test");
    const Rig rig{hooks, config, **observer};
    const auto programs = std::filesystem::path(config.program_prefix);
    const auto ui = std::filesystem::path(COFFEECNC_SOURCE_UI_DIRECTORY);
    // The panel in the window must be the bundle of the ui/ sources of this checkout: the
    // checker re-hashes the inputs and the bundle against the stamp the build wrote.
    const auto stamp = output_of(COFFEECNC_PANEL_STAMP_CHECK);
    const bool current = stamp.starts_with("panel=match ");
    check("panel_bundle_matches_sources", current,
          stamp + (current ? "" : " (run tests/shell/build_panel.sh, or ctest, which does)"));
    const auto commit =
        output_of(std::format("git -C {} rev-parse HEAD", ui.parent_path().string()));
    fact("panel", std::format("ui={} {} commit={}", ui.string(), stamp, commit));
    if (!current)
      throw Failure("the panel bundle is not the one of these sources: nothing was tested");
    const auto evidence = std::filesystem::path(COFFEECNC_EVIDENCE_DIRECTORY);
    std::filesystem::create_directories(evidence);
    write(evidence / "panel-stamp.txt", std::format("{}\ncommit={}\n", stamp, commit));
    first_page(rig);
    // And the application serves that bundle: the stamp it hands the page is the checked one.
    const auto served = rig.js("return await (await fetch('/build-stamp.json')).json();");
    check("served_panel_is_the_stamped_one",
          stamp.find("inputs=" + served.value("inputs_digest", "none") + " ") != std::string::npos &&
              stamp.find("assets=" + served.value("assets_digest", "none")) != std::string::npos,
          std::format("served_inputs={} served_assets={}", served.value("inputs_digest", "none"),
                      served.value("assets_digest", "none")));
    if (scenario != Scenario::everything) {
      if (scenario == Scenario::tool_models_first_run)
        tool_models_saved(rig);
      else
        tool_models_restored(rig);
      status_gaps(rig, "tool_models");
      throw ScenarioDone{};
    }
    power_on(rig);
    status_gaps(rig, "power_on");
    home(rig);
    status_gaps(rig, "home");
    positions_in_mm(rig);
    status_gaps(rig, "positions_in_mm");
    jog_by_held_press(rig);
    status_gaps(rig, "jog_by_held_press");
    reload_mid_jog(rig);
    status_gaps(rig, "reload_mid_jog");
    web_process_killed_mid_jog(rig);
    status_gaps(rig, "web_process_killed_mid_jog");
    scheme(rig);
    status_gaps(rig, "scheme");
    operator_error(rig, programs);
    status_gaps(rig, "operator_error");
    tool_table(rig);
    status_gaps(rig, "tool_table");
    program_with_tool_change(rig, programs);
    status_gaps(rig, "program_with_tool_change");
    platform(rig, ui);
    status_gaps(rig, "platform");
    paired_sample(rig, programs);
    status_gaps(rig, "platform");
    estop(rig);
    status_gaps(rig, "estop");
    const auto state = hooks.state();
    check("no_request_off_the_scheme_at_end", state.foreign_resources.empty(),
          state.foreign_resources.empty() ? std::format("resources={}", state.resources.size())
                                          : state.foreign_resources.front());
    fact("delivery", std::format("delivered={} dropped={} commits={}", state.delivered,
                                 state.dropped, state.commits));
  } catch (const ScenarioDone&) {
    // The scenario's checks are made; the window is closed as after every run.
    fact("scenario", "tool-models done");
  } catch (const std::exception& error) {
    check("no_exception", false, error.what());
  }
  // The operator closes the window: the application ends, and LinuxCNC with it. Work handed to
  // the GTK thread behind the close, and after it, is refused: its caller is told and does not
  // wait for ever.
  try {
    const auto asked = Clock::now();
    const auto fate = hooks.close_with_work_pending();
    const auto pending_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - asked).count();
    check("work_pending_at_close_is_dropped",
          fate == ShellTestHooks::WorkFate::dropped && pending_ms < 5000,
          std::format("fate={} after_ms={:.0f}",
                      fate == ShellTestHooks::WorkFate::dropped
                          ? "dropped"
                          : fate == ShellTestHooks::WorkFate::refused ? "refused" : "ran",
                      pending_ms));
    const auto again = Clock::now();
    bool refused = false;
    try {
      static_cast<void>(hooks.state());
    } catch (const ShellTestHooks::WindowClosed& closed) {
      refused = closed.fate == ShellTestHooks::WorkFate::refused;
    }
    const auto after_ms = std::chrono::duration<double, std::milli>(Clock::now() - again).count();
    check("work_after_close_is_refused", refused && after_ms < 1000,
          std::format("refused={} after_ms={:.0f}", refused, after_ms));
  } catch (const std::exception& error) {
    check("close", false, error.what());
  }
}

} // namespace

auto main(int argc, char** argv) -> int {
  std::string ini_file;
  for (int i = 1; i + 1 < argc; ++i)
    if (std::string_view(argv[i]) == "-ini")
      ini_file = argv[i + 1];
  // `tool-models`: the tool-model scenario; with `second-run` its second half, after the
  // restart (tests/shell/tool_models_restart.sh).
  auto scenario = Scenario::everything;
  for (int i = 1; i < argc; ++i)
    if (std::string_view(argv[i]) == "tool-models")
      scenario = Scenario::tool_models_first_run;
  for (int i = 1; i < argc; ++i)
    if (std::string_view(argv[i]) == "second-run" && scenario != Scenario::everything)
      scenario = Scenario::tool_models_second_run;
  int code = 1;
  try {
    if (ini_file.empty())
      throw std::runtime_error("usage: coffeecnc_shell_tests -ini <file> (see tests/sim/run.sh)");
    configure_environment();
    std::unique_ptr<ShellTestHooks> hooks;
    std::thread driver;
    Options options;
    options.ini_file = ini_file;
    options.ui_directory = COFFEECNC_SOURCE_UI_DIRECTORY;
    const int result = run_application(
        options, {.before_load = [&](PanelWindow& window, const MachineConfig& config) -> void {
          hooks = ShellTestHooks::install(window, std::string(instrumentation));
          driver = std::thread(
              [&hooks, config, scenario]() -> void { drive(*hooks, config, scenario); });
        }});
    check("application_started", driver.joinable(), std::format("exit={}", result));
    if (driver.joinable())
      driver.join();
    check("application_exit_code", result == 0, std::format("exit={}", result));
    // WebKit keeps a spare web process and its network process for as long as this process
    // lives; tests/sim/run.sh counts what is left once it has exited.
    fact("web_processes_at_return",
         std::format("web={} network={}", descendants("WebKitWebProcess").size(),
                     descendants("WebKitNetworkProcess").size()));
    code = failures == 0 ? 0 : 1;
  } catch (const std::exception& error) {
    check("no_exception", false, error.what());
  }
  // LinuxCNC's start script ends with its own cleanup: this line carries the exit code.
  std::cout << "exit=" << code << '\n' << std::flush;
  return code;
}
