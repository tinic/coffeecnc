# LinuxCNC interface reference

What CoffeeCNC uses from LinuxCNC and what each item obliges the executable to do. Source lines are LinuxCNC [v2.9.10](https://github.com/LinuxCNC/linuxcnc/tree/v2.9.10); paths are relative to `src/`. Protocol names refer to [`ui/src/protocol.ts`](../ui/src/protocol.ts). Nothing here has run against LinuxCNC from CoffeeCNC yet.

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
| (axis jog) | `EMC_TRAJ_SET_TELEOP_ENABLE` | 973 | Required before axis-mode jogging on a homed machine |
| `mdi`, `select_wcs`, `touch_off`, `tool_touch_off` | `EMC_TASK_PLAN_EXECUTE` | 1335 | `G54`…, `G10 L20 P<n> <axis><value>`, `G10 L10 P<tool> Z<value>` then `G43`; text limited to `LINELEN` (255); refused when not homed (`emctaskmain.cc:2209`) |
| `open_program` | `EMC_TASK_PLAN_OPEN` | 1300 | Path limited to `LINELEN`; task reads the file itself |
| `run` | `EMC_TASK_PLAN_RUN` | 1312 | `line` 0 or 1 runs from the start; other values are run-from-line, not used; refused when not homed (`emctaskmain.cc:2296`) |
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
| `active_codes` | `task.activeGCodes`, `activeMCodes` | 1511 | |

## Constraints

| Fact | Evidence | Obligation |
|---|---|---|
| A continuous jog runs until `EMC_JOG_STOP`; NML has no dead-man | `emc_nml.hh:399` | The executable stops the jog after 150 ms without a panel refresh |
| That does not cover the executable dying mid-jog | | Its HAL component toggles a heartbeat pin; a HAL `watchdog` drives `motion.jog-stop` (`emc/motion/motion.c:477`) |
| Reading `emcError` consumes the message | `emc/usr_intf/axis/extensions/emcmodule.cc:1567` uses `read()`, not `peek()` | One reader per machine: CoffeeCNC cannot share a session with another GUI that reads errors |
| `EMC_STAT.task.heartbeat` stays 0; a dead task leaves the last status readable | Elle `backend/src/core/include/elle/core/task.hpp`, `TaskHeartbeat` | `link` is `live` only while the status buffer's NML write counter advances |
| `EMC_STAT` layout changes between LinuxCNC releases | NML passes the struct as bytes | Build against the installed `linuxcnc-uspace-dev`; the package depends on that release series |
| A manual tool change is a HAL handshake: `iocontrol.0.tool-change` out, `tool-changed` in, `tool-prep-number` | `emc/iotask/ioControl.cc:433`, `443`, `381` | The executable owns a HAL component with `tool-change` (in), `tool-number` (in), `tool-changed` (out) and reports `wait: tool_change`; this replaces `hal_manualtoolchange` (`emc/usr_intf/axis/scripts/hal_manualtoolchange.py:55`) |
| LinuxCNC starts the `[DISPLAY] DISPLAY` program and stops when it exits | Elle `README.md`, "How it fits together" | `coffeecnc` is the `DISPLAY` program and runs `[HAL] POSTGUI_HALFILE` after its component is ready |
| Machine and program units differ | `task.programUnits`, `emc_nml.hh:1484` | Convert once in the executable; the protocol is millimetres only |

## Preview

| Fact | Evidence | Consequence |
|---|---|---|
| The interpreter is a shared library with public headers | `emc/rs274ngc/Submakefile:42` (`librs274.so`); `Makefile:376`, `391`, `393` (`canon.hh`, `interp_base.hh`, `rs274ngc.hh`); `debian/linuxcnc-uspace-dev.install` | A C++ preview can run LinuxCNC's own interpreter; no second G-code parser |
| The interpreter reports motion through about 160 free functions the host must define | `emc/nml_intf/canon.hh` (`STRAIGHT_TRAVERSE` 245, `ARC_FEED` 406, `STRAIGHT_FEED` 464) | One implementation per process, and the interpreter keeps global state: the preview is a separate helper process |
| AXIS does exactly this, into Python callbacks | `emc/rs274ngc/gcodemodule.cc:718` (`parse_file`), `193`–`238` | Reference for call order, unit and init codes, and arc handling |
| `rs274` is the same interpreter as a command line program printing canonical commands | `emc/sai/Submakefile:9`, `emc/sai/driver.cc:578` (`-t` tool table, `-v` parameters, `-i` INI, `-g`) | Usable for a first preview and as a test oracle for the helper |
| Probing and `M66` input depend on the machine | `canon.hh:875` (`GET_EXTERNAL_PROBE_POSITION_X`) | The helper returns the programmed target; such programs are marked in `Preview` as not deterministic (field to add) |
| The preview depends on offsets, tool table and parameters at load time | `-t`, `-v` above | Re-run on any change of those; `Status.preview_context` identifies the inputs and `Preview.context` says which it was computed for |

## Open

| Item | Question |
|---|---|
| Remaps and `M100`–`M199` | The helper needs the Python plugin and INI `[RS274NGC]` settings; which side effects must it suppress? |
| Non-trivial kinematics | Joint and axis indexes are assumed equal; reject other configurations at start-up or map them? |
| Actual spindle speed | Which HAL signal, and is it a pin on the CoffeeCNC component or read by name? |
| Jog watchdog wiring | Shipped in `postgui.hal` or checked at start-up? |
| Run from line | AXIS warns it is wrong with subroutines; no version 1 command |
