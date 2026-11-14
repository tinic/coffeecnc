// Records how the installed LinuxCNC behaves, as `fact=<name> ...` lines. Run it as the DISPLAY
// program of the simulated mill: tests/sim/run.sh build/dev/src/linuxcnc/coffeecnc_linuxcnc_probe
// Not production code: raw NML, halcmd through the shell, no error handling beyond printing.

#include <coffeecnc/linuxcnc/hal_component.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#include <cmd_msg.hh>
#include <cms.hh>
#include <emc.hh>
#include <emc_nml.hh>
#include <inifile.hh>
#include <nml.hh>
#include <nml_oi.hh>
#include <rcs.hh>
#include <stat_msg.hh>

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

RCS_CMD_CHANNEL* command;
RCS_STAT_CHANNEL* status;
NML* errors;
int serial;

void nap(std::chrono::milliseconds time) { std::this_thread::sleep_for(time); }

auto st() -> EMC_STAT* {
  status->peek();
  return static_cast<EMC_STAT*>(status->get_address());
}

// Prints and consumes the error channel.
auto drain(const char* tag) -> int {
  int count = 0;
  for (NMLTYPE type = errors->read(); type > 0; type = errors->read()) {
    const char* text = "?";
    NMLmsg* message = errors->get_address();
    if (type == EMC_OPERATOR_ERROR_TYPE)
      text = static_cast<EMC_OPERATOR_ERROR*>(message)->error;
    else if (type == EMC_OPERATOR_TEXT_TYPE)
      text = static_cast<EMC_OPERATOR_TEXT*>(message)->text;
    else if (type == EMC_OPERATOR_DISPLAY_TYPE)
      text = static_cast<EMC_OPERATOR_DISPLAY*>(message)->display;
    else if (type == NML_ERROR_TYPE)
      text = static_cast<NML_ERROR*>(message)->error;
    std::printf("fact=operator_message after=%s type=%ld text=\"%s\"\n", tag, long{type}, text);
    ++count;
  }
  return count;
}

void show(const char* tag) {
  const EMC_STAT* s = st();
  const auto& t = s->task;
  const auto& m = s->motion;
  std::printf(
      "fact=stat at=%s rcs=%d state=%d mode=%d interp=%d exec=%d paused=%d mline=%d cline=%d "
      "qmdi=%d tmode=%d inpos=%d queue=%d pos=%.3f,%.3f,%.3f homed=%d%d%d homing=%d%d%d "
      "vel=%.2f units=%d tool=%d prep=%d delay=%.2f\n",
      tag, s->status, t.state, t.mode, t.interpState, t.execState, t.task_paused, t.motionLine,
      t.currentLine, t.queuedMDIcommands, m.traj.mode, int{m.traj.inpos}, m.traj.queue,
      m.traj.actualPosition.tran.x, m.traj.actualPosition.tran.y, m.traj.actualPosition.tran.z,
      m.joint[0].homed, m.joint[1].homed, m.joint[2].homed, m.joint[0].homing, m.joint[1].homing,
      m.joint[2].homing, m.traj.current_vel, static_cast<int>(t.programUnits),
      s->io.tool.toolInSpindle, s->io.tool.pocketPrepped, t.delayLeft);
  std::fflush(stdout);
}

// Sends and waits for the echo. Milliseconds until echoed, or -1.
auto send(RCS_CMD_MSG& message) -> long {
  const auto start = Clock::now();
  if (command->write(&message) != 0)
    return -2;
  serial = message.serial_number;
  while (Clock::now() - start < 3s) {
    if (st()->echo_serial_number - serial >= 0)
      return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    nap(1ms);
  }
  return -1;
}

// RCS status of the last command once it is no longer executing, or 0 after the timeout.
auto done(std::chrono::milliseconds timeout = 5s) -> int {
  const auto start = Clock::now();
  while (Clock::now() - start < timeout) {
    const EMC_STAT* s = st();
    if (s->echo_serial_number - serial > 0)
      return RCS_DONE;
    if (s->echo_serial_number == serial && s->status != RCS_EXEC)
      return s->status;
    nap(1ms);
  }
  return 0;
}

template <typename Message> auto go(const char* tag, Message& message, bool report = true) -> int {
  const long echo = send(message);
  const auto start = Clock::now();
  const int result = done();
  const auto took =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
  nap(30ms);
  if (report)
    std::printf("fact=command name=%s echo_ms=%ld result=%d done_ms=%ld\n", tag, echo, result,
                long{took});
  drain(tag);
  return result;
}

auto state(EMC_TASK_STATE_ENUM wanted) -> int {
  EMC_TASK_SET_STATE m;
  m.state = wanted;
  return go("set_state", m);
}
auto mode(EMC_TASK_MODE_ENUM wanted) -> int {
  EMC_TASK_SET_MODE m;
  m.mode = wanted;
  return go("set_mode", m);
}
auto teleop(bool on) -> int {
  EMC_TRAJ_SET_TELEOP_ENABLE m;
  m.enable = on ? 1 : 0;
  return go("teleop", m);
}
auto mdi(const char* text, bool wait_idle = true) -> int {
  EMC_TASK_PLAN_EXECUTE m;
  std::strncpy(m.command, text, sizeof(m.command) - 1);
  const long echo = send(m);
  nap(20ms);
  show(text);
  int result = done(wait_idle ? 30000ms : 300ms);
  std::printf("fact=mdi text=\"%s\" echo_ms=%ld result=%d\n", text, echo, result);
  nap(30ms);
  drain(text);
  return result;
}
auto jog(int index, double velocity, int joint_mode) -> int {
  EMC_JOG_CONT m;
  m.joint_or_axis = index;
  m.vel = velocity;
  m.jjogmode = joint_mode;
  return go(joint_mode ? "jog_cont_joint" : "jog_cont_axis", m);
}
auto jog_stop(int index, int joint_mode) -> int {
  EMC_JOG_STOP m;
  m.joint_or_axis = index;
  m.jjogmode = joint_mode;
  return go(joint_mode ? "jog_stop_joint" : "jog_stop_axis", m);
}

void hal(const std::string& text) {
  const int result = std::system(("halcmd " + text).c_str());
  if (result != 0)
    std::printf("fact=halcmd_failed command=\"%s\" result=%d\n", text.c_str(), result);
}
auto getp(const std::string& pin) -> std::string {
  std::string out;
  if (FILE* pipe = popen(("halcmd getp " + pin).c_str(), "r")) {
    char buffer[64];
    if (std::fgets(buffer, sizeof(buffer), pipe))
      out = buffer;
    pclose(pipe);
  }
  while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
    out.pop_back();
  return out;
}

auto x() -> double { return st()->motion.traj.actualPosition.tran.x; }
auto j0() -> double { return st()->motion.joint[0].input; }

// Moves 100 ms with the pin low, raises it, and reports whether the jog stops; then what a jog
// started while the pin is still high does, and what one does after the pin has fallen.
void jog_stop_pin(const char* pin, int joint_mode) {
  const auto pos = [&]() -> double { return joint_mode ? j0() : x(); };
  const double velocity = 5;
  jog(0, velocity, joint_mode);
  nap(300ms);
  const double a = pos();
  nap(100ms);
  const double b = pos();
  hal(std::string("setp ") + pin + " 1");
  nap(300ms);
  const double c = pos();
  nap(200ms);
  const double d = pos();
  std::printf("fact=jog_stop_pin pin=%s space=%s moving_before=%d moved_after_rise=%.4f "
              "still_moving=%d jog_active=%s\n",
              pin, joint_mode ? "joint" : "axis", int{std::fabs(b - a) > 0.1}, c - b,
              int{std::fabs(d - c) > 0.001}, getp("motion.jog-is-active").c_str());
  drain(pin);
  // No NML stop was sent. A new jog with the pin still high:
  jog(0, velocity, joint_mode);
  nap(300ms);
  const double e = pos();
  nap(100ms);
  const double f = pos();
  std::printf("fact=jog_while_pin_high pin=%s space=%s moving=%d\n", pin,
              joint_mode ? "joint" : "axis", int{std::fabs(f - e) > 0.1});
  // Falling edge while that jog runs (if it runs), then a second rising edge.
  hal(std::string("setp ") + pin + " 0");
  nap(200ms);
  const double g = pos();
  nap(100ms);
  const double h = pos();
  hal(std::string("setp ") + pin + " 1");
  nap(300ms);
  const double i = pos();
  nap(100ms);
  const double k = pos();
  hal(std::string("setp ") + pin + " 0");
  std::printf(
      "fact=jog_pin_edges pin=%s space=%s moving_after_fall=%d moving_after_second_rise=%d\n", pin,
      joint_mode ? "joint" : "axis", int{std::fabs(h - g) > 0.1}, int{std::fabs(k - i) > 0.001});
  // With the pin low again and no NML stop in between: does the next jog simply work?
  jog(0, -velocity, joint_mode);
  nap(300ms);
  const double l = pos();
  nap(100ms);
  const double n = pos();
  std::printf("fact=jog_after_pin pin=%s space=%s moving=%d\n", pin, joint_mode ? "joint" : "axis",
              int{std::fabs(n - l) > 0.1});
  jog_stop(0, joint_mode);
  nap(300ms);
  drain("jog_stop_pin_end");
}

void follow(const char* tag, std::chrono::milliseconds time, std::chrono::milliseconds step) {
  for (const auto end = Clock::now() + time; Clock::now() < end; nap(step))
    show(tag);
}

auto wait_idle(std::chrono::milliseconds timeout = 30s) -> bool {
  for (const auto end = Clock::now() + timeout; Clock::now() < end; nap(5ms)) {
    const EMC_STAT* s = st();
    if (s->task.interpState == EMC_TASK_INTERP_IDLE && s->motion.traj.inpos &&
        s->motion.traj.queue == 0)
      return true;
  }
  return false;
}

} // namespace

auto main(int argc, char** argv) -> int {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  std::string ini_file;
  for (int i = 1; i + 1 < argc; ++i)
    if (std::string(argv[i]) == "-ini")
      ini_file = argv[i + 1];
  IniFile ini;
  ini.Open(ini_file.c_str());
  const char* nml = ini.Find("NML_FILE", "EMC");
  const std::string nml_file = nml ? nml : "/usr/share/linuxcnc/linuxcnc.nml";
  const std::string directory = ini_file.substr(0, ini_file.rfind('/'));
  std::printf("fact=start ini=%s nml=%s\n", ini_file.c_str(), nml_file.c_str());

  auto component = coffeecnc::HalComponent::create();
  if (!component) {
    std::printf("fact=hal_component error=\"%s\"\nexit=1\n", component.error().c_str());
    return 1;
  }
  hal("unlinkp iocontrol.0.tool-change");
  hal("unlinkp iocontrol.0.tool-changed");
  hal("net coffeecnc-tool-change iocontrol.0.tool-change coffeecnc.tool-change");
  hal("net coffeecnc-tool-number iocontrol.0.tool-prep-number coffeecnc.tool-number");
  hal("net coffeecnc-tool-changed coffeecnc.tool-changed iocontrol.0.tool-changed");

  command = new RCS_CMD_CHANNEL(emcFormat, "emcCommand", "xemc", nml_file.c_str());
  status = new RCS_STAT_CHANNEL(emcFormat, "emcStatus", "xemc", nml_file.c_str());
  errors = new NML(emcFormat, "emcError", "xemc", nml_file.c_str());
  std::printf("fact=q1_channels command=%d status=%d errors=%d\n", command->valid(),
              status->valid(), errors->valid());

  // Q1: the write counter.
  {
    const NMLTYPE first = status->peek();
    const long a = status->cms->header.write_id;
    const NMLTYPE again = status->peek();
    nap(200ms);
    const NMLTYPE later = status->peek();
    const long b = status->cms->header.write_id;
    std::printf("fact=q1_heartbeat first_peek=%ld immediate_peek=%ld later_peek=%ld "
                "write_id_delta_200ms=%ld task_heartbeat=%u motion_heartbeat=%u\n",
                long{first}, long{again}, long{later}, b - a, st()->task.heartbeat,
                st()->motion.heartbeat);
  }
  show("start");
  std::printf("fact=q9_units linear_units_per_mm=%.6f program_units=%d (1 inch, 2 mm, 3 cm) "
              "axis0_limits=%.3f..%.3f joint0_limits=%.3f..%.3f kinematics_type=%d joints=%d "
              "axis_mask=%d\n",
              st()->motion.traj.linearUnits, static_cast<int>(st()->task.programUnits),
              st()->motion.axis[0].minPositionLimit, st()->motion.axis[0].maxPositionLimit,
              st()->motion.joint[0].minPositionLimit, st()->motion.joint[0].maxPositionLimit,
              st()->motion.traj.kinematics_type, st()->motion.traj.joints,
              st()->motion.traj.axis_mask);
  drain("start");

  // Commands in E-stop.
  std::printf("fact=section name=estop\n");
  jog_stop(0, 1);
  jog(0, 5, 1);
  state(EMC_TASK_STATE_ON);
  show("on_from_estop");

  // Q2: E-stop reset, on.
  std::printf("fact=section name=q2_power\n");
  state(EMC_TASK_STATE_ESTOP_RESET);
  show("estop_reset");
  jog_stop(0, 1);
  state(EMC_TASK_STATE_ON);
  show("on");

  // Q10 / Q5: MDI when not homed.
  std::printf("fact=section name=q5_mdi_unhomed\n");
  mdi("G0 X1", false);
  show("mdi_in_manual");
  mode(EMC_TASK_MODE_MDI);
  show("mode_mdi");
  mdi("G0 X1", false);
  show("mdi_unhomed");
  jog(0, 5, 1);
  show("jog_in_mdi_mode");
  jog_stop(0, 1);
  mode(EMC_TASK_MODE_MANUAL);

  // Q3: joint jog before homing.
  std::printf("fact=section name=q3_joint_jog\n");
  {
    const double a = j0();
    jog(0, 5, 1);
    nap(500ms);
    show("joint_jog");
    const double b = j0();
    jog_stop(0, 1);
    nap(300ms);
    const double c = j0();
    nap(200ms);
    std::printf("fact=q3_joint_jog start=%.4f after_500ms=%.4f after_stop=%.4f settled=%.4f\n", a,
                b, c, j0());
    jog(0, 5, 0);
    nap(200ms);
    std::printf("fact=q3_axis_jog_unhomed moved=%.4f\n", j0() - c);
    jog_stop(0, 0);
    teleop(true);
    show("teleop_unhomed");
    EMC_JOG_INCR incr;
    incr.joint_or_axis = 1;
    incr.incr = 2;
    incr.vel = -5;
    incr.jjogmode = 1;
    const double y0 = st()->motion.joint[1].input;
    go("jog_incr_joint", incr);
    nap(1500ms);
    std::printf("fact=q3_jog_incr_joint_in_teleop incr=2 vel=-5 moved=%.4f\n",
                st()->motion.joint[1].input - y0);
    teleop(false);
    show("teleop_off");
    go("jog_incr_joint", incr);
    nap(1500ms);
    std::printf("fact=q3_jog_incr_joint incr=2 vel=-5 moved=%.4f\n",
                st()->motion.joint[1].input - y0);
  }

  // Q4: the jog-stop pins on a joint jog.
  std::printf("fact=section name=q4_joint\n");
  jog_stop_pin("motion.jog-stop", 1);
  jog_stop_pin("motion.jog-stop-immediate", 1);
  {
    hal("setp motion.jog-inhibit 1");
    jog(0, 5, 1);
    nap(200ms);
    const double a = j0();
    nap(100ms);
    std::printf("fact=jog_inhibit_start space=joint moving=%d\n", int{std::fabs(j0() - a) > 0.1});
    hal("setp motion.jog-inhibit 0");
    jog(0, 5, 1);
    nap(200ms);
    hal("setp motion.jog-inhibit 1");
    nap(300ms);
    const double b = j0();
    nap(100ms);
    std::printf("fact=jog_inhibit_during space=joint still_moving=%d\n",
                int{std::fabs(j0() - b) > 0.001});
    hal("setp motion.jog-inhibit 0");
    jog_stop(0, 1);
    nap(300ms);
    drain("jog_inhibit");
  }

  // Q2: home all.
  std::printf("fact=section name=q2_home\n");
  {
    EMC_JOINT_HOME home;
    home.joint = -1;
    const auto start = Clock::now();
    go("home_all", home);
    bool was_homing = false;
    for (int i = 0; i < 300; ++i) {
      const EMC_STAT* s = st();
      const auto* j = s->motion.joint;
      if (i == 3)
        show("homing");
      was_homing = was_homing || j[0].homing || j[1].homing || j[2].homing;
      if (j[0].homed && j[1].homed && j[2].homed && !j[0].homing && !j[1].homing && !j[2].homing)
        break;
      nap(100ms);
    }
    std::printf("fact=q2_home_all joint=-1 seconds=%.1f saw_homing=%d\n",
                std::chrono::duration<double>(Clock::now() - start).count(), int{was_homing});
    nap(500ms);
    show("homed");
    drain("home");
  }

  // Q3: axis jog after homing.
  std::printf("fact=section name=q3_axis_jog\n");
  {
    const double a = x();
    jog(0, 5, 0);
    nap(300ms);
    std::printf("fact=q3_axis_jog_without_teleop moved=%.4f\n", x() - a);
    jog_stop(0, 0);
    jog(0, 5, 1);
    nap(300ms);
    std::printf("fact=q3_joint_jog_homed_free_mode moved=%.4f\n", x() - a);
    jog_stop(0, 1);
    nap(200ms);
    teleop(true);
    show("teleop");
    const double b = x();
    jog(0, 5, 0);
    nap(500ms);
    show("axis_jog");
    const double c = x();
    jog_stop(0, 0);
    nap(300ms);
    std::printf("fact=q3_axis_jog start=%.4f after_500ms=%.4f settled=%.4f\n", b, c, x());
    jog(0, 5, 1);
    nap(200ms);
    jog_stop(0, 1);
    EMC_JOG_INCR incr;
    incr.joint_or_axis = 1;
    incr.incr = 2;
    incr.vel = -5;
    incr.jjogmode = 0;
    const double y0 = st()->motion.traj.actualPosition.tran.y;
    go("jog_incr_axis", incr);
    nap(1500ms);
    std::printf("fact=q3_jog_incr_axis incr=2 vel=-5 moved=%.4f\n",
                st()->motion.traj.actualPosition.tran.y - y0);
  }

  // Q4: the jog-stop pins on an axis jog.
  std::printf("fact=section name=q4_axis\n");
  jog_stop_pin("motion.jog-stop", 0);
  jog_stop_pin("motion.jog-stop-immediate", 0);

  // What a mode change does to a running jog.
  {
    jog(0, 5, 0);
    nap(300ms);
    mode(EMC_TASK_MODE_MDI);
    nap(300ms);
    const double a = x();
    nap(100ms);
    std::printf("fact=jog_then_mode_mdi still_moving=%d\n", int{std::fabs(x() - a) > 0.001});
    show("after_mode_mdi");
    jog_stop(0, 0);
    mode(EMC_TASK_MODE_MANUAL);
    show("back_to_manual");
  }

  // Q5: MDI.
  std::printf("fact=section name=q5_mdi\n");
  mode(EMC_TASK_MODE_MDI);
  show("mdi_mode");
  mdi("G21 G90 G0 X5 Y5 Z20");
  show("after_g0");
  {
    EMC_TASK_PLAN_EXECUTE m;
    std::strncpy(m.command, "G1 X25 F600", sizeof(m.command) - 1);
    const long echo = send(m);
    std::printf("fact=q5_g1_sent echo_ms=%ld\n", echo);
    follow("g1", 300ms, 50ms);
    // A second block while the first moves, then a mode change while it moves.
    std::strncpy(m.command, "G1 Y10 F600", sizeof(m.command) - 1);
    std::printf("fact=q5_second_block_while_moving echo_ms=%ld\n", send(m));
    nap(50ms);
    show("queued");
    drain("second_block");
    mode(EMC_TASK_MODE_AUTO);
    show("auto_refused?");
    mode(EMC_TASK_MODE_MANUAL);
    show("manual_during_mdi?");
    mode(EMC_TASK_MODE_MDI);
    std::printf("fact=q5_wait_idle ok=%d\n", int{wait_idle()});
    show("after_g1");
  }
  mdi("G10 L20 P0 X0");
  show("after_g10");
  std::printf("fact=q5_touch_off g5x_index=%d g5x_x=%.4f machine_x=%.4f\n", st()->task.g5x_index,
              st()->task.g5x_offset.tran.x, x());
  mdi("G55");
  std::printf("fact=q5_g55 g5x_index=%d\n", st()->task.g5x_index);
  mdi("G54");
  mdi("G1 X1000 F600", false);
  show("beyond_limit");
  mdi("G1 Q", false);
  show("syntax_error");
  mdi("G20");
  std::printf("fact=q9_g20 program_units=%d\n", static_cast<int>(st()->task.programUnits));
  mdi("G21");
  std::printf("fact=q9_g21 program_units=%d\n", static_cast<int>(st()->task.programUnits));

  // Q8: overrides, spindle, coolant, in MDI mode and in manual mode.
  std::printf("fact=section name=q8\n");
  for (const auto task_mode : {EMC_TASK_MODE_MDI, EMC_TASK_MODE_MANUAL}) {
    mode(task_mode);
    EMC_TRAJ_SET_SCALE feed;
    feed.scale = 0.85;
    go("feed_override", feed);
    EMC_TRAJ_SET_RAPID_SCALE rapid;
    rapid.scale = 0.5;
    go("rapid_override", rapid);
    EMC_TRAJ_SET_SPINDLE_SCALE spindle;
    spindle.spindle = 0;
    spindle.scale = 1.1;
    go("spindle_override", spindle);
    EMC_SPINDLE_ON on;
    on.spindle = 0;
    on.speed = 6000;
    go("spindle_forward", on);
    const EMC_STAT* s = st();
    std::printf("fact=q8_status mode=%d feed=%.2f rapid=%.2f spindle_scale=%.2f speed=%.1f "
                "direction=%d enabled=%d\n",
                s->task.mode, s->motion.traj.scale, s->motion.traj.rapid_scale,
                s->motion.spindle[0].spindle_scale, s->motion.spindle[0].speed,
                s->motion.spindle[0].direction, s->motion.spindle[0].enabled);
    on.speed = -3000;
    go("spindle_reverse", on);
    s = st();
    std::printf("fact=q8_reverse speed=%.1f direction=%d\n", s->motion.spindle[0].speed,
                s->motion.spindle[0].direction);
    EMC_SPINDLE_OFF off;
    off.spindle = 0;
    go("spindle_off", off);
    s = st();
    std::printf("fact=q8_off speed=%.1f direction=%d\n", s->motion.spindle[0].speed,
                s->motion.spindle[0].direction);
    EMC_COOLANT_FLOOD_ON flood_on;
    go("flood_on", flood_on);
    EMC_COOLANT_MIST_ON mist_on;
    go("mist_on", mist_on);
    nap(250ms);
    s = st();
    std::printf("fact=q8_coolant flood=%d mist=%d\n", s->io.coolant.flood, s->io.coolant.mist);
    EMC_COOLANT_FLOOD_OFF flood_off;
    go("flood_off", flood_off);
    EMC_COOLANT_MIST_OFF mist_off;
    go("mist_off", mist_off);
    nap(250ms);
    s = st();
    std::printf("fact=q8_coolant_off flood=%d mist=%d\n", s->io.coolant.flood, s->io.coolant.mist);
    feed.scale = 1;
    go("feed_override", feed, false);
    rapid.scale = 1;
    go("rapid_override", rapid, false);
    spindle.scale = 1;
    go("spindle_override", spindle, false);
  }

  // Q7: manual tool change.
  std::printf("fact=section name=q7_tool\n");
  mode(EMC_TASK_MODE_MDI);
  {
    show("before_t1m6");
    EMC_TASK_PLAN_EXECUTE m;
    std::strncpy(m.command, "T1 M6", sizeof(m.command) - 1);
    send(m);
    for (int i = 0; i < 50 && !component->tool_change_request(); ++i)
      nap(100ms);
    const auto request = component->tool_change_request();
    show("tool_change_waiting");
    std::printf(
        "fact=q7_request present=%d tool=%d iocontrol_tool_change=%s prep_number=%s "
        "tool_number_pin=%s\n",
        int{request.has_value()}, request.value_or(-1), getp("iocontrol.0.tool-change").c_str(),
        getp("iocontrol.0.tool-prep-number").c_str(), getp("iocontrol.0.tool-number").c_str());
    component->confirm_tool_changed();
    for (int i = 0; i < 50 && getp("iocontrol.0.tool-change") == "TRUE"; ++i)
      nap(50ms);
    std::printf("fact=q7_confirmed iocontrol_tool_change=%s tool_changed_pin=%s\n",
                getp("iocontrol.0.tool-change").c_str(), getp("coffeecnc.tool-changed").c_str());
    component->beat();
    std::printf("fact=q7_after_beat tool_changed_pin=%s result=%d\n",
                getp("coffeecnc.tool-changed").c_str(), done(5s));
    nap(300ms);
    show("after_t1m6");
    drain("t1m6");
  }

  // Q6: a program.
  std::printf("fact=section name=q6_program\n");
  const std::string program = directory + "/nc_files/probe.ngc";
  {
    std::ofstream out{program};
    out << "G21 G90 G54\nG0 X0 Y0 Z20\nG1 X20 F1200\nM1\nG1 Y20\nM0\nG1 X0\nG4 P1.0\nG20\n"
           "G1 Y0 F40\nG21\nM2\n";
  }
  for (const auto task_mode : {EMC_TASK_MODE_MANUAL, EMC_TASK_MODE_MDI, EMC_TASK_MODE_AUTO}) {
    mode(task_mode);
    EMC_TASK_PLAN_OPEN open;
    std::strncpy(open.file, program.c_str(), sizeof(open.file) - 1);
    const int result = go("open", open);
    std::printf("fact=q6_open mode=%d result=%d file=\"%s\"\n", task_mode, result, st()->task.file);
  }
  {
    EMC_TASK_PLAN_SET_OPTIONAL_STOP optional;
    optional.state = true;
    go("optional_stop_on", optional);
    std::printf("fact=q6_optional_stop state=%d\n", int{st()->task.optional_stop_state});
    EMC_TASK_PLAN_RUN run;
    run.line = 0;
    go("run", run);
    follow("run", 400ms, 100ms);
    EMC_TASK_PLAN_PAUSE pause;
    go("pause", pause);
    follow("paused", 300ms, 100ms);
    EMC_TASK_PLAN_RESUME resume;
    go("resume", resume);
    // Runs to M1 (optional stop on).
    for (int i = 0; i < 100 && !st()->task.task_paused; ++i)
      nap(50ms);
    nap(200ms);
    show("at_m1");
    EMC_TASK_PLAN_STEP step;
    go("step", step);
    for (int i = 0; i < 100 && st()->task.interpState != EMC_TASK_INTERP_PAUSED; ++i) {
      if (i % 4 == 0)
        show("stepping");
      nap(50ms);
    }
    nap(200ms);
    show("after_step");
    go("step", step);
    nap(500ms);
    show("after_second_step");
    go("resume", resume);
    for (int i = 0; i < 60; ++i) {
      show("resumed");
      if (st()->task.interpState == EMC_TASK_INTERP_IDLE)
        break;
      nap(250ms);
    }
    drain("program");
    // Again with optional stop off, aborted in the dwell.
    optional.state = false;
    go("optional_stop_off", optional);
    go("run", run);
    for (int i = 0; i < 200 && !st()->task.task_paused; ++i)
      nap(50ms);
    show("at_m0_optional_stop_off");
    go("resume", resume);
    for (int i = 0; i < 200 && st()->task.delayLeft <= 0; ++i)
      nap(20ms);
    show("in_dwell");
    EMC_TASK_ABORT abort;
    go("abort", abort);
    nap(300ms);
    show("aborted");
    // Run and mode changes while running.
    go("run", run);
    nap(300ms);
    mode(EMC_TASK_MODE_MANUAL);
    show("manual_while_running?");
    go("abort", abort);
    nap(300ms);
    mode(EMC_TASK_MODE_MANUAL);
    show("manual_after_abort");
  }

  // Q10: a soft limit reached by a jog; an abort; then the heartbeat with task stopped.
  std::printf("fact=section name=q10\n");
  {
    teleop(true);
    EMC_JOG_INCR incr;
    incr.joint_or_axis = 2;
    incr.incr = 500;
    incr.vel = 25;
    incr.jjogmode = 0;
    go("jog_incr_beyond_limit", incr);
    nap(4000ms);
    show("at_soft_limit");
    drain("soft_limit");
    jog(2, 5, 0);
    nap(300ms);
    drain("jog_at_limit");
    jog_stop(2, 0);
  }
  state(EMC_TASK_STATE_OFF);
  show("off");
  state(EMC_TASK_STATE_ESTOP);
  show("estop");
  {
    const std::string find = "pgrep -f 'milltask -ini " + ini_file + "'";
    std::string pid;
    if (FILE* pipe = popen(find.c_str(), "r")) {
      char buffer[32];
      if (std::fgets(buffer, sizeof(buffer), pipe))
        pid = std::to_string(std::atoi(buffer));
      pclose(pipe);
    }
    status->peek();
    const long a = status->cms->header.write_id;
    nap(200ms);
    status->peek();
    const long b = status->cms->header.write_id;
    if (!pid.empty() && pid != "0") {
      static_cast<void>(std::system(("kill -STOP " + pid).c_str()));
      nap(100ms);
      status->peek();
      const long c = status->cms->header.write_id;
      nap(500ms);
      const NMLTYPE type = status->peek();
      const long d = status->cms->header.write_id;
      std::printf("fact=q1_task_stopped pid=%s running_delta_200ms=%ld stopped_delta_500ms=%ld "
                  "peek=%ld state_still_readable=%d task_heartbeat=%u\n",
                  pid.c_str(), b - a, d - c, long{type}, st()->task.state, st()->task.heartbeat);
      static_cast<void>(std::system(("kill -CONT " + pid).c_str()));
      nap(200ms);
      status->peek();
      std::printf("fact=q1_task_continued delta=%ld\n", status->cms->header.write_id - d);
    } else {
      std::printf("fact=q1_task_stopped error=task-not-found\n");
    }
  }
  drain("end");
  std::printf("exit=0\n");
  return 0;
}
