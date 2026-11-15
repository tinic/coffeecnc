# Tool-model store reference

Saved tool-model defaults and per-tool assignments of the panel, kept by the backend. Contract and reasons: [`design/native-tool-model-storage.md`](design/native-tool-model-storage.md). Sources are under `backend/`. Verified in simulation and on a test file system; durability across a power loss was not tested.

## Configuration

| Key in `[COFFEECNC]` | Rule | Effect |
|---|---|---|
| `MACHINE_ID` | `[A-Za-z0-9][A-Za-z0-9_-]{0,63}`, case-sensitive; no fallback from name, host or path | Absent or empty: `session_only`. Not valid: `unavailable` |
| `DATA_DIRECTORY` | Absolute. Default `$XDG_DATA_HOME/coffeecnc`, else `$HOME/.local/share/coffeecnc` | The machine ID is appended; file `tool-models.json`, lock file `lock` |

## Interface

| Part | Where | Effect |
|---|---|---|
| Types, both commands, `valid_machine_id` | `src/core/include/coffeecnc/core/tool_models.hpp` | Families, dimensions, document, snapshot |
| Rules, limits, codec | `src/app/include/coffeecnc/app/tool_models.hpp`, `src/app/src/tool_models.cpp` | The panel's dimension rules mirrored; factory defaults; strict file decoder |
| `resolve_tool_model_location` | `src/app/include/coffeecnc/app/tool_model_store.hpp` | `session_only`, `unavailable`, or the directory |
| `ToolModelFiles`, `ToolModelArchive`, `ToolModelStore` | same header, `src/app/src/tool_model_store.cpp` | One durable step per call; open and apply; the writer thread and its queue |
| Command decoding | `src/app/src/request.cpp` | Within 16 KiB and depth 8; unknown fields rejected |
| `Session`, `PanelFeed` | `src/app/src/session.cpp`, `src/app/src/panel_feed.cpp` | Commands go to the store, never to the NML worker; the snapshot follows `hello` |
| Wiring | `src/coffeecnc/application.cpp` | Store before runtime and feed |

## Messages

| Message | Rule |
|---|---|
| `Hello.machine_id`, `Hello.tool_model_storage: 1` | A valid ID or null |
| `tool_models` | After every `ready` and every write: `machine_id`, `revision`, `availability`, `reason`, `document` or null |
| `set_tool_model_defaults` | All seven families; keeps assignments |
| `set_tool_model_assignment` | One tool; `family: null` with `overrides: {}` removes it |
| Order | The snapshot precedes its result: after a durable write, after a stale `base_revision`, after an uncertain write |
| Policy | Permitted whatever the machine and link state; a wrong `machine_id` is rejected |

## Behaviour

| Situation | Result |
|---|---|
| No file | `available`, revision 0, factory defaults; nothing is written until the first accepted command |
| Missing directories | Made 0700 outermost first, the parent synced after each; lock and files 0600 |
| Write | The file is read again and must equal what this process read or wrote last; then temporary file, sync, rename, directory sync |
| Changed from outside, in revision or content | Nothing written; `unavailable` until restart |
| Corrupt, over 1 MiB, later version, another machine's file | `unavailable`, reason names the file; never overwritten or renamed; the operator moves it and restarts |
| Second process | `unavailable`: in use |
| Failure up to the rename | `failed`, file unchanged |
| Failure after the rename | The file is read back and sent, then `failed`: outcome uncertain |
| Page ended | A waiting command is dropped; a write begun finishes and the next page's snapshot shows it |

## Bounds

| Bound | Value |
|---|---|
| File and snapshot | 1 MiB as the backend encodes them; its refusal is authoritative |
| Assignments | 1001 |
| Tool comment in a binding | 255 bytes, no NUL or line break |
| Commands waiting | 8; more are rejected at once |
| Revision | 0 to 2^53 - 1; a saved file starts at 1 |

## Limits

| Limit | Detail |
|---|---|
| Binding | Tool number, pocket, diameter in mm as reported, comment. A matching row is not proof of the physical tool |
| Scope | No STEP files, placement, jobs or offsets |
| Not verified | Power-loss durability; the cone rule's last bit against the panel's `Math.tan`; the inch shell profile with a machine ID |
| Tests and user data | `run.sh` sets `XDG_DATA_HOME` to the run directory and refuses a machine INI whose `DATA_DIRECTORY` is outside it, as LinuxCNC's own INI reader sees the key |
