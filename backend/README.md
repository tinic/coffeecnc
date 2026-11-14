# Native backend foundation

Portable C++23 core for CoffeeCNC. This first increment contains typed command bodies, state-dependent command validation and per-axis continuous-jog leases. It has no LinuxCNC connection, GTK shell, wire decoder or executable yet. It cannot operate a machine.

## Build and verify

```sh
cmake -S backend -B backend/build -DCMAKE_BUILD_TYPE=Debug
cmake --build backend/build
ctest --test-dir backend/build --output-on-failure
```

Requires CMake 3.25+ and a C++23 compiler. The core has no third-party dependencies. Tests exercise rejected commands during execution, unhomed motion restrictions, stale-state stop requests, invalid numeric inputs, travel limits, manual tool-change waits, and independent jog timeout/release behavior.

## Boundaries

`CommandPolicy::evaluate` determines whether the app may attempt an action using an observed controller snapshot. A successful decision does **not** mean LinuxCNC accepted it. If a mode change is required, the adapter must request the mode, observe completion, then revalidate before submitting the original action. It must report actual command outcomes using the protocol's request ID.

Read-only requests and stop attempts can pass even with unavailable controller state. The adapter must not fabricate successful stops when disconnected. New motion requests need fresh state. The first supported machine profile will be a three-joint XYZ mill with trivial kinematics; nontrivial axis/joint mappings need a separate implementation.

All command limits must be passed explicitly from machine configuration or an explicitly named simulator profile. Unknown axis limits stay invalid, and no axes are implicitly configured. A jog decision includes joint versus axis space: before all axes home, the adapter must use joint jogging; afterward it must observe teleop enabled before axis jogging.

`JogLease` uses monotonic time and tracks held jogs independently for each axis. Refresh it only for submitted jog commands. Own and poll it on the native control thread, independently of GTK/WebKit's event loop, and issue a controller stop for each expired lease. Explicit release must issue a stop even if a lease was absent. Cancel active leases on page/session teardown, state loss, incompatible mode changes, abort and shutdown.

The 150 ms userspace lease cannot protect against a dead or stalled executable. The live implementation also needs a correctly configured independent HAL watchdog. No such wiring or machine behavior has been validated by these portable tests.

## Coordination and next increments

Protocol baseline: Claude's [ec0b4d4](https://github.com/tinic/coffeecnc/blob/ec0b4d427e5da1b87552236f14ade3e21e8fa0c5/ui/src/protocol.ts), including feed modes, preview context, command acceptance semantics and panel receive-time freshness. `TaskMode::automatic` maps to the wire string `auto`.

Codex owns `backend/`; Claude owns `ui/` and the LinuxCNC interface reference. Coordinate changes through AgentNet `#coffeecnc` and keep commits reviewable. Preserve the other agent's files.

Agreed transport for the upcoming shell: the panel posts a JSON string to `window.webkit.messageHandlers.coffeecnc.postMessage`; the shell calls `window.coffeecncReceive(<json>)`. Bundled assets use `coffeecnc://panel/index.html`. The shell must treat a page replacement as a session boundary and cancel outstanding jogs.

Next: app-layer request decoding/session lifecycle and command outcomes; controller adapter abstraction and a clearly labeled demo; GTK/WebKit transport; then LinuxCNC NML/HAL integration with simulation tests. File containment, tool-table atomic updates and actual interpreter preview belong to those later layers. The current policy does not replace those checks or LinuxCNC's own validation.
