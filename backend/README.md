# Native backend

C++23 sources of the `coffeecnc` executable, a LinuxCNC `DISPLAY` program that shows the panel of `ui/` in a GTK/WebKit window. It has run only against LinuxCNC 2.9.4 in a simulation configuration; nothing here has operated a machine. Window, threads, start-up and qualification: [`docs/shell.md`](../docs/shell.md). Toolpath preview: [`docs/preview.md`](../docs/preview.md). Installation, package and machine configurations: [`docs/packaging.md`](../docs/packaging.md). Saved tool models: [`docs/tool-model-store.md`](../docs/tool-model-store.md).

## Layers

| Layer | Directory | Contents | Depends on |
|---|---|---|---|
| core | `src/core` | `CommandPolicy`, `JogLease`, `ControllerPort`, `ControllerState`, `TaskHeartbeat`, `StatusSource`, `SafetyOutputs`, panel message structs | C++ standard library |
| app | `src/app` | `decode_request`, message encoders, `Session`, `Runtime`, `StatusPublisher`, `PreviewContext`, `ProgramStore`, `PanelFeed`, panel asset resolver | core, nlohmann/json 3.11+ |
| linuxcnc | `src/linuxcnc` | `NmlPort`, `NmlStatusReader`, the NML gate, `HalComponent`, `HalControl`, `HalShared`, `MachineConfig`, `NmlPanelSource`, `PanelComposer`, the loaded tool table, tool table edits | core, LinuxCNC development files with `libtooldata`, Boost headers |
| shell | `src/shell` | `PanelWindow`: window, web view, `coffeecnc://panel/` scheme, page generations, outbox; test hooks | app, GTK 3, WebKitGTK 4.1 |
| coffeecnc | `src/coffeecnc` | `main`, command line, start-up and shutdown order | app, linuxcnc, shell |

Only `src/linuxcnc` includes LinuxCNC headers. Each layer's public headers are under `src/<layer>/include/coffeecnc/<layer>/`.

## Build

Run inside `backend/`. Requires CMake 3.25+, Ninja and a C++23 compiler (Clang on macOS, GCC on Linux). Nothing is downloaded during configuration.

| Preset | Command | Purpose |
|---|---|---|
| `dev` | `cmake --preset dev && cmake --build --preset dev && ctest --preset dev` | Debug build, warnings are errors |
| `asan` | same with `asan` | AddressSanitizer and UndefinedBehaviorSanitizer |
| `tsan` | same with `tsan` | ThreadSanitizer. On hosts with high ASLR entropy: `setarch -R ctest --preset tsan` |
| `msan` | `tests/sanitizers/build_msan_libcxx.sh clang++ $PWD/build/msan-libcxx` once, then as `dev` | MemorySanitizer with origins, Clang 20 or later, against an instrumented libc++ of the same LLVM release; without the adapter and the shell |
| `analyze` | same with `analyze` | clang-tidy and cppcheck; configuration fails if either is missing |
| `package` | `cmake --preset package && cmake --build --preset package && cpack --preset package` | Release build without development paths; the Debian package |

| Option | Values | Effect |
|---|---|---|
| `COFFEECNC_BUILD_APP` | `ON` (default), `OFF` | `OFF` builds core only, without nlohmann/json |
| `COFFEECNC_LINUXCNC` | `AUTO` (default), `ON`, `OFF` | `AUTO` builds the adapter where LinuxCNC's development files and Boost headers are found; `ON` fails configuration without them |
| `COFFEECNC_SHELL` | `AUTO` (default), `ON`, `OFF` | `AUTO` builds the shell where `pkg-config` finds `webkit2gtk-4.1` and `gtk+-3.0`. The executable is built where adapter and shell both are |

The executable needs `libarchive-dev` (backup and restore). The adapter needs `linuxcnc-uspace-dev` and `libboost-dev` (LinuxCNC's headers include Boost's; its package does not depend on it). Where Boost cannot be installed, headers unpacked under `build/deps/usr/include` are accepted. The package ships `libtooldata.so` without `tooldata.hh`; `src/linuxcnc/upstream/tooldata.hh` is LinuxCNC's own header, used where none is installed.

Launch: `[DISPLAY] DISPLAY = coffeecnc` in the machine's INI file; LinuxCNC starts `coffeecnc -ini <file>`. Options: `--windowed` (1280 x 800 instead of full screen), `--ui <directory>` (the built panel; default is the installed one, and in a development build `ui/dist` of the checkout), `--inspector`.

## Tests

| CTest name | Label | Covers |
|---|---|---|
| `core_behavior` | | Policy decisions, jog leases |
| `tool_models` | | Tool-model document, validation, codec and store on a file system that fails at each step; real lock and file modes |
| `sim_data_directory` | | `run.sh -m` refuses a machine INI whose data directory is outside the run directory; starts nothing |
| `linuxcnc_shell_tool_models` | `linuxcnc` | The real panel saves defaults and a tool's model; LinuxCNC and the application are started again; the panel shows what was saved |
| `request_boundary` | | Request decoding limits and rejections |
| `protocol_fixtures` | | Every entry of `protocol/fixtures/commands.json` decodes and every command is in the file; the encoders reproduce `protocol/fixtures/messages.json` exactly and every message variant is in it |
| `response_encoding` | | Result and message JSON |
| `message_encoding` | | Every machine-to-panel message; non-finite numbers and out-of-range counters refused |
| `status_publishing` | | Sequence numbers, change detection, snapshots after `ready`, `preview_context` |
| `program_store` | | Path containment, symlink escapes, listing caps, program text |
| `panel_assets` | | Panel file resolution: missing 404, traversal and absolute path 403, media types |
| `preview_service` | | Composer and preview worker on fakes: pending, result, stale and recomputed, frozen inputs during a run, block delete during a run, coherent start, reuse after revalidation |
| `preview_wire` | | Request round trip; hostile helper results, each with a time bound; refused blocks, O-word names, forbidden comments; bounded name collection |
| `preview_engine` | | The real helper on `librs274`: lines, arcs, units, offsets, tools, subroutine files, every refusal, limits, helper endings, file manifest, repository samples. Built with the adapter |
| `panel_feed` | | Publisher thread: period, snapshots after every `ready`, no reading begun after stop |
| `panel_mapping` | | LinuxCNC status to panel messages: link age, prepared tool, programmed feed across G20/G21, wait order, tool-table load bracketing. Built with the adapter |
| `session_lifecycle` | | Page generations, ready gate, ids, policy, mode switching, stop decisions, hold and reconcile, release latch, tracked increments, cancellation, on a stepped driver |
| `runtime_threads` | | The same on real threads: dead-man on time while the worker blocks, stale actions dropped, stop priority, bounded queues, bounded safe shutdown; about 4 s |
| `hal_control` | | Pin ownership by the control thread, tool-change confirmation bound to its request. Built with the adapter |
| `machine_config` | | INI reading: units, limits, per-axis jog ceilings, increments, spindle ceiling, homing requirement, optional signals. Built with the adapter |
| `linuxcnc_sim_mm`, `linuxcnc_sim_inch` | `linuxcnc` | The real `Session` on `NmlPort` against LinuxCNC in simulation, through the `Runtime`; line counters; the preview compared with what the simulated machine does; 121 checks, about 85 s each |
| `linuxcnc_sim_g0759` | `linuxcnc` | The same against a profile with the G0759's values (`run.sh -p g0759`): unhomed MDI and run, per-axis ceilings, spindle ceiling, tool change without tool lengths, optional signals; about 13 s |
| `linuxcnc_sim_runner` | `linuxcnc` | `tests/sim/run.sh` ends every process after a timeout, SIGTERM and SIGHUP, and fails a program that prints `exit=0` and then exits 7, dies of a signal, or prints a failure and exits 0; a program that prints nothing and exits 0 passes; about 110 s. SIGINT is handled by the same trap but not tested: bash starts a background job with it ignored |
| `coffeecnc_cli` | | Command line and start-up refusals of the executable; no LinuxCNC, no display |
| `panel_bundle`, `panel_stamp` | | Build `ui/dist` from this checkout; the stamp checker refuses a changed source, a changed bundle and an unbuilt source |
| `linuxcnc_shell_mm`, `linuxcnc_shell_inch` | `linuxcnc` | The application with the real panel under Xvfb as LinuxCNC's DISPLAY program: 229 checks, about 32 s each, with a paired STEP sample over a computed preview |
| `text_file` | | The bounded file reader: bound, block edges, missing file, directory |
| `machine_data` | | Backup and restore on a file system that fails at each step: archive round trip, garbage and cut archives; what a backup holds and leaves out; new, changed and same files; another machine's backup; path traversal and unlisted, duplicate and symbolic-link entries refused with nothing written; refusal states of the policy, the recheck and a changed file; staging, the helper's writes, the pre-restore copy and the next start's notice; a partial write put back. Review #274: a crash at every file system call of the helper, the .var decided after exit, an abandoned restore and a target changed after the plan, HAL files only as an INI names them and never through a linked subfolder, a backup without a machine ID, a parent of PID 1 and a LinuxCNC that runs or starts again, the rollback of a renamed file whose directory fsync failed and a truthful notice, the start-up check under the lock, modes and groups kept, linked configuration files. Re-review #274: a plan staged long before the run ended, a page replaced after the quit, data files written after staging and the alarm log never restored, a target in a missing folder, a link made again by the rollback, a group the user is not in, `/tmp/linuxcnc.lock` held while the helper writes and gone during it; on real files the openat2 walk, modes and links, and a spawned helper without inherited descriptors. Built where libarchive is |
| `linuxcnc_restore` | linuxcnc | A restore on the sim profile: staged while LinuxCNC runs, LinuxCNC stops and writes its .var file, the helper (`coffeecnc --apply-restore`) writes the backup's .var and tool table after that, keeps what LinuxCNC left in the pre-restore copy, and the next start reports it once (`tests/sim/restore_tests.sh`) |
| `alarm_log` | | The persistent alarm log on a file system that fails at each step: what is recorded and with which command, the view a page gets, order across restarts with the clock going back, damaged and torn lines, rotation and a failed rotation, a full disk with nothing lost or repeated and no rotation of the older files out, records added during a write of a full queue, a stderr that blocks, the data directory's lock (held by another process, refused by the caller, held by the tool models until the last append), this run only; one real file and its mode, ENOSPC from `/dev/full` |
| `xvfb_wrapper` | | `tests/shell/under_xvfb.sh`: an early exit, a program gone after its checks and a failed check are nonzero exits |
| `install_tree`, `package_deb` | | The installed layout; the package's control fields, dependencies and contents; no build-host path in an installed-only build |
| `linuxcnc_installed_prefix_start`, `linuxcnc_installed_package_start` | `linuxcnc` | The installed executable as DISPLAY program without overrides: names its own panel and helper, a preview becomes ready, SIGTERM leaves no process |
| `linuxcnc_machine_g0759`, `linuxcnc_machine_g0759_start` | `linuxcnc` | A machine's own INI and HAL files with the hardware simulated (`run.sh -m <INI> -s <hardware HAL>`); E-stop request and external input, sampled. Skipped without `COFFEECNC_MACHINES_DIRECTORY` |
| `linuxcnc_coffeecnc_start` | `linuxcnc` | The executable starts, shows the panel, and SIGTERM ends it and LinuxCNC with no process left |

CI (`.github/workflows/sanitizers.yml`) builds `asan`, `tsan` and `msan` on every pull request and push to `main` and runs `ctest -LE linuxcnc` in each; a sanitizer report fails the job. Sanitizer suppressions and what each preset leaves out: [backend limits](../docs/backend-rules.md#limits-of-the-current-state).

`ctest --preset dev -LE linuxcnc` skips the simulation tests; they exist only where the adapter builds. One LinuxCNC runs per host: `run.sh` holds an exclusive lock (`~/ai/coffeecnc-sim-runner-<uid>.lock`, else under `/tmp`) from before it touches the run directory until it exits, waits for it at most `-w` seconds (300), then prints `sim=refused reason=locked` and exits 123; on acquiring it prints `sim=locked ... at=<epoch seconds>`; it sets `XDG_DATA_HOME` to the run directory, takes `-i <machine-id>` for the simulation profile and `-k` to keep the data of the run before, and with `-m` refuses (122) a machine INI whose `DATA_DIRECTORY` is outside the run directory; under the lock it refuses (125) while another LinuxCNC runs and never cleans one up. `-n` assembles without the lock. Each simulation test runs in `build/<preset>/sim/<CTest name>/`. `run.sh` goes by how the DISPLAY program really ended (`display.status`: exit code or signal) and prints `printed_exit=` and `child_status=`. Keep the logs of a failing run before re-running (`build/sim-history/<commit>/<preset>/`, with `Testing/Temporary/LastTest.log`). Simulation output is one `check=<name> result=pass|fail detail=…` line per check. A shell test first checks that `ui/dist` was built from the `ui/` sources of the checkout (`tests/sim/check-panel-stamp.mjs`).

## Rules and limits

What each layer enforces, with the function that enforces it, and the limits of the current state: [`docs/backend-rules.md`](../docs/backend-rules.md).

## Coordination

Claude owns `backend/`; Codex owns `ui/` design and components. `ui/src/protocol.ts` and `protocol/fixtures/commands.json` are the shared contract; changes are announced on AgentNet `#coffeecnc` first. Transport: the panel posts a JSON string to `window.webkit.messageHandlers.coffeecnc.postMessage`; the shell calls `window.coffeecncReceive(<object>)`; assets load from `coffeecnc://panel/index.html`. `ui/build-stamp.ts` and `ui/build-stamp-node.d.ts` are backend-owned files in `ui/`. LinuxCNC facts and source lines: [`docs/linuxcnc-interface.md`](../docs/linuxcnc-interface.md).
