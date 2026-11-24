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
#include <stdexcept>
#include <string>
#include <thread>
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

void wire_z_feedback() {
  // Report the motor's commanded position plus the backlash in the last positive Z move.
  // The offset starts at zero and the test enables it only for one probe cycle.
  halcmd("loadrt mux2 names=sim-z-backlash");
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
      throw std::runtime_error(result.error());
    return *result;
  };
  const auto setter_status = [&]() -> SetterState {
    const auto result = port->fixed_setter({.action = SetterAction::status}, epoch, {});
    if (!result)
      throw std::runtime_error(result.error());
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
    require(result.has_value(), result ? "material probe completed" : result.error());
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
  require(!missed_corner && missed_corner.error().contains("lifted clear") &&
              std::abs(position()[2] - 20) < 0.2 && port->status().g5x == corner_before.g5x,
          missed_corner ? "expected a missed outside face" : missed_corner.error());
  start_at();
  corner_window('z', -100, 12);
  const auto corner = port->fixed_setter(
      {SetterAction::corner, 99, position(), ProbeDirection::x_plus, ProbeDirection::y_plus}, epoch,
      {});
  require(corner.has_value(), corner ? "corner probe completed" : corner.error());
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
  require(!low && low.error().contains("above the top"), low ? "expected a refusal" : low.error());

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
  require(calibrated && calibrated->ball_diameter && std::abs(*calibrated->ball_diameter - 2.1) < 0.02,
          calibrated ? std::format("the gauge measured the ball at {:.3f} mm",
                                   calibrated->ball_diameter.value_or(0))
                     : calibrated.error());
  require(std::abs(position()[2] - 20) < 0.2, "the calibration lifted the probe afterwards");
  const auto wrong = gauge(24);
  require(!wrong && wrong.error().contains("more than 0.5 mm"),
          wrong ? "expected a refused diameter" : wrong.error());
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
                             boss->center_diameter.value_or(0), boss_after.work[0], boss_after.work[1])
               : boss.error());

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
  require(!wrong_boss && wrong_boss.error().contains("do not enclose") &&
              port->status().g5x == before_wrong_boss && std::abs(position()[2] - 20) < 0.2,
          wrong_boss ? "expected a refusal for a boss outside the start" : wrong_boss.error());

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
                             bore->center_diameter.value_or(0), bore_after.work[0], bore_after.work[1])
               : bore.error());
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
          edge ? std::format("the square front edge measured {:.3f}°", edge->edge_angle_deg.value_or(99))
               : edge.error());
  angle_request.gauge_width_mm = 2;
  angle_request.start = position();
  const auto close = port->fixed_setter(angle_request, epoch, {});
  require(!close && close.error().contains("5 to 300 mm"), close ? "expected a refusal" : close.error());

  start_at();
  mdi("G10 L2 P2 R90");
  window('y', 6, 100);
  check_cycle(ProbeDirection::x_plus, 0, -7);

  start_at();
  window('z', -100, -50);
  const auto start = position();
  const auto before = port->status().g5x;
  const auto failed_cleanly = [&](const auto& result, std::string_view reason) {
    require(!result && result.error().contains(reason),
            result ? "expected a probe failure" : result.error());
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
    require(touched.has_value(), touched ? "the reference and probe have known lengths" : touched.error());
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
      require(hit && hit->material && hit->material->corner &&
                  std::abs(status.work[0] - (2 - edge_x)) < 0.03 &&
                  std::abs(status.work[1] - (1 - edge_y)) < 0.03 &&
                  std::abs(machine()[2] - 20) < 0.03 && std::abs(status.g5x[2] - initial.g5x[2]) < 1e-6,
              hit ? "inside corner sets both axes using the selected wall signs" : hit.error());
    }
  }
  at_start();
  const auto pocket = run(SetterAction::pocket);
  const auto pocket_status = port.status();
  require(pocket && pocket->feature_size[0] && pocket->feature_size[1] &&
              std::abs(*pocket->feature_size[0] - 20) < 0.03 &&
              std::abs(*pocket->feature_size[1] - 16) < 0.03 &&
              std::abs(pocket_status.work[0] + 2) < 0.03 && std::abs(pocket_status.work[1] - 3) < 0.03 &&
              std::abs(pocket_status.g5x[2] - initial.g5x[2]) < 1e-6,
          pocket ? "rectangular pocket finds both physical widths and sets XY together" : pocket.error());

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
                std::abs(after.work[across] - (machine()[across] - (across == 0 ? 4 : -2))) < 0.03 &&
                std::abs(after.g5x[1 - across] - before.g5x[1 - across]) < 1e-6 &&
                std::abs(after.g5x[2] - before.g5x[2]) < 1e-6 &&
                after.g92 == before.g92 && std::abs(machine()[2] - 20) < 0.03,
            measured ? "span sets one axis with ball compensation and leaves the other axes intact"
                     : measured.error());
  };
  span(SetterAction::slot, 0, across == 0 ? 20 : 16);

  at_start();
  const auto before_miss = port.status();
  limits('y', -100, 100);
  const auto miss = run(SetterAction::inside_corner, ProbeDirection::x_minus, ProbeDirection::y_plus);
  require(!miss && miss.error().contains("No probe contact") && port.status().g5x == before_miss.g5x &&
              std::abs(machine()[0] - 2) < 0.03 && std::abs(machine()[1] - 1) < 0.03,
          miss ? "expected a missed second wall" : miss.error());
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
  require(!ridge_miss && ridge_miss.error().contains("lifted clear") &&
              port.status().g5x == ridge_origin && std::abs(machine()[2] - 20) < 0.03,
          ridge_miss ? "expected a missed ridge face" : ridge_miss.error());
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
                       bool measure = false, const std::string& tool_table = {}) {
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
    require(touched.has_value(), touched ? "the reference and probe have known lengths" : touched.error());
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

  struct Expected {
    std::string_view name;
    std::array<std::optional<double>, 2> origin; // frame; empty: the axis keeps its origin
    std::array<std::optional<double>, 2> size{};
    std::optional<double> diameter{};
  };
  const auto verify = [&](const Expected& expected, const NmlStatus& before,
                          const std::expected<SetterState, std::string>& result) {
    if (!result)
      throw std::runtime_error(std::format("{} at {}°: {}", expected.name, degrees, result.error()));
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
                                           result ? "no measurement" : result.error()));
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
      throw std::runtime_error(std::format("{} apply at {}°: {}", expected.name, degrees, applied.error()));
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
      throw std::runtime_error(std::format("Z top measure at {}°: {}", degrees, z ? "no measurement" : z.error()));
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
            z_applied ? "applying the measured top sets Z0 there and keeps XY" : z_applied.error());
    // A measurement is applied once; a new probe action forgets it.
    const auto again = port.fixed_setter(
        {.action = SetterAction::apply_measurement, .tool = 99, .edge_measurement = zm.id}, epoch, {});
    require(!again && again.error().contains("Probe the feature again"),
            "a measurement cannot be applied twice");
    // A changed work origin refuses an applied measurement before writing.
    at(2, 1);
    top.start = machine();
    const auto stale = port.fixed_setter(top, epoch, {});
    if (!stale || !stale->measured)
      throw std::runtime_error(stale ? "no measurement" : stale.error());
    mdi("G21 G10 L20 P0 Z7");
    mdi(scale == 1 ? "G21 G91" : "G20 G91");
    const auto stale_writes = port.writes().size();
    const auto stale_origin = port.status().g5x;
    const auto refused = port.fixed_setter(
        {.action = SetterAction::apply_measurement, .tool = 99, .edge_measurement = stale->measured->id},
        epoch, {});
    require(!refused && refused.error().contains("work frame") &&
                port.writes().size() == stale_writes && port.status().g5x == stale_origin,
            "a measurement taken in another work frame is refused before writing");
    // Every other way a measurement goes stale refuses it before writing (one each). `change`
    // runs between measuring and applying; `at_apply` is the session epoch the apply carries.
    // Epoch fields: page (a reconnect or a new panel session), datum (E-stop, power, homing),
    // tool (a tool change, an MDI block, a program run, step or resume, an offset edit), stop.
    const auto refused_after = [&](std::string_view name, const auto& change, SetterEpoch at_apply,
                                   std::string_view reason) {
      at(2, 1);
      top.start = machine();
      const auto taken = port.fixed_setter(top, epoch, {});
      if (!taken || !taken->measured)
        throw std::runtime_error(std::format("{}: {}", name, taken ? "no measurement" : taken.error()));
      change();
      const auto before_apply = port.status();
      const auto writes_before = port.writes().size();
      const auto table_before = table_text();
      const auto result = port.fixed_setter(
          {.action = SetterAction::apply_measurement, .tool = 99, .edge_measurement = taken->measured->id},
          at_apply, {});
      const auto after = port.status();
      std::cout << std::format("fact=measure_only action=stale_{} rotation={} refused={} writes={}", name,
                               degrees, !result, port.writes().size() - writes_before)
                << std::endl;
      require(!result && result.error().contains(reason) && port.writes().size() == writes_before &&
                  after.g5x == before_apply.g5x && after.g92 == before_apply.g92 &&
                  after.rotation_xy == before_apply.rotation_xy && table_text() == table_before,
              result ? std::format("{}: expected a refused measurement", name) : result.error());
    };
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
    const bool forgotten_clean = port.writes().size() == forgotten_writes;
    const auto latest = port.fixed_setter(
        {.action = SetterAction::apply_measurement, .tool = 99, .edge_measurement = second->measured->id},
        epoch, {});
    std::cout << std::format("fact=measure_only action=forgotten rotation={} first_refused={} writes={} "
                             "second_applied={}",
                             degrees, !forgotten, forgotten_clean ? 0 : 1, latest.has_value())
              << std::endl;
    require(!forgotten && forgotten.error().contains("Probe the feature again") && forgotten_clean &&
                latest.has_value() && first->measured->id != second->measured->id,
            "a new probe action forgets the measurement before it");
    // An action that always writes is refused as a measurement before anything is sent.
    for (const auto action : {SetterAction::ball, SetterAction::touch_off, SetterAction::check}) {
      const auto writes_before = port.writes().size();
      FixedSetterRequest writer{.action = action, .tool = 99, .start = machine(), .measure_only = true};
      writer.gauge_width_mm = 20;
      const auto refused_writer = port.fixed_setter(writer, epoch, {});
      require(!refused_writer && refused_writer.error().contains("always writes its result") &&
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
  require(!refused_ball && refused_ball.error().contains("without rotation") &&
              port.writes().size() == writes_before_ball &&
              port.status().g5x == origin_before_ball,
          refused_ball ? "expected a refused ball calibration" : refused_ball.error());

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
  require(!outside && outside.error().contains("outside the configured travel") &&
              port.writes().size() == writes && port.status().g5x == origin,
          outside ? "expected a travel refusal" : outside.error());
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
      throw std::runtime_error(std::format("angle at {}°: {}", rotation, measured.error()));
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
                  : applied.error());
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
          wrapped ? "applying past 180° writes the same turn inside -180..180°" : wrapped.error());
  static_cast<void>(angle(ProbeDirection::y_plus, 0));
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
                      double degrees, bool pin_run, const std::string& runout_file,
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
    require(touched.has_value(), touched ? "the reference and probe have known lengths" : touched.error());
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
  const auto expect = [](std::string_view name, std::initializer_list<Value> values,
                         double tolerance) {
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
      throw std::runtime_error(std::format("{} runout: {}", name, result ? "not reported" : result.error()));
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
      throw std::runtime_error(std::format("{}: {}", name, result.error()));
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
    require(!result && result.error().contains(reason) && (moves || port->writes().size() == writes) &&
                saved() == file,
            result ? std::format("{} is refused", name) : result.error());
  };

  // Two runs, each within the sim's 480 s: the ring (bores, ring features, refusals, abort) and
  // the pin (pin features, measure-only, tool mismatch, travel, miss, ball, Clear).
  if (!pin_run) {
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
                           missed ? "" : missed.error(), saved() == file)
            << std::endl;
  require(!missed && missed.error().starts_with("−X face") && saved() == file,
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
          cleared ? "Clear removes the correction with T98 loaded" : cleared.error());
  load(99);
  mdi("G54");
  require(port->status().g5x == g54, "runout calibration changes no other work system");
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
                     : measured.error());
    return FixedSetterRequest{.action = SetterAction::apply_angle,
                              .tool = 99,
                              .edge_measurement = measured->edge_rotation->id};
  };
  const double expected = std::atan(0.05) * 180 / std::numbers::pi;
  const auto stale = measure(ProbeDirection::y_plus, 20, expected);
  mdi("G10 L2 P2 X6");
  const auto before_stale = port.writes().size();
  const auto refused = port.fixed_setter(stale, epoch, {});
  require(!refused && refused.error().contains("work frame") &&
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
                    : applied.error());
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
  const bool runout = scenario == "runout" || scenario == "runout-pin";
  const bool measure = scenario == "measure";
  const bool rotated = scenario == "rotated" || measure;
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
  if (material || edge || features || rotated || runout)
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
  if (rotated) {
    // Millimetres at +7.5°, inches at -30°: each rotated cycle in both units and turns.
    rotated_scenarios(*port, scale, scale == 1 ? 7.5 : -30, mdi, measure, config->tool_table);
    return;
  }
  if (runout) {
    // The compensated bore also in G56 rotated +7.5° (mm) or -30° (inch).
    // A reconnect changes the material search travel.
    runout_scenarios(port, shared, scale, scale == 1 ? 7.5 : -30, scenario == "runout-pin",
                     config->probe_runout_file(), mdi,
                     [&](double travel) {
                       config->fixed_setter->material_travel_mm = travel;
                       port.reset();
                       port = connect();
                     });
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
      throw std::runtime_error(result.error());
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
  require(!premature && premature.error().contains("Teach the setter") && old_table == read_table(),
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
    require(!failed && failed.error().contains(direction) && failed.error().contains("tolerance") &&
                intact(),
            std::format("a {} tool fails after retract and keeps its table and offset ({})",
                        direction, failed ? "unexpected pass" : failed.error()));
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
  require(shifted && trips == 4 && !unstable && unstable.error().contains("exceeds 0.01 mm") &&
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
  require(!stale && stale.error().contains("Teach the setter with T98 or a touched-off tool first") &&
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
       scenario != "runout-pin" && scenario != "measure" &&
       scenario != "measure-m6") ||
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
