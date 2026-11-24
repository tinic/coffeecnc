// The HAL side of the diagnostics view (#251) without a running HAL: what a reading makes of
// what HAL holds (a lookup of the test's own), the wiring read from `halcmd -s show sig`, and the
// launcher of halshow, halmeter and halscope on programs the test writes.
//   coffeecnc_hal_diagnostics_tests <scratch directory>

#include <coffeecnc/linuxcnc/hal_diagnostics.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <signal.h> // kill: POSIX, not in <csignal>
#include <sys/wait.h>
#include <unistd.h>

using namespace coffeecnc;
using namespace std::chrono_literals;

namespace {
int checks = 0;
void require(bool condition, const std::string& message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

class FakeHal final : public HalLookup {
public:
  std::map<std::string, PinReading> pins;
  std::map<std::string, SignalReading> signals;
  auto pin(const std::string& name) -> std::optional<PinReading> override {
    const auto found = pins.find(name);
    return found == pins.end() ? std::nullopt : std::optional{found->second};
  }
  auto signal(const std::string& name) -> std::optional<SignalReading> override {
    const auto found = signals.find(name);
    return found == signals.end() ? std::nullopt : std::optional{found->second};
  }
};

// `halcmd -s show sig` on the G0759's E-stop and limit wiring: estop-ext is written by the
// Mesa input, x-neg-limit by nothing.
constexpr std::string_view shown =
    "bit   TRUE  estop-ext  <== hm2_7i76e.0.7i76.0.0.input-16 ==> estop-chain.in0\n"
    "bit   TRUE  estop-ok  <== estop-chain.out ==> probe-estop-gate.in0\n"
    "bit   FALSE  x-neg-limit  ==> joint.0.neg-lim-sw-in\n"
    "float  0  spindle-vel-fb-rpm-abs\n"
    "bit   FALSE  probe-in  <== mux-gen.00.out-bit ==> motion.probe-input "
    "==> coffeecnc.probe-input\n"
    "garbage line\n"
    "bit TRUE odd ==> a.b <=\n";

void wiring() {
  const auto signals = pin_signals(shown);
  require(signals.at("joint.0.neg-lim-sw-in") == "x-neg-limit" &&
              signals.at("hm2_7i76e.0.7i76.0.0.input-16") == "estop-ext" &&
              signals.at("motion.probe-input") == "probe-in" &&
              signals.at("coffeecnc.probe-input") == "probe-in",
          "Every pin of each signal, writers and readers");
  require(!signals.contains("a.b") && !signals.contains("line") && signals.size() == 8,
          "Lines without the form are skipped: " + std::to_string(signals.size()));
  require(pin_signals("").empty(), "No output, no wiring");
}

void readings() {
  FakeHal hal;
  hal.pins["joint.0.neg-lim-sw-in"] = {false, true};  // on x-neg-limit, written by nothing
  hal.pins["joint.0.home-sw-in"] = {true, false};     // linked to nothing, set by setp
  hal.pins["iocontrol.0.emc-enable-in"] = {true, true}; // on estop-gated: not in the wiring
  hal.pins["motion.probe-input"] = {true, true};        // on probe-in, written by the mux
  hal.pins["spindle.0.speed-in"] = {12.5, true};
  hal.pins["svd-ps_vfd.modbus-errors"] = {3.0, false};
  hal.signals["x-neg-limit"] = {false, false};
  hal.signals["probe-in"] = {true, true};
  hal.signals["estop-ext"] = {true, true};
  hal.signals["probe-route"] = {false, false};
  const std::vector<DiagnosticInput> inputs{
      {.role = DiagnosticRole::min_limit, .joint = 0, .name = "joint.0.neg-lim-sw-in"},
      {.role = DiagnosticRole::home_switch, .joint = 0, .name = "joint.0.home-sw-in"},
      {.role = DiagnosticRole::estop_chain, .name = "iocontrol.0.emc-enable-in"},
      {.role = DiagnosticRole::probe, .name = "motion.probe-input"},
      {.role = DiagnosticRole::spindle_speed_in, .name = "spindle.0.speed-in"},
      {.role = DiagnosticRole::spindle_fault, .name = "spindle.0.amp-fault-in"},
      {.role = DiagnosticRole::configured, .label = "E-stop button", .name = "estop-ext"},
      {.role = DiagnosticRole::configured, .label = "Route", .name = "probe-route"},
      {.role = DiagnosticRole::configured, .label = "VFD", .name = "svd-ps_vfd.modbus-errors"},
      {.role = DiagnosticRole::configured, .label = "Gone", .name = "no-such-thing"},
  };
  const auto read = read_diagnostics(inputs, hal, pin_signals(shown));
  require(read.size() == inputs.size(), "One reading per row");
  const auto is = [&](std::size_t row, DiagnosticState state, DiagnosticValue value,
                      bool signal) -> bool {
    return read.at(row).input == inputs.at(row) && read.at(row).state == state &&
           read.at(row).value == value && read.at(row).signal == signal;
  };
  require(is(0, DiagnosticState::undriven, false, false),
          "A pin on a signal nothing writes: no switch, not an open one");
  require(is(1, DiagnosticState::unwired, true, false), "A pin on no signal, with its own value");
  require(is(2, DiagnosticState::live, true, false), "A linked pin the wiring does not know is live");
  require(is(3, DiagnosticState::live, true, false), "A pin on a driven signal");
  require(is(4, DiagnosticState::live, 12.5, false), "A float");
  require(is(5, DiagnosticState::missing, {}, false), "No such pin: missing, no value");
  require(is(6, DiagnosticState::live, true, true), "A configured signal");
  require(is(7, DiagnosticState::undriven, false, true), "A configured signal nothing writes");
  require(is(8, DiagnosticState::unwired, 3.0, false), "A configured name that is a pin");
  require(is(9, DiagnosticState::missing, {}, false), "A configured name HAL does not have");
}

// A program that does what `body` says, as a HAL tool would be found in a PATH directory.
void program(const std::filesystem::path& directory, std::string_view name, std::string_view body) {
  const auto file = directory / name;
  std::ofstream(file) << "#!/bin/sh\n" << body << '\n';
  std::filesystem::permissions(file, std::filesystem::perms::owner_all);
}

auto wait_for(const std::function<bool()>& done) -> bool {
  for (const auto end = std::chrono::steady_clock::now() + 10s;
       std::chrono::steady_clock::now() < end;) {
    if (done())
      return true;
    std::this_thread::sleep_for(10ms);
  }
  return false;
}

void launcher(const std::filesystem::path& scratch) {
  const auto bin = scratch / "bin";
  const auto other = scratch / "other";
  std::filesystem::remove_all(scratch);
  std::filesystem::create_directories(bin);
  std::filesystem::create_directories(other);
  const auto marks = scratch / "marks";
  std::filesystem::create_directories(marks);
  // halscope stays open until it is told to end; it writes what it was started with.
  program(bin, "halscope",
          std::format("echo \"$INI_FILE_NAME $(cat) $$\" > {}/halscope\nexec sleep 60",
                      marks.string()));
  // halmeter ends at once.
  program(bin, "halmeter", std::format("echo started >> {}/halmeter", marks.string()));
  // Not executable: not a tool.
  std::ofstream(other / "halshow") << "#!/nonexistent-coffeecnc-test-interpreter\n";
  std::filesystem::permissions(other / "halshow", std::filesystem::perms::owner_read);
  pid_t scope{};
  {
    const auto search_path = std::format("relative:{}:{}", other.string(), bin.string());
    HalToolLauncher tools{search_path, {"INI_FILE_NAME=/machine/mill.ini", "PATH=/usr/bin:/bin"}};
    auto states = tools.states();
    require(states.size() == 3 && states[0].tool == HalTool::halshow && !states[0].available &&
                states[1].available && states[2].available && !states[1].open && !states[2].open,
            "Installed: an executable file of the name in the search path");
    require(tools.program(HalTool::halscope) == (bin / "halscope").string(),
            "The first match in the search path");
    const auto missing = tools.launch(HalTool::halshow);
    require(missing && missing->text.starts_with("halshow is not installed") &&
                missing->localization == MessageKey{"backend.halDiagnostics.notInstalled",
                                                    {{"name", "halshow"}, {"path", search_path}}},
            "A missing tool reports its name and the search path");

    std::filesystem::permissions(other / "halshow", std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::add);
    const auto failed = tools.launch(HalTool::halshow);
    const auto detail = std::make_error_code(std::errc::no_such_file_or_directory).message();
    require(failed && failed->text == std::format("halshow could not be started: {}.", detail) &&
                failed->localization ==
                    MessageKey{"backend.halDiagnostics.notStartedBecause",
                               {{"name", "halshow"}, {"system_error", detail}}} &&
                !tools.states()[0].open,
            "A spawn failure still reports the operating system error and leaves the tool closed");

    require(!tools.launch(HalTool::halscope), "halscope starts");
    require(wait_for([&]() -> bool { return std::filesystem::exists(marks / "halscope"); }),
            "halscope ran");
    std::string ini;
    std::ifstream(marks / "halscope") >> ini >> scope;
    require(ini == "/machine/mill.ini" && scope > 0,
            "With the environment given, and nothing on standard input");
    require(getsid(scope) == scope, "In a session of its own");
    require(tools.states()[2].open, "Shown open");
    const auto twice = tools.launch(HalTool::halscope);
    require(twice && twice->text == "halscope is already open." &&
                twice->localization ==
                    MessageKey{"backend.halDiagnostics.alreadyOpen", {{"name", "halscope"}}},
            "An open tool is refused with its name");

    require(!tools.launch(HalTool::halmeter), "halmeter starts");
    require(wait_for([&]() -> bool { return !tools.states()[1].open; }),
            "A tool that has ended is no longer open");
    require(!tools.launch(HalTool::halmeter), "and can be started again");
    require(wait_for([&]() -> bool { return !tools.states()[1].open; }), "It ended again");
    std::ifstream count(marks / "halmeter");
    int lines = 0;
    for (std::string line; std::getline(count, line);)
      ++lines;
    require(lines == 2, "halmeter ran twice");
    require(kill(scope, 0) == 0, "halscope is still open");
  }
  // The launcher is gone: what it started and was still open has been told to end. Its process
  // is a child of this one still, until it is reaped here.
  require(wait_for([&]() -> bool {
            int status{};
            return waitpid(scope, &status, WNOHANG) == scope && WIFSIGNALED(status) &&
                   WTERMSIG(status) == SIGTERM;
          }),
          "A tool still open ends with the launcher, by SIGTERM");
}

void environment() {
  ::setenv("COFFEECNC_DIAGNOSTICS_TEST", "1", 1);
  ::unsetenv("INI_FILE_NAME");
  auto made = hal_tool_environment("/machine/mill.ini");
  require(std::ranges::count(made, "INI_FILE_NAME=/machine/mill.ini") == 1 &&
              std::ranges::count(made, "COFFEECNC_DIAGNOSTICS_TEST=1") == 1,
          "This process's environment, and the INI file where LinuxCNC set none");
  ::setenv("INI_FILE_NAME", "/given.ini", 1);
  made = hal_tool_environment("/machine/mill.ini");
  require(std::ranges::count(made, "INI_FILE_NAME=/given.ini") == 1 &&
              std::ranges::count_if(made, [](const std::string& entry) -> bool {
                return entry.starts_with("INI_FILE_NAME=");
              }) == 1,
          "LinuxCNC's own INI_FILE_NAME is kept");
}

} // namespace

auto main(int argc, char** argv) -> int {
  try {
    if (argc != 2)
      throw std::runtime_error("usage: coffeecnc_hal_diagnostics_tests <scratch directory>");
    wiring();
    readings();
    launcher(std::filesystem::path(argv[1]) / "hal_diagnostics");
    environment();
    std::cout << checks << " HAL diagnostics checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Check " << checks << " failed: " << error.what() << '\n';
    return 1;
  }
}
