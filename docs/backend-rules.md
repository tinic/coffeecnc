# Backend rules and limits

The rules the C++ layers implement and where, and the known limits. Build and test instructions: [native backend](../backend/README.md); sources are under `backend/`. Native qualification uses LinuxCNC 2.9.4 in simulation only.

## Rules the layers implement

| Rule | Where |
|---|---|
| A policy decision permits an attempt; it is not LinuxCNC's acceptance | `CommandPolicy::evaluate` |
| Stops, E-stop and power-off are never gated on link state; new motion needs live state | `CommandPolicy` |
| Limits come from the machine configuration; there are no defaults | `Limits`, `MachineConfig` |
| Homing is required for MDI, run, step, WCS selection and touch-off unless `[TRAJ] NO_FORCE_HOMING` is set; nothing of that is allowed while an axis is homing | `Limits::homing_required`, `CommandPolicy` |
| A jog above its axis ceiling (`[AXIS_*] MAX_VELOCITY`) is rejected | `Limits::max_axis_jog_velocity` |
| The spindle ceiling is `[COFFEECNC] MAX_SPINDLE_RPM`, else `[SPINDLE_0] MAX_FORWARD_VELOCITY`, else `[DISPLAY] MAX_SPINDLE_0_SPEED`; with none, start-up fails | `load_machine_config` |
| Actual spindle speed, at-speed and probe input are reported only when `[COFFEECNC] SPINDLE_RPM_SIGNAL`, `SPINDLE_AT_SPEED_SIGNAL`, `PROBE_SIGNAL` say the pins are wired; an unconnected pin is never a measured zero | `HalComponent` |
| An axis's reference mark is reported (found: `coffeecnc.reference-mark-x`, `-y`, `-z`; the axis stands at it: `coffeecnc.at-reference-mark-x`, `-y`, `-z`), and Home refused for an axis whose mark is not found, only when `[COFFEECNC] REFERENCE_MARKS` says those pins are wired; a reading the control thread has not renewed counts as not found | `HalComponent`, `to_status`, `CommandPolicy` |
| An E-stop request is answered by the state a live status shows after the command: accepted only when E-stop is reported; else `failed` with "LinuxCNC reports machine off; E-stop was not confirmed.", "LinuxCNC still reports machine on; ..." or "LinuxCNC's status cannot be read; ..." | `estop_request_result`, `NmlPort` |
| Files are read whole through one bounded reader: 16 MiB for a program, 4 MiB for the tool table | `core/text_file.hpp` |
| Home all is refused unless every joint has a `HOME_SEQUENCE`: LinuxCNC would accept it and do nothing | `NmlPort::home` |
| A program path must resolve to a regular file inside `[DISPLAY] PROGRAM_PREFIX`, through symlinks, and be at most 254 bytes | `ProgramStore`, `Session` |
| A listing shows at most 1000 entries and a program at most 16 MiB of whole lines; both say `truncated` | `ProgramStore` |
| `preview_context` goes up by one whenever a preview input changes: program, files of the program, block delete, and the machine state (work systems, G92, the tool table with every field, interpreter start state). The machine state is frozen while a program runs or is paused | `PreviewContext` |
| A preview is computed only from a coherent start: at rest, tool table accepted, link live, for that program revision. During a run it is the plan from before the run | `PanelComposer` |
| Every `ctest` checkpoint of a C++ change includes a build of the `analyze` preset | |
| No non-finite number reaches the wire: the encoder refuses the message | `encode_*` |
| Joint jog until all axes are homed, axis jog after | `Decision::jog_space`, `NmlPort::start_jog` |
| A page load is a generation; messages of another page are dropped; ids strictly increase | `Session::begin_page`, `handle` |
| Every executed or rejected request gets exactly one result | `Session` |
| Control thread, 10 ms: the only owner of the HAL pins; heartbeat, dead-man, jog-stop level; no NML call. Status thread, 5 ms: its own NML status channel. Worker: the only writer of NML commands | `Runtime` |
| Every libnml call is made under one process-wide gate, one call at a time: libnml shares unlocked state between channels | `src/linuxcnc/src/nml_gate.hpp` |
| A stop decision (release, lease expiry after 150 ms, page end, link older than 100 ms, machine leaving on, a jog the policy would no longer start) raises the jog-stop level within one control period, without the worker | `Session::control` |
| The level stays high until no start is in flight and an NML jog stop is accepted for every such axis after its last start; a failed stop is retried every period; with a task that never answers it stays high | `Session` |
| A jog ended by anything but its own release needs `held: false` before a new start | `Session` |
| A jog step is tracked as possible motion until a jog stop is accepted after it | `Session` |
| Every non-stop port action takes a `Wanted` check, asked after each wait and after the gate is taken, before each write. `cancelled` means nothing was written; after a first write the result is `failed` and says what was sent | `ControllerPort`, `NmlPort` |
| Stop requests (abort, E-stop, power off, jog release, stop-all) are served first; 16 stops and 32 requests may wait, more are rejected | `Session` |
| From close on the level is high; the control thread writes it last | `Runtime` |
| Composed G-code lengths are written in the interpreter's unit | `Session`, `ControllerState::program_units` |
| The dead-man does not need the command channel: the level on `coffeecnc.jog-stop` drives `motion.jog-stop` and `motion.jog-inhibit` | `HalControl`, `share/hal/coffeecnc_postgui.hal` |
| A machine that pulses `motion.jog-stop` on probe contact can wire its contact level to `coffeecnc.jog-contact`. CoffeeCNC shows the reason when LinuxCNC reports a jog aborted near the contact edge. | `HalControl`, `NmlPort`, `linuxcnc_jog_contact_*` |
| `link` is live only while the status write counter advances | `TaskHeartbeat` |
| HAL diagnostics are read on a thread of their own (`DiagnosticsFeed`), about every 100 ms and only while a page watches; never by the control thread. Values come from `hal_get_pin_value_by_name` and `hal_get_signal_value_by_name`; the pin-to-signal wiring, which that API does not give, from one `halcmd -s show sig` per watch. Nothing in HAL is created, linked or set | `DiagnosticsFeed`, `read_diagnostics` |
| A HAL tool (halshow, halmeter, halscope) is started at most once at a time, in a session of its own with LinuxCNC's environment and no descriptor of CoffeeCNC's but standard output and error; one that is not in `PATH` is refused; the ones still open get SIGTERM when CoffeeCNC ends | `HalToolLauncher` |
| Only an XYZ mill with trivial kinematics, in mm or inch, with `[AXIS_*] MAX_VELOCITY` set, is accepted | `load_machine_config` |

## Limits of the current state

| Limit | Consequence |
|---|---|
| All-axis release | The jog-stop level is common to all axes: releasing one held axis stops every held axis, and each needs a new press |
| Cancellation boundary | A write that has begun inside libnml is committed; a small check-to-write window remains by design |
| Page end | Stops jogs. A program, MDI block, homing or spindle start already committed carries on |
| Abort behind a command | An abort waits behind the command the worker is in, up to that command's timeout. A libnml call can wait 10 s on its buffer semaphore (from the source; never observed) |
| After a jog step | The next stop decision raises the level and sends an NML stop for that axis, even if the step finished long ago; a step within about 30 ms of a stop is refused |
| Result order | Results can arrive out of id order: a jog refresh is answered before a queued request |
| Tool-change generation | A request withdrawn and raised again for the same tool within one control period is not told apart |
| Adapter tests | Adapter cancellation is tested in simulation only; there is no fake channel layer |
| ThreadSanitizer | It does not see inside libnml; the argument for libnml rests on the gate |
| Program path | The path check and LinuxCNC's own open are separate calls: a path changed in between is not caught. The preview works on a private copy whose originals are compared again every 1 s (a polling period, not a deadline: a change is seen at the next comparison); the copy does not bind what task opens at run |
| Preview | Public interpreter API only. Parameters, `G28`/`G30` and every work system are interpreted from the machine's parameter file, which task writes at its next synchronisation; Run checks the file again after its switch to Auto. A program stops being previewed at a parameter it reads before it sets it (tracked per line in the helper), a computed parameter number, `EXISTS` of a global (of any name outside a subroutine), `M100`–`M199`, a remapped code, probe, `M66`; the live G92 holds (`DISABLE_G92_PERSISTENCE` is not passed on); Restart synchronises task and compares the file before any motion; programs with comments that run code or write files are refused. Run and Run to line on the path before the end stay available after the review of the partial check. See [preview](preview.md) |
| Work offsets | `EMC_STAT` carries only the active system. The other eight come from `[RS274NGC] PARAMETER_FILE`, which LinuxCNC writes only when task synchronises its interpreter, never after an MDI block or a program. They are sent `current: true` only when the file read is the one CoffeeCNC's own `EMC_TASK_PLAN_SYNCH` wrote (its stamp, taken once task reported the synch done) and nothing ran since (`SynchPoint`, `NmlPanelSource`); a file replaced by anything else is `current: false` until the next synch. Else a synch is asked for, which the worker sends from idle maintenance; its wait gives way to every queued stop or request, so none waits behind it. Task refuses the synch while the machine is on in auto mode: after a program the values stay `current: false` until task leaves auto mode. Not seen: another client's command that task takes up and finishes between two 5 ms status samples followed at once by one of CoffeeCNC's. Values have six decimals in machine units (0.0000254 mm on an inch machine). Without the file the eight are sent `available: false` with zeros |
| Tool table | Shown, and used for the preview with every field, from LinuxCNC's tool data memory after two identical passes with task idle between them: a stability check on samples, not an atomic read. A program that writes the memory directly is not bracketed. Shown only with the comments read from the file for that table: while the file cannot be read (missing, a directory, over 4 MiB) the panel gets no rows and one message, and the table returns when the file reads again. The preview is not affected |
| ThreadSanitizer and the GUI | GLib, GTK and WebKitGTK are not instrumented. The window programs tell ThreadSanitizer to ignore the interceptor calls those libraries make (`called_from_lib`, `src/shell/sanitizer_suppressions.cpp`); every access of CoffeeCNC code is checked. Without it `quit_delivery` had 116 reports, none with a first frame in CoffeeCNC code; with it, no report in `quit_delivery` or the window's simulation tests. A race inside those libraries, or one ordered only by their atomics, is not seen |
| MemorySanitizer | The `msan` preset builds and runs only what is instrumented: our code, libc++ and header-only libraries. Run: `core_behavior`, `text_file`, `durable_file`, `request_boundary`, `protocol_fixtures`, `response_encoding`, `message_encoding`, `status_publishing`, `panel_assets`, `panel_feed`, `program_progress`, `offset_history`, `preview_service`, `diagnostics_feed`, `preview_wire`, `program_edit`, `program_store`, `tool_models`, `operator_settings`, `session_lifecycle`, `runtime_threads`. Not built: the adapter's tests (`machine_config`, `panel_mapping`, `hal_control`, `hal_diagnostics`, `preview_engine`, `sim_data_directory`: LinuxCNC's libraries), the shell's and the executable's (`quit_delivery`, `remote_server`, `coffeecnc_cli`, `xvfb_wrapper`, `panel_bundle`, `panel_stamp`, `install_tree`, `package_deb`: GTK, WebKitGTK, GLib), and every `linuxcnc` test. The NML, HAL and window code is not checked for uninitialised reads |
| LeakSanitizer | Suppressed in asan builds (`src/shell/sanitizer_suppressions.cpp`): `libfontconfig.so`, `libGLX_mesa.so`, `libgallium-*.so` (Mesa's software driver, CI), `libjavascriptcoregtk-4.1.so`, nothing else; a leak this program causes through an allocation in one of them is hidden too. The GUI tests run with `NO_AT_BRIDGE=1`: with GTK's accessibility bridge on, libdbus holds 800 to 1216 bytes at exit. The executable does not set it; execution with accessibility is unqualified for leaks |
| HAL diagnostics | A row is read with one lookup each: the rows of one message are not one instant of HAL. A pin linked or unlinked while the view is open has its signal's writer looked up again only when the view opens again. The value of a pin whose signal is deleted between the lookup and the load is read from memory HAL has freed |
| Start-up signal | SIGTERM before the window runs is not handled; the HAL component is then left to LinuxCNC's shutdown |
