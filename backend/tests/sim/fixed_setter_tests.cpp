#include <coffeecnc/linuxcnc/hal_component.hpp>
#include <coffeecnc/linuxcnc/hal_control.hpp>
#include <coffeecnc/linuxcnc/machine_config.hpp>
#include <coffeecnc/linuxcnc/nml_port.hpp>
#include <coffeecnc/linuxcnc/tool_table.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

using namespace coffeecnc;
using namespace std::chrono_literals;

namespace {

void require(bool value, std::string_view message) {
  if (!value)
    throw std::runtime_error(std::string(message));
  std::cout << "check=" << message << std::endl;
}

void checked(const ActionResult& result) {
  if (result.status != ActionStatus::accepted)
    throw std::runtime_error(result.reason);
}

void halcmd(std::string_view command) {
  if (std::system(std::format("halcmd {}", command).c_str()) != 0)
    throw std::runtime_error(std::format("HAL failed: {}", command));
}

void wire(double scale) {
  halcmd("loadrt wcomp names=sim-tool-setter");
  halcmd("loadrt not names=sim-probe-inactive");
  halcmd("loadrt and2 names=sim-probe-lease,sim-probe-estop");
  halcmd("loadrt or2 names=sim-probe-permit");
  for (const auto* component : {"sim-tool-setter", "sim-probe-inactive", "sim-probe-lease",
                                "sim-probe-permit", "sim-probe-estop"})
    halcmd(std::format("addf {} servo-thread", component));
  halcmd(std::format("setp sim-tool-setter.min {}", -100 / scale));
  halcmd(std::format("setp sim-tool-setter.max {}", -45 / scale));
  halcmd("net sim-setter-z axis.z.pos-cmd sim-tool-setter.in");
  halcmd("net sim-setter-trip sim-tool-setter.out motion.probe-input coffeecnc.probe-input");
  halcmd("net sim-setter-selected coffeecnc.probe-setter-select coffeecnc.probe-setter-selected");
  halcmd("net sim-cycle-active coffeecnc.probe-cycle-active sim-probe-inactive.in");
  halcmd("net sim-cycle-lease coffeecnc.probe-cycle-lease sim-probe-lease.in0");
  halcmd("net coffeecnc-watchdog-ok sim-probe-lease.in1");
  halcmd("net sim-cycle-safe sim-probe-lease.out sim-probe-permit.in0");
  halcmd("net sim-cycle-inactive sim-probe-inactive.out sim-probe-permit.in1");
  halcmd("net sim-cycle-permit sim-probe-permit.out motion.enable sim-probe-estop.in0 "
         "coffeecnc.probe-motion-permitted");
  halcmd("unlinkp iocontrol.0.emc-enable-in");
  halcmd("net estop-loop sim-probe-estop.in1");
  halcmd("net sim-enable sim-probe-estop.out iocontrol.0.emc-enable-in");
}

template <typename Predicate> void wait_for(NmlPort& port, Predicate predicate) {
  const auto deadline = HalShared::Clock::now() + 6s;
  while (!predicate(port.status())) {
    if (HalShared::Clock::now() >= deadline)
      throw std::runtime_error("Simulation did not settle in time.");
    std::this_thread::sleep_for(2ms);
  }
}

void run(const std::string& ini) {
  auto config = load_machine_config(ini);
  if (!config)
    throw std::runtime_error(config.error());
  const double scale = config->millimetres_per_unit;
  config->signals.probe = true;
  config->signals.spindle_rpm = true;
  config->fixed_setter = FixedSetterLimits{15, 240, 1, 0.5};
  auto component = HalComponent::create("coffeecnc", config->signals);
  if (!component)
    throw std::runtime_error(component.error());
  halcmd(std::format("-f '{}'", config->postgui_halfile));
  wire(scale);
  HalShared shared;
  HalControl control{*component, shared};
  std::jthread control_thread{[&](std::stop_token stop) {
    while (!stop.stop_requested()) {
      control.beat();
      std::this_thread::sleep_for(5ms);
    }
  }};
  auto connected = NmlPort::connect(*config, &shared);
  if (!connected)
    throw std::runtime_error(connected.error());
  auto& port = **connected;
  wait_for(port, [](const NmlStatus& s) { return s.controller.link == Link::live; });
  checked(port.set_machine_state(MachineState::off, {}));
  checked(port.set_machine_state(MachineState::on, {}));
  checked(port.home({}, {}));
  wait_for(port, [](const NmlStatus& s) {
    return std::ranges::all_of(s.controller.axes, [](auto axis) { return axis.homed; });
  });
  checked(port.set_task_mode(TaskMode::mdi, {}));
  const auto mdi = [&](std::string_view line) {
    checked(port.mdi(line, {}));
    wait_for(port, [](const NmlStatus& s) {
      return s.controller.interp == InterpState::idle && s.controller.in_position;
    });
  };
  mdi("G21 G90 G94 G40");
  mdi("M61 Q1");
  mdi("G43 H1");
  mdi("G53 G0 Z20");
  mdi("G92 Z12");
  mdi(scale == 1 ? "G20 G91" : "G21 G91");
  mdi(scale == 1 ? "F2" : "F50.8");
  halcmd(std::format("setp sim-tool-setter.max {}", 10 / scale));
  const auto starting = port.status();
  const auto& axes = starting.controller.axes;
  FixedSetterMove move{1, {axes[0].machine, axes[1].machine, axes[2].machine}, 15, 240};

  auto invalid_move = move;
  invalid_move.travel_mm = 16;
  const auto commands_before = port.writes().size();
  require(!port.probe_fixed_setter(invalid_move, {}) && port.writes().size() == commands_before,
          "travel beyond the configured limit sends no command");
  const auto contact = port.probe_fixed_setter(move, {});
  require(contact.has_value(), contact ? "native fixed setter completed" : contact.error());
  require(std::abs(contact->machine[2] - 10) < 0.03, "native latch is in machine millimetres");
  const auto returned = port.status();
  std::cout << std::format("fact=start_feed={} end_feed={} end_incremental={}\n",
                           starting.programmed_feed, returned.programmed_feed,
                           std::ranges::contains(returned.active_codes, "G91"));
  require(std::abs(returned.controller.axes[2].machine - 20) < 0.03,
          "native probe returns to the confirmed start");
  require(std::ranges::contains(returned.active_codes, "G91") &&
              std::abs(returned.programmed_feed - 50.8) < 0.001,
          "native probe restores the distance mode and feed");

  auto epoch = SetterEpoch{1, 1, 1, 0};
  const auto setter = [&](const FixedSetterRequest& request) -> SetterState {
    const auto result = port.fixed_setter(request, epoch, {});
    if (!result)
      throw std::runtime_error(result.error());
    return *result;
  };
  const double height = 20 - starting.g5x[2] - starting.g92[2] - 50;
  const FixedSetterRequest capture{.action = SetterAction::capture,
                                   .tool = 1,
                                   .wcs = starting.wcs,
                                   .height = height,
                                   .capture_z = 20};
  require(setter(capture).reference_captured,
          "capture derives the reference from the active work frame");
  const auto calibrated =
      setter({.action = SetterAction::calibrate, .tool = 1, .start = move.start});
  require(calibrated.review && !calibrated.calibration &&
              std::abs(calibrated.review->value - (-40)) < 0.03,
          "calibration reviews the derived trigger plane before saving it");
  const auto calibrated_id = calibrated.review->id;
  require(setter({.action = SetterAction::accept, .review = calibrated_id}).calibration.has_value(),
          "accepting the calibration stores the learned setter plane");
  require(!port.fixed_setter({.action = SetterAction::accept, .review = calibrated_id}, epoch, {}),
          "an accepted result cannot be reused");

  mdi("M61 Q2");
  mdi("G43 H2");
  ++epoch.tool;
  halcmd(std::format("setp sim-tool-setter.max {}", 12 / scale));
  const auto read_table = [&]() {
    std::ifstream input(config->tool_table);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  };
  const auto old_table = read_table();
  const auto measured = setter({.action = SetterAction::measure, .tool = 2, .start = move.start});
  require(measured.review && std::abs(measured.review->value - 52) < 0.03 &&
              old_table == read_table(),
          "measurement reviews machine-frame tool length without writing it");
  require(setter({.action = SetterAction::accept, .review = measured.review->id}).review ==
              std::nullopt,
          "accepting the measurement uses G10 L1 and verifies G43");
  const auto saved_table = read_table();
  require(std::abs(port.status().tool_offset[2] - 52) < 0.03 &&
              std::abs(tool_z(saved_table, 2).value_or(0) * scale - 52) < 0.03 &&
              tool_z(saved_table, 1) == tool_z(old_table, 1) &&
              saved_table.contains(scale == 1 ? "D+3.000000" : "D+0.125000"),
          "only this tool's length changes and it is applied");
  ++epoch.datum;
  require(!setter({.action = SetterAction::status}).calibration,
          "rehoming clears the in-memory setter calibration");

  mdi("M61 Q1");
  mdi("G43 H1");
  halcmd(std::format("setp sim-tool-setter.max {}", 10 / scale));
  const auto stop_at = HalShared::Clock::now() + 600ms;
  require(!port.probe_fixed_setter(move, [&]() { return HalShared::Clock::now() < stop_at; }),
          "withdrawing a running probe invalidates its result");
  const auto interrupted = port.status();
  const double interrupted_z = interrupted.controller.axes[2].machine;
  require(interrupted.controller.machine != MachineState::on && interrupted_z > 10 &&
              interrupted_z < 19,
          "the real HAL gate stops an interrupted native descent before contact");

  // Reset only this isolated simulation, then exercise a trajectory that cannot reach the setter.
  checked(port.set_machine_state(MachineState::off, {}));
  checked(port.set_machine_state(MachineState::on, {}));
  checked(port.set_task_mode(TaskMode::mdi, {}));
  mdi("G21 G90 G94");
  mdi("G53 G0 Z20");

  move.travel_mm = 5;
  require(!port.probe_fixed_setter(move, {}), "missing contact invalidates the native probe");
  const auto stopped = port.status();
  require(stopped.controller.machine != MachineState::on && stopped.controller.axes[2].machine < 19,
          "a missed probe stops without returning to the starting height");
}

} // namespace

auto main(int argc, char** argv) -> int {
  const auto* run_dir = std::getenv("COFFEECNC_SIM_RUN");
  if (argc != 3 || std::string_view(argv[1]) != "-ini" || run_dir == nullptr ||
      std::filesystem::path(argv[2]) != std::filesystem::path(run_dir) / "coffeecnc_sim.ini")
    return 1;
  try {
    run(argv[2]);
    return 0;
  } catch (const std::exception& failure) {
    std::cout << "check=native fixed setter failed: " << failure.what() << '\n';
    return 1;
  }
}
