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
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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

  // Ball calibration over a gauge: the ball centres touch at -9 and +9 mm. Declared 20.1 mm
  // wide, the effective ball is 20.1 - 18 = 2.1 mm.
  corner_window('x', -9, 9);
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
  const auto calibrated = gauge(20.1);
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

void run(const std::string& ini, std::string_view scenario) {
  const bool material = scenario == "material";
  const bool edge = scenario == "edge-rotation";
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
  if (material || edge)
    config->touch_probe_tool = 99;
  // The runner recreates the table, but retains files beside it between runs.
  std::filesystem::remove(std::filesystem::path(config->tool_table).parent_path() /
                          "coffeecnc-touched-off.txt");
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
  const auto connect = [&] {
    auto connected = NmlPort::connect(*config, &shared);
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
  if ((scenario != "" && scenario != "material" && scenario != "edge-rotation") ||
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
