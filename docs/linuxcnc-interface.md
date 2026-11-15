# LinuxCNC interface reference

What CoffeeCNC uses from LinuxCNC and what each item obliges the executable to do. Source lines are LinuxCNC [v2.9.10](https://github.com/LinuxCNC/linuxcnc/tree/v2.9.10); paths are relative to `src/`. Protocol names refer to [`ui/src/protocol.ts`](../ui/src/protocol.ts). The **Observed on 2.9.4** section records what the adapter's integration test measured against LinuxCNC 2.9.4 in a simulation configuration (`backend/tests/sim/run.sh`); no machine has been used.

## Commands

All are NML messages on `emcCommand`, declared in `emc/nml_intf/emc_nml.hh`. Task echoes the serial number, then reports `RCS_DONE` or `RCS_ERROR` for it in `EMC_STAT`.

| Protocol command | NML message | Line | Notes |
|---|---|---|---|
| `set_machine` | `EMC_TASK_SET_STATE` | 1288 | `off` is `EMC_TASK_STATE_ESTOP_RESET` |
| (implied mode) | `EMC_TASK_SET_MODE` | 1276 | Refused in auto while the interpreter is not idle (`emc/task/emctaskmain.cc:2130`) |
| `home` | `EMC_JOINT_HOME` | 379 | A joint command, not an axis command |
| `jog`, held | `EMC_JOG_CONT` | 399 | `jjogmode` 1 before homing (joint), 0 after (axis, needs teleop) |
| `jog`, released; `jog_stop_all` | `EMC_JOG_STOP` | 440 | |
| `jog_step` | `EMC_JOG_INCR` | 412 | |
| (jog space) | `EMC_TRAJ_SET_TELEOP_ENABLE` | 973 | Task enables teleop itself once all joints are homed; send it to leave teleop for homing or a joint jog |
| `mdi`, `select_wcs`, `touch_off`, `tool_touch_off` | `EMC_TASK_PLAN_EXECUTE` | 1335 | `G54`…, `G10 L20 P<n> <axis><value>`, `G10 L10 P<tool> Z<value>` then `G43`; text limited to `LINELEN` (255); refused when not homed unless `[TRAJ] NO_FORCE_HOMING` is above 0 (`emctaskmain.cc:2209`) |
| `open_program` | `EMC_TASK_PLAN_OPEN` | 1300 | Path limited to `LINELEN`; task reads the file itself |
| `run` | `EMC_TASK_PLAN_RUN` | 1312 | `line` 0 or 1 runs from the start; other values are run-from-line, not used; refused when not homed unless `[TRAJ] NO_FORCE_HOMING` is above 0 (`emctaskmain.cc:2296`) |
| `pause`, `resume`, `step` | `EMC_TASK_PLAN_PAUSE`, `_RESUME`, `_STEP` | 1348, 1385, 1375 | |
| `abort` | `EMC_TASK_ABORT` | 1266 | |
| `set_override` | `EMC_TRAJ_SET_SCALE`, `_RAPID_SCALE`, `_SPINDLE_SCALE` | 661, 673, 685 | |
| `set_spindle` | `EMC_SPINDLE_ON`, `EMC_SPINDLE_OFF` | 1813, 1830 | Reverse is a negative speed (`emc/usr_intf/axis/extensions/emcmodule.cc:938`) |
| `set_coolant` | `EMC_COOLANT_FLOOD_ON`/`_OFF`, `EMC_COOLANT_MIST_ON`/`_OFF` | 1942, 1920 | |
| `set_program_option` | `EMC_TASK_PLAN_SET_OPTIONAL_STOP`, `_SET_BLOCK_DELETE` | 1435, 1447 | |
| `set_tool`, `remove_tool` | `EMC_TOOL_LOAD_TOOL_TABLE` | 1597 | Rewrite the tool table file, then reload; idle interpreter only |

## Status

`EMC_STAT` (`emc_nml.hh:2203`) is read with `peek()` from `emcStatus`. Values are in machine units ([TRAJ] `LINEAR_UNITS`); the executable converts to millimetres.

| Protocol field | `EMC_STAT` field | Line | Notes |
|---|---|---|---|
| `machine`, `mode`, `interp` | `task.state`, `task.mode`, `task.interpState` | 1484 | |
| `axes[].homed`, `homing`, `on_hard_limit` | `motion.joint[n].homed`, `homing`, `minHardLimit`/`maxHardLimit` | 534, 533, 539 | Per joint; equals per axis only with trivial kinematics |
| `axes[].machine` | `motion.traj.actualPosition` | 1053 | |
| `axes[].work` | `actualPosition` − `task.g5x_offset` − `task.g92_offset` − `task.toolOffset`, then `task.rotation_xy` | 1506, 1510 | Computed in the executable, as AXIS does in its own code |
| `axes[].dtg` | `motion.traj.dtg` | 1069 | |
| `offsets.wcs` | `task.g5x_index` | 1507 | |
| `motion.velocity` | `motion.traj.current_vel` | 1070 | Units per second |
| `motion.feed_mode`, `programmed_feed` | `task.activeGCodes` (G93/G94/G95), `task.activeSettings[1]` | 1511 | F is units/min, 1/min or units/rev by mode |
| `motion.feed_override`, `rapid_override` | `motion.traj.scale`, `rapid_scale` | 1048, 1049 | Report these, so pendant or HAL changes show |
| `spindle.commanded_rpm`, `override`, `direction` | `motion.spindle[0].speed`, `spindle_scale`, `direction` | 1193, 1194, 1198 | Commanded only; actual speed is a HAL signal |
| `tool.in_spindle` | `io.tool.toolInSpindle` | 1668 | |
| `tool.prepared` | `io.tool.pocketPrepped` | 1667 | An index into the tool table, not a tool number |
| `program.current_line`, `motion_line` | `task.currentLine`, `task.motionLine` | 1498, 1497 | `motionLine` lags; `currentLine` reads ahead |
| `wait: paused` | `task.task_paused` | 1518 | Operator pause and M0/M1/M60 are not distinguished |
| `wait: dwell` | `task.delayLeft` | 1519 | |
| `wait: tool_change` | HAL pin, see below | | Not in `EMC_STAT` |
| `spindle.actual_rpm`, `spindle.at_speed`, `probe_input` | HAL pins `coffeecnc.spindle-rpm`, `coffeecnc.spindle-at-speed`, `coffeecnc.probe-input` | | `null` unless `[COFFEECNC] SPINDLE_RPM_SIGNAL`, `SPINDLE_AT_SPEED_SIGNAL`, `PROBE_SIGNAL` declare them wired |
| `active_codes` | `task.activeGCodes`, `activeMCodes` | 1511 | |

## Constraints

| Fact | Evidence | Obligation |
|---|---|---|
| A continuous jog runs until `EMC_JOG_STOP`; NML has no dead-man | `emc_nml.hh:399` | The executable stops the jog after 150 ms without a panel refresh |
| That does not cover the executable dying mid-jog | | Its HAL component toggles a heartbeat pin; a HAL `watchdog` drives `motion.jog-stop` (`emc/motion/motion.c:477`) |
| Reading `emcError` consumes the message | `emc/usr_intf/axis/extensions/emcmodule.cc:1567` uses `read()`, not `peek()` | One reader per machine: CoffeeCNC cannot share a session with another GUI that reads errors |
| A stopped task leaves the last status readable | Write counter delta 0 per 500 ms with task stopped, status still readable (2.9.4) | `link` is `live` only while the status buffer's NML write counter advances |
| `EMC_STAT` layout changes between LinuxCNC releases | NML passes the struct as bytes | Build against the installed `linuxcnc-uspace-dev`; the package depends on that release series |
| A manual tool change is a HAL handshake: `iocontrol.0.tool-change` out, `tool-changed` in, `tool-prep-number` | `emc/iotask/ioControl.cc:433`, `443`, `381` | The executable owns a HAL component with `tool-change` (in), `tool-number` (in), `tool-changed` (out) and reports `wait: tool_change`; this replaces `hal_manualtoolchange` (`emc/usr_intf/axis/scripts/hal_manualtoolchange.py:55`) |
| LinuxCNC starts the `[DISPLAY] DISPLAY` program and stops when it exits | Elle `README.md`, "How it fits together" | `coffeecnc` is the `DISPLAY` program and runs `[HAL] POSTGUI_HALFILE` after its component is ready |
| Machine and program units differ | `task.programUnits`, `emc_nml.hh:1484` | Convert once in the executable; the protocol is millimetres only |

## Observed on 2.9.4

| Topic | Observation | Consequence in the adapter |
|---|---|---|
| `motion.jog-stop` | Stops a jog started over NML in joint and axis mode, within 0.04–0.06 mm at 300 mm/min; a level (`emc/motion/control.c:542`): jogs are aborted in every servo period it is high; after it falls the next jog needs no reset | The dead-man path needs no command channel. The runtime holds the level until starts and stops reconcile |
| `emcCommand` | A queue: commands written while task is stopped are all executed, in order, when it runs again | A jog start written before a stop decision can run afterwards; an accepted stop proves every earlier start was consumed |
| Late jog start under the hold | Refused with "Cannot jog while jog-inhibit is active"; measured in simulation: start written with task frozen, level high 1–9 ms after the decision, held 370–530 ms until the stop was accepted, axis moved 0.00000 mm. Without the runtime the same start moved 3.2 mm | The post-GUI file feeds the level into `motion.jog-inhibit` as well |
| Dead-man timing | Last refresh to level 154.5 ms, stopping 22.2 ms, 0.125 mm at 300 mm/min; control period 10 ms with 0.02 ms jitter while a command blocked for 1 s | |
| libnml and threads | Channels share unlocked state, also on error-free calls (`libnml/cms/shmem.cc:44-57` `not_zero()`, the channel list, the print buffers). Each peek/read/write takes the buffer's semaphore with a 10 s timeout | One process-wide gate around every libnml call; no NML call on the control thread |
| `motion.jog-inhibit` | Refuses `JOG_CONT`, `JOG_INCR` and `JOG_ABS` (`emc/motion/command.c:781`); does not stop a running jog | Driven by the level and by the watchdog, together with `jog-stop` |
| Status write counter | About 945 per second with a 1 ms task cycle | `TaskHeartbeat` ignores an advance seen after a reading gap of 1 s or more |
| `task.heartbeat` | Counts on 2.9.4 (Elle saw 0 on its build) | Not used |
| Machine on from E-stop | `EMC_TASK_SET_STATE` ON returns `RCS_DONE`; the state stays ESTOP | Verify the reported state, not the command status |
| Machine off | Reported as ESTOP_RESET, never as OFF | Protocol `off` |
| E-stop reset | `off` sent while the machine is on is a power-off; `off` sent while off or in E-stop is the E-stop reset (`EMC_TASK_STATE_ESTOP_RESET`). Observed on the G0759's HAL files in simulation: after an E-stop request, `on` alone was accepted with `iocontrol.0.user-enable-out` still false, a power-off left it false, and `off` sent while off set it true; a reset sent right after the E-stop input closed was undone by task | A machine whose chain reads `user-enable-out` needs the reset before on. A reset control available while off is planned in the panel; it is not in this tree |
| E-stop where `emc-enable-in` is an external input only | A display program's request ends with the machine reported off; no sampled state was E-stop (sequence on, then off), and motion and spindle enable ended. The external input opening was followed by a reported E-stop. Sampled observations in simulation on the G0759's HAL files (`linuxcnc_machine_g0759`): no interval or bound is claimed, and none of it is stop-latency evidence for a machine | The request's result is `failed`: E-stop was not confirmed |
| Status buffer | Keeps what task wrote last when task stops | A state counts only from a live status that echoes the command's serial number |
| INI lines | LinuxCNC reads 255 characters of a line | The DISPLAY line carries the program only; arguments go into a wrapper |
| Second display program | A second start of the post-GUI file under one running LinuxCNC fails at `loadrt` (already loaded) | The application is started once per LinuxCNC session |
| Home all | `EMC_JOINT_HOME` with joint −1; needs joint mode; 2–4 s in simulation | |
| Teleop | Task enters teleop by itself when all joints are homed; `EMC_TRAJ_SET_TELEOP_ENABLE` is needed to leave it for homing or a joint jog | Corrects the Commands table: the message is for leaving, not entering |
| Axis jog while unhomed | Moves 0 without teleop; with teleop enabled an unhomed axis jog moves (2.3 mm in 300 ms). Neither task nor motion ties teleop to homing | CoffeeCNC still uses joint jogs until all axes are homed |
| Unhomed with `NO_FORCE_HOMING = 1` | An MDI move, `G10 L20`, `G55` and a program run with `T2 M6` are accepted; task does not switch mode itself. Soft limits are enforced against the power-up coordinates: `G1 X1500` fails with "would exceed X's positive limit" | `Limits::homing_required` follows the INI |
| Home all without `HOME_SEQUENCE` | `EMC_JOINT_HOME` joint −1 is accepted, homes nothing, reports nothing | The adapter refuses it; axes are homed one by one |
| Homing without switches | Immediate: the present position becomes `HOME`, without motion; task enters teleop after the last joint | |
| One `loadrt` per module | A second `loadrt` of a loaded module fails, with or without `names=`; `watchdog` has no `names=` | The post-GUI fragment loads only `watchdog` and `lut5 names=coffeecnc-jog-stop,coffeecnc-jog-inhibit`; a machine that already loads one of them merges the instances into its own line |
| Jog status | Joint jog: `RCS_EXEC`, `inpos` 0. Teleop jog: `RCS_DONE`, `inpos` 1 | `in_position` also checks velocity; jog commands wait for the echo, not completion |
| Mode change | Refused only in auto with a busy interpreter. During an MDI move it is accepted and aborts the move. During a jog it is ignored with `RCS_DONE` | Verify `task.mode` after every change |
| Pause and step | At an M0/M1 stop `currentLine` is the line before it. The first step after such a stop only takes up the next block; `task_paused` stays set with the interpreter reading | Report paused whenever `task_paused` is set |
| Line counters | Close + open zero `currentLine` and `motionLine` (`emc/task/emctask.cc:547`). After a 4-line program ends: 3 / 3. E-stop with an idle interpreter then gives 3 / 0, also after another file was opened (`emc/task/emctaskmain.cc:2615`). An MDI block after the run counts on: 4 / 4. An abort in line 3 leaves 5 / 3 (read ahead). A run does not reset them (`emctaskmain.cc:2294`) | `current_line` and `motion_line` are attributed per program revision, never passed through: 0 at load; as read while auto is not idle and the counter moved; the value at the end of the run is kept; 0 after a manual or MDI status until the next run. A run shorter than the 50 ms publishing period leaves 0 |
| Status time | The status buffer holds what task published last, however old | A reading is dated before it is acquired: the time orders CoffeeCNC's samples against its own commands, not LinuxCNC's publication |
| Manual tool change | `T1 M6` raises `iocontrol.0.tool-change`, `tool-prep-number` 1; `tool-changed` high releases it; `toolInSpindle` changes afterwards. M6 for the loaded tool asks for nothing | `requested_tool` comes from the HAL pins |
| Units | `traj.linearUnits` is units per mm; `task.programUnits` 1 is G20, 2 is G21 and stays inch after an abort inside a G20 section | One scale factor from the INI; composed G-code in the program unit |
| Error channel | Text arrives a few ms after the status, sometimes as several messages. A jog into a soft limit produces no error | All errors of a failed action become its reason |
| Build | `emc_nml.hh` includes Boost headers; `linuxcnc-uspace-dev` does not depend on `libboost-dev` | The adapter needs Boost headers at build time |
| Parameter file | Not written within 3 s of MDI `G10 L2`, `G10 L20`, `G55`, `G92` or `#5262=…`. Written when a program ends (M2) and when LinuxCNC exits; values in machine units | Persistence, not a live source |
| Work offsets | `EMC_STAT` carries only the active system; no live source found for the other eight | Sent `available: false` with zeros |
| `pocketPrepped` | Row n of the loaded tool table is index n; index 0 is the spindle; a `T0` row is an ordinary entry | `tool.prepared` is that entry's tool number, null while the table shown is not the accepted one |
| Tool data | The loaded table is shared memory (`$HOME/.tool.mmap`, `libtooldata`): a lock per entry, no sequence number, no load-in-progress mark, no comments. The package ships the library without `tooldata.hh` | A table is shown after two identical passes one period apart with task idle between; comments from the file by tool number at load time |
| F word | An F in the block that changes unit is read in the old unit and converted: `G21 … F1200` on an inch machine reports 30480 mm/min. `activeSettings[1]` is in the program unit | `programmed_feed` is scaled by 25.4 under G20, not in G93 |
| Program open | `EMC_TASK_PLAN_OPEN` is refused with "A file is already open" until `EMC_TASK_PLAN_CLOSE` | The port closes first |
| Start state | Optional stop and block delete both report true at start | |

## Preview

| Fact | Evidence | Consequence |
|---|---|---|
| The interpreter is a shared library with public headers | `emc/rs274ngc/Submakefile:42` (`librs274.so`); `Makefile:376`, `391`, `393` (`canon.hh`, `interp_base.hh`, `rs274ngc.hh`); `debian/linuxcnc-uspace-dev.install` | A C++ preview can run LinuxCNC's own interpreter; no second G-code parser |
| The interpreter reports motion through about 160 free functions the host must define | `emc/nml_intf/canon.hh` (`STRAIGHT_TRAVERSE` 245, `ARC_FEED` 406, `STRAIGHT_FEED` 464) | One implementation per process, and the interpreter keeps global state: the preview is a separate helper process |
| AXIS does exactly this, into Python callbacks | `emc/rs274ngc/gcodemodule.cc:718` (`parse_file`), `193`–`238` | Reference for call order, unit and init codes, and arc handling |
| `rs274` is the same interpreter as a command line program printing canonical commands | `emc/sai/Submakefile:9`, `emc/sai/driver.cc:578` (`-t` tool table, `-v` parameters, `-i` INI, `-g`) | Usable for a first preview and as a test oracle for the helper |
| Probing and `M66` input depend on the machine | `canon.hh:875` (`GET_EXTERNAL_PROBE_POSITION_X`) | The trusted path stops at the first such operation: `Preview` is `failed` with the line and the segments up to it. No marker field |
| A line beginning `;py,` runs Python while the line is READ, also on lines never executed | `emc/rs274ngc/interp_read.cc:316` (`read_semicolon`) | Every file is scanned whole before the interpreter reads anything; a program with one is refused |
| `(py,)`, `(pyrun,)`, `(pyreload)`, `(log,)`, `(logopen,)`, `(logappend,)`, `(logclose)` act at execute; `(PRINT,)` only writes to stdout | `emc/rs274ngc/interp_convert.cc:1499` (`convert_comment`) | The Python, log and probe-file comments are refused in the same scan. `(PRINT,)` is allowed: the helper's result travels on its own descriptor, and stdout is a bounded diagnostic stream that is never parsed |
| A called O-word name is opened as given (absolute paths first) and after `wordexp`; search order: working directory, `PROGRAM_PREFIX`, `SUBROUTINE_PATH` | `emc/rs274ngc/rs274ngc_pre.cc:2672` (`find_ngc_file`) | Names must be `[a-z0-9_.-]+` or a number, else the program is refused; the helper sees only a private directory |
| A run does not initialise the interpreter; `M2` sets G54 G17 G90 G94 G40 G1; an abort keeps the modal codes | `emc/task/emctaskmain.cc:2294`, `interp_convert.cc` (`convert_stop`) | The start state is replayed from task's live active codes. Measured: a first block of axis words only fails after a fresh start (G80) and after `M2` (G1, feed 0), and feeds after an MDI `G1 F` or an aborted G1 program |
| A `G10` value is read in the interpreter's active unit, not the machine's | observed | |
| Interpreter internals (`Interp::_setup`) are not in the installed headers | `linuxcnc-uspace-dev` 2.9.4 | Parameter values cannot be attributed with the public API: any line with `#` ends the preview. Macros need interpreter and helper built from one source; not done |
| The preview depends on offsets, tool table and start state | | `Status.preview_context` identifies the inputs and `Preview.context` says which it was computed for. The saved parameter file is persistence, not live: `G28`/`G30`, other work systems and parameters end the preview |

## Open

| Item | Question |
|---|---|
| Remaps and `M100`–`M199` | Decided for the preview helper: a configuration with `[RS274NGC] REMAP` is refused; the path stops at the first `M100`–`M199`. Modelling either is open |
| Non-trivial kinematics | Joint and axis indexes are assumed equal; reject other configurations at start-up or map them? |
| Jog watchdog wiring | Shipped in `postgui.hal` or checked at start-up? |
| Step and resume around M0/M1 | Observed on one program; confirm before the panel relies on it |
| Run from line | AXIS warns it is wrong with subroutines; no version 1 command |
