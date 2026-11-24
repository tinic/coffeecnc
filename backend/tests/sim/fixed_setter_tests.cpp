#include <coffeecnc/app/panel_feed.hpp>
#include <coffeecnc/app/runtime.hpp>
#include <coffeecnc/core/text_file.hpp>
#include <coffeecnc/linuxcnc/hal_component.hpp>
#include <coffeecnc/linuxcnc/hal_control.hpp>
#include <coffeecnc/linuxcnc/machine_config.hpp>
#include <coffeecnc/linuxcnc/nml_panel_source.hpp>
#include <coffeecnc/linuxcnc/nml_port.hpp>
#include <coffeecnc/linuxcnc/preview_engine.hpp>
#include <coffeecnc/linuxcnc/tool_table.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

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

void wire(double scale, bool g0759) {
  halcmd("loadrt wcomp names=sim-tool-setter");
  if (g0759) {
    halcmd("unlinkp motion.probe-input");
    halcmd("unlinkp coffeecnc.probe-input");
  } else {
    halcmd("loadrt not names=sim-probe-inactive");
    halcmd("addf sim-probe-inactive servo-thread");
  }
  const auto* inactive = g0759 ? "not.0" : "sim-probe-inactive";
  halcmd("loadrt and2 names=sim-probe-lease,sim-probe-estop");
  halcmd("loadrt or2 names=sim-probe-permit");
  for (const auto* component :
       {"sim-tool-setter", "sim-probe-lease", "sim-probe-permit", "sim-probe-estop"})
    halcmd(std::format("addf {} servo-thread", component));
  halcmd(std::format("setp sim-tool-setter.min {}", -100 / scale));
  halcmd(std::format("setp sim-tool-setter.max {}", -45 / scale));
  halcmd("net sim-setter-z axis.z.pos-cmd sim-tool-setter.in");
  halcmd("net sim-setter-trip sim-tool-setter.out motion.probe-input coffeecnc.probe-input");
  halcmd("net sim-setter-selected coffeecnc.probe-setter-select coffeecnc.probe-setter-selected");
  halcmd(std::format("net sim-cycle-active coffeecnc.probe-cycle-active {}.in", inactive));
  halcmd("net sim-cycle-lease coffeecnc.probe-cycle-lease sim-probe-lease.in0");
  halcmd("net coffeecnc-watchdog-ok sim-probe-lease.in1");
  halcmd("net sim-cycle-safe sim-probe-lease.out sim-probe-permit.in0");
  halcmd(std::format("net sim-cycle-inactive {}.out sim-probe-permit.in1", inactive));
  halcmd("net sim-cycle-permit sim-probe-permit.out motion.enable sim-probe-estop.in0 "
         "coffeecnc.probe-motion-permitted");
  halcmd("unlinkp iocontrol.0.emc-enable-in");
  halcmd(g0759 ? "net estop-out sim-probe-estop.in1" : "net estop-loop sim-probe-estop.in1");
  halcmd("net sim-enable sim-probe-estop.out iocontrol.0.emc-enable-in");
}

// `load_sum`: false when the caller has loaded sum2 with sim-z-feedback among its names already
// (HAL loads a component once).
void wire_z_feedback(bool load_sum = true) {
  // Report the motor's commanded position plus the backlash in the last positive Z move.
  // The offset starts at zero and the test enables it only for one probe cycle.
  halcmd("loadrt mux2 names=sim-z-backlash");
  if (load_sum)
    halcmd("loadrt sum2 names=sim-z-feedback");
  halcmd("addf sim-z-backlash servo-thread");
  halcmd("addf sim-z-feedback servo-thread");
  halcmd("unlinkp joint.2.motor-pos-fb");
  halcmd("net Zpos sim-z-feedback.in0");
  halcmd("net Z-positive-direction sim-z-backlash.sel");
  halcmd("net sim-z-backlash-error sim-z-backlash.out sim-z-feedback.in1");
  halcmd("net sim-z-encoder sim-z-feedback.out joint.2.motor-pos-fb");
}

template <typename Predicate> void wait_for(NmlPort& port, Predicate predicate) {
  const auto deadline = HalShared::Clock::now() + 6s;
  while (!predicate(port.status())) {
    if (HalShared::Clock::now() >= deadline)
      throw std::runtime_error("Simulation did not settle in time.");
    std::this_thread::sleep_for(2ms);
  }
}

void material_scenarios(std::unique_ptr<NmlPort>& port, HalShared& shared, double scale,
                        const auto& mdi, const auto& restart) {
  checked(port->set_tool({98, 98, 50, 0, "Setter reference"}, {}));
  checked(port->set_tool({99, 99, 45, 2, "Ruby probe"}, {}));
  auto epoch = SetterEpoch{1, 1, 1, 0};
  const auto position = [&] {
    const auto axes = port->status().controller.axes;
    return std::array{axes[0].machine, axes[1].machine, axes[2].machine};
  };
  const auto start_at = [&] { mdi("G21 G90 G53 G0 X0 Y0 Z20"); };
  const auto window = [&](char axis, double low, double high) {
    halcmd("unlinkp sim-tool-setter.in");
    const auto net =
        axis == 'z' ? std::string("sim-setter-z") : std::format("sim-contact-{}", axis);
    halcmd(std::format("net {} axis.{}.pos-cmd sim-tool-setter.in", net, axis));
    halcmd(std::format("setp sim-tool-setter.min {}", low / scale));
    halcmd(std::format("setp sim-tool-setter.max {}", high / scale));
  };
  const auto load = [&](int tool) {
    mdi(std::format("M61 Q{}", tool));
    mdi(std::format("G43 H{}", tool));
    ++epoch.tool;
    start_at();
  };
  const auto setter = [&](int tool, const Wanted& wanted = {}) -> SetterState {
    const auto result = port->fixed_setter({SetterAction::touch_off, tool, position()}, epoch, wanted);
    if (!result)
      throw std::runtime_error(result.error().text);
    return *result;
  };
  const auto setter_status = [&]() -> SetterState {
    const auto result = port->fixed_setter({.action = SetterAction::status}, epoch, {});
    if (!result)
      throw std::runtime_error(result.error().text);
    return *result;
  };
  const auto material = [&](ProbeDirection direction, const Wanted& wanted = {}) {
    return port->fixed_setter({SetterAction::material, 99, position(), direction}, epoch, wanted);
  };
  load(98);
  require(!material(ProbeDirection::z_top), "material touch-off requires the ruby probe");
  window('z', -100, 10);
  setter(98);
  load(99);
  require(!material(ProbeDirection::z_top), "material Z requires a measured probe");
  window('z', -100, 12);
  setter(99);
  load(1);
  window('z', -100, 11.5);
  setter(1);
  const double original_t1 = port->status().tool_offset[2];
  load(99);
  restart();
  ++epoch.page;
  const auto restarted = setter_status();
  require(!restarted.calibration && restarted.measured_tools == std::vector<int>{1, 98, 99},
          "restarting restores the saved ruby and tool marks without a setter plane");

  mdi("G54");
  const auto g54 = port->status().g5x;
  mdi("G10 L2 P2 X5 Y-7 Z2");
  mdi("G55");
  mdi("G92 X2 Y3 Z4");
  const auto check_cycle = [&](ProbeDirection direction, std::size_t axis, double expected,
                               const Wanted& wanted = {}, double z_lag = 0) {
    mdi(scale == 1 ? "G20" : "G21");
    const auto before = port->status();
    const auto start = position();
    const auto result = material(direction, wanted);
    require(result.has_value(), result ? "material probe completed" : result.error().text);
    const auto after = port->status();
    require(result->material && result->material->wcs == 2 &&
                result->material->axis == "XYZ"[axis] && after.wcs == 2 &&
                std::abs(after.work[axis] - expected - (axis == 2 ? z_lag : 0)) < 0.03,
            "the active G55 axis is zeroed at the physical surface");
    const auto end = position();
    for (std::size_t i = 0; i < start.size(); ++i) {
      require(std::abs(end[i] - start[i] - (i == 2 ? z_lag : 0)) < 0.03,
              "the probe returned to its start within the expected encoder lag");
      if (i != axis)
        require(std::abs(after.work[i] - before.work[i]) < 0.03, "another axis is unchanged");
    }
  };
  start_at();
  window('z', -100, 13);
  wire_z_feedback();
  constexpr double z_lag = -0.125;
  bool backlash_enabled = false;
  check_cycle(ProbeDirection::z_top, 2, 7, [&] {
    const auto pins = shared.view(HalShared::Clock::now());
    if (!backlash_enabled && pins && pins->probe_cycle_active && pins->probe_input &&
        *pins->probe_input) {
      halcmd(std::format("setp sim-z-backlash.in1 {}", z_lag / scale));
      backlash_enabled = true;
    }
    return true;
  }, z_lag);
  require(backlash_enabled, "Z top set the origin with 0.125 mm of simulated reversal backlash");
  halcmd("setp sim-z-backlash.in1 0");
  require(!setter_status().calibration, "T99 probes material Z after a restart without teaching");

  start_at();
  window('z', -100, 12);
  bool saw_ruby_input = false;
  const auto taught_by_ruby = setter(99, [&] {
    const auto pins = shared.view(HalShared::Clock::now());
    if (pins && pins->probe_cycle_active && !pins->probe_setter_selected && pins->probe_input)
      saw_ruby_input = true;
    return true;
  });
  require(taught_by_ruby.calibration && taught_by_ruby.measurement &&
              taught_by_ruby.measurement->calibration && saw_ruby_input,
          "a marked T99 teaches the setter through its own input");
  load(1);
  window('z', -100, 11.5);
  const auto remeasured = setter(1);
  require(remeasured.measurement && !remeasured.measurement->calibration &&
              std::abs(port->status().tool_offset[2] - original_t1) < 0.01,
          "T1 remeasures to the same length when T99 teaches the setter");
  load(99);

  halcmd("loadrt timedelay names=sim-ruby-release");
  halcmd("addf sim-ruby-release servo-thread");
  halcmd("setp sim-ruby-release.on-delay 0");
  halcmd("setp sim-ruby-release.off-delay 0");
  halcmd("unlinkp motion.probe-input");
  halcmd("unlinkp coffeecnc.probe-input");
  halcmd("net sim-setter-trip sim-ruby-release.in");
  halcmd("net sim-ruby-released sim-ruby-release.out motion.probe-input coffeecnc.probe-input");

  struct Edge {
    std::string_view face;
    ProbeDirection direction;
    std::size_t axis;
    bool from_negative_side;
  };
  for (const auto& edge :
       {Edge{"X- left", ProbeDirection::x_plus, 0, true},
        Edge{"X+ right", ProbeDirection::x_minus, 0, false},
        Edge{"Y- front", ProbeDirection::y_plus, 1, true},
        Edge{"Y+ back", ProbeDirection::y_minus, 1, false}}) {
    start_at();
    window("xy"[edge.axis], edge.from_negative_side ? 6 : -100,
           edge.from_negative_side ? 100 : -6);
    std::cout << "workpiece-edge=" << edge.face << std::endl;
    if (edge.direction != ProbeDirection::y_plus) {
      check_cycle(edge.direction, edge.axis, edge.from_negative_side ? -7 : 7);
      continue;
    }

    // Keep the ruby tripped after the fourth contact, long enough to outlast
    // the 6 mm retract at 8 mm/s. The three preceding back-offs release promptly.
    bool tripped = false;
    int contacts = 0;
    std::optional<HalShared::Clock::time_point> final_contact;
    bool still_tripped_at_retract = false;
    check_cycle(edge.direction, edge.axis, -7, [&] {
      const auto now = HalShared::Clock::now();
      const auto pins = shared.view(now);
      if (!pins || !pins->probe_cycle_active || !pins->probe_input)
        return true;
      if (*pins->probe_input && !tripped && ++contacts == 4) {
        halcmd("setp sim-ruby-release.off-delay 1.6");
        final_contact = HalShared::Clock::now();
      }
      tripped = *pins->probe_input;
      if (final_contact && now - *final_contact > 1200ms && tripped)
        still_tripped_at_retract = true;
      return true;
    });
    require(contacts == 4 && still_tripped_at_retract,
            "the front edge converged after delayed release past the retract");
    halcmd("setp sim-ruby-release.off-delay 0");
  }
  start_at();
  mdi("G21 G10 L2 P2 R30");
  window('x', 6, 100);
  check_cycle(ProbeDirection::x_plus, 0, -(6 / std::sqrt(0.75) + 1));
  require(std::abs(port->status().rotation_xy - 30) < 0.01,
          "rotated X probing follows the work axis and keeps the work rotation");
  mdi("G21 G10 L2 P2 R0");
  mdi("G54");
  require(port->status().g5x == g54, "probing G55 left G54 unchanged");
  mdi("G55");

  // A corner of a block: X and Y faces at machine -5 mm, its top at 12 mm (controlled point,
  // the ruby's length applied). The probe starts 5 mm inside both faces, 8 mm above the top.
  // Each axis inside its window (near: |in1 - in2| <= difference), all three at once (logic AND).
  // wcomp and and2 are loaded already, and a realtime module loads once.
  // The pocket instances are loaded here too: a realtime module loads once.
  halcmd("loadrt near names=sim-corner-x,sim-corner-y,sim-corner-z,sim-pocket-x,sim-pocket-y");
  halcmd("loadrt logic names=sim-corner-in,sim-pocket-out personality=0x103,0x802");
  for (const auto* component : {"sim-corner-x", "sim-corner-y", "sim-corner-z", "sim-corner-in",
                                "sim-pocket-x", "sim-pocket-y", "sim-pocket-out"})
    halcmd(std::format("addf {} servo-thread", component));
  const auto corner_window = [&](char axis, double low, double high) {
    const auto name = std::format("sim-corner-{}", axis);
    // The edge cycles above left each axis's commanded position on a signal of its own.
    halcmd(std::format("net {} {}.in1", axis == 'z' ? std::string("sim-setter-z")
                                                    : std::format("sim-contact-{}", axis),
                       name));
    halcmd(std::format("setp {}.in2 {}", name, (low + high) / 2 / scale));
    halcmd(std::format("setp {}.difference {}", name, (high - low) / 2 / scale));
    halcmd(std::format("setp {}.scale 1", name));
  };
  corner_window('x', -5, 100);
  corner_window('y', -5, 100);
  corner_window('z', -100, 12);
  halcmd("net sim-corner-inx sim-corner-x.out sim-corner-in.in-00");
  halcmd("net sim-corner-iny sim-corner-y.out sim-corner-in.in-01");
  halcmd("net sim-corner-inz sim-corner-z.out sim-corner-in.in-02");
  halcmd("unlinkp sim-ruby-release.in");
  halcmd("net sim-corner-trip sim-corner-in.and sim-ruby-release.in");
  start_at();
  mdi("G21");
  mdi("G10 L20 P0 Z8");
  const auto corner_before = port->status();
  corner_window('z', -100, 0); // No contact at the lateral search depth.
  const auto missed_corner = port->fixed_setter(
      {SetterAction::corner, 99, position(), ProbeDirection::x_plus, ProbeDirection::y_plus},
      epoch, {});
  require(!missed_corner && missed_corner.error().text.contains("lifted clear") &&
              std::abs(position()[2] - 20) < 0.2 && port->status().g5x == corner_before.g5x,
          missed_corner ? "expected a missed outside face" : missed_corner.error().text);
  start_at();
  corner_window('z', -100, 12);
  const auto corner = port->fixed_setter(
      {SetterAction::corner, 99, position(), ProbeDirection::x_plus, ProbeDirection::y_plus}, epoch,
      {});
  require(corner.has_value(), corner ? "corner probe completed" : corner.error().text);
  const auto corner_after = port->status();
  // Faces at -5 mm plus the 1 mm ball radius; the probe ends lifted above the Y approach point.
  require(corner && corner->material && corner->material->corner && corner->material->wcs == 2 &&
              corner_after.wcs == 2 && std::abs(corner_after.work[0] - 4) < 0.03 &&
              std::abs(corner_after.work[1] + 10) < 0.03 &&
              std::abs(corner_after.work[2] - corner_before.work[2]) < 0.03 &&
              std::abs(position()[2] - 20) < 0.2,
          std::format("the corner set G55 X0 Y0 at both faces and lifted (work {:.3f} {:.3f}, z {:.3f})",
                      corner_after.work[0], corner_after.work[1], position()[2]));
  start_at();
  mdi("G21");
  mdi("G10 L20 P0 Z-10");
  const auto low = port->fixed_setter(
      {SetterAction::corner, 99, position(), ProbeDirection::x_plus, ProbeDirection::y_plus}, epoch,
      {});
  require(!low && low.error().text.contains("above the top"),
          low ? "expected a refusal" : low.error().text);

  // Ball calibration over a 20 mm gauge with faces at -10 and +10 mm. Both faces are probed from
  // outside, so a 2.1 mm effective ball touches with its centre 1.05 mm outside each face, at
  // -11.05 and +11.05 mm. The centres are 22.1 mm apart: 22.1 - 20 = 2.1 mm.
  corner_window('x', -11.05, 11.05);
  corner_window('y', -100, 100);
  corner_window('z', -100, 12);
  const auto gauge = [&](double width) {
    start_at();
    mdi("G21");
    mdi("G90 G53 G0 X1");
    mdi("G10 L20 P0 Z8");
    FixedSetterRequest request{SetterAction::ball, 99, position()};
    request.gauge_width_mm = width;
    return port->fixed_setter(request, epoch, {});
  };
  const auto calibrated = gauge(20);
  require(calibrated && calibrated->ball_diameter &&
              std::abs(*calibrated->ball_diameter - 2.1) < 0.02,
          calibrated ? std::format("the gauge measured the ball at {:.3f} mm",
                                   calibrated->ball_diameter.value_or(0))
                     : calibrated.error().text);
  require(std::abs(position()[2] - 20) < 0.2, "the calibration lifted the probe afterwards");
  const auto wrong = gauge(24);
  require(!wrong && wrong.error().text.contains("more than 0.5 mm"),
          wrong ? "expected a refused diameter" : wrong.error().text);
  mdi("G10 L1 P99 R1");

  // A boss: the block's ball-centre contacts at x -9/+9 and y -7/+7, its top at 12 mm. Faces
  // lie one radius outward of the contacts in the real world, here inward: centre 0, 0 and a
  // 12 mm chord in Y.
  corner_window('x', -9, 9);
  corner_window('y', -7, 7);
  corner_window('z', -100, 12);
  start_at();
  mdi("G21");
  mdi("G90 G53 G0 X1 Y2");
  mdi("G10 L20 P0 Z8");
  FixedSetterRequest boss_request{SetterAction::boss, 99, position()};
  boss_request.gauge_width_mm = 16;
  const auto boss = port->fixed_setter(boss_request, epoch, {});
  const auto boss_after = port->status();
  require(boss && boss->center_diameter && std::abs(*boss->center_diameter - 12) < 0.03 &&
              boss->material && boss->material->corner &&
              std::abs(boss_after.work[0] - (position()[0] - 0)) < 0.03 &&
              std::abs(boss_after.work[1] - (position()[1] - 0)) < 0.03 &&
              std::abs(position()[2] - 20) < 0.2,
          boss ? std::format("the boss centre is X0 Y0, diameter {:.3f} mm (work {:.3f} {:.3f})",
                             boss->center_diameter.value_or(0), boss_after.work[0],
                             boss_after.work[1])
               : boss.error().text);

  // Both contacts can be real yet lie on the same side of the confirmed starting point.
  // Refuse to write their midpoint to the work origin.
  start_at();
  mdi("G10 L20 P0 Z8");
  corner_window('x', 1, 5);
  boss_request.start = position();
  // The simulator searches only 20 mm. Start close enough that both contacts are reachable.
  boss_request.gauge_width_mm = 2;
  const auto before_wrong_boss = port->status().g5x;
  const auto wrong_boss = port->fixed_setter(boss_request, epoch, {});
  require(!wrong_boss && wrong_boss.error().text.contains("do not enclose") &&
              port->status().g5x == before_wrong_boss && std::abs(position()[2] - 20) < 0.2,
          wrong_boss ? "expected a refusal for a boss outside the start" : wrong_boss.error().text);

  // A bore: the ball trips outside x -9..+9 and y -7..+7 (NAND of both windows); centre 0, 0, a
  // 14 mm centre chord in Y plus the 2 mm ball: 16 mm.
  const auto pocket_window = [&](char axis, double from, double to) {
    const auto name = std::format("sim-pocket-{}", axis);
    halcmd(std::format("net {} {}.in1", std::format("sim-contact-{}", axis), name));
    halcmd(std::format("setp {}.in2 {}", name, (from + to) / 2 / scale));
    halcmd(std::format("setp {}.difference {}", name, (to - from) / 2 / scale));
    halcmd(std::format("setp {}.scale 1", name));
  };
  pocket_window('x', -9, 9);
  pocket_window('y', -7, 7);
  halcmd("net sim-pocket-inx sim-pocket-x.out sim-pocket-out.in-00");
  halcmd("net sim-pocket-iny sim-pocket-y.out sim-pocket-out.in-01");
  // Into the pocket before its walls exist: outside them the probe input is tripped, and a
  // tripped probe stops any move that is not a probe.
  start_at();
  mdi("G21");
  mdi("G90 G53 G0 X2 Y1");
  halcmd("unlinkp sim-ruby-release.in");
  halcmd("net sim-pocket-trip sim-pocket-out.nand sim-ruby-release.in");
  const auto bore = port->fixed_setter({SetterAction::bore, 99, position()}, epoch, {});
  const auto bore_after = port->status();
  require(bore && bore->center_diameter && std::abs(*bore->center_diameter - 16) < 0.03 &&
              std::abs(bore_after.work[0] - position()[0]) < 0.03 &&
              std::abs(bore_after.work[1] - position()[1]) < 0.03,
          bore ? std::format("the bore centre is X0 Y0, diameter {:.3f} mm (work {:.3f} {:.3f})",
                             bore->center_diameter.value_or(0), bore_after.work[0],
                             bore_after.work[1])
               : bore.error().text);
  halcmd("unlinkp sim-ruby-release.in");
  halcmd("net sim-corner-trip sim-ruby-release.in");
  halcmd("unlinkp sim-ruby-release.in");
  halcmd("net sim-setter-trip sim-ruby-release.in");

  // An edge angle: the front edge (contact at y 6 mm) at two points 20 mm apart in X; the
  // simulated edge is square to X.
  start_at();
  window('y', 6, 100);
  FixedSetterRequest angle_request{SetterAction::angle, 99, position(), ProbeDirection::y_plus};
  angle_request.gauge_width_mm = 20;
  const auto edge = port->fixed_setter(angle_request, epoch, {});
  require(edge && edge->edge_angle_deg && std::abs(*edge->edge_angle_deg) < 0.05,
          edge ? std::format("the square front edge measured {:.3f}°",
                             edge->edge_angle_deg.value_or(99))
               : edge.error().text);
  angle_request.gauge_width_mm = 2;
  angle_request.start = position();
  const auto close = port->fixed_setter(angle_request, epoch, {});
  require(!close && close.error().text.contains("5 to 300 mm"),
          close ? "expected a refusal" : close.error().text);

  start_at();
  mdi("G10 L2 P2 R90");
  window('y', 6, 100);
  check_cycle(ProbeDirection::x_plus, 0, -7);

  start_at();
  window('z', -100, -50);
  const auto start = position();
  const auto before = port->status().g5x;
  const auto failed_cleanly = [&](const auto& result, std::string_view reason) {
    require(!result && result.error().text.contains(reason),
            result ? "expected a probe failure" : result.error().text);
    require(port->status().g5x == before && port->snapshot().machine == MachineState::on &&
                std::abs(position()[2] - start[2]) < 0.03,
            "a probe failure returned to its start and left the work origin intact");
  };
  failed_cleanly(material(ProbeDirection::z_top), "No probe contact");

  window('z', -100, 13);
  int hits = 0;
  bool tripped = false;
  bool shifted = false;
  const auto unstable = material(ProbeDirection::z_top, [&] {
    const auto pins = shared.view(HalShared::Clock::now());
    if (pins && pins->probe_cycle_active && pins->probe_input) {
      if (*pins->probe_input && !tripped)
        ++hits;
      if (!*pins->probe_input && tripped && hits == 2 && !shifted) {
        halcmd(std::format("setp sim-tool-setter.max {}", 12.95 / scale));
        shifted = true;
      }
      tripped = *pins->probe_input;
    }
    return true;
  });
  require(shifted && hits == 4, "all three slow contacts were observed");
  failed_cleanly(unstable, "exceeds 0.01 mm");

  mdi(std::format("G10 L1 P99 Z{}", port->status().tool_offset[2] + 1));
  mdi("G43 H99");
  const auto writes_before = port->writes().size();
  failed_cleanly(material(ProbeDirection::z_top), "Touch off T99");
  require(port->writes().size() == writes_before, "the changed ruby length prevents probe motion");
  const auto edited = setter_status();
  require(!edited.calibration && edited.measured_tools == std::vector<int>{1, 98},
          "editing the teaching tool drops the plane and preserves other saved marks");
}

void feature_scenarios(NmlPort& port, double scale, const auto& mdi) {
  checked(port.set_tool({98, 98, 50, 0, "Setter reference"}, {}));
  checked(port.set_tool({99, 99, 45, 2, "Ruby probe"}, {}));
  const auto machine = [&] {
    const auto axes = port.status().controller.axes;
    return std::array{axes[0].machine, axes[1].machine, axes[2].machine};
  };
  const auto at_start = [&] {
    mdi("G21 G90 G53 G0 X2 Y1 Z20");
    mdi(scale == 1 ? "G21 G91" : "G20 G91");
  };
  SetterEpoch epoch{1, 1, 1, 0};
  const auto run = [&](SetterAction action, ProbeDirection x = ProbeDirection::x_plus,
                       ProbeDirection y = ProbeDirection::y_plus, double width = 0) {
    return port.fixed_setter({action, 99, machine(), x, y, width}, epoch, {});
  };
  for (const int tool : {98, 99}) {
    mdi(std::format("M61 Q{}", tool));
    mdi(std::format("G43 H{}", tool));
    at_start();
    ++epoch.tool;
    halcmd(std::format("setp sim-tool-setter.max {}", (tool == 98 ? 10 : 12) / scale));
    const auto touched = port.fixed_setter({SetterAction::touch_off, tool, machine()}, epoch, {});
    require(touched.has_value(),
            touched ? "the reference and probe have known lengths" : touched.error().text);
  }
  mdi("G21 G90 G54");
  const auto g54 = port.status().g5x;
  mdi("G10 L2 P2 X5 Y-7 Z2 R0");
  mdi("G55");
  mdi("G92 X2 Y3 Z4");
  mdi("G10 L20 P0 Z8");
  const auto initial = port.status();
  halcmd("loadrt near names=sim-feature-x,sim-feature-y,sim-feature-z");
  halcmd("loadrt logic names=sim-feature-hollow,sim-feature-solid personality=0x802,0x103");
  for (const auto* part : {"sim-feature-x", "sim-feature-y", "sim-feature-z",
                           "sim-feature-hollow", "sim-feature-solid"})
    halcmd(std::format("addf {} servo-thread", part));
  const auto limits = [&](char axis, double low, double high) {
    halcmd(std::format("setp sim-feature-{}.in2 {}", axis, (high + low) / 2 / scale));
    halcmd(std::format("setp sim-feature-{}.difference {}", axis, (high - low) / 2 / scale));
  };
  for (const auto axis : {'x', 'y', 'z'}) {
    halcmd(std::format("setp sim-feature-{}.scale 1", axis));
    if (axis == 'z')
      halcmd("net sim-setter-z sim-feature-z.in1");
    else
      halcmd(std::format("net sim-position-{0} axis.{0}.pos-cmd sim-feature-{0}.in1", axis));
  }
  limits('x', -5, 13);
  limits('y', -9, 5);
  limits('z', -100, 12);
  halcmd("net sim-feature-inx sim-feature-x.out sim-feature-hollow.in-00 sim-feature-solid.in-00");
  halcmd("net sim-feature-iny sim-feature-y.out sim-feature-hollow.in-01 sim-feature-solid.in-01");
  halcmd("net sim-feature-inz sim-feature-z.out sim-feature-solid.in-02");
  halcmd("unlinkp motion.probe-input");
  halcmd("unlinkp coffeecnc.probe-input");
  halcmd("net sim-feature-inner sim-feature-hollow.nand motion.probe-input coffeecnc.probe-input");

  for (const auto x : {ProbeDirection::x_minus, ProbeDirection::x_plus}) {
    for (const auto y : {ProbeDirection::y_minus, ProbeDirection::y_plus}) {
      at_start();
      const auto hit = run(SetterAction::inside_corner, x, y);
      const auto status = port.status();
      const double edge_x = x == ProbeDirection::x_minus ? -6 : 14;
      const double edge_y = y == ProbeDirection::y_minus ? -10 : 6;
      require(
          hit && hit->material && hit->material->corner &&
              std::abs(status.work[0] - (2 - edge_x)) < 0.03 &&
              std::abs(status.work[1] - (1 - edge_y)) < 0.03 &&
              std::abs(machine()[2] - 20) < 0.03 && std::abs(status.g5x[2] - initial.g5x[2]) < 1e-6,
          hit ? "inside corner sets both axes using the selected wall signs" : hit.error().text);
    }
  }
  at_start();
  const auto pocket = run(SetterAction::pocket);
  const auto pocket_status = port.status();
  require(pocket && pocket->feature_size[0] && pocket->feature_size[1] &&
              std::abs(*pocket->feature_size[0] - 20) < 0.03 &&
              std::abs(*pocket->feature_size[1] - 16) < 0.03 &&
              std::abs(pocket_status.work[0] + 2) < 0.03 &&
              std::abs(pocket_status.work[1] - 3) < 0.03 &&
              std::abs(pocket_status.g5x[2] - initial.g5x[2]) < 1e-6,
          pocket ? "rectangular pocket finds both physical widths and sets XY together"
                 : pocket.error().text);

  const auto selected = scale == 1 ? ProbeDirection::y_plus : ProbeDirection::x_plus;
  const std::size_t across = scale == 1 ? 1 : 0;
  const auto span = [&](SetterAction action, double width, double expected_width) {
    at_start();
    const auto before = port.status();
    const auto measured = run(action, selected, ProbeDirection::y_plus, width);
    const auto after = port.status();
    require(measured && measured->material && !measured->material->corner &&
                measured->material->axis == "XY"[across] && measured->feature_size[across] &&
                std::abs(*measured->feature_size[across] - expected_width) < 0.03 &&
                !measured->feature_size[1 - across] &&
                std::abs(after.work[across] - (machine()[across] - (across == 0 ? 4 : -2))) <
                    0.03 &&
                std::abs(after.g5x[1 - across] - before.g5x[1 - across]) < 1e-6 &&
                std::abs(after.g5x[2] - before.g5x[2]) < 1e-6 && after.g92 == before.g92 &&
                std::abs(machine()[2] - 20) < 0.03,
            measured ? "span sets one axis with ball compensation and leaves the other axes intact"
                     : measured.error().text);
  };
  span(SetterAction::slot, 0, across == 0 ? 20 : 16);

  at_start();
  const auto before_miss = port.status();
  limits('y', -100, 100);
  const auto miss = run(SetterAction::inside_corner, ProbeDirection::x_minus, ProbeDirection::y_plus);
  require(!miss && miss.error().text.contains("No probe contact") &&
              port.status().g5x == before_miss.g5x && std::abs(machine()[0] - 2) < 0.03 &&
              std::abs(machine()[1] - 1) < 0.03,
          miss ? "expected a missed second wall" : miss.error().text);
  limits('y', -9, 5);

  halcmd("unlinkp motion.probe-input");
  halcmd("unlinkp coffeecnc.probe-input");
  halcmd("net sim-feature-outer sim-feature-solid.and motion.probe-input coffeecnc.probe-input");
  span(SetterAction::ridge, across == 0 ? 16 : 12, across == 0 ? 16 : 12);
  at_start();
  const auto ridge_origin = port.status().g5x;
  limits('z', -100, -99);
  const auto ridge_miss =
      run(SetterAction::ridge, selected, ProbeDirection::y_plus, across == 0 ? 16 : 12);
  require(!ridge_miss && ridge_miss.error().text.contains("lifted clear") &&
              port.status().g5x == ridge_origin && std::abs(machine()[2] - 20) < 0.03,
          ridge_miss ? "expected a missed ridge face" : ridge_miss.error().text);
  require(std::abs(port.status().g5x[2] - initial.g5x[2]) < 1e-6 && port.status().g92 == initial.g92 &&
              port.status().tool_offset == initial.tool_offset &&
              std::ranges::contains(port.status().active_codes, "G91"),
          "feature cycles preserve Z, tool compensation, G92 and distance mode");
  mdi("G54");
  require(port.status().g5x == g54, "feature probing changes only the active work coordinate system");
}

// Every feature cycle in a rotated G55 (#245). The simulated parts are square to the work axes:
// HAL turns the commanded machine XY into the frame (u, v) of the work axes, about the machine
// origin, and the windows lie in that frame. Ball-centre contacts:
//   hollow (pocket, inside corner, slot, bore): trips outside u -5..13, v -9..5;
//   solid (corner, boss, ridge): trips inside u -5..13, v -9..5 below Z 12;
//   edge (angle): trips where v + g*u >= 6 (front) or u + g*v >= 6 (left), g = -0.05.
// Each result is the work origin in the frame (where the work system reads X0 Y0) and the size.
// With `measure` (#247), every cycle runs measure-only first: it must write nothing and report
// the expected point and faces. Applying that measurement must then set the same origin.
void rotated_scenarios(NmlPort& port, double scale, double degrees, const auto& mdi,
                       bool measure = false, const std::string& tool_table = {},
                       const std::string& programs = {}, const std::string& runout_file = {},
                       bool geometry_only = false) {
  // The MDI blocks written since `from` that write an offset or the tool table: G10.
  const auto g10_since = [&](std::size_t from) {
    const auto all = port.writes();
    return std::count_if(all.begin() + static_cast<std::ptrdiff_t>(std::min(from, all.size())), all.end(),
                         [](const NmlPort::Written& written) { return written.mdi.contains("G10"); });
  };
  const auto table_text = [&] {
    return read_text_file(tool_table, std::size_t{1} << 20).value_or(std::string{});
  };
  checked(port.set_tool({98, 98, 50, 0, "Setter reference"}, {}));
  checked(port.set_tool({99, 99, 45, 2, "Ruby probe"}, {}));
  constexpr double none = std::numeric_limits<double>::quiet_NaN();
  const double turn = degrees * std::numbers::pi / 180;
  const double c = std::cos(turn);
  const double s = std::sin(turn);
  const auto machine = [&] {
    const auto axes = port.status().controller.axes;
    return std::array{axes[0].machine, axes[1].machine, axes[2].machine};
  };
  const auto to_frame = [&](const std::array<double, 3>& m) {
    return std::array{c * m[0] + s * m[1], -s * m[0] + c * m[1], m[2]};
  };
  // The frame position of the active work system's X0 Y0.
  const auto origin_of = [&](const NmlStatus& status) {
    const auto& axes = status.controller.axes;
    const auto f = to_frame({axes[0].machine, axes[1].machine, axes[2].machine});
    return std::array{f[0] - status.work[0], f[1] - status.work[1]};
  };
  const auto at = [&](double u, double v) {
    mdi(std::format("G21 G90 G53 G0 X{:.8f} Y{:.8f} Z20", c * u - s * v, s * u + c * v));
    mdi(scale == 1 ? "G21 G91" : "G20 G91");
  };
  SetterEpoch epoch{1, 1, 1, 0};
  for (const int tool : {98, 99}) {
    mdi(std::format("M61 Q{}", tool));
    mdi(std::format("G43 H{}", tool));
    at(0, 0);
    ++epoch.tool;
    halcmd(std::format("setp sim-tool-setter.max {}", (tool == 98 ? 10 : 12) / scale));
    const auto touched = port.fixed_setter({SetterAction::touch_off, tool, machine()}, epoch, {});
    require(touched.has_value(),
            touched ? "the reference and probe have known lengths" : touched.error().text);
  }
  mdi("G21 G90 G54");
  const auto g54 = port.status().g5x;
  mdi(std::format("G10 L2 P2 X5 Y-7 Z2 R{}", degrees));
  mdi("G55");
  mdi("G92 X2 Y3 Z4");
  at(0, 0);
  mdi("G21 G10 L20 P0 Z8"); // Z0 at machine Z 12, the top of the solid part.
  const auto initial = port.status();
  require(std::abs(initial.rotation_xy - degrees) < 1e-9, std::format("G55 is rotated {}°", degrees));

  halcmd("loadrt sum2 names=sim-rot-u,sim-rot-v,sim-rot-edge");
  halcmd("loadrt near names=sim-rot-x,sim-rot-y,sim-rot-z");
  halcmd("loadrt logic names=sim-rot-hollow,sim-rot-solid personality=0x802,0x103");
  for (const auto* part : {"sim-rot-u", "sim-rot-v", "sim-rot-edge", "sim-rot-x", "sim-rot-y",
                           "sim-rot-z", "sim-rot-hollow", "sim-rot-solid"})
    halcmd(std::format("addf {} servo-thread", part));
  halcmd("net sim-rot-mx axis.x.pos-cmd sim-rot-u.in0 sim-rot-v.in0 sim-rot-edge.in0");
  halcmd("net sim-rot-my axis.y.pos-cmd sim-rot-u.in1 sim-rot-v.in1 sim-rot-edge.in1");
  halcmd(std::format("setp sim-rot-u.gain0 {:.12f}", c));
  halcmd(std::format("setp sim-rot-u.gain1 {:.12f}", s));
  halcmd(std::format("setp sim-rot-v.gain0 {:.12f}", -s));
  halcmd(std::format("setp sim-rot-v.gain1 {:.12f}", c));
  halcmd("net sim-rot-fu sim-rot-u.out sim-rot-x.in1");
  halcmd("net sim-rot-fv sim-rot-v.out sim-rot-y.in1");
  halcmd("net sim-setter-z sim-rot-z.in1");
  const auto window = [&](char axis, double low, double high) {
    halcmd(std::format("setp sim-rot-{}.scale 1", axis));
    halcmd(std::format("setp sim-rot-{}.in2 {}", axis, (high + low) / 2 / scale));
    halcmd(std::format("setp sim-rot-{}.difference {}", axis, (high - low) / 2 / scale));
  };
  window('x', -5, 13);
  window('y', -9, 5);
  window('z', -100, 12);
  halcmd("net sim-rot-inx sim-rot-x.out sim-rot-hollow.in-00 sim-rot-solid.in-00");
  halcmd("net sim-rot-iny sim-rot-y.out sim-rot-hollow.in-01 sim-rot-solid.in-01");
  halcmd("net sim-rot-inz sim-rot-z.out sim-rot-solid.in-02");
  const auto probe_input = [&](std::string_view net, std::string_view pin) {
    halcmd("unlinkp motion.probe-input");
    halcmd("unlinkp coffeecnc.probe-input");
    halcmd(std::format("net {} {} motion.probe-input coffeecnc.probe-input", net, pin));
  };

  // Every other way a measurement goes stale refuses it before writing (one each). `change`
  // runs between measuring and applying; `at_apply` is the session epoch the apply carries.
  // Epoch fields: page (a reconnect or a new panel session), datum (E-stop, power, homing),
  // tool (a tool change, an MDI block, a program run, step or resume, an offset edit), stop.
  const auto refused_after = [&](std::string_view name, const auto& change, SetterEpoch at_apply,
                                 std::string_view reason) {
    at(2, 1);
    const FixedSetterRequest top{.action = SetterAction::material,
                                 .tool = 99,
                                 .start = machine(),
                                 .direction = ProbeDirection::z_top,
                                 .measure_only = true};
    const auto taken = port.fixed_setter(top, epoch, {});
    if (!taken || !taken->measured)
      throw std::runtime_error(
          std::format("{}: {}", name, taken ? "no measurement" : taken.error().text));
    change();
    const auto before_apply = port.status();
    const auto writes_before = port.writes().size();
    const auto table_before = table_text();
    const auto result = port.fixed_setter({.action = SetterAction::apply_measurement,
                                           .tool = 99,
                                           .edge_measurement = taken->measured->id},
                                          at_apply, {});
    const auto after = port.status();
    std::cout << std::format("fact=measure_only action=stale_{} rotation={} refused={} writes={}",
                             name, degrees, !result, port.writes().size() - writes_before)
              << std::endl;
    require(!result && result.error().text.contains(reason) &&
                port.writes().size() == writes_before && after.g5x == before_apply.g5x &&
                after.g92 == before_apply.g92 && after.rotation_xy == before_apply.rotation_xy &&
                table_text() == table_before,
            result ? std::format("{}: expected a refused measurement", name) : result.error().text);
  };
  if (geometry_only) {
    probe_input("sim-rot-outer", "sim-rot-solid.and");
    const auto units = scale == 1 ? "G21 G91" : "G20 G91";
    mdi(units);
    // These edits come from another client. Neither the session epoch nor the applied G43
    // offset changes, so Set must check the table row and correction itself.
    const auto probe_length = tool_z(table_text(), 99);
    require(probe_length.has_value(), "T99 has a stored length");
    const auto write_table = [&](std::string_view text) {
      std::ofstream output{tool_table};
      require(static_cast<bool>(output << text << std::flush), "write the simulated tool table");
    };
    const auto edit_tool = [&](const Tool& row) {
      const auto edit = with_tool(table_text(), row, scale);
      if (!edit)
        throw std::runtime_error(edit.error().text);
      write_table(*edit);
    };
    const auto original_tool_offset = port.status().tool_offset;
    const Tool probe_row{99, 99, probe_length.value() * scale, 2, "Ruby probe"};
    auto updated = probe_row;
    updated.diameter = 3;
    refused_after("probe_diameter", [&] { edit_tool(updated); }, epoch, "work frame or tool");
    edit_tool(probe_row);
    updated = probe_row;
    updated.length += 1;
    refused_after("probe_length", [&] { edit_tool(updated); }, epoch, "work frame or tool");
    edit_tool(probe_row);
    const auto table_with_probe = table_text();
    refused_after(
        "probe_row_removed",
        [&] {
          const auto edited = without_tool(table_with_probe, 99);
          if (!edited)
            throw std::runtime_error("T99 cannot be removed from the simulated table");
          write_table(*edited);
        },
        epoch, "work frame or tool");
    write_table(table_with_probe);
    require(port.status().tool_offset == original_tool_offset,
            "editing the saved probe row leaves the applied G43 offset in effect");
    const ProbeRunout correction{.tool = 99, .table_diameter_mm = 2, .radius_mm = {0.02, -0.03}};
    const auto save_runout = [&] {
      std::ofstream output{runout_file};
      require(static_cast<bool>(output << format_probe_runout(correction) << std::flush),
              "save a runout correction in the simulation");
    };
    refused_after("runout_added", save_runout, epoch, "work frame or tool");
    refused_after(
        "runout_removed",
        [&] { require(std::filesystem::remove(runout_file), "clear the correction"); }, epoch,
        "work frame or tool");
    at(2, 1);
    const auto pending = port.fixed_setter({.action = SetterAction::material,
                                            .tool = 99,
                                            .start = machine(),
                                            .direction = ProbeDirection::z_top,
                                            .measure_only = true},
                                           epoch, {});
    if (!pending || !pending->measured)
      throw std::runtime_error(pending ? "no measurement" : pending.error().text);
    edit_tool({1, 1, 8, 2, "Unrelated cutter"});
    const auto applied = port.fixed_setter({.action = SetterAction::apply_measurement,
                                            .tool = 99,
                                            .edge_measurement = pending->measured->id},
                                           epoch, {});
    require(applied && applied->material,
            applied ? "an unrelated tool edit keeps the measurement usable" : applied.error().text);
    return;
  }

  struct Expected {
    std::string_view name;
    std::array<std::optional<double>, 2> origin; // frame; empty: the axis keeps its origin
    std::array<std::optional<double>, 2> size{};
    std::optional<double> diameter{};
  };
  const auto verify = [&](const Expected& expected, const NmlStatus& before,
                          const std::expected<SetterState, LocalizedReason>& result) {
    if (!result)
      throw std::runtime_error(
          std::format("{} at {}°: {}", expected.name, degrees, result.error().text));
    const auto after = port.status();
    const auto kept = origin_of(before);
    const auto measured = origin_of(after);
    bool ok = after.wcs == 2 && std::abs(after.rotation_xy - degrees) < 1e-9 &&
              after.g92 == initial.g92 && after.tool_offset == initial.tool_offset &&
              std::abs(after.g5x[2] - initial.g5x[2]) < 1e-6 && std::abs(machine()[2] - 20) < 0.03;
    std::string line = std::format("fact=rotated_probe action={} rotation={}", expected.name, degrees);
    for (std::size_t i = 0; i < 2; ++i) {
      const double want = expected.origin[i].value_or(kept[i]);
      ok = ok && std::abs(measured[i] - want) < 0.03;
      line += std::format(" origin_{0}_expected={1:.4f} origin_{0}_measured={2:.4f}", "uv"[i], want,
                          measured[i]);
      if (expected.size[i] || result->feature_size[i]) {
        ok = ok && expected.size[i] && result->feature_size[i] &&
             std::abs(*result->feature_size[i] - *expected.size[i]) < 0.03;
        line += std::format(" size_{0}_expected={1:.4f} size_{0}_measured={2:.4f}", "uv"[i],
                            expected.size[i].value_or(none), result->feature_size[i].value_or(none));
      }
    }
    if (expected.diameter) {
      ok = ok && result->center_diameter &&
           std::abs(*result->center_diameter - *expected.diameter) < 0.03;
      line += std::format(" diameter_expected={:.4f} diameter_measured={:.4f}", *expected.diameter,
                          result->center_diameter.value_or(none));
    }
    std::cout << line << std::endl;
    require(ok, std::format("{} probes along the {}° work axes and sets G55 in them", expected.name,
                            degrees));
  };
  const auto to_machine = [&](double u, double v) { return std::array{c * u - s * v, s * u + c * v}; };
  // A measure-only cycle: nothing written, the point and faces reported; then applied.
  const auto measure_cycle = [&](const Expected& expected, const NmlStatus& before,
                            const FixedSetterRequest& request, std::size_t faces) {
    auto probe = request;
    probe.measure_only = true;
    const auto writes = port.writes().size();
    const auto table_before = table_text();
    const auto result = port.fixed_setter(probe, epoch, {});
    if (!result || !result->measured)
      throw std::runtime_error(std::format("{} measure at {}°: {}", expected.name, degrees,
                                           result ? "no measurement" : result.error().text));
    const auto after = port.status();
    const auto& m = *result->measured;
    const auto kept = origin_of(before);
    const auto start = to_frame(request.start);
    bool ok = after.g5x == before.g5x && after.rotation_xy == before.rotation_xy &&
              after.g92 == before.g92 && !result->material && m.id != 0 && m.wcs == 2 &&
              m.faces.size() == faces && m.machine && std::abs(machine()[2] - request.start[2]) < 0.03;
    // Probing moves only: blocks were written, none of them a G10, and the tool table is the same.
    const auto g10 = g10_since(writes);
    ok = ok && port.writes().size() > writes && g10 == 0 && table_text() == table_before;
    std::string line = std::format("fact=measure_only action={} rotation={} axes={} faces={} g10_blocks={}",
                                   expected.name, degrees, m.axes, m.faces.size(), g10);
    std::array<double, 2> point{};
    for (std::size_t i = 0; i < 2; ++i) {
      point[i] = expected.origin[i].value_or(start[i]);
      if (expected.origin[i]) {
        const double work = *expected.origin[i] - kept[i];
        ok = ok && m.work[i] && std::abs(*m.work[i] - work) < 0.03;
        line += std::format(" work_{0}_expected={1:.4f} work_{0}_measured={2:.4f}", "xy"[i], work,
                            m.work[i].value_or(none));
      } else {
        ok = ok && !m.work[i];
      }
    }
    const auto machine_point = to_machine(point[0], point[1]);
    for (std::size_t i = 0; i < 2; ++i) {
      ok = ok && std::abs((*m.machine)[i] - machine_point[i]) < 0.03;
      line += std::format(" machine_{0}_expected={1:.4f} machine_{0}_measured={2:.4f}", "xy"[i],
                          machine_point[i], (*m.machine)[i]);
    }
    std::cout << line << std::endl;
    require(ok, std::format("{} measures along the {}° work axes and writes nothing", expected.name,
                            degrees));
    const auto applied = port.fixed_setter(
        {.action = SetterAction::apply_measurement, .tool = 99, .edge_measurement = m.id}, epoch, {});
    if (!applied)
      throw std::runtime_error(
          std::format("{} apply at {}°: {}", expected.name, degrees, applied.error().text));
    auto reported = *result;
    reported.material = applied->material;
    return reported;
  };
  const auto run = [&](const Expected& expected, FixedSetterRequest request, std::size_t faces = 0) {
    const auto before = port.status();
    request.tool = 99;
    request.start = machine();
    if (measure)
      verify(expected, before, measure_cycle(expected, before, request, faces));
    else
      verify(expected, before, port.fixed_setter(request, epoch, {}));
  };

  // Hollow: walls (ball contacts) at u -5 and 13, v -9 and 5; physical faces one radius out.
  probe_input("sim-rot-inner", "sim-rot-hollow.nand");
  at(2, 1);
  run({.name = "inside_corner", .origin = {-6, 6}},
      {.action = SetterAction::inside_corner, .direction = ProbeDirection::x_minus,
       .y_direction = ProbeDirection::y_plus},
      2);
  at(2, 1);
  run({.name = "pocket", .origin = {4, -2}, .size = {20, 16}}, {.action = SetterAction::pocket}, 4);
  at(2, 1);
  run({.name = "slot", .origin = {4, std::nullopt}, .size = {20, std::nullopt}},
      {.action = SetterAction::slot, .direction = ProbeDirection::x_plus}, 2);
  at(2, 1);
  run({.name = "slot_y", .origin = {std::nullopt, -2}, .size = {std::nullopt, 16}},
      {.action = SetterAction::slot, .direction = ProbeDirection::y_plus}, 2);
  at(2, 1);
  // The bore's Y chord passes through its centre: 14 mm between contacts plus the 2 mm ball.
  run({.name = "bore", .origin = {4, -2}, .diameter = 16}, {.action = SetterAction::bore}, 4);

  // Solid: contacts at u -5 and 13, v -9 and 5; the faces lie one radius inward of them.
  probe_input("sim-rot-outer", "sim-rot-solid.and");
  at(2, 1);
  run({.name = "corner", .origin = {-4, -8}},
      {.action = SetterAction::corner, .direction = ProbeDirection::x_plus,
       .y_direction = ProbeDirection::y_plus},
      2);
  // From the far corner: contacts at u 13 and v 5, faces one radius inward at 12 and 4.
  at(10, 2);
  run({.name = "corner_xminus_yminus", .origin = {12, 4}},
      {.action = SetterAction::corner, .direction = ProbeDirection::x_minus,
       .y_direction = ProbeDirection::y_minus},
      2);
  at(3, -1);
  run({.name = "boss", .origin = {4, -2}, .diameter = 12},
      {.action = SetterAction::boss, .gauge_width_mm = 16}, 4);
  at(2, 1);
  run({.name = "ridge", .origin = {std::nullopt, -2}, .size = {std::nullopt, 12}},
      {.action = SetterAction::ridge, .direction = ProbeDirection::y_plus, .gauge_width_mm = 12},
      2);

  if (measure) {
    // A single edge from beside the solid part, below its top: contact at u -5, face at u -4.
    at(-9, -2);
    mdi("G21 G90 G53 G0 Z8");
    mdi(scale == 1 ? "G21 G91" : "G20 G91");
    auto before = port.status();
    FixedSetterRequest edge_request{.action = SetterAction::material, .tool = 99, .start = machine(),
                                    .direction = ProbeDirection::x_plus};
    const auto edge = measure_cycle({.name = "edge_x", .origin = {-4, std::nullopt}}, before, edge_request, 1);
    const auto edge_after = port.status();
    std::cout << std::format("fact=measure_only action=edge_x_apply rotation={} origin_u_expected=-4.0000 "
                             "origin_u_measured={:.4f} origin_v_kept={:.4f} origin_v_measured={:.4f}",
                             degrees, origin_of(edge_after)[0], origin_of(before)[1],
                             origin_of(edge_after)[1])
              << std::endl;
    require(edge.material && edge.material->axis == 'X' &&
                std::abs(origin_of(edge_after)[0] + 4) < 0.03 &&
                std::abs(origin_of(edge_after)[1] - origin_of(before)[1]) < 0.03,
            "applying a measured X edge sets work X0 on the face and keeps Y");
    // Z top over the part: Z0 is moved 1 mm down first, so the top reads Z1 until applied.
    mdi("G21 G90 G53 G0 Z20"); // up first: the part is between here and there
    at(2, 1);
    mdi("G21 G10 L20 P0 Z9");
    mdi(scale == 1 ? "G21 G91" : "G20 G91");
    before = port.status();
    const auto top_writes = port.writes().size();
    const auto top_table = table_text();
    FixedSetterRequest top{.action = SetterAction::material, .tool = 99, .start = machine(),
                           .direction = ProbeDirection::z_top, .measure_only = true};
    const auto z = port.fixed_setter(top, epoch, {});
    if (!z || !z->measured)
      throw std::runtime_error(
          std::format("Z top measure at {}°: {}", degrees, z ? "no measurement" : z.error().text));
    const auto& zm = *z->measured;
    const auto measured_status = port.status();
    const bool unchanged = measured_status.g5x == before.g5x && measured_status.g92 == before.g92 &&
                           measured_status.rotation_xy == before.rotation_xy &&
                           g10_since(top_writes) == 0 && table_text() == top_table;
    std::cout << std::format("fact=measure_only action=z_top rotation={} axes={} work_z_expected=1.0000 "
                             "work_z_measured={:.4f} machine_z_expected=12.0000 machine_z_measured={:.4f} "
                             "g5x_g92_rotation_table_unchanged={}",
                             degrees, zm.axes, zm.work[2].value_or(none),
                             zm.machine ? (*zm.machine)[2] : none, unchanged)
              << std::endl;
    require(unchanged && zm.axes == "Z" && zm.work[2] && std::abs(*zm.work[2] - 1) < 0.03 &&
                !zm.work[0] && !zm.work[1] && zm.machine && std::abs((*zm.machine)[2] - 12) < 0.03 &&
                port.writes().size() > top_writes,
            "Z top measures the top without writing Z0");
    const auto z_applied = port.fixed_setter(
        {.action = SetterAction::apply_measurement, .tool = 99, .edge_measurement = zm.id}, epoch, {});
    const auto z_after = port.status();
    std::cout << std::format("fact=measure_only action=z_top_apply rotation={} work_z_expected=8.0000 "
                             "work_z_measured={:.4f}",
                             degrees, z_after.work[2])
              << std::endl;
    require(z_applied && z_applied->material && z_applied->material->axis == 'Z' &&
                std::abs(z_after.work[2] - 8) < 0.03 &&
                std::abs(origin_of(z_after)[0] - origin_of(before)[0]) < 0.03 &&
                std::abs(origin_of(z_after)[1] - origin_of(before)[1]) < 0.03,
            z_applied ? "applying the measured top sets Z0 there and keeps XY"
                      : z_applied.error().text);
    // A measurement is applied once; a new probe action forgets it.
    const auto again = port.fixed_setter(
        {.action = SetterAction::apply_measurement, .tool = 99, .edge_measurement = zm.id}, epoch, {});
    require(!again && again.error().text.contains("Probe the feature again"),
            "a measurement cannot be applied twice");
    // A changed work origin refuses an applied measurement before writing.
    at(2, 1);
    top.start = machine();
    const auto stale = port.fixed_setter(top, epoch, {});
    if (!stale || !stale->measured)
      throw std::runtime_error(stale ? "no measurement" : stale.error().text);
    mdi("G21 G10 L20 P0 Z7");
    mdi(scale == 1 ? "G21 G91" : "G20 G91");
    const auto stale_writes = port.writes().size();
    const auto stale_origin = port.status().g5x;
    const auto refused = port.fixed_setter(
        {.action = SetterAction::apply_measurement, .tool = 99, .edge_measurement = stale->measured->id},
        epoch, {});
    require(!refused && refused.error().text.contains("work frame") &&
                port.writes().size() == stale_writes && port.status().g5x == stale_origin,
            "a measurement taken in another work frame is refused before writing");
    const auto units = scale == 1 ? "G21 G91" : "G20 G91";
    refused_after("tool_offset", [&] { mdi("G43 H98"); }, epoch, "work frame or tool");
    mdi("G43 H99");
    refused_after("tool_change", [&] { mdi("M61 Q98"); }, epoch, "Load T99");
    mdi("M61 Q99");
    mdi("G43 H99");
    refused_after("g92", [&] { mdi("G92 X1"); mdi(units); }, epoch, "work frame or tool");
    refused_after("g52", [&] { mdi("G52 X1"); mdi(units); }, epoch, "work frame or tool");
    refused_after("rotation", [&] { mdi(std::format("G21 G10 L2 P2 R{}", degrees + 1)); mdi(units); },
                  epoch, "work frame or tool");
    mdi(std::format("G21 G10 L2 P2 R{}", degrees));
    mdi(units);
    const auto none_changed = [] {};
    auto page = epoch;
    ++page.page;
    refused_after("epoch_page", none_changed, page, "work frame or tool");
    auto datum = epoch;
    ++datum.datum;
    refused_after("epoch_datum", none_changed, datum, "work frame or tool");
    auto tool_epoch = epoch;
    ++tool_epoch.tool;
    refused_after("epoch_tool", none_changed, tool_epoch, "work frame or tool");
    auto stop = epoch;
    ++stop.stop;
    refused_after("epoch_stop", none_changed, stop, "work frame or tool");
    // A new probe action forgets the measurement before it: the first ID is refused, the second
    // is applied.
    at(2, 1);
    top.start = machine();
    const auto first = port.fixed_setter(top, epoch, {});
    at(2, 1);
    top.start = machine();
    const auto second = port.fixed_setter(top, epoch, {});
    if (!first || !first->measured || !second || !second->measured)
      throw std::runtime_error("two measurements expected");
    const auto forgotten_writes = port.writes().size();
    const auto forgotten = port.fixed_setter(
        {.action = SetterAction::apply_measurement, .tool = 99, .edge_measurement = first->measured->id},
        epoch, {});
    const auto forgotten_program_writes = g10_since(forgotten_writes);
    const auto latest = port.fixed_setter(
        {.action = SetterAction::apply_measurement, .tool = 99, .edge_measurement = second->measured->id},
        epoch, {});
    std::cout << std::format(
                     "fact=measure_only action=forgotten rotation={} first_refused={} writes={} "
                     "second_applied={}",
                     degrees, !forgotten, forgotten_program_writes, latest.has_value())
              << std::endl;
    require(!forgotten && forgotten.error().text.contains("Probe the feature again") &&
                forgotten_program_writes == 0 && latest.has_value() &&
                first->measured->id != second->measured->id,
            "a new probe action forgets the measurement before it");
    // An action that always writes is refused as a measurement before anything is sent.
    for (const auto action : {SetterAction::ball, SetterAction::touch_off, SetterAction::check}) {
      const auto writes_before = port.writes().size();
      FixedSetterRequest writer{.action = action, .tool = 99, .start = machine(), .measure_only = true};
      writer.gauge_width_mm = 20;
      const auto refused_writer = port.fixed_setter(writer, epoch, {});
      require(!refused_writer && refused_writer.error().text.contains("always writes its result") &&
                  port.writes().size() == writes_before,
              "the ball calibration and the tool setter refuse measure-only natively too");
    }
    mdi("G92.1");
    mdi("G21 G90 G53 G0 X0 Y0 Z20");
    mdi("G21 G10 L20 P0 Z8");
    mdi("G54");
    require(port.status().g5x == g54, "measuring and applying change only the active work system");
    return;
  }

  // Ball calibration stays refused in a rotated system, before any command.
  at(2, 1);
  const auto writes_before_ball = port.writes().size();
  const auto origin_before_ball = port.status().g5x;
  FixedSetterRequest ball{SetterAction::ball, 99, machine()};
  ball.gauge_width_mm = 20;
  const auto refused_ball = port.fixed_setter(ball, epoch, {});
  std::cout << std::format("fact=rotated_probe action=ball rotation={} refused={} writes={}", degrees,
                           !refused_ball, port.writes().size() - writes_before_ball)
            << std::endl;
  require(!refused_ball && refused_ball.error().text.contains("without rotation") &&
              port.writes().size() == writes_before_ball && port.status().g5x == origin_before_ball,
          refused_ball ? "expected a refused ball calibration" : refused_ball.error().text);

  // The travel check is on the machine endpoint: 1 mm inside the Y limit, the +X work axis
  // leaves it although the X search alone would not.
  // Two moves: each settles within the 6 s wait at 30 mm/s.
  const double edge_y = s > 0 ? 249 : -249;
  mdi(std::format("G21 G90 G53 G0 X0 Y{} Z20", edge_y / 2));
  mdi(std::format("G21 G90 G53 G0 X0 Y{} Z20", edge_y));
  const auto writes = port.writes().size();
  const auto origin = port.status().g5x;
  const auto outside = port.fixed_setter({SetterAction::bore, 99, machine()}, epoch, {});
  std::cout << std::format("fact=rotated_probe action=travel start_y={} endpoint_y={:.4f} "
                           "limit_y={} writes={}",
                           edge_y, edge_y + 20 * s, s > 0 ? 250 : -250,
                           port.writes().size() - writes)
            << std::endl;
  require(!outside && outside.error().text.contains("outside the configured travel") &&
              port.writes().size() == writes && port.status().g5x == origin,
          outside ? "expected a travel refusal" : outside.error().text);
  mdi(std::format("G21 G90 G53 G0 X0 Y{} Z20", edge_y / 2));
  mdi("G21 G90 G53 G0 X0 Y0 Z20");

  // Edge angle: the edge turns atan(0.05) from the work axis, measured from the rotated axes.
  halcmd("unlinkp sim-tool-setter.in");
  halcmd("net sim-rot-edge-normal sim-rot-edge.out sim-tool-setter.in");
  halcmd(std::format("setp sim-tool-setter.min {}", 6 / scale));
  halcmd(std::format("setp sim-tool-setter.max {}", 100 / scale));
  probe_input("sim-setter-trip", "");
  constexpr double g = -0.05;
  // The edge turns atan(-g) from the axes of a system rotated `frame` degrees.
  const auto edge = [&](ProbeDirection direction, double frame) {
    const double fc = std::cos(frame * std::numbers::pi / 180);
    const double fs = std::sin(frame * std::numbers::pi / 180);
    // front: v + g u = (-s + g c) x + (c + g s) y; left: u + g v = (c - g s) x + (s + g c) y.
    const bool front = direction == ProbeDirection::y_plus;
    halcmd(std::format("setp sim-rot-edge.gain0 {:.12f}", front ? -fs + g * fc : fc - g * fs));
    halcmd(std::format("setp sim-rot-edge.gain1 {:.12f}", front ? fc + g * fs : fs + g * fc));
  };
  const double residual = std::atan(-g) * 180 / std::numbers::pi;
  const auto angle = [&](ProbeDirection direction, double expected) {
    mdi("G21 G90 G53 G0 X0 Y0 Z20");
    mdi(scale == 1 ? "G21" : "G20");
    const double rotation = port.status().rotation_xy;
    FixedSetterRequest request{SetterAction::angle, 99, machine(), direction};
    request.gauge_width_mm = 20;
    const auto measured = port.fixed_setter(request, epoch, {});
    if (!measured)
      throw std::runtime_error(std::format("angle at {}°: {}", rotation, measured.error().text));
    std::cout << std::format("fact=rotated_probe action=angle edge={} rotation={:.4f} angle_expected={:.4f} "
                             "angle_measured={:.4f} apply_expected={:.4f} apply_offered={:.4f}",
                             direction == ProbeDirection::y_plus ? "front" : "left", rotation,
                             expected, measured->edge_angle_deg.value_or(none),
                             wrapped_rotation(rotation + expected),
                             measured->edge_rotation ? measured->edge_rotation->rotation_deg : none)
              << std::endl;
    require(measured->edge_angle_deg && measured->edge_rotation &&
                std::abs(*measured->edge_angle_deg - expected) < 0.05 &&
                std::abs(measured->edge_rotation->rotation_deg - wrapped_rotation(rotation + expected)) <
                    0.05 &&
                port.status().rotation_xy == rotation,
            "the edge angle is measured from the rotated work axis and not written");
    // Back clear of the edge before its simulation changes.
    mdi("G21 G90 G53 G0 X0 Y0 Z20");
    return *measured;
  };
  edge(ProbeDirection::x_plus, degrees);
  static_cast<void>(angle(ProbeDirection::x_plus, -residual));
  edge(ProbeDirection::y_plus, degrees);
  const auto front = angle(ProbeDirection::y_plus, residual);
  // The feature cycles above moved the G55 origin: applying keeps it where they left it.
  const auto unturned = port.status().g5x;
  const auto applied = port.fixed_setter(
      {.action = SetterAction::apply_angle, .tool = 99, .edge_measurement = front.edge_rotation->id},
      epoch, {});
  const auto turned = port.status();
  std::cout << std::format("fact=rotated_probe action=apply_angle rotation_expected={:.4f} rotation_measured={:.4f}",
                           degrees + residual, turned.rotation_xy)
            << std::endl;
  require(applied && applied->edge_angle_deg &&
              std::abs(*applied->edge_angle_deg - (degrees + residual)) < 0.05 &&
              std::abs(turned.rotation_xy - (degrees + residual)) < 0.05 &&
              std::ranges::equal(turned.g5x, unturned,
                                 [](double a, double b) { return std::abs(a - b) < 1e-6; }),
          applied ? "applying adds the measured turn to the present rotation"
                  : applied.error().text);
  // Square to the edge now: the remaining turn is zero.
  static_cast<void>(angle(ProbeDirection::y_plus, 0));

  // Near 180°: 179 + 2.8624 is written as -178.1376, inside the panel's ±360° limit.
  constexpr double near_half_turn = 179;
  mdi(std::format("G21 G10 L2 P2 R{}", near_half_turn));
  edge(ProbeDirection::y_plus, near_half_turn);
  const auto wide = angle(ProbeDirection::y_plus, residual);
  const auto wrapped = port.fixed_setter(
      {.action = SetterAction::apply_angle, .tool = 99, .edge_measurement = wide.edge_rotation->id},
      epoch, {});
  const double wrapped_expected = near_half_turn + residual - 360;
  std::cout << std::format("fact=rotated_probe action=apply_angle_wrap rotation_expected={:.4f} "
                           "rotation_measured={:.4f}",
                           wrapped_expected, port.status().rotation_xy)
            << std::endl;
  require(wrapped && std::abs(port.status().rotation_xy - wrapped_expected) < 0.05,
          wrapped ? "applying past 180° writes the same turn inside -180..180°"
                  : wrapped.error().text);
  static_cast<void>(angle(ProbeDirection::y_plus, 0));
  // A stop mid-cycle drops the machine's enable: the cycle has turned the feed override (M50)
  // and adaptive feed (M52) off, and switched to G90 once it has sent a move. Once the machine
  // can take MDI, each item comes back once, only while it is what the cycle left and no
  // command ran since; the operator is told about the rest.
  // Last: it homes the machine again, and the edge rig above expects the first homing.
  probe_input("sim-rot-inner", "sim-rot-hollow.nand");
  {
    const auto codes = [](const NmlStatus& status) {
      return std::ranges::contains(status.active_codes, "G91") ? "G91" : "G90";
    };
    const auto last_write = [&] {
      const auto all = port.writes();
      return all.empty() ? NmlPort::Written{} : all.back();
    };
    const auto idle = [&] { port.idle_maintenance([] { return true; }); };
    const auto state = [&](const NmlStatus& status) {
      return std::format("{} m50={} m52={}", codes(status), status.feed_override_enabled,
                         status.adaptive_feed_enabled);
    };
    // The panel messages since the last call, joined.
    const auto told = [&] {
      std::string text;
      for (const auto& message : port.take_messages())
        text += message.text + " | ";
      return text;
    };
    // G91, M50 P1 and M52 P1, a pocket cycle stopped 2.2 s in (in its first search), the
    // machine on again in MDI: what the cycle left.
    const auto stop_cycle = [&](std::string_view name) {
      at(2, 1);
      mdi("M50 P1");
      mdi("M52 P1");
      const auto before = port.status();
      const auto stop_at = std::chrono::steady_clock::now() + 2200ms;
      const auto stopped = port.fixed_setter({.action = SetterAction::pocket, .tool = 99, .start = machine()},
                                             epoch, [&] { return std::chrono::steady_clock::now() < stop_at; });
      const auto off = port.status();
      checked(port.set_machine_state(MachineState::off, {}));
      checked(port.set_machine_state(MachineState::on, {}));
      checked(port.set_task_mode(TaskMode::mdi, {}));
      const auto on = port.status();
      static_cast<void>(told());
      std::cout << std::format("fact=probe_abort case={} rotation={} stopped={} machine_on_after_stop={} "
                               "before=\"{}\" at_machine_on=\"{}\"",
                               name, degrees, !stopped, off.controller.machine == MachineState::on,
                               state(before), state(on))
                << std::endl;
      require(!stopped && std::ranges::contains(before.active_codes, "G91") &&
                  before.feed_override_enabled && before.adaptive_feed_enabled &&
                  !on.feed_override_enabled && !on.adaptive_feed_enabled,
              "a stopped pocket cycle leaves M50 and M52 off");
      return on;
    };

    // 1. An operator MDI block after the stop, the same distance mode the cycle left: nothing
    //    is restored, and the operator is told what the cycle left.
    auto left = stop_cycle("operator_mdi");
    mdi(codes(left));
    auto written = last_write();
    idle();
    auto message = told();
    idle();
    auto now = port.status();
    std::cout << std::format("fact=probe_abort case=operator_mdi restore_blocks={} after=\"{}\" "
                             "message=\"{}\"",
                             last_write() == written ? 0 : 1, state(now), message)
              << std::endl;
    require(last_write() == written && state(now) == state(left) &&
                message.contains("did not restore it") && message.contains("M50 P0") &&
                message.contains("M52 P0"),
            "an operator MDI block after the stop drops the restore and says so");

    // 2. A program runs before the restore: its M2 sets G90 and M50 on. Nothing is sent.
    stop_cycle("program_ran");
    {
      const auto path = programs + "/coffeecnc-probe-abort.ngc";
      {
        std::ofstream out{path};
        out << "G90\nM2\n";
      }
      checked(port.set_task_mode(TaskMode::automatic, {}));
      checked(port.open_program(path, {}));
      checked(port.run({}));
      wait_for(port, [](const NmlStatus& current) {
        return current.controller.interp == InterpState::idle && current.controller.in_position &&
               !current.task_executing;
      });
      checked(port.set_task_mode(TaskMode::mdi, {}));
    }
    const auto after_program = port.status();
    written = last_write();
    idle();
    message = told();
    idle();
    now = port.status();
    std::cout << std::format("fact=probe_abort case=program_ran after_program=\"{}\" restore_blocks={} "
                             "after=\"{}\" message=\"{}\"",
                             state(after_program), last_write() == written ? 0 : 1, state(now), message)
              << std::endl;
    require(last_write() == written && state(now) == state(after_program) &&
                message.contains("did not restore it"),
            "a program run before the restore suppresses it and says so");

    // 3. Another client sets G91 only after the stop (as AXIS or an ON_ABORT_COMMAND could):
    //    M50 and M52 are still as the cycle left them and come back; the distance mode stays
    //    the client's, and the operator is told what the cycle left there.
    left = stop_cycle("g91_by_another_client");
    const bool left_g90 = !std::ranges::contains(left.active_codes, "G91");
    if (std::system("python3 -c \"import linuxcnc; c = linuxcnc.command(); c.mode(linuxcnc.MODE_MDI); "
                    "c.wait_complete(); c.mdi('G91'); c.wait_complete()\"") != 0)
      throw std::runtime_error("the second command client could not send G91");
    wait_for(port, [](const NmlStatus& current) {
      return std::ranges::contains(current.active_codes, "G91") &&
             current.controller.interp == InterpState::idle;
    });
    written = last_write();
    idle();
    message = told();
    now = port.status();
    std::cout << std::format("fact=probe_abort case=g91_by_another_client left_g90={} restore_blocks={} "
                             "after=\"{}\" message=\"{}\"",
                             left_g90, last_write() == written ? 0 : 1, state(now), message)
              << std::endl;
    require(std::ranges::contains(now.active_codes, "G91") && now.feed_override_enabled &&
                now.adaptive_feed_enabled && message.contains("Restored the distance mode") &&
                (!left_g90 || (message.contains("left G90 F") && message.contains("did not restore it"))),
            "G91 set after the stop keeps M50 and M52 restored and tells about G90 and F");

    // 4. A refused switch to MDI backs off: the next pass within a second sends nothing, a
    //    pass after it restores.
    stop_cycle("mode_refused");
    checked(port.set_task_mode(TaskMode::manual, {}));
    int asked = 0;
    port.idle_maintenance([&] { return ++asked == 1; }); // the switch to MDI is withdrawn
    const auto refused_message = told();
    written = last_write();
    idle();
    const auto backed_off = port.status();
    const bool sent_while_backing_off = last_write() != written;
    // Status stays polled meanwhile, as the session does: an unread status channel reads as stale.
    for (const auto until = std::chrono::steady_clock::now() + 1200ms; std::chrono::steady_clock::now() < until;) {
      static_cast<void>(port.status());
      std::this_thread::sleep_for(20ms);
    }
    const auto ready_before = port.status();
    written = last_write();
    idle();
    const bool sent_after = last_write() != written;
    now = port.status();
    message = told();
    std::cout << std::format("fact=probe_abort case=mode_refused_retry live={} sent={} mode={} interp_idle={} "
                             "in_position={} executing={} queued={} paused={} requested_tool={} "
                             "homed={} mode_after={}",
                             ready_before.controller.link == Link::live, sent_after,
                             static_cast<int>(ready_before.controller.mode),
                             ready_before.controller.interp == InterpState::idle,
                             ready_before.controller.in_position, ready_before.task_executing,
                             ready_before.queued_mdi, ready_before.paused,
                             ready_before.controller.requested_tool.has_value(),
                             std::ranges::all_of(ready_before.controller.axes, [](auto axis) { return axis.homed; }),
                             static_cast<int>(now.controller.mode))
              << std::endl;
    std::cout << std::format("fact=probe_abort case=mode_refused asked={} refused_message=\"{}\" "
                             "while_backing_off=\"{}\" mode_manual={} sent_while_backing_off={} after=\"{}\" "
                             "message=\"{}\"",
                             asked, refused_message, state(backed_off),
                             backed_off.controller.mode == TaskMode::manual, sent_while_backing_off,
                             state(now), message)
              << std::endl;
    require(!sent_while_backing_off && backed_off.controller.mode == TaskMode::manual &&
                !backed_off.feed_override_enabled && now.feed_override_enabled &&
                now.adaptive_feed_enabled && message.contains("Restored the distance mode"),
            "a refused switch to MDI waits a second, then the restore runs");
    // The restore switched back to the operator's manual mode.
    checked(port.set_task_mode(TaskMode::mdi, {}));

    // 5. Homing: the restore waits while an axis homes, then runs once; a second pass sends nothing.
    const auto before = stop_cycle("homing_gate");
    checked(port.set_task_mode(TaskMode::manual, {}));
    checked(port.home({}, {}));
    wait_for(port, [](const NmlStatus& current) {
      return std::ranges::any_of(current.controller.axes, [](const auto& axis) { return axis.homing; });
    });
    written = last_write();
    idle();
    const auto homing = port.status();
    const bool pending_while_homing = last_write() == written;
    wait_for(port, [](const NmlStatus& current) {
      return std::ranges::all_of(current.controller.axes, [](auto axis) { return axis.homed && !axis.homing; }) &&
             current.controller.in_position;
    });
    idle();
    const auto restored = port.status();
    message = told();
    written = last_write();
    idle();
    const bool second = last_write() != written;
    std::cout << std::format("fact=probe_abort case=homing_gate while_homing=\"{}\" pending_while_homing={} "
                             "restored=\"{}\" restored_message={} second_restore_blocks={}",
                             state(homing), pending_while_homing, state(restored),
                             message.contains("Restored the distance mode"), second ? 1 : 0)
              << std::endl;
    require(pending_while_homing && !homing.feed_override_enabled && !homing.adaptive_feed_enabled,
            "the restore waits while an axis homes");
    require(std::ranges::contains(restored.active_codes, "G91") && restored.feed_override_enabled &&
                restored.adaptive_feed_enabled && restored.g5x == before.g5x && restored.g92 == before.g92 &&
                message.contains("Restored the distance mode") && !second,
            "after homing the stopped cycle's G91, M50 and M52 come back once");
    checked(port.set_task_mode(TaskMode::mdi, {}));
  }
  mdi("M52 P0");

  mdi("G54");
  require(port.status().g5x == g54 && port.status().rotation_xy == 0,
          "rotated probing changes only the active work coordinate system");
}

// Stylus runout (#247). The simulated ball sits e = (+0.05, -0.03) mm off the spindle axis and
// triggers at 0.99 mm, though the table says Ø2: HAL trips where the ball centre, the commanded
// spindle position plus e, is 9.01 mm from the ring centre C = machine (4, -2), a Ø20 ring; and,
// for the pin, below Z 12 within 6.99 mm of C, a Ø12 pin. The trigger radius correction is then
// -0.01 mm on each axis, less the latch stopping distance. The eccentricity is not corrected
// (no spindle orientation): every result sees the surfaces around C - e, where the spindle is
// when the ball centre is on C.
void runout_scenarios(std::unique_ptr<NmlPort>& port, HalShared& shared, double scale,
                      double degrees, std::string_view gauge, const std::string& runout_file,
                      const auto& mdi, const auto& reconnect) {
  checked(port->set_tool({98, 98, 50, 0, "Setter reference"}, {}));
  checked(port->set_tool({99, 99, 45, 2, "Ruby probe"}, {}));
  constexpr double none = std::numeric_limits<double>::quiet_NaN();
  constexpr std::array<double, 2> centre{4, -2};
  constexpr std::array<double, 2> eccentricity{0.05, -0.03};
  constexpr std::array<double, 2> seen{centre[0] - eccentricity[0], centre[1] - eccentricity[1]};
  // Compensated results lie within this of the true geometry. Without the correction each face
  // is 0.008 to 0.010 mm off, and a width or diameter twice that.
  constexpr double tight = 0.004;
  const auto machine = [&] {
    const auto axes = port->status().controller.axes;
    return std::array{axes[0].machine, axes[1].machine, axes[2].machine};
  };
  const auto at = [&](double x, double y, double z) {
    mdi(std::format("G21 G90 G53 G0 Z{:.8f}", std::max(z, machine()[2])));
    mdi(std::format("G21 G90 G53 G0 X{:.8f} Y{:.8f}", x, y));
    mdi(std::format("G21 G90 G53 G0 Z{:.8f}", z));
    mdi(scale == 1 ? "G21 G91" : "G20 G91");
  };
  SetterEpoch epoch{1, 1, 1, 0};
  const auto load = [&](int tool) {
    mdi(std::format("M61 Q{}", tool));
    mdi(std::format("G43 H{}", tool));
    ++epoch.tool;
  };
  for (const int tool : {98, 99}) {
    load(tool);
    at(0, 0, 20);
    halcmd(std::format("setp sim-tool-setter.max {}", (tool == 98 ? 10 : 12) / scale));
    const auto touched = port->fixed_setter({SetterAction::touch_off, tool, machine()}, epoch, {});
    require(touched.has_value(),
            touched ? "the reference and probe have known lengths" : touched.error().text);
  }
  // G55: X0 Y0 at the ring centre, Z0 at machine Z 12.
  mdi("G21 G90 G54");
  const auto g54 = port->status().g5x;
  mdi(std::format("G10 L2 P2 X{} Y{} R0", centre[0], centre[1]));
  mdi("G55");
  at(4, -2, 20);
  mdi("G21 G10 L20 P0 Z8");

  // comp, wcomp and and2 belong to the machine's HAL: windows with near, gates with logic.
  halcmd("loadrt sum2 names=sim-run-du,sim-run-dv,sim-run-r2");
  halcmd("loadrt mult2 names=sim-run-du2,sim-run-dv2");
  halcmd("loadrt near names=sim-run-ring,sim-run-pin,sim-run-z");
  halcmd("loadrt logic names=sim-run-hollow,sim-run-solid personality=0x801,0x102");
  for (const auto* part : {"sim-run-du", "sim-run-dv", "sim-run-du2", "sim-run-dv2", "sim-run-r2",
                           "sim-run-ring", "sim-run-pin", "sim-run-z", "sim-run-hollow", "sim-run-solid"})
    halcmd(std::format("addf {} servo-thread", part));
  halcmd("net sim-run-mx axis.x.pos-cmd sim-run-du.in0");
  halcmd("net sim-run-my axis.y.pos-cmd sim-run-dv.in0");
  halcmd("setp sim-run-du.gain1 0");
  halcmd("setp sim-run-dv.gain1 0");
  halcmd(std::format("setp sim-run-du.offset {:.12f}", (eccentricity[0] - centre[0]) / scale));
  halcmd(std::format("setp sim-run-dv.offset {:.12f}", (eccentricity[1] - centre[1]) / scale));
  halcmd("net sim-run-u sim-run-du.out sim-run-du2.in0 sim-run-du2.in1");
  halcmd("net sim-run-v sim-run-dv.out sim-run-dv2.in0 sim-run-dv2.in1");
  halcmd("net sim-run-u2 sim-run-du2.out sim-run-r2.in0");
  halcmd("net sim-run-v2 sim-run-dv2.out sim-run-r2.in1");
  // r² within [0, R²]: inside the ring's contact circle (9.01) or the pin's (6.99).
  halcmd("net sim-run-rr sim-run-r2.out sim-run-ring.in1 sim-run-pin.in1");
  for (const auto& [name, contact] : {std::pair{"sim-run-ring", 9.01}, std::pair{"sim-run-pin", 6.99}}) {
    const double r2 = contact * contact / (scale * scale);
    halcmd(std::format("setp {}.in2 {:.12f}", name, r2 / 2));
    halcmd(std::format("setp {}.difference {:.12f}", name, r2 / 2));
    halcmd(std::format("setp {}.scale 1", name));
  }
  // Below the pin top: Z within [-100, 12].
  halcmd("net sim-setter-z sim-run-z.in1");
  halcmd(std::format("setp sim-run-z.in2 {:.12f}", -44 / scale));
  halcmd(std::format("setp sim-run-z.difference {:.12f}", 56 / scale));
  halcmd("setp sim-run-z.scale 1");
  halcmd("net sim-run-in-ring sim-run-ring.out sim-run-hollow.in-00");
  halcmd("net sim-run-in-pin sim-run-pin.out sim-run-solid.in-00");
  halcmd("net sim-run-low sim-run-z.out sim-run-solid.in-01");
  const auto probe_input = [&](std::string_view net, std::string_view pin) {
    halcmd("unlinkp motion.probe-input");
    halcmd("unlinkp coffeecnc.probe-input");
    halcmd(std::format("net {} {} motion.probe-input coffeecnc.probe-input", net, pin));
  };
  const auto ring = [&] { probe_input("sim-run-wall", "sim-run-hollow.nand"); };
  const auto pin = [&] { probe_input("sim-run-boss", "sim-run-solid.and"); };
  const auto status = [&] { return port->fixed_setter({SetterAction::status}, epoch, {}); };
  const auto saved = [&] {
    // Line by line: GCC 14 -O3 flags istreambuf_iterator with -Wnull-dereference.
    std::ifstream input{runout_file};
    std::string text;
    for (std::string line; std::getline(input, line);)
      text += line + '\n';
    return text;
  };
  struct Value {
    std::string_view key;
    double expected;
    double measured;
  };
  // One fact line of expected and measured values, all within `tolerance`.
  const auto expect = [](std::string_view name, const std::vector<Value>& values, double tolerance) {
    std::string line = std::format("fact=runout action={}", name);
    bool ok = true;
    for (const auto& value : values) {
      line += std::format(" {0}_expected={1:.4f} {0}_measured={2:.4f}", value.key, value.expected,
                          value.measured);
      ok = ok && std::abs(value.measured - value.expected) < tolerance;
    }
    std::cout << line << std::endl;
    require(ok, std::format("{} gives the expected result", name));
  };
  // A calibration from machine (x, y, Z 20): the trigger radius correction it saves.
  const auto calibrate = [&](std::string_view name, bool outside, std::array<double, 2> from) {
    at(from[0], from[1], 20);
    FixedSetterRequest request{.action = SetterAction::runout, .tool = 99, .start = machine()};
    request.gauge_width_mm = outside ? 12 : 20;
    request.outside = outside;
    const auto result = port->fixed_setter(request, epoch, {});
    if (!result || !result->runout)
      throw std::runtime_error(
          std::format("{} runout: {}", name, result ? "not reported" : result.error().text));
    const auto& runout = *result->runout;
    expect(name, {{"radius_x", -0.01, runout.radius_mm[0]}, {"radius_y", -0.01, runout.radius_mm[1]},
                  {"table_diameter", 2, runout.table_diameter_mm}},
           tight);
    require(runout.tool == 99 && saved() == format_probe_runout(runout),
            std::format("{} saves the correction it reports", name));
    return runout.radius_mm;
  };
  struct Done {
    SetterState state;
    std::array<double, 2> origin;
  };
  // A probe cycle in G56, a copy of G55 turned `turn` degrees, from machine (x, y, Z 20): its
  // result and the G56 XY origin it leaves, in machine mm.
  const auto cycle = [&](std::string_view name, FixedSetterRequest request, std::array<double, 2> from,
                         double turn = 0) -> Done {
    mdi(std::format("G21 G10 L2 P3 X{} Y{} Z{} R{}", centre[0], centre[1], port->status().g5x[2], turn));
    mdi("G56");
    at(from[0], from[1], 20);
    request.tool = 99;
    request.start = machine();
    const auto result = port->fixed_setter(request, epoch, {});
    if (!result)
      throw std::runtime_error(std::format("{}: {}", name, result.error().text));
    const auto after = port->status();
    mdi("G55");
    return {*result, {after.g5x[0], after.g5x[1]}};
  };
  const auto bore = [&](std::string_view name, double turn, double diameter, double tolerance) {
    const auto done = cycle(name, {.action = SetterAction::bore}, {5, -1.5}, turn);
    expect(name, {{"centre_x", seen[0], done.origin[0]}, {"centre_y", seen[1], done.origin[1]},
                  {"diameter", diameter, done.state.center_diameter.value_or(none)}},
           tolerance);
  };
  // A request that fails, before any motion unless `moves`, and leaves the saved correction.
  const auto refused = [&](std::string_view name, FixedSetterRequest request, std::string_view reason,
                           bool moves = false) {
    const auto file = saved();
    const auto writes = port->writes().size();
    request.tool = 99;
    request.start = machine();
    const auto result = port->fixed_setter(request, epoch, {});
    std::cout << std::format("fact=runout action={} refused={} writes={} file_kept={}", name, !result,
                             port->writes().size() - writes, saved() == file)
              << std::endl;
    require(!result && result.error().text.contains(reason) &&
                (moves || port->writes().size() == writes) && saved() == file,
            result ? std::format("{} is refused", name) : result.error().text);
  };

  // Three runs, each within the sim's 480 s: the ring (bores, ring features, refusals, abort),
  // the pin (pin features, measure-only, tool mismatch, travel, miss, ball, Clear) and the
  // three-point bore and boss with a known correction.
  if (gauge == "ring") {
    ring();
    // Uncompensated: the diameter large by twice the trigger radius left over.
    bore("bore_uncompensated", 0, 20.02, 0.01);
    const auto ring_radius = calibrate("ring", false, {5, -1.5});
    bore("bore_compensated", 0, 20, tight);
    bore("bore_compensated_rotated", degrees, 20, tight);
    const auto kept = status();
    require(kept && kept->runout && kept->runout->radius_mm == ring_radius, "status reports the correction in use");

    // Every lateral result in the ring is corrected, along its travel.
    const auto edge = cycle("edge", {.action = SetterAction::material, .direction = ProbeDirection::x_plus}, seen);
    expect("edge_x_plus", {{"face_x", seen[0] + 10, edge.origin[0]}}, tight);
    const auto pocket = cycle("pocket", {.action = SetterAction::pocket}, seen);
    expect("pocket", {{"centre_x", seen[0], pocket.origin[0]}, {"centre_y", seen[1], pocket.origin[1]},
                      {"size_x", 20, pocket.state.feature_size[0].value_or(none)},
                      {"size_y", 20, pocket.state.feature_size[1].value_or(none)}},
           tight);
    const auto slot = cycle("slot", {.action = SetterAction::slot, .direction = ProbeDirection::x_plus}, seen);
    expect("slot_x", {{"centre_x", seen[0], slot.origin[0]}, {"width", 20, slot.state.feature_size[0].value_or(none)}},
           tight);

    // Refused, the saved correction kept: a diameter typo of 0.1 mm after the contacts (the
    // radius part is then more than 0.05 mm); before any motion a ring beyond the search travel
    // and a rotated work system.
    at(5, -1.5, 20);
    refused("ring_typo", {.action = SetterAction::runout, .gauge_width_mm = 19.9}, "more than 0.05 mm", true);
    require(status().value().runout == kept->runout, "a refused calibration keeps the correction");
    refused("ring_beyond_travel", {.action = SetterAction::runout, .gauge_width_mm = 44},
            "shorter than the ring radius");
    mdi("G21 G10 L2 P3 R10");
    mdi("G56");
    refused("ring_rotated", {.action = SetterAction::runout, .gauge_width_mm = 20}, "without rotation");
    mdi("G55");

    // Stopped part way: an abort after the third probe trip saves nothing.
    const auto file = saved();
    at(5, -1.5, 20);
    int trips = 0;
    bool tripped = false;
    FixedSetterRequest stopped{.action = SetterAction::runout, .tool = 99, .start = machine()};
    stopped.gauge_width_mm = 20;
    const auto aborted = port->fixed_setter(stopped, epoch, [&] {
      const auto pins = shared.view(HalShared::Clock::now());
      const bool now = pins && pins->probe_input && *pins->probe_input;
      trips += now && !tripped ? 1 : 0;
      tripped = now;
      return trips < 3;
    });
    std::cout << std::format("fact=runout action=abort trips={} refused={} file_kept={}", trips, !aborted,
                             saved() == file)
              << std::endl;
    require(!aborted && trips >= 3 && saved() == file &&
                port->status().controller.machine != MachineState::on,
            "an aborted calibration stops the machine and leaves the file as it was");
    // Reset only this simulation, as after the interrupted setter descent.
    checked(port->set_machine_state(MachineState::off, {}));
    checked(port->set_machine_state(MachineState::on, {}));
    checked(port->set_task_mode(TaskMode::mdi, {}));
    mdi("G21 G90 G94");
    mdi("G54");
    require(port->status().g5x == g54, "runout calibration changes no other work system");
    return;
  }

  if (gauge == "arc") {
    // The three-point bore (#318) in the ring and boss on the pin, unrotated and in G56 turned
    // `degrees`, with a known correction of unequal axes in the file. Each contact moves along
    // its own heading, once: a corrected cycle's faces are those of the same uncorrected cycle
    // plus correction(heading), and its centre and diameter are those of the circle through the
    // moved contacts. Corrected twice or along +X, the faces are up to 0.04 mm off.
    const ProbeRunout known{.tool = 99, .table_diameter_mm = 2, .radius_mm = {-0.04, 0.02}};
    constexpr std::array angles{0.0, 120.0, 240.0};
    constexpr double ball_radius = 1; // the table's Ø2
    struct Arc {
      SetterState state;
      std::array<std::array<double, 2>, 3> faces{}; // machine XY, measure-only
      std::array<double, 2> centre{};               // machine XY, measure-only
      std::array<double, 2> origin{};               // G56 XY after the cycle, or after applying
    };
    // A three-point cycle in G56, a copy of G55 turned `turn`: in the ring from (5, -1.5), over
    // the pin from C - e.
    const auto arc = [&](std::string_view name, bool boss, double turn, bool measure_only,
                         bool apply) -> Arc {
      mdi(std::format("G21 G10 L2 P3 X{} Y{} Z{} R{}", centre[0], centre[1], port->status().g5x[2], turn));
      mdi("G56");
      const auto from = boss ? seen : std::array{5.0, -1.5};
      at(from[0], from[1], 20);
      FixedSetterRequest request{.action = boss ? SetterAction::boss_arc : SetterAction::bore_arc,
                                 .tool = 99,
                                 .start = machine(),
                                 .gauge_width_mm = boss ? 12.0 : 0.0,
                                 .measure_only = measure_only,
                                 .angles_deg = angles};
      const auto result = port->fixed_setter(request, epoch, {});
      if (!result)
        throw std::runtime_error(std::format("{}: {}", name, result.error().text));
      Arc done{.state = *result};
      if (measure_only) {
        const auto& found = result->measured;
        require(found && found->faces.size() == 3 && found->machine && !result->material,
                std::format("{} measures three faces and writes nothing", name));
        for (std::size_t i = 0; i < done.faces.size(); ++i)
          done.faces.at(i) = {found->faces.at(i).machine[0], found->faces.at(i).machine[1]};
        done.centre = {found->machine->at(0), found->machine->at(1)};
        if (apply) {
          const auto applied = port->fixed_setter(
              {.action = SetterAction::apply_measurement, .tool = 99, .edge_measurement = found->id},
              epoch, {});
          require(applied && applied->material,
                  applied ? std::format("{} applies its measurement", name) : applied.error().text);
        }
      }
      const auto after = port->status();
      done.origin = {after.g5x[0], after.g5x[1]};
      mdi("G55");
      return done;
    };
    struct Expected {
      std::array<std::array<double, 2>, 3> faces;
      std::array<double, 2> centre;
      double diameter;
    };
    // What a corrected cycle gives, from the same cycle uncorrected.
    const auto corrected = [&](const Arc& bare, bool boss, double turn) {
      Expected result{};
      std::array<std::array<double, 2>, 3> contacts{};
      for (std::size_t i = 0; i < contacts.size(); ++i) {
        // The search line of the cycle: a boss searches inward, its heading turned 180°.
        const auto line = search_line(ProbeDirection::x_plus, angles.at(i) + (boss ? 180 : 0), turn);
        const auto shift = known.correction(line.forward[0], line.forward[1]);
        for (std::size_t axis = 0; axis < 2; ++axis) {
          result.faces.at(i).at(axis) = bare.faces.at(i).at(axis) + shift.at(axis);
          contacts.at(i).at(axis) = result.faces.at(i).at(axis) - ball_radius * line.forward.at(axis);
        }
      }
      const auto circle = circle_through(contacts);
      if (!circle)
        throw std::runtime_error("the expected contacts have no circle");
      result.centre = circle->centre;
      result.diameter = 2 * (circle->radius + (boss ? -ball_radius : ball_radius));
      return result;
    };
    const auto faces = [](const Expected& want, const Arc& got) {
      constexpr std::array<std::array<std::string_view, 2>, 3> keys{
          {{"face1_x", "face1_y"}, {"face2_x", "face2_y"}, {"face3_x", "face3_y"}}};
      std::vector<Value> values;
      for (std::size_t i = 0; i < want.faces.size(); ++i)
        for (std::size_t axis = 0; axis < 2; ++axis)
          values.push_back({keys.at(i).at(axis), want.faces.at(i).at(axis), got.faces.at(i).at(axis)});
      return values;
    };
    const auto in_use = [&](const SetterState& state) {
      return state.runout && state.runout->tool == 99 &&
             std::abs(state.runout->radius_mm[0] - known.radius_mm[0]) < 1e-6 &&
             std::abs(state.runout->radius_mm[1] - known.radius_mm[1]) < 1e-6;
    };

    // Uncorrected first: no file. The bore is large and the boss small by twice the radius part.
    ring();
    require(!status().value().runout, "no correction before the file is written");
    const auto bare_bore = arc("bore_arc_uncorrected", false, 0, true, false);
    const auto bare_bore_turned = arc("bore_arc_uncorrected_rotated", false, degrees, true, false);
    expect("bore_arc_uncorrected", {{"centre_x", seen[0], bare_bore.centre[0]}, {"centre_y", seen[1], bare_bore.centre[1]},
                                    {"diameter", 20.02, bare_bore.state.center_diameter.value_or(none)}},
           0.01);
    pin();
    const auto bare_boss = arc("boss_arc_uncorrected", true, 0, true, false);
    const auto bare_boss_turned = arc("boss_arc_uncorrected_rotated", true, degrees, true, false);
    expect("boss_arc_uncorrected", {{"centre_x", seen[0], bare_boss.centre[0]}, {"centre_y", seen[1], bare_boss.centre[1]},
                                    {"diameter", 11.98, bare_boss.state.center_diameter.value_or(none)}},
           0.01);

    // The known correction, then each cycle again: measure-only and applied unrotated, written
    // rotated.
    std::ofstream{runout_file} << format_probe_runout(known);
    require(status().has_value() && in_use(*status()), "the written correction is in use");
    for (const bool is_boss : {true, false}) {
      if (!is_boss) {
        at(5, -1.5, 20); // inside the ring before its wall is the probe input
        ring();
      }
      const auto& bare = is_boss ? bare_boss : bare_bore;
      const auto& bare_turned = is_boss ? bare_boss_turned : bare_bore_turned;
      const std::string kind = is_boss ? "boss_arc" : "bore_arc";
      const auto want = corrected(bare, is_boss, 0);
      const auto only = arc(kind + "_measure_only", is_boss, 0, true, true);
      auto values = faces(want, only);
      values.insert(values.end(), {{"centre_x", want.centre[0], only.centre[0]},
                                   {"centre_y", want.centre[1], only.centre[1]},
                                   {"diameter", want.diameter, only.state.center_diameter.value_or(none)},
                                   {"applied_x", want.centre[0], only.origin[0]},
                                   {"applied_y", want.centre[1], only.origin[1]}});
      expect(kind + "_measure_only", values, tight);
      require(in_use(only.state), std::format("{} reports the correction it used", kind));
      const auto want_turned = corrected(bare_turned, is_boss, degrees);
      const auto written = arc(kind + "_rotated", is_boss, degrees, false, false);
      expect(kind + "_rotated", {{"centre_x", want_turned.centre[0], written.origin[0]},
                                 {"centre_y", want_turned.centre[1], written.origin[1]},
                                 {"diameter", want_turned.diameter, written.state.center_diameter.value_or(none)}},
             tight);
      require(written.state.material && written.state.material->corner && in_use(written.state),
              std::format("{} writes X0 Y0 with the correction", kind));
    }
    std::filesystem::remove(runout_file);
    mdi("G54");
    require(port->status().g5x == g54, "three-point probing with the correction changes no other work system");
    return;
  }

  // The pin from outside, below Z0 (machine Z 12), its centre found, then the pin features.
  pin();
  const auto pin_radius = calibrate("pin", true, {4.5, -2.5});
  const auto corner = cycle("corner", {.action = SetterAction::corner, .direction = ProbeDirection::x_plus,
                                       .y_direction = ProbeDirection::y_plus}, seen);
  expect("corner_front_left", {{"face_x", seen[0] - 6, corner.origin[0]}, {"face_y", seen[1] - 6, corner.origin[1]}},
         tight);
  const auto boss = cycle("boss", {.action = SetterAction::boss, .gauge_width_mm = 12}, seen);
  expect("boss", {{"centre_x", seen[0], boss.origin[0]}, {"centre_y", seen[1], boss.origin[1]},
                  {"diameter", 12, boss.state.center_diameter.value_or(none)}},
         tight);
  const auto ridge = cycle("ridge", {.action = SetterAction::ridge, .direction = ProbeDirection::x_plus,
                                     .gauge_width_mm = 12}, seen);
  expect("ridge_x", {{"centre_x", seen[0], ridge.origin[0]}, {"width", 12, ridge.state.feature_size[0].value_or(none)}},
         tight);

  // A measure-only boss (#313) is corrected the same way: its centre, size and faces; nothing
  // is written (G56 keeps its origin at C).
  const auto only = cycle("boss_measure_only", {.action = SetterAction::boss, .gauge_width_mm = 12,
                                                .measure_only = true}, seen);
  const auto& found = only.state.measured;
  const auto face = [&](std::size_t i, std::size_t axis) {
    return found && i < found->faces.size() ? found->faces.at(i).machine.at(axis) : none;
  };
  expect("boss_measure_only",
         {{"centre_x", seen[0], found && found->machine ? found->machine->at(0) : none},
          {"centre_y", seen[1], found && found->machine ? found->machine->at(1) : none},
          {"diameter", 12, only.state.center_diameter.value_or(none)},
          {"face_y_plus", seen[1] - 6, face(2, 1)}, {"face_y_minus", seen[1] + 6, face(3, 1)},
          {"origin_x", centre[0], only.origin[0]}, {"origin_y", centre[1], only.origin[1]}},
         tight);
  require(found && found->faces.size() == 4 && !only.state.material, "measure-only writes nothing");

  // A file for another tool is not this probe's: no correction.
  const auto file = saved();
  require(file.starts_with("tool 99\n"), "the file names the probe");
  std::ofstream{runout_file} << "tool 98\n" << file.substr(8);
  const auto other = status();
  const auto unmatched = cycle("ridge_other_tool", {.action = SetterAction::ridge, .direction = ProbeDirection::x_plus,
                                                    .gauge_width_mm = 12}, seen);
  expect("ridge_other_tool", {{"width", 12 + 2 * pin_radius[0], unmatched.state.feature_size[0].value_or(none)}}, tight);
  require(other && !other->runout && !unmatched.state.runout, "another tool number turns the correction off");
  std::ofstream{runout_file} << file;

  // A search travel of 14 mm: too short beside the pin (14 + 1 mm), refused before motion; in
  // the ring 5.5 mm off centre the -X wall is 14.5 mm away, a miss after the +X contact.
  reconnect(14);
  at(4.5, -2.5, 20);
  refused("pin_beyond_travel", {.action = SetterAction::runout, .gauge_width_mm = 12, .outside = true},
          "too short to probe beside the work");
  ring();
  at(seen[0] + 5.5, seen[1], 20);
  FixedSetterRequest far{.action = SetterAction::runout, .tool = 99, .start = machine()};
  far.gauge_width_mm = 20;
  const auto missed = port->fixed_setter(far, epoch, {});
  std::cout << std::format("fact=runout action=miss refused={} reason=\"{}\" file_kept={}", !missed,
                           missed ? "" : missed.error().text, saved() == file)
            << std::endl;
  require(!missed && missed.error().text.starts_with("−X face") && saved() == file,
          "a miss part way leaves the file as it was");
  reconnect(20);

  // The ball calibration measures the bare ball with the correction in use: the table diameter
  // plus twice the radius part, not the table diameter. Its new diameter voids the correction.
  pin();
  require(status().value().runout.has_value(), "the pin correction is in use before the ball calibration");
  const auto ball = cycle("ball", {.action = SetterAction::ball, .gauge_width_mm = 12}, seen);
  expect("ball_with_correction_in_use", {{"diameter", 2 + 2 * pin_radius[0], ball.state.ball_diameter.value_or(none)}},
         tight);
  require(!ball.state.runout && !status().value().runout, "a new ball diameter voids the correction");
  checked(port->set_tool({99, 99, 45, 2, "Ruby probe"}, {}));
  require(status().value().runout.has_value(), "the table diameter it was measured against brings it back");

  // Clear needs no probe in the spindle: it only removes the file.
  load(98);
  const auto cleared = port->fixed_setter({.action = SetterAction::clear_runout, .tool = 99}, epoch, {});
  require(cleared && !cleared->runout && !std::filesystem::exists(runout_file),
          cleared ? "Clear removes the correction with T98 loaded" : cleared.error().text);
  load(99);
  mdi("G54");
  require(port->status().g5x == g54, "runout calibration changes no other work system");
}

// Three-point bore and boss (#247) in G55 rotated `degrees`. HAL turns the commanded machine XY
// into the frame (u, v) of the work axes and computes r² = (u - 4)² + (v + 2)² there. With the
// 2 mm ball, the ball centre touches at r = 7: a bore of Ø16 trips outside it, a boss of Ø12
// inside it below Z 12. The expected origin is the frame point (4, -2).
// A box in the frame, (u - 4, v + 2) within set limits, changes the part: on a bore it is an
// obstacle (an early trip) or a notch (no contact); on a boss a burr below its top. With
// `faults` the scenario runs those cases and a stop mid-cycle instead of the measured cycles.
void arc_scenarios(NmlPort& port, double scale, double degrees, const auto& mdi, bool faults,
                   HalShared& shared, const std::string& tool_table, bool verify = false) {
  // The MDI blocks written since `from` that write an offset or the tool table: G10.
  const auto g10_since = [&](std::size_t from) {
    const auto all = port.writes();
    return std::count_if(all.begin() + static_cast<std::ptrdiff_t>(std::min(from, all.size())), all.end(),
                         [](const NmlPort::Written& written) { return written.mdi.contains("G10"); });
  };
  const auto table_text = [&] {
    return read_text_file(tool_table, std::size_t{1} << 20).value_or(std::string{});
  };
  checked(port.set_tool({98, 98, 50, 0, "Setter reference"}, {}));
  checked(port.set_tool({99, 99, 45, 2, "Ruby probe"}, {}));
  constexpr double none = std::numeric_limits<double>::quiet_NaN();
  const double turn = degrees * std::numbers::pi / 180;
  const double c = std::cos(turn);
  const double s = std::sin(turn);
  const auto machine = [&] {
    const auto axes = port.status().controller.axes;
    return std::array{axes[0].machine, axes[1].machine, axes[2].machine};
  };
  const auto origin_of = [&](const NmlStatus& status) {
    const auto& axes = status.controller.axes;
    const double x = axes[0].machine;
    const double y = axes[1].machine;
    return std::array{c * x + s * y - status.work[0], -s * x + c * y - status.work[1]};
  };
  const auto at = [&](double u, double v) {
    mdi(std::format("G21 G90 G53 G0 X{:.8f} Y{:.8f} Z20", c * u - s * v, s * u + c * v));
    mdi(scale == 1 ? "G21 G91" : "G20 G91");
  };
  SetterEpoch epoch{1, 1, 1, 0};
  for (const int tool : {98, 99}) {
    mdi(std::format("M61 Q{}", tool));
    mdi(std::format("G43 H{}", tool));
    at(0, 0);
    ++epoch.tool;
    halcmd(std::format("setp sim-tool-setter.max {}", (tool == 98 ? 10 : 12) / scale));
    const auto touched = port.fixed_setter({SetterAction::touch_off, tool, machine()}, epoch, {});
    require(touched.has_value(),
            touched ? "the reference and probe have known lengths" : touched.error().text);
  }
  mdi("G21 G90 G54");
  const auto g54 = port.status().g5x;
  mdi(std::format("G10 L2 P2 X5 Y-7 Z2 R{}", degrees));
  mdi("G55");
  mdi("G92 X2 Y3 Z4");
  at(0, 0);
  mdi("G21 G10 L20 P0 Z8"); // Z0 at machine Z 12, the top of the boss.
  const auto initial = port.status();

  // comp, wcomp and and2 belong to the machine's HAL: windows with near, gates with logic.
  halcmd("loadrt sum2 names=sim-arc-du,sim-arc-dv,sim-arc-r2");
  halcmd("loadrt mult2 names=sim-arc-du2,sim-arc-dv2");
  halcmd("loadrt near names=sim-arc-in,sim-arc-z,sim-arc-bu,sim-arc-bv");
  // Personalities: 0x1nn AND, 0x2nn OR, 0x8nn NAND of nn inputs (0x801 is NOT).
  halcmd("loadrt logic names=sim-arc-hollow,sim-arc-solid,sim-arc-box,sim-arc-notbox,"
         "sim-arc-hollowburr,sim-arc-hollownotch,sim-arc-burrlow,sim-arc-solidburr "
         "personality=0x801,0x102,0x102,0x801,0x202,0x102,0x102,0x202");
  for (const auto* part : {"sim-arc-du", "sim-arc-dv", "sim-arc-du2", "sim-arc-dv2", "sim-arc-r2",
                           "sim-arc-in", "sim-arc-z", "sim-arc-bu", "sim-arc-bv", "sim-arc-hollow",
                           "sim-arc-solid", "sim-arc-box", "sim-arc-notbox", "sim-arc-hollowburr",
                           "sim-arc-hollownotch", "sim-arc-burrlow", "sim-arc-solidburr"})
    halcmd(std::format("addf {} servo-thread", part));
  halcmd("net sim-arc-mx axis.x.pos-cmd sim-arc-du.in0 sim-arc-dv.in0");
  halcmd("net sim-arc-my axis.y.pos-cmd sim-arc-du.in1 sim-arc-dv.in1");
  halcmd(std::format("setp sim-arc-du.gain0 {:.12f}", c));
  halcmd(std::format("setp sim-arc-du.gain1 {:.12f}", s));
  halcmd(std::format("setp sim-arc-du.offset {:.12f}", -4 / scale));
  halcmd(std::format("setp sim-arc-dv.gain0 {:.12f}", -s));
  halcmd(std::format("setp sim-arc-dv.gain1 {:.12f}", c));
  halcmd(std::format("setp sim-arc-dv.offset {:.12f}", 2 / scale));
  halcmd("net sim-arc-u sim-arc-du.out sim-arc-du2.in0 sim-arc-du2.in1 sim-arc-bu.in1");
  halcmd("net sim-arc-v sim-arc-dv.out sim-arc-dv2.in0 sim-arc-dv2.in1 sim-arc-bv.in1");
  halcmd("net sim-arc-u2 sim-arc-du2.out sim-arc-r2.in0");
  halcmd("net sim-arc-v2 sim-arc-dv2.out sim-arc-r2.in1");
  // Inside the contact circle: r² within [0, 7²].
  const double contact_r2 = 7 * 7 / (scale * scale);
  halcmd("net sim-arc-rr sim-arc-r2.out sim-arc-in.in1");
  halcmd(std::format("setp sim-arc-in.in2 {:.12f}", contact_r2 / 2));
  halcmd(std::format("setp sim-arc-in.difference {:.12f}", contact_r2 / 2));
  halcmd("setp sim-arc-in.scale 1");
  // Below the boss top: Z within [-100, 12].
  halcmd("net sim-setter-z sim-arc-z.in1");
  halcmd(std::format("setp sim-arc-z.in2 {:.12f}", -44 / scale));
  halcmd(std::format("setp sim-arc-z.difference {:.12f}", 56 / scale));
  halcmd("setp sim-arc-z.scale 1");
  halcmd("net sim-arc-inside sim-arc-in.out sim-arc-hollow.in-00 sim-arc-solid.in-00");
  halcmd("net sim-arc-low sim-arc-z.out sim-arc-solid.in-01 sim-arc-burrlow.in-01");
  // The box: both frame coordinates within their window.
  halcmd("net sim-arc-box-u sim-arc-bu.out sim-arc-box.in-00");
  halcmd("net sim-arc-box-v sim-arc-bv.out sim-arc-box.in-01");
  halcmd("net sim-arc-in-box sim-arc-box.and sim-arc-notbox.in-00 sim-arc-hollowburr.in-01 "
         "sim-arc-burrlow.in-00");
  halcmd("net sim-arc-out-box sim-arc-notbox.nand sim-arc-hollownotch.in-01");
  // The probe inputs: the plain bore wall and boss, the bore with an obstacle or a notch, the
  // boss with a burr below its top.
  halcmd("net sim-arc-wall sim-arc-hollow.nand sim-arc-hollowburr.in-00 sim-arc-hollownotch.in-00");
  halcmd("net sim-arc-boss sim-arc-solid.and sim-arc-solidburr.in-00");
  halcmd("net sim-arc-burr-low sim-arc-burrlow.and sim-arc-solidburr.in-01");
  halcmd("net sim-arc-wall-obstacle sim-arc-hollowburr.or");
  halcmd("net sim-arc-wall-notch sim-arc-hollownotch.and");
  halcmd("net sim-arc-boss-burr sim-arc-solidburr.or");
  const auto probe_input = [&](std::string_view net) {
    halcmd("unlinkp motion.probe-input");
    halcmd("unlinkp coffeecnc.probe-input");
    halcmd(std::format("net {} motion.probe-input coffeecnc.probe-input", net));
  };
  // The box in millimetres of the frame relative to the circle centre: u - 4 and v + 2.
  const auto box = [&](double u_low, double u_high, double v_low, double v_high) {
    halcmd(std::format("setp sim-arc-bu.in2 {:.12f}", (u_low + u_high) / 2 / scale));
    halcmd(std::format("setp sim-arc-bu.difference {:.12f}", (u_high - u_low) / 2 / scale));
    halcmd("setp sim-arc-bu.scale 1");
    halcmd(std::format("setp sim-arc-bv.in2 {:.12f}", (v_low + v_high) / 2 / scale));
    halcmd(std::format("setp sim-arc-bv.difference {:.12f}", (v_high - v_low) / 2 / scale));
    halcmd("setp sim-arc-bv.scale 1");
  };
  box(1000, 1001, 1000, 1001); // out of the way

  if (verify) {
    // Exercise three primary contacts in mm and four in inch. Put a small obstacle exactly
    // 1 mm ahead of the nominal contact on each ray, one ray at a time.
    const bool three = scale == 1;
    const std::array primary{0.0, 90.0, 180.0};
    const double extra = three ? 270.0 : 45.0;
    const auto headings =
        three ? std::vector{0.0, 90.0, 180.0, extra} : std::vector{0.0, 90.0, 180.0, 270.0, extra};
    for (const bool inside : {true, false}) {
      const auto action = inside ? (three ? SetterAction::bore_arc : SetterAction::bore)
                                 : (three ? SetterAction::boss_arc : SetterAction::boss);
      const auto request = [&](bool measure_only) {
        return FixedSetterRequest{.action = action,
                                  .tool = 99,
                                  .start = machine(),
                                  .gauge_width_mm = inside ? 0.0 : 12.0,
                                  .measure_only = measure_only,
                                  .angles_deg = primary,
                                  .check_angle_deg = extra};
      };
      probe_input(inside ? "sim-arc-wall" : "sim-arc-boss");
      at(3.6, -1.7); // a sound circle still fits from off centre
      const auto clear = port.fixed_setter(request(true), epoch, {});
      if (!clear)
        throw std::runtime_error(clear.error().text);
      if (!clear->measured)
        throw std::runtime_error(clear->origin_refusal
                                     ? clear->origin_refusal->text
                                     : "the sound circle did not yield a measurement");
      require(clear->measured->faces.size() == headings.size() && clear->center_diameter &&
                  std::abs(*clear->center_diameter - (inside ? 16 : 12)) < 0.03,
              "the sound circle measures all contacts and its diameter");
      const auto accepted = port.fixed_setter({.action = SetterAction::apply_measurement,
                                               .tool = 99,
                                               .edge_measurement = clear->measured->id},
                                              epoch, {});
      const auto origin = origin_of(port.status());
      require(accepted && accepted->material && std::abs(origin[0] - 4) < 0.03 &&
                  std::abs(origin[1] + 2) < 0.03,
              "the sound circle can set its measured centre");
      for (const auto heading : headings) {
        at(4, -2);
        const auto before = port.status();
        const auto writes = port.writes().size();
        const auto table = table_text();
        const double angle = heading * std::numbers::pi / 180;
        const double x = std::cos(angle), y = std::sin(angle);
        constexpr double half = 0.4;
        const double radius =
            7 + (inside ? -1 : 1) * (1 - half / std::max(std::abs(x), std::abs(y)));
        box(radius * x - half, radius * x + half, radius * y - half, radius * y + half);
        probe_input(inside ? "sim-arc-wall-obstacle" : "sim-arc-boss-burr");
        const auto refused = port.fixed_setter(request(false), epoch, {});
        const auto after = port.status();
        const auto fault = refused ? refused->origin_refusal : std::nullopt;
        std::cout << std::format(
                         "fact=circle_check bore={} contacts={} early_heading={} refused={} g10={}",
                         inside, headings.size(), heading, fault.has_value(), g10_since(writes))
                  << '\n';
        require(fault && fault->localization &&
                    fault->localization->key == "backend.nmlPort.circleCheckResidual" && refused &&
                    !refused->material && !refused->measured && after.g5x == before.g5x &&
                    after.g92 == before.g92 && after.rotation_xy == before.rotation_xy &&
                    after.tool_offset == before.tool_offset && port.writes().size() > writes &&
                    g10_since(writes) == 0 && table_text() == table,
                refused ? "an early trip refuses the origin write with the residual key"
                        : refused.error().text);
      }
      box(1000, 1001, 1000, 1001);
    }
    at(4, -2);
    probe_input("sim-arc-wall");
    FixedSetterRequest bore{.action = three ? SetterAction::bore_arc : SetterAction::bore,
                            .tool = 99,
                            .start = machine(),
                            .angles_deg = primary,
                            .check_angle_deg = extra};
    if (three) {
      auto partial = bore;
      partial.angles_deg = {195, 240, 285};
      partial.check_angle_deg = 217.5;
      const auto writes = port.writes().size();
      const auto rejected = port.fixed_setter(partial, epoch, {});
      require(rejected && rejected->origin_refusal && rejected->origin_refusal->localization &&
                  rejected->origin_refusal->localization->key ==
                      "backend.nmlPort.circleCheckSensitivity" &&
                  port.writes().size() == writes,
              "a narrow partial arc is refused before motion");
      partial.angles_deg = {0, 10, 275};
      partial.check_angle_deg = 285;
      const auto risky = port.fixed_setter(partial, epoch, {});
      require(risky && risky->origin_refusal && risky->origin_refusal->localization &&
                  risky->origin_refusal->localization->key ==
                      "backend.nmlPort.circleCheckCentreRisk" &&
                  port.writes().size() == writes,
              "a 95-degree layout with excessive centre error is refused before motion");
      partial.angles_deg = {0, 85, 175};
      partial.check_angle_deg = 260;
      partial.measure_only = true;
      const auto sound = port.fixed_setter(partial, epoch, {});
      require(sound && sound->measured && sound->measured->faces.size() == 4,
              "an adequately spread partial arc passes its check");
      at(4, -2);
      bore.start = machine();
      box(-1, 1, -100, -6.7);
      probe_input("sim-arc-wall-notch");
      const auto before = port.status().g5x;
      const auto missed_writes = port.writes().size();
      const auto missed = port.fixed_setter(bore, epoch, {});
      require(!missed && missed.error().text.contains("Contact 4") && port.status().g5x == before &&
                  g10_since(missed_writes) == 0,
              "a missed verification contact aborts without writing");
    } else {
      int completed = 0;
      bool active = false;
      bool stopped = false;
      std::optional<HalShared::Clock::time_point> fifth;
      const auto before = port.status().g5x;
      const auto writes = port.writes().size();
      const auto result = port.fixed_setter(bore, epoch, [&] {
        const auto now = HalShared::Clock::now();
        const auto pins = shared.view(now);
        const bool running = pins && pins->probe_cycle_active;
        if (active && !running)
          ++completed;
        active = running;
        if (completed >= 4 && running && !fifth)
          fifth = now;
        stopped = stopped || (fifth && now - *fifth >= 250ms);
        return !stopped;
      });
      require(stopped && !result && result.error().text.contains("Contact 5") &&
                  port.status().g5x == before && g10_since(writes) == 0,
              "Stop during the extra search aborts without writing");
    }
    return;
  }

  // A cycle that the controller refuses after moving: nothing set, the probe back at its start.
  const auto refused_run = [&](std::string_view name, SetterAction action,
                               std::array<double, 3> angles, std::string_view reason,
                               const Wanted& wanted = {}) {
    const auto before = port.status();
    FixedSetterRequest request{.action = action, .tool = 99, .start = machine()};
    request.angles_deg = angles;
    if (action == SetterAction::boss_arc)
      request.gauge_width_mm = 12;
    const auto writes = port.writes().size();
    const auto table_before = table_text();
    const auto result = port.fixed_setter(request, epoch, wanted);
    const auto after = port.status();
    const auto g10 = g10_since(writes);
    const bool unchanged = after.g5x == before.g5x && after.g92 == before.g92 &&
                           after.rotation_xy == before.rotation_xy &&
                           after.tool_offset == before.tool_offset && after.wcs == before.wcs &&
                           g10 == 0 && table_text() == table_before;
    std::cout << std::format("fact=arc_fault action={} rotation={} angles={:.0f},{:.0f},{:.0f} "
                             "refused={} moved={} g10_blocks={} offsets_unchanged={} reason=\"{}\"",
                             name, degrees, angles[0], angles[1], angles[2], !result,
                             port.writes().size() > writes, g10, unchanged,
                             result ? std::string{} : result.error().text)
              << std::endl;
    require(!result && result.error().text.contains(reason) && unchanged,
            result ? std::format("{}: expected a refusal", name) : result.error().text);
    return after;
  };
  const auto run = [&](std::string_view name, SetterAction action, std::array<double, 3> angles,
                       double expected_diameter) {
    const auto before = port.status();
    require(std::ranges::contains(before.active_codes, "G91"), "the rig sets G91 before each run");
    FixedSetterRequest request{.action = action, .tool = 99, .start = machine()};
    request.angles_deg = angles;
    if (action == SetterAction::boss_arc)
      request.gauge_width_mm = 12;
    const auto result = port.fixed_setter(request, epoch, {});
    if (!result)
      throw std::runtime_error(std::format("{} at {}°: {}", name, degrees, result.error().text));
    const auto after = port.status();
    const auto origin = origin_of(after);
    const double span = arc_spread(angles).span_deg;
    const bool incremental = std::ranges::contains(after.active_codes, "G91");
    std::cout << std::format("fact=arc_probe action={} rotation={} angles={:.0f},{:.0f},{:.0f} "
                             "requested_span={:.0f} origin_u_expected=4.0000 origin_u_measured={:.4f} "
                             "origin_v_expected=-2.0000 origin_v_measured={:.4f} diameter_expected={:.4f} "
                             "diameter_measured={:.4f} arc_span_measured={:.1f} "
                             "centre_uncertainty_mm={:.4f} g91_after={}",
                             name, degrees, angles[0], angles[1], angles[2], span, origin[0],
                             origin[1], expected_diameter, result->center_diameter.value_or(none),
                             result->arc_span_deg.value_or(none),
                             result->center_uncertainty_mm.value_or(none), incremental)
              << std::endl;
    require(result->material && result->material->corner && result->center_diameter &&
                std::abs(*result->center_diameter - expected_diameter) < 0.03 &&
                std::abs(origin[0] - 4) < 0.03 && std::abs(origin[1] + 2) < 0.03 &&
                result->arc_span_deg && *result->arc_span_deg >= arc_min_measured_span_deg &&
                result->center_uncertainty_mm && *result->center_uncertainty_mm > 0 &&
                *result->center_uncertainty_mm <= arc_max_centre_uncertainty_mm && after.wcs == 2 &&
                std::abs(after.rotation_xy - degrees) < 1e-9 && after.g92 == initial.g92 &&
                after.tool_offset == initial.tool_offset &&
                std::abs(after.g5x[2] - before.g5x[2]) < 1e-6 &&
                std::abs(machine()[2] - before.controller.axes[2].machine) < 0.03,
            std::format("{} finds the centre and diameter through three contacts at {}°", name,
                        degrees));
    require(incremental, std::format("{} restores G91 after the cycle", name));
  };

  if (faults) {
    // Bore: no wall within the travel at point 2, then at point 3 (a notch in the wall).
    probe_input("sim-arc-wall-notch");
    box(-14, -1, 4, 22);
    at(2, 1);
    refused_run("bore_missed_point_2", SetterAction::bore_arc, {0, 120, 240}, "Contact 2 at 120.0°");
    box(-14, -3, -16, -2);
    at(2, 1);
    refused_run("bore_missed_point_3", SetterAction::bore_arc, {0, 120, 240}, "Contact 3 at 240.0°");
    // A flat wall across all three directions: the contacts lie on a line.
    probe_input("sim-arc-wall-obstacle");
    box(1, 50, -50, 50);
    at(2, 1);
    refused_run("bore_collinear", SetterAction::bore_arc, {-45, 0, 45}, "lie almost on a line");
    // An obstacle 2 mm ahead of the start at 0°: the circle bulges away from the start.
    box(0, 0.5, 2.5, 3.5);
    at(2, 1);
    refused_run("bore_start_outside", SetterAction::bore_arc, {-45, 0, 45},
                "does not enclose the confirmed starting point");
    // Boss: a large burr on the face at 180° (point 2), then at 270° (point 3): an early trip of
    // 6 mm, a gross fault the diameter check catches. A burr of 1-2 mm would pass it.
    probe_input("sim-arc-boss-burr");
    box(-13, -12, 0, 2);
    at(3, -1);
    refused_run("boss_early_point_2", SetterAction::boss_arc, {90, 180, 270}, "differs from the entered");
    box(-2, 0, -13, -12);
    at(3, -1);
    refused_run("boss_early_point_3", SetterAction::boss_arc, {90, 180, 270}, "differs from the entered");
    // A stop while the second contact searches, 0.7 s after its probe cycle began: the cycle
    // ends without a write.
    box(1000, 1001, 1000, 1001);
    probe_input("sim-arc-wall");
    at(2, 1);
    int cycles = 0;
    bool active = false;
    bool stopped = false;
    std::optional<HalShared::Clock::time_point> second;
    const auto stopped_run = refused_run(
        "bore_stopped_contact_2", SetterAction::bore_arc, {0, 120, 240}, "Contact 2", [&] {
          const auto now = HalShared::Clock::now();
          const auto pins = shared.view(now);
          const bool running = pins && pins->probe_cycle_active;
          if (active && !running)
            ++cycles;
          active = running;
          if (cycles >= 1 && running && !second)
            second = now;
          stopped = stopped || (second && now - *second >= 700ms);
          return !stopped;
        });
    std::cout << std::format("fact=arc_fault action=stop rotation={} stopped_during_contact=2 "
                             "machine_on={}",
                             degrees, stopped_run.controller.machine == MachineState::on)
              << std::endl;
    require(stopped, "the stop came while the second contact ran");
    return;
  }

  // A bore: from inside, three walls; one set around the circle, one partial arc on one side
  // that covers the 90° minimum.
  probe_input("sim-arc-wall");
  at(2, 1);
  run("bore_arc", SetterAction::bore_arc, {0, 120, 240}, 16);
  at(2, 1);
  run("bore_arc_partial", SetterAction::bore_arc, {-45, 0, 45}, 16);
  // A boss of Ø12 entered as Ø12: from outside, three faces; again a partial arc.
  probe_input("sim-arc-boss");
  at(3, -1);
  run("boss_arc", SetterAction::boss_arc, {90, 180, 270}, 12);
  at(3, -1);
  run("boss_arc_partial", SetterAction::boss_arc, {195, 240, 285}, 12);

  // Measure only (#247): both cycles move as usual, write no G10 and leave the tool table as it
  // was, report three surface points and the centre; applying the measurement sets it.
  const auto measure = [&](std::string_view name, SetterAction action, std::array<double, 3> angles) {
    const auto before = port.status();
    FixedSetterRequest request{.action = action, .tool = 99, .start = machine(), .measure_only = true};
    request.angles_deg = angles;
    if (action == SetterAction::boss_arc)
      request.gauge_width_mm = 12;
    const auto writes = port.writes().size();
    const auto table_before = table_text();
    const auto result = port.fixed_setter(request, epoch, {});
    if (!result || !result->measured)
      throw std::runtime_error(std::format("{} measure at {}°: {}", name, degrees,
                                           result ? "no measurement" : result.error().text));
    const auto after = port.status();
    const auto& m = *result->measured;
    const auto g10 = g10_since(writes);
    const bool unchanged = after.g5x == before.g5x && after.g92 == before.g92 &&
                           after.rotation_xy == before.rotation_xy && g10 == 0 &&
                           table_text() == table_before;
    const auto kept = origin_of(before);
    const std::array work{4 - kept[0], -2 - kept[1]};
    std::cout << std::format("fact=measure_only action={} rotation={} axes={} faces={} g10_blocks={} "
                             "table_unchanged={} work_x_expected={:.4f} work_x_measured={:.4f} "
                             "work_y_expected={:.4f} work_y_measured={:.4f} centre_uncertainty_mm={:.4f}",
                             name, degrees, m.axes, m.faces.size(), g10, table_text() == table_before,
                             work[0], m.work[0].value_or(none), work[1], m.work[1].value_or(none),
                             result->center_uncertainty_mm.value_or(none))
              << std::endl;
    require(unchanged && !result->material && m.id != 0 && m.axes == "XY" && m.faces.size() == 3 &&
                port.writes().size() > writes && m.work[0] && m.work[1] &&
                std::abs(*m.work[0] - work[0]) < 0.03 && std::abs(*m.work[1] - work[1]) < 0.03 &&
                result->center_uncertainty_mm,
            std::format("{} measures only and writes nothing", name));
    const auto applied = port.fixed_setter(
        {.action = SetterAction::apply_measurement, .tool = 99, .edge_measurement = m.id}, epoch, {});
    const auto origin = origin_of(port.status());
    std::cout << std::format("fact=measure_only action={}_apply rotation={} origin_u_measured={:.4f} "
                             "origin_v_measured={:.4f}",
                             name, degrees, origin[0], origin[1])
              << std::endl;
    require(applied && applied->material && applied->material->corner &&
                std::abs(origin[0] - 4) < 0.03 && std::abs(origin[1] + 2) < 0.03,
            applied ? std::format("{}: applying sets X0 Y0 at the centre", name)
                    : applied.error().text);
  };
  // Move G55 off the centre first, so applying has something to change.
  mdi("G21 G10 L20 P0 X1 Y1");
  mdi(scale == 1 ? "G21 G91" : "G20 G91");
  probe_input("sim-arc-wall");
  at(2, 1);
  measure("bore_arc", SetterAction::bore_arc, {0, 120, 240});
  probe_input("sim-arc-boss");
  at(3, -1);
  measure("boss_arc", SetterAction::boss_arc, {90, 180, 270});

  // Directions too close together, or covering less than 90°, are refused before moving.
  probe_input("sim-arc-wall");
  at(2, 1);
  for (const auto& [angles, reason] :
       {std::pair{std::array{0.0, 5.0, 90.0}, "at least 10° apart"},
        std::pair{std::array{0.0, 12.0, 24.0}, "at least 10° apart"},
        std::pair{std::array{350.0, 365.0, 10.0}, "at least 10° apart"},
        std::pair{std::array{-44.0, 0.0, 44.0}, "cover at least 90°"},
        std::pair{std::array{-80.0, -40.0, 0.0}, "cover at least 90°"}}) {
    const auto writes = port.writes().size();
    FixedSetterRequest request{.action = SetterAction::bore_arc, .tool = 99, .start = machine()};
    request.angles_deg = angles;
    const auto refused = port.fixed_setter(request, epoch, {});
    std::cout << std::format("fact=arc_probe action=refused rotation={} angles={},{},{} closest={:.0f} "
                             "span={:.0f} refused={} writes={}",
                             degrees, angles[0], angles[1], angles[2],
                             arc_spread(angles).closest_deg, arc_spread(angles).span_deg, !refused,
                             port.writes().size() - writes)
              << std::endl;
    require(!refused && refused.error().text.contains(reason) && port.writes().size() == writes,
            refused ? "expected a refused angle set" : refused.error().text);
  }

  // The travel check covers every direction before the first move: 1 mm inside the Y limit,
  // only the third direction aims at the limit in machine space. Nothing moves and nothing is
  // written. Outside the bore its wall input would trip during the moves there: the boss input
  // is clear above Z 12.
  probe_input("sim-arc-boss");
  const double edge_y = s >= 0 ? 249 : -249;
  mdi(std::format("G21 G90 G53 G0 X0 Y{} Z20", edge_y / 2));
  mdi(std::format("G21 G90 G53 G0 X0 Y{} Z20", edge_y));
  const auto writes = port.writes().size();
  const auto origin = port.status().g5x;
  const auto start = machine();
  FixedSetterRequest outward{.action = SetterAction::bore_arc, .tool = 99, .start = start};
  // Machine directions: the first two away from the limit, the third towards it.
  const double up = s >= 0 ? 90 : -90;
  outward.angles_deg = {up + 120 - degrees, up + 240 - degrees, up - degrees};
  const auto outside = port.fixed_setter(outward, epoch, {});
  const auto end = machine();
  const bool still = std::abs(end[0] - start[0]) < 1e-6 && std::abs(end[1] - start[1]) < 1e-6 &&
                     std::abs(end[2] - start[2]) < 1e-6;
  std::cout << std::format("fact=arc_probe action=travel_point_3 rotation={} start_y={} "
                           "endpoint_3_y={:.4f} limit_y={} writes={} moved={} reason=\"{}\"",
                           degrees, edge_y, edge_y + (s >= 0 ? 20 : -20), s >= 0 ? 250 : -250,
                           port.writes().size() - writes, !still,
                           outside ? std::string{} : outside.error().text)
            << std::endl;
  require(!outside && outside.error().text.contains("Contact 3") &&
              outside.error().text.contains("outside the configured travel") &&
              port.writes().size() == writes && still && port.status().g5x == origin,
          outside ? "expected a travel refusal" : outside.error().text);
  mdi(std::format("G21 G90 G53 G0 X0 Y{} Z20", edge_y / 2));
  mdi("G21 G90 G53 G0 X0 Y0 Z20");
  mdi("G54");
  require(port.status().g5x == g54, "three-point probing changes only the active work system");
}

// Z height map (#247). The simulated top is the plane Z = 12 + 0.01 u - 0.02 v (machine Z, mm),
// with (u, v) the commanded machine XY turned into the G55 axes: HAL trips where the commanded Z
// is below it. Z0 of G55 is at machine Z 12, so each point's work Z must read 0.01 u - 0.02 v.
// The grid is work X -5..15, Y -4..12 at 10 mm: 3 x 3 points, first in G55 unrotated, then in
// G55 rotated `degrees` with the Z encoder lagging 0.125 mm after each retract. Then the refusals
// before any motion, and the maps that stop: a position change, an early trip, a miss, a stop, an
// E-stop and a top too close under the clearance.
void height_map_scenarios(NmlPort& port, HalShared& shared, double scale, double degrees,
                          const auto& mdi) {
  checked(port.set_tool({98, 98, 50, 0, "Setter reference"}, {}));
  checked(port.set_tool({99, 99, 45, 2, "Ruby probe"}, {}));
  constexpr double slope_u = 0.01;
  constexpr double slope_v = -0.02;
  const auto machine = [&] {
    const auto axes = port.status().controller.axes;
    return std::array{axes[0].machine, axes[1].machine, axes[2].machine};
  };
  SetterEpoch epoch{1, 1, 1, 0};
  for (const int tool : {98, 99}) {
    mdi(std::format("M61 Q{}", tool));
    mdi(std::format("G43 H{}", tool));
    mdi("G21 G90 G53 G0 X0 Y0 Z20");
    ++epoch.tool;
    halcmd(std::format("setp sim-tool-setter.max {}", (tool == 98 ? 10 : 12) / scale));
    const auto touched = port.fixed_setter({SetterAction::touch_off, tool, machine()}, epoch, {});
    require(touched.has_value(),
            touched ? "the reference and probe have known lengths" : touched.error().text);
  }
  mdi("G21 G90 G54");
  const auto g54 = port.status().g5x;
  halcmd("loadrt sum2 names=sim-map-plane,sim-map-height,sim-z-feedback");
  halcmd("loadrt near names=sim-map-below");
  for (const auto* part : {"sim-map-plane", "sim-map-height", "sim-map-below"})
    halcmd(std::format("addf {} servo-thread", part));
  halcmd("net sim-map-mx axis.x.pos-cmd sim-map-plane.in0");
  halcmd("net sim-map-my axis.y.pos-cmd sim-map-plane.in1");
  halcmd("net sim-setter-z sim-map-height.in0");
  halcmd("net sim-map-w sim-map-plane.out sim-map-height.in1");
  halcmd("setp sim-map-height.gain1 -1");
  halcmd("net sim-map-d sim-map-height.out sim-map-below.in1");
  // Z less the plane's rise within [-100, 12].
  halcmd(std::format("setp sim-map-below.in2 {:.12f}", -44 / scale));
  halcmd(std::format("setp sim-map-below.difference {:.12f}", 56 / scale));
  halcmd("setp sim-map-below.scale 1");
  halcmd("unlinkp motion.probe-input");
  halcmd("unlinkp coffeecnc.probe-input");
  halcmd("net sim-map-top sim-map-below.out motion.probe-input coffeecnc.probe-input");

  // Z feedback with the reversal backlash of the G0759, off until a scenario sets it.
  wire_z_feedback(false);
  const auto set_lag = [&](double mm) {
    halcmd(std::format("setp sim-z-backlash.in1 {:.12f}", mm / scale));
    // The encoder lags only after an upward move, which every scenario ends with.
    wait_for(port, [&](const NmlStatus& s) {
      return std::abs(s.controller.axes[2].machine - s.commanded_position[2] - mm) < 0.001;
    });
  };
  // G55 turned `turn`, with G92 taken at machine X0 Y0 Z20, so a point's machine XY depends only
  // on its work XY: unrotated, the top at work X, Y is 0.01 (X - 2) - 0.02 (Y - 3).
  const auto frame = [&](double turn) {
    const double c = std::cos(turn * std::numbers::pi / 180);
    const double s = std::sin(turn * std::numbers::pi / 180);
    // The plane follows the work axes: its rise is slope_u u + slope_v v.
    halcmd(std::format("setp sim-map-plane.gain0 {:.12f}", slope_u * c - slope_v * s));
    halcmd(std::format("setp sim-map-plane.gain1 {:.12f}", slope_u * s + slope_v * c));
    mdi(std::format("G21 G10 L2 P2 X5 Y-7 Z2 R{}", turn));
    mdi("G55");
    mdi("G21 G90 G53 G0 X0 Y0 Z20");
    mdi("G92 X2 Y3 Z4");
    mdi("G21 G10 L20 P0 Z8"); // Z0 at machine Z 12.
    mdi(scale == 1 ? "G21 G91" : "G20 G91");
  };
  const auto map_request = [&](const std::array<double, 4>& grid, double step, double clearance) {
    FixedSetterRequest request{.action = SetterAction::height_map, .tool = 99, .start = machine()};
    request.grid_mm = grid;
    request.step_mm = step;
    request.clearance_mm = clearance;
    return request;
  };
  // `z_lag`: from the first point's first touch on, the Z encoder reads this much below the
  // command after every upward move, so every later point starts lagged.
  const auto map = [&](double turn, double z_lag) {
    const double c = std::cos(turn * std::numbers::pi / 180);
    const double s = std::sin(turn * std::numbers::pi / 180);
    frame(turn);
    const auto before = port.status();
    const auto request = map_request({-5, -4, 15, 12}, 10, 3);
    const auto writes = port.writes().size();
    bool lagging = false;
    const auto result = port.fixed_setter(request, epoch, [&] {
      const auto pins = shared.view(HalShared::Clock::now());
      if (z_lag != 0 && !lagging && pins && pins->probe_cycle_active && pins->probe_input.value_or(false)) {
        halcmd(std::format("setp sim-z-backlash.in1 {:.12f}", z_lag / scale));
        lagging = true;
      }
      return true;
    });
    if (!result || !result->height_map)
      throw std::runtime_error(std::format("height map at {}° with {} mm Z lag: {}", turn, z_lag,
                                           result ? "no map" : result.error().text));
    const auto& points = result->height_map->points;
    bool ok = result->height_map->columns == 3 && result->height_map->rows == 3 && points.size() == 9 &&
              result->height_map->wcs == 2 && !result->height_map->partial &&
              std::abs(result->height_map->rotation_deg - turn) < 1e-6 && port.status().g5x == before.g5x &&
              port.status().g92 == before.g92 && port.writes().size() > writes;
    // The rig left G91 active: the map restores it.
    const bool incremental = std::ranges::contains(port.status().active_codes, "G91");
    double worst = 0;
    for (std::size_t i = 0; i < points.size(); ++i) {
      const auto& point = points.at(i);
      const double x = -5 + 10.0 * static_cast<double>(i % 3);
      const double y = -4 + 8.0 * static_cast<double>(i / 3);
      const double u = c * point.machine[0] + s * point.machine[1];
      const double v = -s * point.machine[0] + c * point.machine[1];
      const double expected = slope_u * u + slope_v * v;
      worst = std::max(worst, std::abs(point.work[2] - expected));
      ok = ok && std::abs(point.work[0] - x) < 0.01 && std::abs(point.work[1] - y) < 0.01 &&
           std::abs(point.work[2] - expected) < 0.01 &&
           std::abs(point.machine[2] - (12 + expected)) < 0.01;
      std::cout << std::format("fact=height_map rotation={} point={} work_x={:.4f} work_y={:.4f} "
                               "work_z_expected={:.4f} work_z_measured={:.4f} machine_z_measured={:.4f}",
                               turn, i, point.work[0], point.work[1], expected, point.work[2],
                               point.machine[2])
                << std::endl;
    }
    // The probe ends at the clearance, work Z 3, less the encoder lag of the last retract.
    const double end_z = port.status().work[2];
    std::cout << std::format("fact=height_map rotation={} z_lag={} points={} worst_z_error={:.4f} "
                             "end_work_z={:.4f} g5x_unchanged={} g92_unchanged={} g91_restored={} "
                             "reported_rotation={:.9f} partial={} grid_ok={}",
                             turn, z_lag, points.size(), worst, end_z, port.status().g5x == before.g5x,
                             port.status().g92 == before.g92, incremental, result->height_map->rotation_deg,
                             result->height_map->partial.has_value(), ok)
              << std::endl;
    require(ok && incremental && std::abs(end_z - 3 - z_lag) < 0.03,
            std::format("the height map at {}° with {} mm Z lag probes every point of the grid, writes "
                        "nothing and restores G91",
                        turn, z_lag));
    set_lag(0);
  };
  map(0, 0);
  // Each later point starts after a retract, with the Z encoder 0.125 mm below the command as on
  // the G0759, within the map's 0.15 mm (HEIGHT_MAP_Z_LAG_TOLERANCE_MM's default).
  map(degrees, -0.125);

  // Refused before any motion: a grid point beyond the travel, the search depth below the travel
  // (clearance work Z -45 is machine Z -33, the search ends at -53 below Z min -50), and too many
  // points. The point is named in the machine's units.
  const std::string unit = scale == 1 ? "mm" : "in";
  frame(0);
  for (const auto& [grid, step, clearance, reason] :
       {std::tuple{std::array{-5.0, -4.0, 300.0, 12.0}, 10.0, 3.0, std::format(" {} is outside the configured travel", unit)},
        std::tuple{std::array{-5.0, -4.0, 15.0, 12.0}, 10.0, -45.0,
                   std::format("Grid point X{} Y{} {} is outside the configured travel at the clearance or the search depth.",
                               scale == 1 ? "-5.000" : "-0.1969", scale == 1 ? "-4.000" : "-0.1575", unit)},
        std::tuple{std::array{-5.0, -4.0, 15.0, 12.0}, 1.0, 3.0, std::string("2 to 150 points")}}) {
    const auto writes = port.writes().size();
    const auto refused = port.fixed_setter(map_request(grid, step, clearance), epoch, {});
    std::cout
        << std::format(
               "fact=height_map action=refused x_max={} step={} clearance={} refused={} writes={} "
               "reason=\"{}\"",
               grid[2], step, clearance, !refused, port.writes().size() - writes,
               refused ? "" : refused.error().text)
        << std::endl;
    require(!refused && refused.error().text.contains(reason) && port.writes().size() == writes,
            refused ? "expected a refused height map" : refused.error().text);
  }

  // Stopped maps. Each runs a single row of three points (work Y -4, X -5, 5 and 15; tops 0.07,
  // 0.17 and 0.27) and keeps the points measured before it stopped.
  struct Watch {
    HalShared& shared;
    int ended{};        // probe cycles (points) finished
    bool active{};      // a probe cycle runs
    bool touching{};    // and the probe touches
    void operator()() {
      const auto pins = shared.view(HalShared::Clock::now());
      const bool now = pins && pins->probe_cycle_active;
      if (active && !now)
        ++ended;
      active = now;
      touching = now && pins->probe_input.value_or(false);
    }
  };
  const auto count = [](const SetterState::HeightMap& m) {
    return std::ranges::count_if(m.points, [](const auto& point) { return point.measured; });
  };
  const auto at_rest = [&] {
    const auto first = machine();
    std::this_thread::sleep_for(300ms);
    const auto second = machine();
    return std::ranges::all_of(std::array{0, 1, 2}, [&](int i) {
      return std::abs(first.at(static_cast<std::size_t>(i)) - second.at(static_cast<std::size_t>(i))) < 0.001;
    });
  };
  const auto reset = [&] {
    checked(port.set_machine_state(MachineState::off, {}));
    checked(port.set_machine_state(MachineState::on, {}));
    checked(port.set_task_mode(TaskMode::mdi, {}));
    mdi("G21 G90 G94 G40");
  };
  // Runs the row with `during` called whenever the port asks whether the map is still wanted;
  // returns the result and whether the machine was still on afterwards.
  const auto stopped_map = [&](std::string_view name, double clearance, const auto& during,
                               std::size_t expected_points, std::string_view reason) {
    frame(0);
    Watch watch{shared};
    const auto result = port.fixed_setter(map_request({-5, -4, 15, -4}, 10, clearance), epoch,
                                          [&] { watch(); return during(watch); });
    const bool on = port.status().controller.machine == MachineState::on;
    const bool rest = at_rest();
    const auto points = result && result->height_map ? count(*result->height_map) : 0;
    const std::string why = !result ? result.error().text
                            : result->height_map && result->height_map->partial
                                ? *result->height_map->partial
                                : "";
    std::cout << std::format("fact=height_map action={} machine_on={} at_rest={} points={} partial={} reason=\"{}\"",
                             name, on, rest, points, result && result->height_map && result->height_map->partial,
                             why)
              << std::endl;
    const bool partial = expected_points > 0;
    require(rest && why.contains(reason) &&
                (partial ? result && result->height_map && result->height_map->partial &&
                               static_cast<std::size_t>(points) == expected_points &&
                               result->height_map->points.at(0).measured &&
                               std::abs(result->height_map->points.at(0).work[2] - 0.07) < 0.01
                         : !result),
            std::format("a height map {} with no further motion and {} measured point(s)", name, expected_points));
    return on;
  };
  const auto never = [](const Watch&) { return true; };

  // Any other probe stays strict: a single Z top refuses to start with the same 0.125 mm between
  // the command and the encoder, before it descends, and writes nothing.
  frame(0);
  set_lag(-0.125);
  {
    const auto g5x = port.status().g5x;
    const auto single = port.fixed_setter({SetterAction::material, 99, machine(), ProbeDirection::z_top}, epoch, {});
    std::cout << std::format("fact=height_map action=single_z_top_with_lag z_lag=-0.125 refused={} "
                             "g5x_unchanged={} "
                             "reason=\"{}\"",
                             !single, port.status().g5x == g5x, single ? "" : single.error().text)
              << std::endl;
    require(!single &&
                single.error().text.contains(
                    "Probe input, position or feed controls changed before descent.") &&
                port.status().g5x == g5x,
            "a single Z top still refuses 0.125 mm between the commanded and the measured Z");
  }
  reset();
  set_lag(0);

  // A real move: the Z encoder 0.2 mm off the command between the points, beyond the map's
  // 0.15 mm allowance. The map stops before the second point moves anything.
  bool lagged = false;
  require(stopped_map("refuses_a_position_change", 3,
                      [&](const Watch& watch) {
                        if (watch.ended == 1 && !lagged) {
                          halcmd(std::format("setp sim-z-backlash.in1 {:.12f}", -0.2 / scale));
                          std::this_thread::sleep_for(100ms);
                          lagged = true;
                        }
                        return true;
                      },
                      1, "The machine moved between the height map points."),
          "a Z lag beyond the backlash allowance leaves the machine on");
  set_lag(0);

  // An early trip: the feed down to clearance work Z 0.05 meets the 0.07 top of the first point.
  require(!stopped_map("trips_early_on_the_descent", 0.05, never, 0,
                       "Could not lower the probe to the search height"),
          "an early trip stops the machine");
  reset();

  // A missed trip: no top under the second point. The missed search returns to its start.
  bool lowered = false;
  require(stopped_map("misses_a_point", 3,
                      [&](const Watch& watch) {
                        if (watch.ended == 1 && !lowered) {
                          halcmd(std::format("setp sim-map-below.in2 {:.12f}", -1000 / scale));
                          lowered = true;
                        }
                        return true;
                      },
                      1, "No probe contact within the search travel"),
          "a miss leaves the machine on");
  halcmd(std::format("setp sim-map-below.in2 {:.12f}", -44 / scale));

  // Stop (the panel's abort) and E-stop while the second point's search touches.
  require(!stopped_map("stops_on_abort", 3, [](const Watch& watch) { return !(watch.ended == 1 && watch.touching); },
                       1, "the request was withdrawn"),
          "a stop mid-grid stops the machine");
  reset();
  bool estopped = false;
  require(!stopped_map("stops_on_estop", 3,
                       [&](const Watch& watch) {
                         if (watch.ended == 1 && watch.touching && !estopped) {
                           halcmd("unlinkp sim-probe-estop.in1");
                           halcmd("setp sim-probe-estop.in1 0");
                           estopped = true;
                         }
                         return true;
                       },
                       1, "Point X"),
          "an E-stop mid-grid stops the machine");
  halcmd("net estop-loop sim-probe-estop.in1");
  reset();

  // Item 4: the third top (0.27) is 0.93 mm under clearance work Z 1.2, within the 1 mm latch
  // back-off. The next traverse would skim it: the map stops with three of its six points.
  frame(0);
  {
    const auto result = port.fixed_setter(map_request({-5, -4, 15, 4}, 10, 1.2), epoch, {});
    const auto points = result && result->height_map ? count(*result->height_map) : 0;
    const std::string why = result && result->height_map && result->height_map->partial
                                ? *result->height_map->partial
                            : result ? ""
                                     : result.error().text;
    std::cout << std::format("fact=height_map action=margin points={} machine_on={} reason=\"{}\"", points,
                             port.status().controller.machine == MachineState::on, why)
              << std::endl;
    require(result && result->height_map && points == 3 && result->height_map->points.at(2).measured &&
                !result->height_map->points.at(3).measured && why.starts_with("The top at X") &&
                why.contains(std::format(" {} below the clearance Z, within the ", unit)) &&
                why.contains("Raise the clearance Z."),
            "a top within the latch back-off of the clearance stops the map with the points so far");
  }
  mdi("G54");
  require(port.status().g5x == g54, "the height map changes no work system");
}

void edge_rotations(NmlPort& port, double scale, const auto& mdi) {
  checked(port.set_tool({99, 99, 45, 2, "Ruby probe"}, {}));
  mdi("M61 Q99");
  mdi("G43 H99");
  mdi("G54");
  const auto g54 = port.status().g5x;
  mdi("G10 L2 P2 X5 Y-7 Z2 R0");
  mdi("G55");
  mdi("G92 X2 Y3 Z4");
  const auto position = [&] {
    const auto axes = port.status().controller.axes;
    return std::array{axes[0].machine, axes[1].machine, axes[2].machine};
  };
  halcmd("loadrt sum2 names=sim-edge-coordinate");
  halcmd("addf sim-edge-coordinate servo-thread");
  halcmd("net sim-edge-x axis.x.pos-cmd");
  halcmd("net sim-edge-y axis.y.pos-cmd");
  halcmd("unlinkp sim-tool-setter.in");
  halcmd("net sim-edge-normal sim-edge-coordinate.out sim-tool-setter.in");
  halcmd(std::format("setp sim-tool-setter.min {}", 6 / scale));
  halcmd(std::format("setp sim-tool-setter.max {}", 100 / scale));
  halcmd("setp sim-edge-coordinate.gain1 -0.05");
  const SetterEpoch epoch{1, 1, 1, 0};
  const auto measure = [&](ProbeDirection direction, double spacing, double expected) {
    mdi("G21 G90 G53 G0 X0 Y0 Z20");
    // A horizontal edge has Y = 6 + 0.05X, a vertical edge X = 6 + 0.05Y.
    halcmd("unlinkp sim-edge-coordinate.in0");
    halcmd("unlinkp sim-edge-coordinate.in1");
    const bool front = direction == ProbeDirection::y_plus;
    halcmd(front ? "net sim-edge-y sim-edge-coordinate.in0"
                 : "net sim-edge-x sim-edge-coordinate.in0");
    halcmd(front ? "net sim-edge-x sim-edge-coordinate.in1"
                 : "net sim-edge-y sim-edge-coordinate.in1");
    mdi(scale == 1 ? "G21" : "G20");
    const auto before = port.status();
    FixedSetterRequest request{SetterAction::angle, 99, position(), direction};
    request.gauge_width_mm = spacing;
    const auto measured = port.fixed_setter(request, epoch, {});
    const auto after = port.status();
    require(measured && measured->edge_angle_deg && measured->edge_rotation &&
                measured->edge_rotation->wcs == 2 &&
                std::abs(*measured->edge_angle_deg - expected) < 0.05 && before.g5x == after.g5x &&
                before.g92 == after.g92 && after.rotation_xy == 0,
            measured ? std::format("edge measured {:.3f}° in G55 without writing it",
                                   measured->edge_angle_deg.value_or(99))
                     : measured.error().text);
    return FixedSetterRequest{.action = SetterAction::apply_angle,
                              .tool = 99,
                              .edge_measurement = measured->edge_rotation->id};
  };
  const double expected = std::atan(0.05) * 180 / std::numbers::pi;
  const auto stale = measure(ProbeDirection::y_plus, 20, expected);
  mdi("G10 L2 P2 X6");
  const auto before_stale = port.writes().size();
  const auto refused = port.fixed_setter(stale, epoch, {});
  require(!refused && refused.error().text.contains("work frame") &&
              before_stale == port.writes().size(),
          "a changed work origin refuses the measured rotation before writing");
  for (const auto& [direction, angle] : {std::pair{ProbeDirection::y_plus, expected},
                                         std::pair{ProbeDirection::x_plus, -expected}}) {
    // Travel backwards on the front edge to verify that its orientation is directionless.
    const auto request = measure(direction, direction == ProbeDirection::y_plus ? -20 : 20, angle);
    const auto original = port.status();
    const auto before_apply = position();
    const auto applied = port.fixed_setter(request, epoch, {});
    const auto after = port.status();
    if (!applied)
      std::cout << std::format(
          "verify rotation expected={:.10f} actual={:.10f} wcs={} "
          "g5x_delta={:.12g},{:.12g},{:.12g} g92_delta={:.12g},{:.12g},{:.12g} still={} "
          "executing={} paused={} spindle={} xyz_delta={:.10g},{:.10g},{:.10g}\n",
          angle, after.rotation_xy, after.wcs, after.g5x[0] - original.g5x[0],
          after.g5x[1] - original.g5x[1], after.g5x[2] - original.g5x[2],
          after.g92[0] - original.g92[0], after.g92[1] - original.g92[1],
          after.g92[2] - original.g92[2], after.controller.in_position, after.task_executing,
          after.paused, after.controller.spindle_active, position()[0] - before_apply[0],
          position()[1] - before_apply[1], position()[2] - before_apply[2]);

    require(applied && applied->edge_angle_deg && std::abs(after.rotation_xy - angle) < 0.05 &&
                after.wcs == 2 &&
                std::ranges::equal(original.g5x, after.g5x,
                                   [](double a, double b) { return std::abs(a - b) < 1e-6; }) &&
                std::ranges::equal(original.g92, after.g92,
                                   [](double a, double b) { return std::abs(a - b) < 1e-6; }) &&
                before_apply == position(),
            applied ? std::format("G55 applied {:.3f}° without changing origin, shifts or position",
                                  after.rotation_xy)
                    : applied.error().text);
    const auto replay = port.fixed_setter(request, epoch, {});
    require(!replay, "a rotation measurement can only be applied once");
    mdi("G10 L2 P2 R0");
  }
  mdi("G54");
  require(port.status().g5x == g54 && port.status().rotation_xy == 0,
          "applying G55 rotation leaves G54 unchanged");
}

#ifdef COFFEECNC_PREVIEW_HELPER_BUILT
// The spindle tool number can be published before the independent status reader sees idle
// MDI. Keep the controller's real status and mailbox intact and order just those two reports
// to the session so the handoff cannot pass by chance in the simulation.
class M6StatusOrder final : public StatusSource {
public:
  explicit M6StatusOrder(StatusSource& controller) : controller_(controller) {}

  auto read() -> ControllerState override {
    auto state = controller_.read();
    if (state.link == Link::live && state.mode == TaskMode::mdi &&
        state.tool_in_spindle == 2 && !state.requested_tool) {
      const auto now = HalShared::Clock::now();
      if (!idle_after_)
        idle_after_ = now + 200ms;
      if (now < *idle_after_)
        state.interp = InterpState::reading;
    }
    return state;
  }

private:
  StatusSource& controller_;
  std::optional<HalShared::Clock::time_point> idle_after_;
};

void measure_m6(const MachineConfig& config, NmlPort& port, HalControl& control,
                HalShared& shared, NmlLoads& loads, const auto& mdi) {
  using Json = nlohmann::json;
  const auto scale = config.millimetres_per_unit;
  const double reference_length = scale == 1 ? 50 : 50.8;
  checked(port.set_tool({98, 98, reference_length, 0, "Setter reference"}, {}));
  mdi("M61 Q98");
  mdi("G43 H98");
  mdi("G53 G0 X-12 Y-18 Z22");
  halcmd(std::format("setp sim-tool-setter.max {}", 10 / scale));

  NmlStatusMailbox mailbox{&loads};
  auto reader = NmlStatusReader::connect(config, &shared, &mailbox);
  auto table = LoadedToolTable::attach(scale);
  require(reader.has_value() && table.has_value(), "M6 simulation has status and loaded tools");
  LocalFileSystem files;
  ProgramStore programs{files, config.program_prefix};
  std::vector<RawTool> raw;
  NmlPanelSource source{config, mailbox,
                       [&]() -> std::optional<std::vector<LoadedTool>> {
                         raw = table->read_raw();
                         return loaded_tools(raw, scale);
                       },
                       [&](std::string_view path, std::uint64_t revision) {
                         return programs.read_program(path, revision);
                       },
                       &loads};
  source.raw_tools([&] { return raw; });
  const auto settings = load_preview_settings(config, COFFEECNC_PREVIEW_HELPER_BUILT,
                                               config.directory + "/preview-work");
  HelperPreviewEngine engine{settings};
  HelperPreviewEngine restart_engine{settings};
  PreviewWorker preview{engine};
  PanelComposer composer{Hello{}, &preview};
  std::mutex mutex;
  Json result;
  int request_id = 0;
  const auto receive = [&](const Outcome& outcome) {
    for (const auto& text : outcome.messages) {
      auto message = Json::parse(text);
      if (message.value("type", "") == "result") {
        const std::scoped_lock lock{mutex};
        result = std::move(message);
      }
    }
  };
  M6StatusOrder ordered_status{**reader};
  Runtime runtime{port, ordered_status, control, config.limits, programs, receive, nullptr,
                  Runtime::Quit{}, &preview, &restart_engine};
  const auto page = runtime.begin_page();
  PanelReading reading;
  const auto wait = [&](auto condition, auto timeout) {
    const auto deadline = HalShared::Clock::now() + timeout;
    do {
      reading = source.read();
      composer.observe(reading);
      if (condition())
        return true;
      std::this_thread::sleep_for(20ms);
    } while (HalShared::Clock::now() < deadline);
    return false;
  };
  const auto send = [&](Json request, std::chrono::milliseconds timeout = 12s) {
    request["id"] = ++request_id;
    receive(runtime.submit(page, request.dump()));
    if (!wait([&] {
          const std::scoped_lock lock{mutex};
          return result.is_object() && result.value("id", 0) == request_id;
        }, timeout))
      throw std::runtime_error("Timed out waiting for " + request.dump());
    const std::scoped_lock lock{mutex};
    return result;
  };
  const auto accepted = [&](Json request, std::chrono::milliseconds timeout = 12s) {
    auto reply = send(std::move(request), timeout);
    if (reply.value("outcome", "") != "accepted")
      throw std::runtime_error(reply.dump());
  };
  accepted({{"type", "ready"}});
  require(wait([&] {
    const auto pins = shared.view(HalShared::Clock::now());
    return reading.status.link == Link::live && pins && pins->watchdog_ok && !pins->jog_stop_high;
  }, 4s), "M6 simulation is live with a serviced watchdog");
  accepted({{"type", "fixed_setter"}, {"action", "touch_off"}, {"tool", 98},
            {"x", -12}, {"y", -18}, {"z", 22}});
  accepted({{"type", "mdi"}, {"text", "M61 Q1 G43 H1"}});
  require(wait([&] {
    return reading.status.tool.in_spindle == 1 && reading.status.interp == InterpState::idle;
  }, 4s), "T1 is loaded before opening the program");
  accepted({{"type", "mdi"}, {"text", "G92.1 G10 L2 P1 X0 Y0 Z-40 R0"}});
  require(wait([&] {
    const auto latest = mailbox.latest();
    return latest && latest->status.controller.interp == InterpState::idle &&
           std::abs(latest->status.g5x[2] + 40) < 0.01;
  }, 4s), "the programmed work origin is set");
  const auto original_position = [&] {
    // On the inch fixture M6 itself moves X4 Y6 Z35 to the configured X2 Y3 Z30.
    accepted({{"type", "mdi"}, {"text", scale == 1 ? "G53 G0 X2 Y3 Z30" : "G53 G0 X4 Y6 Z35"}});
    const double start_x = scale == 1 ? 2 : 4;
    require(wait([&] {
      return reading.status.interp == InterpState::idle && reading.status.motion.in_position &&
             reading.commanded_position && std::abs(reading.commanded_position->at(0) - start_x) < 0.01;
    }, 5s), "the original program starts away from the setter");
  };
  original_position();

  const auto begin_exchange = [&](std::string_view name, std::string_view program) {
    const auto path = config.program_prefix + "/" + std::string(name) + ".ngc";
    {
      std::ofstream out{path};
      out << program;
    }
    accepted({{"type", "open_program"}, {"path", path}});
    require(wait([&] {
      const auto check = preview.checked_preview();
      return check.preview && check.preview->state == PreviewState::ready && check.source.inputs &&
             check.source.inputs->program_file == path;
    }, 6s), "the original program has a complete check");
    accepted({{"type", "run"}, {"confirmed", true}, {"display_units", "mm"},
              {"preview_context", preview.checked_preview().preview->context}});
    std::optional<std::uint64_t> generation;
    require(wait([&] {
      const auto* change = std::get_if<WaitToolChange>(&reading.status.wait);
      if (!change || change->tool != 2)
        return false;
      generation = change->measure_request;
      return generation.has_value();
    }, 15s), "the panel offers measurement on the exact M6");
    return generation.value();
  };
  const auto measure = [&](std::uint64_t generation) -> Json {
    return send({{"type", "tool_changed"}, {"measure_request", generation}}, 35s);
  };
  const auto pending = [&](std::uint64_t generation) {
    const auto pins = shared.view(HalShared::Clock::now());
    return pins && pins->tool_change_request && pins->tool_change_request->generation == generation;
  };
  const auto stop = [&] {
    accepted({{"type", "abort"}});
    require(wait([&] {
      return reading.status.interp == InterpState::idle && reading.status.motion.in_position &&
             !std::holds_alternative<WaitToolChange>(reading.status.wait);
    }, 5s), "the simulation is stopped before the next program");
  };
  const auto blocked = begin_exchange("unsupported-m6",
      "N10 G21 G90 G17 G54 G40 G49 G94\nN20 T2 M6\nN30 G0 X8 Y5\nN40 G43 H2 Z25\n"
      "N50 G1 X22 F600\nM2\n");
  const auto unsupported = measure(blocked);
  require(unsupported.value("outcome", "") == "rejected" && pending(blocked),
          "an unsupported first motion leaves M6 pending");
  stop();

  const auto set_t2 = [&](double length) {
    accepted({{"type", "set_tool"}, {"tool", {{"number", 2}, {"pocket", 2}, {"length", length},
                                             {"diameter", 3}, {"comment", "Test cutter"}}}});
  };
  set_t2(70);
  const auto tall = begin_exchange("outside-clearance-m6",
      "N10 G21 G90 G17 G54 G40 G43 H1 G94\nN20 G53 G0 Z95\nN30 G53 G0 Z30\nN40 T2 M6\n"
      "N50 G43 H2\nN60 G0 Z25\nN70 G1 X22 F600\nM2\n");
  const auto before_aborts = loads.aborts.load();
  for (int attempt = 0; attempt < 2; ++attempt) {
    const auto refused = measure(tall);
    require(refused.value("reason", "").find("outside the configured machine travel") !=
                std::string::npos && pending(tall) && loads.aborts.load() == before_aborts,
            refused.dump());
  }
  stop();
  set_t2(scale == 1 ? 40 : 38.1);
  original_position();

  const auto generation = begin_exchange("measure-m6",
      "N10 G21 G90 G17 G54 G40 G49 G94\nN20 T2 M6\nN30 G43 H2\nN40 G0 Z25\n"
      "N50 G0 X8 Y5\nN60 G1 Z15 F300\nN70 G1 X22 F600\nN80 G0 Z20\nM2\n");
  const auto wrong = measure(generation + 1);
  require(wrong.value("outcome", "") == "rejected" && pending(generation),
          "a different request cannot start measurement or acknowledge M6");
  halcmd(std::format("setp sim-tool-setter.max {}", (51 - reference_length) / scale));
  const Json measured = measure(generation);
  require(measured.value("outcome", "") == "accepted" && measured.contains("restart"),
          measured.dump());
  const auto& plan = measured.at("restart");
  require(plan.at("line") == 4 && plan.at("h") == 2 && !plan.at("needs_tool_change").get<bool>() &&
              std::abs(plan.at("start").at(0).get<double>() - 2) < 0.03 &&
              std::abs(plan.at("start").at(1).get<double>() - 3) < 0.03 &&
              reading.status.interp == InterpState::idle,
          "measurement leaves a stopped restart review at the original XY and first post-M6 motion");
  const auto applied = mailbox.latest();
  require(applied && std::abs(applied->status.tool_offset[2] - 41) < 0.03,
          "the measured length has been applied by G43");
  accepted({{"type", "start_restart"}, {"plan_id", plan.at("plan_id")}}, 60s);
  require(wait([&] {
    return reading.status.interp == InterpState::idle && reading.commanded_position &&
           std::abs(reading.commanded_position->at(0) - 22) < 0.03;
  }, 25s), "the reviewed restart completes without another M6");
}
#endif

void run(const std::string& ini, std::string_view scenario) {
  const bool material = scenario == "material";
  const bool edge = scenario == "edge-rotation";
  const bool features = scenario == "features";
  const bool runout = scenario == "runout" || scenario == "runout-pin" || scenario == "runout-arc";
  const bool measure = scenario == "measure";
  const bool measure_geometry = scenario == "measure-geometry";
  const bool rotated = scenario == "rotated" || measure || measure_geometry;
  const bool verify_circle = scenario == "circle-check";
  const bool arc =
      scenario == "arc" || scenario == "arc-rotated" || scenario == "arc-faults" || verify_circle;
  const bool height_map = scenario == "height-map";
  const bool g0759 = std::filesystem::path(ini).filename() == "coffeecnc_g0759.ini";
  auto config = load_machine_config(ini);
  if (!config)
    throw std::runtime_error(config.error());
  const double scale = config->millimetres_per_unit;
  config->signals.probe = true;
  config->signals.spindle_rpm = true;
  constexpr int reference_tool = 98;
  const double reference_length = scale == 1 ? 50 : 50.8;
  config->fixed_setter = FixedSetterLimits{15, 240, reference_tool, 0.5, 12, 60, 1};
  if (scenario == "measure-m6")
    config->fixed_setter->travel_mm = 30;
  if (verify_circle) {
    config->fixed_setter->feed_mm_min = 600;
    config->fixed_setter->latch_feed_mm_min = 180;
    config->fixed_setter->latch_backoff_mm = 0.5;
  }
  if (material || edge || features || rotated || runout || arc || height_map)
    config->touch_probe_tool = 99;
  // The runner recreates the table, but retains files beside it between runs.
  std::filesystem::remove(std::filesystem::path(config->tool_table).parent_path() /
                          "coffeecnc-touched-off.txt");
  std::filesystem::remove(config->probe_runout_file());
  auto component = HalComponent::create("coffeecnc", config->signals);
  if (!component)
    throw std::runtime_error(component.error());
  halcmd(std::format("-f '{}'", config->postgui_halfile));
  wire(scale, g0759);
  HalShared shared;
  HalControl control{*component, shared};
  std::jthread control_thread{[&](std::stop_token stop) {
    while (!stop.stop_requested()) {
      control.beat();
      std::this_thread::sleep_for(5ms);
    }
  }};
  NmlLoads loads;
  const auto connect = [&] {
    auto connected = NmlPort::connect(*config, &shared, &loads);
    if (!connected)
      throw std::runtime_error(connected.error());
    wait_for(**connected, [](const NmlStatus& s) { return s.controller.link == Link::live; });
    return std::move(*connected);
  };
  auto port = connect();
  checked(port->set_machine_state(MachineState::off, {}));
  checked(port->set_machine_state(MachineState::on, {}));
  if (g0759) {
    for (const auto axis : {Axis::x, Axis::y, Axis::z})
      checked(port->home(axis, {}));
  } else {
    checked(port->home({}, {}));
  }
  wait_for(*port, [](const NmlStatus& s) {
    return std::ranges::all_of(s.controller.axes, [](auto axis) { return axis.homed; });
  });
  checked(port->set_task_mode(TaskMode::mdi, {}));
  const auto mdi = [&](std::string_view line) {
    checked(port->mdi(line, {}));
    wait_for(*port, [](const NmlStatus& s) {
      return s.controller.interp == InterpState::idle && s.controller.in_position;
    });
  };
  mdi("G21 G90 G94 G40");
  if (arc) {
    const double rotation =
        scenario == "arc-rotated" || verify_circle ? (scale == 1 ? 7.5 : -30) : 0;
    arc_scenarios(*port, scale, rotation, mdi, scenario == "arc-faults", shared, config->tool_table,
                  verify_circle);
    return;
  }
  if (rotated) {
    // Millimetres at +7.5°, inches at -30°: each rotated cycle in both units and turns.
    rotated_scenarios(*port, scale, scale == 1 ? 7.5 : -30, mdi, measure, config->tool_table,
                      config->program_prefix, config->probe_runout_file(), measure_geometry);
    return;
  }
  if (runout) {
    // The compensated bore also in G56 rotated +7.5° (mm) or -30° (inch).
    // A reconnect changes the material search travel.
    runout_scenarios(port, shared, scale, scale == 1 ? 7.5 : -30,
                     scenario == "runout-pin" ? "pin" : scenario == "runout-arc" ? "arc" : "ring",
                     config->probe_runout_file(), mdi,
                     [&](double travel) {
                       config->fixed_setter->material_travel_mm = travel;
                       port.reset();
                       port = connect();
                     });
    return;
  }
  if (height_map) {
    // Unrotated, then +7.5° in mm and -30° in inch.
    height_map_scenarios(*port, shared, scale, scale == 1 ? 7.5 : -30, mdi);
    return;
  }
  if (scenario == "measure-m6") {
#ifdef COFFEECNC_PREVIEW_HELPER_BUILT
    control_thread.request_stop();
    control_thread.join();
    measure_m6(*config, *port, control, shared, loads, mdi);
    return;
#else
    throw std::runtime_error("The M6 scenario needs the preview helper.");
#endif
  }
  if (features) {
    feature_scenarios(*port, scale, mdi);
    return;
  }
  if (edge) {
    edge_rotations(*port, scale, mdi);
    return;
  }
  if (material) {
    material_scenarios(port, shared, scale, mdi, [&] {
      port.reset();
      port = connect();
    });
    return;
  }
  checked(port->set_tool({.number = reference_tool,
                          .pocket = reference_tool,
                          .length = reference_length,
                          .diameter = 0,
                          .comment = "Setter reference"},
                         {}));
  mdi("M61 Q98");
  mdi("G43 H98");
  mdi("G53 G0 Z20");
  mdi("G92 Z12");
  mdi(scale == 1 ? "G20 G91" : "G21 G91");
  mdi(scale == 1 ? "F2" : "F50.8");
  halcmd(std::format("setp sim-tool-setter.max {}", 10 / scale));
  const auto starting = port->status();
  const auto& axes = starting.controller.axes;
  FixedSetterMove move{reference_tool, {axes[0].machine, axes[1].machine, axes[2].machine}, 15, 240};

  auto invalid_move = move;
  invalid_move.travel_mm = 16;
  const auto commands_before = port->writes().size();
  require(!port->probe_fixed_setter(invalid_move, {}) && port->writes().size() == commands_before,
          "travel beyond the configured limit sends no command");
  const auto contact = port->probe_fixed_setter(move, {});
  require(contact.has_value(), contact ? "native fixed setter completed" : contact.error());
  require(std::abs(contact->machine[2] - 10) < 0.03, "native latch is in machine millimetres");
  std::cout << std::format("fact=stopping_distance_mm={:.6f}\n", contact->stopping_distance);
  require(contact->stopping_distance > 0.001 && contact->stopping_distance < 0.1,
          "native stopping distance comes from the settled Z before retract");
  const auto returned = port->status();
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
    const auto result = port->fixed_setter(request, epoch, {});
    if (!result)
      throw std::runtime_error(result.error().text);
    return *result;
  };
  const auto read_table = [&]() {
    // Line by line: GCC 14 -O3 flags istreambuf_iterator with -Wnull-dereference.
    std::ifstream input(config->tool_table);
    std::string text;
    for (std::string line; std::getline(input, line);)
      text += line + '\n';
    return text;
  };
  const auto old_table = read_table();
  const auto premature = port->fixed_setter(
      {.action = SetterAction::check, .tool = reference_tool, .start = move.start}, epoch, {});
  require(!premature && premature.error().text.contains("Teach the setter") &&
              premature.error().localization ==
                  MessageKey{"backend.nmlPort.teachSetter",
                             {{"tool", std::to_string(reference_tool)}}} &&
              old_table == read_table(),
          "checking requires a taught plane even with the reference tool loaded");
  const auto taught = setter(
      {.action = SetterAction::touch_off, .tool = reference_tool, .start = move.start});
  require(taught.measurement && taught.measurement->calibration && taught.calibration &&
              std::abs(taught.calibration->at(2) - (10 - reference_length)) < 0.03 &&
              std::abs(taught.measurement->stopping_distance - contact->stopping_distance) < 0.01 &&
              old_table == read_table(),
          "teaching uses the reference tool's existing length and leaves the table unchanged");

  mdi("M61 Q1");
  mdi("G43 H1");
  ++epoch.tool;
  mdi("G21 G90 G53 G0 X4 Y6 Z17");
  mdi(scale == 1 ? "G20 G91" : "G21 G91");
  halcmd(std::format("setp sim-tool-setter.max {}", 12 / scale));
  const auto at_work = port->status().controller.axes;
  const FixedSetterRequest touch_off{
      .action = SetterAction::touch_off,
      .tool = 1,
      .start = {at_work[0].machine, at_work[1].machine, at_work[2].machine}};
  const auto measured = setter(touch_off);
  const double tool_length = reference_length + 2;
  require(measured.measurement && !measured.measurement->calibration &&
              std::abs(measured.measurement->value - tool_length) < 0.03 &&
              measured.measurement->stopping_distance > 0.001,
          "one touch-off moves to the setter and reports the applied length");
  const auto after = port->status();
  const auto saved_table = read_table();
  require(std::abs(after.controller.axes[0].machine - move.start[0]) < 0.03 &&
              std::abs(after.controller.axes[1].machine - move.start[1]) < 0.03 &&
              std::abs(after.controller.axes[2].machine - (taught.measurement->contact[2] + 12)) <
                  0.03,
          "the cycle returns to the configured clearance above the reference latch");
  require(std::abs(after.tool_offset[2] - tool_length) < 0.03 &&
              std::abs(tool_z(saved_table, 1).value_or(0) * scale - tool_length) < 0.03 &&
              tool_z(saved_table, reference_tool) == tool_z(old_table, reference_tool) &&
              saved_table.contains(scale == 1 ? "D+6.000000" : "D+0.250000"),
          "only this tool's length changes and it is applied");

  int trips = 0;
  bool tripped = false;
  bool shifted = false;
  auto repeat = touch_off;
  repeat.start = {after.controller.axes[0].machine, after.controller.axes[1].machine,
                  after.controller.axes[2].machine};
  auto check = repeat;
  check.action = SetterAction::check;
  const auto intact = [&] {
    const auto observed = port->status();
    return read_table() == saved_table && observed.tool_offset == after.tool_offset &&
           std::abs(observed.controller.axes[2].machine - repeat.start[2]) < 0.03;
  };
  halcmd(std::format("setp sim-tool-setter.max {}", 11.94 / scale));
  const auto passed = setter(check);
  require(passed.measurement && passed.measurement->check_tolerance_mm == 0.1 &&
              std::abs(passed.measurement->value - (tool_length - 0.06)) < 0.03 &&
              passed.measured_tools == measured.measured_tools && intact(),
          "a check passes within tolerance and retracts with the old table and offset");
  for (const auto& [height, direction] :
       {std::pair{11.7, "shorter"}, std::pair{12.3, "longer"}}) {
    halcmd(std::format("setp sim-tool-setter.max {}", height / scale));
    const auto failed = port->fixed_setter(check, epoch, {});
    require(!failed && failed.error().text.contains(direction) &&
                failed.error().text.contains("tolerance") && intact(),
            std::format("a {} tool fails after retract and keeps its table and offset ({})",
                        direction, failed ? "unexpected pass" : failed.error().text));
  }
  halcmd(std::format("setp sim-tool-setter.max {}", 12 / scale));
  const auto unstable = port->fixed_setter(repeat, epoch, [&]() {
    const auto pins = shared.view(HalShared::Clock::now());
    if (pins && pins->probe_cycle_active && pins->probe_input) {
      if (*pins->probe_input && !tripped)
        ++trips;
      if (!*pins->probe_input && tripped && trips == 2 && !shifted) {
        halcmd(std::format("setp sim-tool-setter.max {}", 11.95 / scale));
        shifted = true;
      }
      tripped = *pins->probe_input;
    }
    return true;
  });
  require(shifted && trips == 4 && !unstable && unstable.error().text.contains("exceeds 0.01 mm") &&
              saved_table == read_table() && port->snapshot().machine == MachineState::on,
          "three slow contacts with excessive spread retract and leave the table untouched");

  const auto setter_status = [&] { return setter({.action = SetterAction::status}); };
  const auto touch_request = [&](int tool) -> FixedSetterRequest {
    const auto current = port->status().controller.axes;
    return {.action = SetterAction::touch_off,
            .tool = tool,
            .start = {current[0].machine, current[1].machine, current[2].machine}};
  };
  const auto teach_reference = [&] {
    mdi("M61 Q98");
    mdi("G43 H98");
    ++epoch.tool;
    mdi("G21 G90 G53 G0 X0 Y0 Z20");
    halcmd(std::format("setp sim-tool-setter.max {}", 10 / scale));
    return setter(touch_request(reference_tool));
  };

  require(setter_status().measured_tools == std::vector<int>{1, reference_tool},
          "T1 and the reference are marked before restarting the port");
  port.reset();
  port = connect();
  ++epoch.page;
  const auto restarted = setter_status();
  require(!restarted.calibration &&
              restarted.measured_tools == std::vector<int>{1, reference_tool},
          "restarting the port keeps the valid saved marks without a taught setter");
  // turo: a tool with a valid saved length teaches the setter like the reference.
  mdi("M61 Q1");
  mdi("G43 H1");
  ++epoch.tool;
  mdi("G21 G90 G53 G0 X0 Y0 Z20");
  halcmd(std::format("setp sim-tool-setter.max {}", 10 / scale));
  const auto table_before_t1_teach = read_table();
  const auto taught_by_t1 = setter(touch_request(1));
  require(taught_by_t1.calibration && taught_by_t1.measurement &&
              taught_by_t1.measurement->calibration && taught_by_t1.measurement->tool == 1 &&
              taught_by_t1.measured_tools == std::vector<int>{1, reference_tool} &&
              read_table() == table_before_t1_teach,
          "after a restart a touched-off T1 teaches the setter and keeps its length");
  port.reset();
  port = connect();
  ++epoch.page;
  require(teach_reference().measured_tools == std::vector<int>{1, reference_tool},
          "re-teaching T98 after restart restores the persisted T1 mark");

  // Keep a second measured tool marked so an invalid reference must clear more than itself.
  mdi("M61 Q2");
  mdi("G43 H2");
  const double original_t2_length = scale == 1 ? 40 : 38.1;
  wait_for(*port, [&](const NmlStatus& s) {
    return s.controller.tool_in_spindle == 2 &&
           std::abs(s.tool_offset[2] - original_t2_length) < 0.001;
  });
  ++epoch.tool;
  mdi("G53 G0 X4 Y6 Z24");
  halcmd(std::format("setp sim-tool-setter.max {}", 12 / scale));
  require(setter(touch_request(2)).measured_tools == std::vector<int>{1, 2, reference_tool},
          "T2 joins the restored T1 mark and the reference");
  mdi(std::format("G10 L1 P1 Z{}", tool_length + 1));
  const auto edited_tool = setter_status();
  require(edited_tool.calibration &&
              edited_tool.measured_tools == std::vector<int>{2, reference_tool},
          "G10 changing T1 length clears only its mark");
  mdi(std::format("G10 L1 P98 Z{}", reference_length + 1));
  const auto writes_before_reteach = port->writes().size();
  const auto stale = port->fixed_setter(touch_request(2), epoch, {});
  require(
      !stale &&
          stale.error().text.contains("Teach the setter with T98 or a touched-off tool first") &&
          port->writes().size() == writes_before_reteach,
      "a changed reference blocks the next touch-off before any command");
  const auto edited_reference = setter_status();
  require(!edited_reference.calibration && edited_reference.measured_tools.empty(),
          "changing T98 length clears every mark and invalidates calibration");
  mdi(std::format("G10 L1 P98 Z{}", reference_length));
  require(teach_reference().measured_tools == std::vector<int>{reference_tool},
          "re-teaching after restoring the old T98 length does not revive cleared marks");

  ++epoch.datum;
  require(!setter_status().calibration, "rehoming clears the in-memory setter calibration");

  mdi("M61 Q98");
  mdi("G43 H98");
  mdi("G21 G90 G53 G0 Z20");
  halcmd(std::format("setp sim-tool-setter.max {}", 10 / scale));
  const auto stop_at = HalShared::Clock::now() + 600ms;
  require(!port->probe_fixed_setter(move, [&]() { return HalShared::Clock::now() < stop_at; }),
          "withdrawing a running probe invalidates its result");
  const auto interrupted = port->status();
  const double interrupted_z = interrupted.controller.axes[2].machine;
  require(interrupted.controller.machine != MachineState::on && interrupted_z > 10 &&
              interrupted_z < 19,
          "the real HAL gate stops an interrupted native descent before contact");

  // Reset only this isolated simulation, then exercise a trajectory that cannot reach the setter.
  checked(port->set_machine_state(MachineState::off, {}));
  checked(port->set_machine_state(MachineState::on, {}));
  checked(port->set_task_mode(TaskMode::mdi, {}));
  mdi("G21 G90 G94");
  mdi("G53 G0 Z20");

  move.travel_mm = 5;
  require(!port->probe_fixed_setter(move, {}), "missing contact invalidates the native probe");
  const auto stopped = port->status();
  require(stopped.controller.machine != MachineState::on && stopped.controller.axes[2].machine < 19,
          "a missed probe stops without returning to the starting height");
}

} // namespace

auto main(int argc, char** argv) -> int {
  const auto* run_dir = std::getenv("COFFEECNC_SIM_RUN");
  if ((argc != 3 && argc != 4) || std::string_view(argv[1]) != "-ini" || run_dir == nullptr)
    return 1;
  const std::string_view scenario = argc == 4 ? argv[3] : "";
  const std::filesystem::path ini = argv[2];
  if ((scenario != "" && scenario != "material" && scenario != "edge-rotation" &&
       scenario != "features" && scenario != "rotated" && scenario != "runout" &&
       scenario != "runout-pin" && scenario != "runout-arc" && scenario != "measure" &&
       scenario != "measure-geometry" && scenario != "arc" && scenario != "arc-rotated" &&
       scenario != "arc-faults" && scenario != "height-map" && scenario != "measure-m6" &&
       scenario != "circle-check") ||
      ini.parent_path() != run_dir ||
      (ini.filename() != "coffeecnc_sim.ini" &&
       !(scenario == "edge-rotation" && ini.filename() == "coffeecnc_g0759.ini")))
    return 1;
  try {
    run(argv[2], scenario);
    return 0;
  } catch (const std::exception& failure) {
    std::cout << "check=native fixed setter failed: " << failure.what() << '\n';
    return 1;
  }
}
