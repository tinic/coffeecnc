# Native backend

C++23 layers of the `coffeecnc` executable, including its threaded runtime. There is no executable and no GTK shell yet. The LinuxCNC adapter has run only against LinuxCNC 2.9.4 in a simulation configuration; nothing here has operated a machine.

## Layers

| Layer | Directory | Contents | Depends on |
|---|---|---|---|
| core | `src/core` | `CommandPolicy`, `JogLease`, `ControllerPort`, `ControllerState`, `TaskHeartbeat`, `StatusSource`, `SafetyOutputs`, panel message structs | C++ standard library |
| app | `src/app` | `decode_request`, message encoders, `Session`, `Runtime`, `StatusPublisher`, `PreviewContext`, `ProgramStore` | core, nlohmann/json 3.11+ |
| linuxcnc | `src/linuxcnc` | `NmlPort`, `NmlStatusReader`, the NML gate, `HalComponent`, `HalControl`, `HalShared`, `MachineConfig`, tool table edits | core, LinuxCNC development files, Boost headers |

Only `src/linuxcnc` includes LinuxCNC headers. Each layer's public headers are under `src/<layer>/include/coffeecnc/<layer>/`.

## Build

Run inside `backend/`. Requires CMake 3.25+, Ninja and a C++23 compiler (Clang on macOS, GCC on Linux). Nothing is downloaded during configuration.

| Preset | Command | Purpose |
|---|---|---|
| `dev` | `cmake --preset dev && cmake --build --preset dev && ctest --preset dev` | Debug build, warnings are errors |
| `asan` | same with `asan` | AddressSanitizer and UndefinedBehaviorSanitizer |
| `tsan` | same with `tsan` | ThreadSanitizer. On hosts with high ASLR entropy: `setarch -R ctest --preset tsan` |
| `analyze` | same with `analyze` | clang-tidy and cppcheck; configuration fails if either is missing |

| Option | Values | Effect |
|---|---|---|
| `COFFEECNC_BUILD_APP` | `ON` (default), `OFF` | `OFF` builds core only, without nlohmann/json |
| `COFFEECNC_LINUXCNC` | `AUTO` (default), `ON`, `OFF` | `AUTO` builds the adapter where LinuxCNC's development files and Boost headers are found; `ON` fails configuration without them |

The adapter needs `linuxcnc-uspace-dev` and `libboost-dev` (LinuxCNC's headers include Boost's; its package does not depend on it). Where Boost cannot be installed, headers unpacked under `build/deps/usr/include` are accepted.

## Tests

| CTest name | Label | Covers |
|---|---|---|
| `core_behavior` | | Policy decisions, jog leases |
| `request_boundary` | | Request decoding limits and rejections |
| `protocol_fixtures` | | Every entry of `protocol/fixtures/commands.json` decodes and every command is in the file; the encoders reproduce `protocol/fixtures/messages.json` exactly and every message variant is in it |
| `response_encoding` | | Result and message JSON |
| `message_encoding` | | Every machine-to-panel message; non-finite numbers and out-of-range counters refused |
| `status_publishing` | | Sequence numbers, change detection, snapshots after `ready`, `preview_context` |
| `program_store` | | Path containment, symlink escapes, listing caps, program text |
| `session_lifecycle` | | Page generations, ready gate, ids, policy, mode switching, stop decisions, hold and reconcile, release latch, tracked increments, cancellation, on a stepped driver |
| `runtime_threads` | | The same on real threads: dead-man on time while the worker blocks, stale actions dropped, stop priority, bounded queues, bounded safe shutdown; about 40 s |
| `hal_control` | | Pin ownership by the control thread, tool-change confirmation bound to its request. Built with the adapter |
| `machine_config` | | INI reading: units, limits, per-axis jog ceilings, increments, spindle ceiling, homing requirement, optional signals. Built with the adapter |
| `linuxcnc_sim_mm`, `linuxcnc_sim_inch` | `linuxcnc` | The real `Session` on `NmlPort` against LinuxCNC in simulation, through the `Runtime`, 104 checks, about 53 s each |
| `linuxcnc_sim_g0759` | `linuxcnc` | The same against a profile with the G0759's values (`run.sh -p g0759`): unhomed MDI and run, per-axis ceilings, spindle ceiling, tool change without tool lengths, optional signals; about 13 s |
| `linuxcnc_sim_runner` | `linuxcnc` | `tests/sim/run.sh` ends every process after a timeout, SIGTERM and SIGHUP, about 110 s. SIGINT is handled by the same trap but not tested: bash starts a background job with it ignored |

`ctest --preset dev -LE linuxcnc` skips the simulation tests; they exist only where the adapter builds. One LinuxCNC runs per host: `run.sh` refuses to start while another instance or `/tmp/linuxcnc.lock` exists. Keep the logs of a failing run before re-running (`build/sim-history/`). Simulation output is one `check=<name> result=pass|fail detail=…` line per check.

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
| Home all is refused unless every joint has a `HOME_SEQUENCE`: LinuxCNC would accept it and do nothing | `NmlPort::home` |
| A program path must resolve to a regular file inside `[DISPLAY] PROGRAM_PREFIX`, through symlinks, and be at most 254 bytes | `ProgramStore`, `Session` |
| A listing shows at most 1000 entries and a program at most 16 MiB of whole lines; both say `truncated` | `ProgramStore` |
| `preview_context` goes up by one whenever a preview input changes: program, offsets, G92, tool table, block delete, and the interpreter start state, which is frozen while a program runs | `PreviewContext` |
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
| The dead-man does not need the command channel: the level on `coffeecnc.jog-stop` drives `motion.jog-stop` and `motion.jog-inhibit` | `HalControl`, `tests/sim/coffeecnc_postgui.hal` |
| `link` is live only while the status write counter advances | `TaskHeartbeat` |
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
| Program path | The path check and LinuxCNC's own open are separate calls: a path changed in between is not caught. Preview and run need an immutable copy before live use |
| Preview | No interpreter preview |
| Status publishing | Encoders and `StatusPublisher` exist; nothing fills the message structs from `NmlPort::status()` yet |

## Coordination

Claude owns `backend/`; Codex owns `ui/` design and components. `ui/src/protocol.ts` and `protocol/fixtures/commands.json` are the shared contract; changes are announced on AgentNet `#coffeecnc` first. Transport for the shell: the panel posts a JSON string to `window.webkit.messageHandlers.coffeecnc.postMessage`; the shell calls `window.coffeecncReceive(<json>)`; assets load from `coffeecnc://panel/index.html`. LinuxCNC facts and source lines: [`docs/linuxcnc-interface.md`](../docs/linuxcnc-interface.md).
