# Native backend foundation

Portable C++23 backend layers for CoffeeCNC: typed command bodies, state-dependent validation, per-axis continuous-jog leases and a JSON request decoder. There is no LinuxCNC connection, GTK shell or executable yet. These libraries cannot operate a machine.

## Build and verify

```sh
cmake -S backend -B backend/build -DCMAKE_BUILD_TYPE=Debug
cmake --build backend/build
ctest --test-dir backend/build --output-on-failure
```

Requires CMake 3.25+, a C++23 compiler and an installed CMake package for [nlohmann/json](https://github.com/nlohmann/json) 3.11+. No dependency is downloaded during configuration. The core itself has no third-party dependencies; configure with `-DCOFFEECNC_BUILD_APP=OFF` to build only that layer.

Tests exercise rejected commands during execution, unhomed motion restrictions, stale-state stop requests, invalid numeric inputs, travel limits, manual tool-change waits, independent jog timeout/release behavior, and the native request boundary.

Warnings are errors for both the library and tests. Ninja presets provide repeatable builds (run these inside `backend/`):

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Replace `dev` with `asan` for AddressSanitizer plus UndefinedBehaviorSanitizer, `tsan` for ThreadSanitizer, or `analyze` for clang-tidy and cppcheck. The analyzer preset fails configuration if either tool is missing; it never silently skips a requested check. The core supports Clang for local macOS development and GCC for Linux builds, rather than assuming Elle's GCC-only toolchain. Each layer owns its headers under `src/<layer>/include/coffeecnc/<layer>/`.

## Boundaries

`decode_request` translates the panel's protocol v1 JSON into a request ID and a typed core command. Every declared field is required, including `list_directory.path` (explicit `null` selects the program directory). Unknown commands, extra fields, duplicate keys, wrong types, invalid UTF-8, NUL in strings, nonfinite numbers and fractional/out-of-range integer fields are rejected. Request IDs are JavaScript safe integers, including zero and negative values; identity/replay handling belongs to the forthcoming session layer. The wire limit is 16 KiB and eight nested containers. These limits apply before controller policy; they do not silently coerce or drop fields.

When a complete, unambiguous object has a valid ID but its command is invalid, `DecodeError.id` retains that ID so the app can send a rejected result. Malformed JSON, duplicate keys, resource-limit violations and invalid IDs have no recoverable ID; the app must emit an operator message instead. No result is sent by the decoder itself. Strings retain their decoded contents; path containment, MDI restrictions and tool-table policy remain separate checks.

`CommandPolicy::evaluate` determines whether the app may attempt an action using an observed controller snapshot. A successful decision does **not** mean LinuxCNC accepted it. If a mode change is required, the adapter must request the mode, observe completion, then revalidate before submitting the original action. It must report actual command outcomes using the protocol's request ID.

Read-only requests and stop attempts can pass even with unavailable controller state. The adapter must not fabricate successful stops when disconnected. New motion requests need fresh state. The first supported machine profile will be a three-joint XYZ mill with trivial kinematics; nontrivial axis/joint mappings need a separate implementation.

All command limits must be passed explicitly from machine configuration or an explicitly named simulator profile. Unknown axis limits stay invalid, and no axes are implicitly configured. A jog decision includes joint versus axis space: before all axes home, the adapter must use joint jogging; afterward it must observe teleop enabled before axis jogging.

`JogLease` uses monotonic time and tracks held jogs independently for each axis. Refresh it only for submitted jog commands. Own and poll it on the native control thread, independently of GTK/WebKit's event loop, and issue a controller stop for each expired lease. Explicit release must issue a stop even if a lease was absent. Cancel active leases on page/session teardown, state loss, incompatible mode changes, abort and shutdown.

The 150 ms userspace lease cannot protect against a dead or stalled executable. The live implementation also needs a correctly configured independent HAL watchdog. No such wiring or machine behavior has been validated by these portable tests.

## Coordination and next increments

Protocol baseline: Claude's [aad44c7](https://github.com/tinic/coffeecnc/blob/aad44c71a82222ab42c498670115203463b1c3d2/ui/src/protocol.ts), including all 24 panel command types, feed modes, preview context, command acceptance semantics and panel receive-time freshness. `TaskMode::automatic` maps to the wire string `auto`.

Codex owns `backend/`; Claude owns `ui/` and the LinuxCNC interface reference. Coordinate changes through AgentNet `#coffeecnc` and keep commits reviewable. Preserve the other agent's files.

Agreed transport for the upcoming shell: the panel posts a JSON string to `window.webkit.messageHandlers.coffeecnc.postMessage`; the shell calls `window.coffeecncReceive(<json>)`. Bundled assets use `coffeecnc://panel/index.html`. The shell must treat a page replacement as a session boundary and cancel outstanding jogs.

Next: app session lifecycle and command outcomes; controller adapter abstraction and a clearly labeled demo; GTK/WebKit transport; then LinuxCNC NML/HAL integration with simulation tests. File containment, tool-table atomic updates and actual interpreter preview belong to those later layers. The current policy does not replace those checks or LinuxCNC's own validation.
