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
#include <coffeecnc/shell/test_hooks.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
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
  std::ifstream in{file, std::ios::binary};
  return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
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
              seqBreaks: 0, sent: [], resultIds: [] };
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
    if (m.type === 'status') {
      if (t.firstSeq === null) t.firstSeq = m.seq;
      else if (m.seq !== t.lastSeq + 1) t.seqBreaks++;
      t.lastSeq = m.seq;
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
  t.axis = (letter) => (t.last.status ? t.last.status.axes.find((a) => a.axis === letter) : null);
})();)js";

struct Failure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

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
  check("preview_unavailable_with_reason",
        preview.is_object() && preview.at("state") == "unavailable" &&
            preview.at("context") == status.at("preview_context") &&
            preview.at("error").is_object(),
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
  // The state menu's own buttons.
  rig.js("document.querySelector('details.state-menu').open = true;");
  check("panel_reset_estop", rig.click("details.state-menu button", "Reset E-stop") == "ok");
  check("linuxcnc_estop_reset",
        rig.wait(5s, [&]() -> bool { return rig.status().controller.machine == MachineState::off; }));
  check("panel_shows_off", rig.wait_js("__t.last.status.machine === 'off' && "
                                       "!!__t.find('details.state-menu button', 'Machine on')"));
  check("panel_machine_on", rig.click("details.state-menu button", "Machine on") == "ok");
  check("linuxcnc_machine_on",
        rig.wait(5s, [&]() -> bool { return rig.status().controller.machine == MachineState::on; }));
  check("panel_shows_on", rig.wait_js("__t.last.status.machine === 'on' && "
                                      "!!__t.find('details.state-menu button', 'Machine off')"),
        rig.js("return __t.text(document.querySelector('.state-pill'));").dump());
  rig.js("document.querySelector('details.state-menu').open = false;");
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
  rig.wait_js("__t.last.status.interp === 'idle' && __t.last.status.motion.in_position", 5s);
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
  check("back_to_g54", rig.wait_js("__t.last.status.offsets.wcs === 2 || true") &&
                           rig.wait_js("__t.last.status.offsets.wcs === 1 && "
                                       "__t.last.status.offsets.g5x[0] === 0 && "
                                       "__t.last.status.interp === 'idle'"));
}

struct Jogging {
  std::pair<double, double> at;
  double start{};
};

// Presses the X+ jog control with the mouse and waits until the axis moves.
auto press_jog(const Rig& rig, const std::string& name) -> std::optional<Jogging> {
  rig.wait_js("__t.last.status.interp === 'idle' && !!__t.find('button.jogbtn', 'Jog X+') && "
              "!__t.find('button.jogbtn', 'Jog X+').disabled",
              10s);
  const auto at = rig.point("button.jogbtn", "Jog X+");
  check(name + "_control_found", at.has_value(),
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
      "/contact/i.test(__t.text(document.querySelector('.jobbar .error')))",
      15s);
  check("operator_error_shown_in_panel", shown,
        rig.js("return [__t.received.filter((m) => m.type === 'message'), "
               "__t.text(document.querySelector('.jobbar .error'))];")
            .dump());
  rig.snapshot("operator-error");
  rig.wait(5s, [&]() -> bool { return rig.status().controller.interp == InterpState::idle; });
  rig.js("__t.errors.length = 0;");
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
      "__t.last.tool_table.tools.length === 4 && __t.last.tool_table.tools[0].number === 9", 5s);
  const auto table = rig.js("return __t.last.tool_table;");
  fact("loaded_tool_table", table.dump());
  check("loaded_table_is_shown", followed && table.at("revision") > before.at("revision"),
        table.dump());
  if (!followed)
    return;
  const auto& tools = table.at("tools");
  check("loaded_table_rows_in_mm",
        tools.at(1).at("number") == 0 && tools.at(2).at("number") == 4 &&
            tools.at(3).at("number") == 2 && near(tools.at(0).at("length").get<double>(), 12.7) &&
            near(tools.at(0).at("diameter").get<double>(), 6.35) &&
            near(tools.at(2).at("length").get<double>(), 25.4) &&
            near(tools.at(3).at("diameter").get<double>(), 3.175) &&
            tools.at(0).at("comment") == "nine" && tools.at(3).at("comment") == "eighth inch" &&
            tools.at(2).at("comment") == "",
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
  // The graphics libraries the web process has loaded: WebKit hides the renderer's name from
  // the page.
  for (const auto pid : descendants("WebKitWebProcess"))
    fact("web_process_graphics_libraries",
         output_of(std::format("grep -oE '[^/ ]*(dri|gallium|LLVM|EGL|GLES|GLX|vulkan|nvidia)[^/ ]*\\.so[^ ]*' "
                               "/proc/{}/maps | sort -u | tr '\\n' ' '",
                               pid)));
  const auto graphics = rig.hooks.graphics_report();
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
    const auto choosers = rig.hooks.state().file_choosers;
    rig.hooks.answer_file_chooser(file.empty() ? std::vector<std::string>{}
                                               : std::vector<std::string>{file.string()});
    const bool clicked = rig.real_click("button", "Choose STEP file");
    const bool asked = rig.wait(5s, [&]() -> bool {
      return rig.hooks.state().file_choosers == choosers + 1;
    });
    Json result{{"clicked", clicked}, {"chooser", asked}};
    if (!file.empty())
      rig.wait_js("!document.querySelector('.load-row + .hint[role=\"status\"]') && "
                  "!__t.find('p.hint', 'Reading and tessellating') && "
                  "(!!document.querySelector('.identity') || !!__t.find('p.error'))",
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
  check("step_file_chooser_opens", result.at("clicked") == true && result.at("chooser") == true,
        result.dump());
  check("step_sm101_imported",
        result.at("identity").get<std::string>().find("1 parts") != std::string::npos &&
            result.at("identity").get<std::string>().find("1,132 triangles") != std::string::npos,
        result.at("identity").dump());
  check("step_sm101_size", result.at("size").get<std::string>().find("120") != std::string::npos &&
                               result.at("size").get<std::string>().find("80") != std::string::npos &&
                               result.at("size").get<std::string>().find("20") != std::string::npos,
        result.at("size").dump());
  // The panel shows a part only over a current toolpath preview, and there is none yet: the
  // model is attached and hidden, and says why.
  check("step_model_hidden_without_preview",
        result.at("summary").get<std::string>().find("Part hidden: waiting for the current toolpath") !=
            std::string::npos,
        result.at("summary").dump());
  // Cancelling the chooser leaves the attachment alone; choosing the same file again works.
  const auto cancelled = import({}, 1s);
  check("step_chooser_cancel_keeps_attachment",
        cancelled.at("chooser") == true && cancelled.at("identity") == result.at("identity"),
        cancelled.dump());
  const auto same = import(sm101, 60s);
  check("step_same_file_again", same.at("chooser") == true &&
                                    same.at("identity") == result.at("identity") &&
                                    same.at("error") == "",
        same.dump());
  const auto block = import(samples / "upstream/camcraft/fixtures/step/ops_block.step", 60s);
  fact("step_ops_block", block.dump());
  check("step_centimetre_source_units",
        block.at("size").get<std::string>().find("100") != std::string::npos &&
            block.at("size").get<std::string>().find("70") != std::string::npos,
        block.at("size").dump());
  const auto lego = import(samples / "upstream/camcraft/fixtures/step/lego_4x2_01.step", 60s);
  fact("step_lego", lego.dump());
  check("step_inch_source_units",
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
  check("step_invalid_file_keeps_attachment",
        invalid.at("error") != "" && invalid.at("identity") == lego.at("identity"),
        invalid.dump());
  rig.js("const close = __t.find('section.dialog button[title=\"Close\"]'); "
         "if (close) close.click(); return !!close;");
  std::this_thread::sleep_for(500ms);
  // The model layer: a WebGL2 canvas of the panel's own with a context, and pixels in it.
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
  check("panel_webgl2_layer_alive", rendering, layer.dump());
  rig.snapshot("step-model");
  check("no_console_errors_after_step", rig.js("return __t.errors.length;") == 0, rig.errors());
  fact("page_errors_after_step", rig.errors());

  // The tool model of the Tools tab is a WebGL2 canvas of its own.
  check("panel_tools_tab", rig.click("nav.em-nav button", "Tools") == "ok");
  rig.wait_js("__t.all('canvas').length > 0", 5s);
  std::this_thread::sleep_for(500ms);
  const auto tools_tab = rig.js(R"(
      return __t.all('canvas').map((canvas) => {
        const gl = canvas.getContext('webgl2');
        return { class: canvas.className, width: canvas.width, height: canvas.height, webgl2: !!gl, lost: gl ? gl.isContextLost() : null };
      });)");
  fact("tools_tab_canvases", tools_tab.dump());
  rig.snapshot("tools-tab");
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
}

void estop(const Rig& rig) {
  check("panel_estop", rig.click("button.estop", "") == "ok");
  check("linuxcnc_estop", rig.wait(5s, [&]() -> bool {
    return rig.status().controller.machine == MachineState::estop;
  }));
  check("panel_shows_estop", rig.wait_js("__t.last.status.machine === 'estop'"),
        rig.js("return __t.text(document.querySelector('.state-pill'));").dump());
  rig.snapshot("estop-after-run");
}

void drive(ShellTestHooks& hooks, const MachineConfig& config) {
  try {
    auto observer = NmlStatusReader::connect(config, nullptr);
    check("observer_connected", observer.has_value(), observer ? "status" : observer.error());
    if (!observer)
      throw Failure("no status channel for the test");
    const Rig rig{hooks, config, **observer};
    const auto programs = std::filesystem::path(config.program_prefix);
    const auto ui = std::filesystem::path(COFFEECNC_SOURCE_UI_DIRECTORY);
    fact("panel", std::format("ui={} index_bytes={}", ui.string(),
                              std::filesystem::file_size(ui / "index.html")));
    first_page(rig);
    power_on(rig);
    home(rig);
    positions_in_mm(rig);
    jog_by_held_press(rig);
    reload_mid_jog(rig);
    web_process_killed_mid_jog(rig);
    scheme(rig);
    operator_error(rig, programs);
    tool_table(rig);
    program_with_tool_change(rig, programs);
    platform(rig, ui);
    estop(rig);
    const auto state = hooks.state();
    check("no_request_off_the_scheme_at_end", state.foreign_resources.empty(),
          state.foreign_resources.empty() ? std::format("resources={}", state.resources.size())
                                          : state.foreign_resources.front());
    fact("delivery", std::format("delivered={} dropped={} commits={}", state.delivered,
                                 state.dropped, state.commits));
  } catch (const std::exception& error) {
    check("no_exception", false, error.what());
  }
  // The operator closes the window: the application ends, and LinuxCNC with it.
  hooks.close();
}

} // namespace

auto main(int argc, char** argv) -> int {
  std::string ini_file;
  for (int i = 1; i + 1 < argc; ++i)
    if (std::string_view(argv[i]) == "-ini")
      ini_file = argv[i + 1];
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
          driver = std::thread([&hooks, config]() -> void { drive(*hooks, config); });
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
