# Panel protocol

The authoritative TypeScript contract is [`ui/src/protocol.ts`](../ui/src/protocol.ts). Shared fixtures cover [panel commands](fixtures/commands.json) and [native messages](fixtures/messages.json). C++ tests verify decoding/encoding against them; TypeScript tests keep typed literal mirrors and require every message kind. The native fixture currently contains 32 cases, including missing feedback versus zero/false, complete versus partial snapshots, and tool-model storage availability. Lengths are millimetres, speeds mm/min, spindle speed rpm and overrides factors. The native core validates every requested action independently of panel controls.

## Fixed tool setter

The optional `hello.fixed_setter` advertises `reference_tool`, `travel_mm`, `feed_mm_min` and `xy_tolerance_mm`. It enables the Tools workflow. The `fixed_setter` request uses an `action` field.

| Action | Additional fields |
| --- | --- |
| `status` | |
| `capture` | `tool`, `wcs`, known work contact `height` and machine contact `z` |
| `calibrate`, `measure` | `tool` and confirmed machine start `x`, `y`, `z` |
| `accept`, `dismiss` | `review` ID from the last valid cycle |

All wire lengths are in millimetres. A successful result includes `setter` with `reference_captured`, `calibration` (machine setter X, Y and effective trigger-plane Z, or null), and `review` (null or an object with `id`, `calibration` as a boolean, `tool`, `contact` XYZ, `value` and `previous_length`). A review can include `stopping_distance`, the downward distance from the contact latch to settled machine Z before retract. Start and review are separate operations. Use `abort` to interrupt an active cycle. See the [workflow and HAL requirements](../docs/design/tool-length-probing.md).

## Tool-model preferences

`hello.machine_id` is an explicit configured identity or null. The optional `hello.tool_model_storage: 1` capability enables the `tool_models` snapshot and the `set_tool_model_defaults` / `set_tool_model_assignment` commands. An older peer without that capability receives no new commands; native visual preferences then remain session-only. No machine name, pathname or display unit supplies a fallback identity.

The [storage contract](../docs/design/native-tool-model-storage.md) defines document validation, size limits, matching tool-table rows, compare-and-swap revisions and durability. Snapshots precede the result of a save. A waiting snapshot is not an empty saved document; a timeout is not cancellation. The panel validates documents and current hello identity before using a revision or restoring an assignment. Browser demonstration preferences remain separate from native settings.

## Operator settings

`hello.operator_settings_storage: 1` enables the `operator_settings` snapshot and `set_operator_settings` command. `hello.max_spindle_rpm` supplies the machine's spindle ceiling. The document contains startup display units, enabled work-system shortcuts, default spindle RPM and default jog velocity in mm/min. Saving never sends controller commands. The snapshot and save revision follow the same identity and durability rules as tool models, with a separate file and the same writer/lock. [Complete contract](../docs/design/operator-settings.md).

## Version mismatch

A hello with a different protocol version puts the panel in an incompatible connection state. It clears controller snapshots and ignores subsequent messages until a compatible hello arrives; fresh status is then required to show a live connection again. New commands are rejected locally with a result and an operator message.

Reconnect (`ready`), abort, jog-stop-all, held-jog release and software E-stop requests remain available as attempts. Their outcomes are not trusted while the protocol is incompatible, and unanswered attempts retain the normal timeout reporting. `set_machine: off` is not exempt: it can reset an E-stop, so it cannot be treated as an unconditional stop request here. Compatible protocol v1 behavior is unchanged.

## Machine-capability additions, 14 November 2026

These are **additive protocol v1 fields**. Existing command bodies and request decoding are unchanged. New native encoders should emit the fields explicitly; optional TypeScript members allow older v1 peers to retain the documented behavior below. Do not use a machine name to infer a capability.

| Message / field | Type | Meaning when present | Older peer / unavailable behavior |
| --- | --- | --- | --- |
| `hello.axis_max_jog_velocity` | Object keyed by configured lowercase axis letter, numeric mm/min | Per-axis jog ceiling; each configured axis must be included and positive/finite. The existing global `max_jog_velocity` still limits the slider and all axes. | Absent map uses the existing global ceiling. A present map with a missing/invalid axis does **not** fall back: the panel disables jogging that axis. |
| `hello.homing_required` | Boolean | Whether starting MDI/program execution requires all axes homed. Native configuration/policy determines this value. | Absent means **true**. Only explicit false relaxes this UI gate. |
| `status.spindle.actual_rpm` | Nonnegative finite number or null | Configured measured speed magnitude, separate from commanded RPM and override. This does not report measured direction. | Null/absent is unknown; never replace it with commanded RPM or zero. |
| `status.spindle.at_speed` | Boolean or null | Configured at-speed feedback. False is a reported negative result. | Null/absent is unknown, not false. |
| `status.probe_input` | Boolean or null | Configured probe input; true is tripped, false is clear. | Null/absent is unknown. |

For example, the G0759 configuration may supply these hello additions after native configuration validation:

```json
{
  "max_jog_velocity": 1500,
  "axis_max_jog_velocity": { "x": 1500, "y": 1500, "z": 750 },
  "homing_required": false
}
```

The panel clamps each continuous and incremental jog request to its axis ceiling. This does not turn the UI into the motion authority: the native policy and LinuxCNC still validate the request. A changed hello ends held panel jogs. The speed slider remains the requested speed; a note identifies axes whose effective speed is lower.

Held controls stop refreshing and send release when status becomes unavailable, the machine turns off or enters E-stop, the interpreter becomes busy, homing starts, or the all-homed state changes. Recovery requires a new press. Local answer timeouts notify the same result listeners as controller failures so held controls can release; a timeout does not prove cancellation of an already queued command. The native runtime must independently enforce leases and require release after an interrupted hold, even if stale panel refreshes continue arriving.

Homing policy does not redefine `AxisStatus.homed`, establish a physical origin, or permit actions while homing is active. Work-coordinate displays, STEP registration and loaded-tool placement keep their independent freshness/reference rules. The loaded-tool body remains hidden until the axes are homed even if the machine permits unhomed execution.

Signal availability must come from explicit configuration, not the existence/default zero value of an unwired HAL pin. A stale link makes readings stale even when their numeric/bool fields remain populated. The browser simulator emits explicit null for all three feedback fields; it does not synthesize measurements from spindle commands. Measured RPM and at-speed appear in the Machine status strip and Manual controls; probe input appears in Manual. Null/absent readings say Unavailable, stale/silent historical values are marked stale, and down/incompatible/connecting values are withheld. The native HAL adapter supports optional inputs; wiring its status into a live publisher remains a subsequent increment.

The browser simulator supports policy-only capability overrides for tests. They exercise asymmetric jog rates and homing requirements without claiming to simulate G0759 mechanics or its hardware. The real machine-definition review is in [`docs/machines/g0759.md`](../docs/machines/g0759.md).

## Work-coordinate availability

`work_offsets.systems[].available` is an additive optional boolean. New senders explicitly supply it for every one of the nine systems. False means `offset` and `rotation_xy` are finite placeholders, not coordinates: the panel displays dashes and Unavailable, and these fields must not enter preview calculations. An omitted field retains the older complete-table semantics. A real zero with `available: true` remains a known coordinate.

The agreed mapping for the first native shell reports only the active system as available, from the controller's current G5x offset and XY rotation. It marks every inactive system unavailable; it does not substitute saved parameter-file values or issue hidden MDI queries. A change of availability increments the work-offset revision. Status still carries the active offsets independently. A preview must remain unavailable until all state it requires is established.

The operator can select an unavailable system through the existing MDI controls to observe its active values; receiving an unavailable row never issues that command automatically. Stale/silent readings are labeled as history, while down/incompatible/connecting values are withheld. Invalid or duplicate rows are also withheld. The browser simulator knows its full internal table and emits explicit true for all nine entries.

## Program text and directory completeness

`program.truncated` and `directory.truncated` are additive optional booleans. New native encoders emit them explicitly. True means the supplied text/listing is partial; the panel keeps a visible notice beside the affected view. A partial program's supplied line count is not its total length, and text delivery limits do not shorten the program LinuxCNC executes. Preview generation must use the complete source independently. The existing simulator sends complete snapshots; absent/false retains the complete-snapshot presentation for older v1 peers.

A live `status.program.file: null` means no program is loaded; no separate empty `program` message is needed. The panel clears cached text on live unload or when path/revision differs. Program text may arrive before its matching status. Stale/down placeholder status does not erase historical text, but text is shown as current only when both path and revision match.

Directory paths and parents are canonical paths within the configured program root. Relative requests resolve from that root. A limited listing need not be a globally sorted prefix; entries omitted because of the enumeration limit may sort before those displayed. Canonical pathname validation alone does not establish immutable file identity between validation, preview and LinuxCNC opening the file.

## Preview identity

`preview_context` is an opaque integer token, compared only for equality. It must change when any input used to compute the path changes: program identity/reload, work offsets and rotations, G92, relevant tool data, block delete, and the interpreter's start state. That start snapshot includes active WCS, units, distance mode, loaded tool/active tool offset and any other modal, parameter or initial-position input consumed by the preview interpreter. Freeze the start snapshot while the corresponding program runs; live progress through its modal changes is not a new starting condition. The earlier list in the TypeScript comment was illustrative, not an exhaustive restriction on invalidation inputs.

## Application shutdown

`hello.quit_supported: true` enables the additive v1 `quit` command. Absent or
false means unsupported; the panel never sends it to such a peer. Its only
fields are `type: "quit"` and the usual request `id` (fixture 27).

The machine-state menu offers **Quit CoffeeCNC…**, followed by a confirmation:
**LinuxCNC shuts down; the machine is switched off.** Cancel has initial focus.
The panel rechecks capability, connection/session and interpreter state when
confirming. A stale/down link or a changed hello prevents submission. Running,
paused, waiting and executing MDI states say **Stop the program before
quitting.** An idle machine can quit while on, already off, or in E-stop.

The native backend independently rejects non-idle execution and unavailable or
stale controller status. Accepted
means the request was accepted, not proof of power-off or process exit. Native
shutdown switches the machine off before closing the window; an active E-stop
must remain asserted (no ESTOP_RESET). A failed switch-off leaves the window
open and reports the failure. The panel shows request acceptance while waiting
for the application to close, and never closes the browser/WebKit itself.
Quit uses a dedicated power-off operation that never sends ESTOP_RESET. Close
intent is bound to the request and page generation; it cannot carry across a
reload. The shell waits for successful WebKit evaluation of posted messages
before destroying that page's window. A replaced page or failed delivery
cancels the close. The native application simulation test observes the accepted
Quit result in the actual page before window closure and LinuxCNC shutdown.

The browser simulator switches its simulated machine off (preserving E-stop)
and explicitly reports that the browser stays open. It refuses busy execution
without aborting it. No physical machine or native process is controlled by
these tests.
