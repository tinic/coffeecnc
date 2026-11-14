# Native backend

C++23 layers of the `coffeecnc` executable. There is no executable and no GTK shell yet. The LinuxCNC adapter has run only against LinuxCNC 2.9.4 in a simulation configuration; nothing here has operated a machine.

## Layers

| Layer | Directory | Contents | Depends on |
|---|---|---|---|
| core | `src/core` | `CommandPolicy`, `JogLease`, `ControllerPort`, `ControllerState`, `TaskHeartbeat`, panel message structs | C++ standard library |
| app | `src/app` | `decode_request`, message encoders, `Session`, `StatusPublisher`, `PreviewContext`, `ProgramStore` | core, nlohmann/json 3.11+ |
| linuxcnc | `src/linuxcnc` | `NmlPort`, `HalComponent`, `MachineConfig`, tool table edits | core, LinuxCNC development files, Boost headers |

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
| `session_lifecycle` | | Page generations, ready gate, ids, policy, mode switching, jog leases, stop retries |
| `machine_config` | | INI reading: units, limits, per-axis jog ceilings, increments, spindle ceiling, homing requirement, optional signals. Built with the adapter |
| `linuxcnc_sim_mm`, `linuxcnc_sim_inch` | `linuxcnc` | The real `Session` on `NmlPort` against LinuxCNC in simulation, about 31 s each |
| `linuxcnc_sim_g0759` | `linuxcnc` | The same against a profile with the G0759's values (`run.sh -p g0759`): unhomed MDI and run, per-axis ceilings, spindle ceiling, tool change without tool lengths, optional signals; about 13 s |
| `linuxcnc_sim_runner` | `linuxcnc` | `tests/sim/run.sh` ends every process after a timeout, SIGTERM and SIGHUP, about 110 s. SIGINT is handled by the same trap but not tested: bash starts a background job with it ignored |

`ctest --preset dev -LE linuxcnc` skips the simulation tests; they exist only where the adapter builds. One LinuxCNC runs per host: `run.sh` refuses to start while another instance or `/tmp/linuxcnc.lock` exists. Simulation output is one `check=<name> result=pass|fail detail=…` line per check.

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
| A held jog stops after 150 ms without a refresh, on link loss, and when the policy would no longer start it | `Session::tick` |
| A failed stop is retried every tick; nothing starts on an axis with an unconfirmed stop | `Session::tracked_stop` |
| Composed G-code lengths are written in the interpreter's unit | `Session`, `ControllerState::program_units` |
| The dead-man does not need the command channel: `coffeecnc.jog-stop` is netted to `motion.jog-stop` | `HalComponent`, `tests/sim/coffeecnc_postgui.hal` |
| `link` is live only while the status write counter advances | `TaskHeartbeat` |
| Only an XYZ mill with trivial kinematics, in mm or inch, with `[AXIS_*] MAX_VELOCITY` set, is accepted | `load_machine_config` |

## Limits of the current state

| Limit | Consequence |
|---|---|
| No worker thread | `NmlPort` blocks up to 1 s for an echo and 3 s for completion per command. Called on the control thread, a call over 300 ms starves the heartbeat and trips the HAL watchdog |
| Delayed start | A jog start queued while task is frozen is not fenced against a later page end |
| Program path | The path check and LinuxCNC's own open are separate calls: a path changed in between is not caught. Preview and run need an immutable copy before live use |
| Preview | No interpreter preview |
| Status publishing | Encoders and `StatusPublisher` exist; nothing fills the message structs from `NmlPort::status()` yet |

## Coordination

Claude owns `backend/`; Codex owns `ui/` design and components. `ui/src/protocol.ts` and `protocol/fixtures/commands.json` are the shared contract; changes are announced on AgentNet `#coffeecnc` first. Transport for the shell: the panel posts a JSON string to `window.webkit.messageHandlers.coffeecnc.postMessage`; the shell calls `window.coffeecncReceive(<json>)`; assets load from `coffeecnc://panel/index.html`. LinuxCNC facts and source lines: [`docs/linuxcnc-interface.md`](../docs/linuxcnc-interface.md).
