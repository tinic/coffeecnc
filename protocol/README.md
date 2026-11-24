# Panel protocol

The authoritative TypeScript contract is [`ui/src/protocol.ts`](../ui/src/protocol.ts). Shared fixtures cover [panel commands](fixtures/commands.json) and [native messages](fixtures/messages.json). C++ tests verify decoding/encoding against them; TypeScript tests keep typed literal mirrors and require every message kind. The command fixture currently contains 44 cases and the native fixture 42, including missing feedback versus zero/false, complete versus partial snapshots, and tool-model storage availability. Lengths are millimetres, speeds mm/min, spindle speed rpm and overrides factors. The native core validates every requested action independently of panel controls.

## Fixed tool setter

The optional `hello.fixed_setter` advertises `reference_tool`, `travel_mm`, `feed_mm_min`, `xy_tolerance_mm` and `clearance_mm`. A zero clearance uses the Z taught by the operator. The `fixed_setter` request uses an `action` field.

`hello.fixed_setter.material_travel_mm` advertises material probing with `hello.touch_probe_tool`. `fixed_setter` with `action: "material"` takes `tool`, the confirmed machine `x`, `y`, `z` in millimetres, and a travel `direction` (`z_top`, `x_minus`, `x_plus`, `y_minus`, `y_plus`). The panel maps a workpiece edge to the approach direction, for example the X− edge sends `x_plus`. An accepted result includes `setter.material` with the active WCS index and the axis set to zero (`X`, `Y`, or `Z`).

| Action | Additional fields |
| --- | --- |
| `status` | |
| `touch_off`, `check` | `tool` and machine start `x`, `y`, `z` |
| `material` | `tool`, machine start `x`, `y`, `z`, and `direction` |
| `corner`, `inside_corner` | `tool`, machine start `x`, `y`, `z`, `x_direction`, and `y_direction` |
| `pocket`, `bore` | `tool` and machine start `x`, `y`, `z` |
| `slot` | `tool`, machine start `x`, `y`, `z`, and `axis` |
| `ridge` | `tool`, machine start `x`, `y`, `z`, `axis`, and `width` |
| `boss` | `tool`, machine start `x`, `y`, `z`, and `diameter` |
| `bore_arc` | `tool`, machine start `x`, `y`, `z`, and `angles` (three directions, degrees counterclockwise from the work +X axis) |
| `boss_arc` | `tool`, machine start `x`, `y`, `z`, `angles`, and `diameter` |
| `ball` | `tool`, machine start `x`, `y`, `z`, and `gauge_width` |
| `angle` | `tool`, machine start `x`, `y`, `z`, `direction`, and `spacing` |
| `apply_angle` | `tool` and the `measurement_id` of an `angle` result |
| `runout` | `tool`, machine start `x`, `y`, `z`, `diameter` and `outside` |
| `clear_runout` | `tool` |
| `apply_measurement` | `tool` and the `measurement_id` of a `measure_only` result |

`runout` calibrates the trigger radius of the touch probe on a ring (`outside: false`, from inside) or a pin (`outside: true`, from outside below Z0) of the known `diameter`. A first pass finds the centre; a second probes +X, −X, +Y and −Y through it. The mean of opposite directions is the trigger radius correction per axis: how far the true surface lies beyond the table radius along the travel. The eccentricity of the ball from the spindle axis cancels in it and is not kept. No configured machine can orient the spindle, so after a spindle run, a tool change or a turn by hand the eccentricity points another way, and a stored one could double the error. Before any motion it refuses a rotated work system, a ring radius beyond `material_travel_mm`, and a pin when `material_travel_mm` is shorter than the boss approach. After the contacts it refuses more than 0.05 mm on an axis (a wrong gauge diameter or no ball calibration). The controller keeps the result beside the tool table. Every result of the probe tool then reports it as `setter.runout`, which has `tool`, `table_diameter` and `radius` (`x`, `y`, mm). Each XY contact of every later probe action moves by the radius correction along its actual travel, between the axes weighted by the squared direction cosines. For `bore_arc` and `boss_arc` that is the heading of each contact. `apply_measurement` sets the corrected result and does not correct it again. The ball calibration and the runout calibration measure without it. A changed table diameter (a new ball calibration) or another probe tool number voids it. A file of the earlier form, with an eccentricity, reads as its radius part only. `clear_runout` removes it. It needs neither the probe in the spindle nor a machine at rest; a probe cycle in progress refuses it.

Every action from `material` to `angle` except `ball` takes an optional `measure_only: true`. The probe then moves as usual but writes no offset. The result has no `setter.material` and has `setter.measured`: `wcs`, `axes` (`X`, `Y`, `Z`, `XY`; empty for an angle), `work` (the measured point, only its measured axes, in the work system at the time of the probe), `machine` (the same point in machine XYZ: an axis not measured at the start, Z of an XY feature at the probing depth), `faces` (each probed surface point: `direction`, `work` and `machine` XYZ; the ball-centre contact, with the runout correction, moved one ball radius along the travel) and, except for an angle, `measurement_id`. `apply_measurement` sets those axes as the original action would have. The controller accepts the ID once, in the same operator session, work frame and tool, and forgets it when another probe action begins.

`bore_arc` probes outward from the start along each direction. `boss_arc` probes inward from beyond `diameter`/2, below Z0. Both set X0 Y0 at the centre of the circle through the three ball-centre contacts. They report `setter.center_diameter`, `setter.arc_span` (the arc in degrees that the contacts cover around that centre) and `setter.center_uncertainty` (an estimate of the 95 % uncertainty of the centre in mm from latch repeatability only: the latch spread of each contact and the arc geometry). The directions must be at least 10° apart and cover at least 90°, and every search must lie within the configured travel. Otherwise the request is refused before any motion. Nothing is written when the contacts cover less than 90° around their centre (almost on a line), when their circle does not enclose the start, when that estimate is more than 0.02 mm, or, for `boss_arc`, when the diameter differs from the entered `diameter` by more than 0.5 mm or 10 %, whichever is larger. The diameter check catches only gross faults: a burr of 1 to 2 mm changes the diameter by about d²/r but moves the centre by about d.

All wire lengths are in millimetres. The reference tool teaches the setter. Another tool positions at the taught XY, probes and saves the offset in one command. A successful result includes `setter.calibration` (machine setter X, Y and effective trigger-plane Z, or null) and `setter.measurement` (null or the result of the completed cycle with `calibration` as a boolean, `tool`, `contact` XYZ, `value`, `previous_length` and `stopping_distance`). Stopping distance is the downward travel from first contact to settled machine Z. Use `abort` to interrupt an active cycle. See the [workflow and HAL requirements](../docs/design/tool-length-probing.md).

## Tool-model preferences

`hello.machine_id` is an explicit configured identity or null. The optional `hello.tool_model_storage: 1` capability enables the `tool_models` snapshot and the `set_tool_model_defaults` / `set_tool_model_assignment` commands. An older peer without that capability receives no new commands; native visual preferences then remain session-only. No machine name, pathname or display unit supplies a fallback identity. A `tool_models_reason` message can follow a snapshot with a nonempty reason. It carries the same `machine_id`, `revision` and English `reason`, plus `localization`. A panel applies it only to the matching snapshot. Older panels ignore it and still receive the exact `tool_models` v1 shape.

The [storage contract](../docs/design/native-tool-model-storage.md) defines document validation, size limits, matching tool-table rows, compare-and-swap revisions and durability. Snapshots precede the result of a save. A waiting snapshot is not an empty saved document; a timeout is not cancellation. The panel validates documents and current hello identity before using a revision or restoring an assignment. Browser demonstration preferences remain separate from native settings.

## Operator settings

`hello.operator_settings_storage: 1` enables the `operator_settings` snapshot and `set_operator_settings` command. `hello.max_spindle_rpm` supplies the machine's spindle ceiling. The document contains startup display units, enabled work-system shortcuts, default spindle RPM and default jog velocity in mm/min. Saving never sends controller commands. The snapshot and save revision follow the same identity and durability rules as tool models, with a separate file and the same writer/lock. [Complete contract](../docs/design/operator-settings.md).

## Backup and restore

`backup` writes an archive of the machine's data into `directory` and is answered with `backup_written` (path, number of files, size, and the HAL files `not_included` with the reason) before its result. `check_restore` reads the archive at `path` and writes nothing; `restore_summary` precedes its result with every file of the archive, its `role` (`ini`, `hal`, `tool_table`, `parameters`, `data`), the `target` it would replace on this machine, its `change` (`new`, `changed`, `same`; `at_exit` for the `.var` file and the data files, which LinuxCNC and CoffeeCNC write as they stop and which are compared after that; the alarm log is in the backup but is not restored and is not listed), whether the target is a `symlink` (the restore replaces the link by the file), and a `token`. `restore` takes `path` and that `token`; an archive that changed since is refused. Both are refused unless the machine is off or in E-stop and no program runs. An accepted `restore` quits the application like `quit`: the files are written after LinuxCNC has stopped, and the next start sends an operator message with the outcome. A restore that changes nothing (and has no `.var` file and no data file) is accepted with a reason and does not quit. File handling: [backend rules](../docs/backend-rules.md). Each omitted file can also carry a `localization` key and named arguments alongside its original English reason. Archive manifests keep their existing shape.

## Rigid tapping

An optional `hello.rigid_tap` object exposes `max_rpm` (programmed rpm), `reversal_turns` (the larger qualified overrun at either reversal, in spindle revolutions), and `max_z_feed_mm_min` (the Z and trajectory feed ceiling in mm/min). Missing limits hide the conversational G33.1 generator and block literal G33.1 in the program and its subprograms, even if the interpreter cannot preview the program. Program Check validates spindle 0, clockwise fixed-speed rotation, disabled spindle override, Z-only motion, and the synchronized feed and reversal envelope. The toolpath preview shows nominal travel.

## Alarm log

`alarm_log` is sent once after `hello`: what the operator was told before the page was loaded, oldest first, at most 500 entries, of earlier runs of CoffeeCNC and of earlier pages of this run. What is told after it arrives as `message` and `result` as before. Each entry has the run it was told in (`session`, from 1; the message's own `session` is this run), `time`, `level`, `text`, the `outcome` of a result (null for an operator message) and the `command` the result answers, as a panel sent it without its id (null when it is not known; only its `type` when it was longer than 512 bytes). A panel reads `command` defensively: an older panel may have sent it. `storage` is `saved` or `session_only` with the `reason`; `reason` also says when the log cannot be read or written now. `skipped` counts lines of the saved log that could not be read. The panel shows the entries under its own, grouped by run. File format and limits: [backend rules](../docs/backend-rules.md).

The alarm-log snapshot may include top-level `localization` for its storage `reason`. The English reason and the saved JSONL record format are preserved.

## Version mismatch

A hello with a different protocol version puts the panel in an incompatible connection state. It clears controller snapshots and ignores subsequent messages until a compatible hello arrives; fresh status is then required to show a live connection again. New commands are rejected locally with a result and an operator message.

A rejected `result` may include `refusal`. Operator `message` records, results with an operator message, `alarm_log.entries`, `preview.error`, and `operator_settings` may include `localization` with the same shape. Each carries a catalog `key` and `params`, an object of named string arguments. Static messages use an empty object. English `reason` and `text` continue to be sent. The panel translates by key when it recognizes the key and its expected arguments. It uses the fallback text for older peers, unknown keys, and invalid arguments. The alarm log retains the optional metadata across reloads with the same version 1 format.

Program Check keeps its English `errors` and `warnings` arrays and may also send `errors_localization` or `warnings_localization`. Entries in these arrays match the corresponding English array by position. A null entry uses its English fallback.

When the decoder has a catalog key and an unambiguous request ID, the rejected result carries the key. Field names are supplied as text parameters. Invalid requests without a usable ID are reported with the `messages.session.requestIgnored` operator message and its detail.

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
| `status.axes[].reference_mark` | Boolean, on every linear axis or on none | Only on a machine that homes on reference marks its HAL has to find first (`[COFFEECNC] REFERENCE_MARKS`): true once the axis's mark has been found. While false, `home` for that axis and for all axes is refused. | Absent means the machine has no such signal: homing is offered as before. |
| `status.axes[].at_reference_mark` | Boolean, sent with `reference_mark` | The axis stands at its reference mark now, whatever the machine state. The Home button of the axis is marked while it does. | Absent: nothing is marked. |
| `status.paused_z` | Object with numeric `offset` and `max_lift` in mm, boolean `displaced`, and string `reason` | Applied external Z lift and its configured maximum. An empty reason permits a step, subject to native bounds. | Absent means the panel omits paused Z controls. |

The optional `jog_paused_z` command takes a signed `distance` in millimetres.
When Z has been lifted, `resume` verifies the return to the paused position
before allowing program motion.

When `status.paused_z.spindle` is `can_stop`, a positive jog can carry
`stop_spindle: true` after an operator confirms the stop. `stopping` reports
that CoffeeCNC is waiting for measured spindle stop. `restore` reports
that Resume also needs to restart the saved spindle command and verify
at-speed. `recover` requires Stop after an interrupted hold.

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

`work_offsets.systems[].current` is an additive optional boolean. False means a known value that the controller may have changed since it was read: the panel shows it in italics and says so. An omitted field means current. A system that is not available is never current.

The native shell reports the active system from the controller's current G5x offset and XY rotation, always current on a live link. It reports the inactive systems from LinuxCNC's parameter file (`[RS274NGC] PARAMETER_FILE`), current only when the file was read after an interpreter synch that CoffeeCNC sent with nothing run since; see `docs/backend-rules.md`. It issues no MDI queries. Without the file the inactive systems are unavailable. A change of a value, of availability or of `current` increments the work-offset revision; `current` is no preview input. Status still carries the active offsets independently. A preview must remain unavailable until all state it requires is established.

The operator can select an unavailable system through the existing MDI controls to observe its active values; receiving an unavailable row never issues that command automatically. Stale/silent readings are labeled as history, while down/incompatible/connecting values are withheld. Invalid or duplicate rows are also withheld. The browser simulator knows its full internal table and emits explicit `available: true` for all nine entries. Like the native shell it marks the inactive ones `current: false` while something runs, and after a program until the mode leaves auto.

## Offset history storage

An `offset_history` snapshot can include `localization` for its storage `reason`. The English reason is still present, and the saved `offset-history.json` remains version 1.

## Program text and directory completeness

`program.truncated` and `directory.truncated` are additive optional booleans. New native encoders emit them explicitly. True means the supplied text/listing is partial; the panel keeps a visible notice beside the affected view. A partial program's supplied line count is not its total length, and text delivery limits do not shorten the program LinuxCNC executes. Preview generation must use the complete source independently. The existing simulator sends complete snapshots; absent/false retains the complete-snapshot presentation for older v1 peers.

A live `status.program.file: null` means no program is loaded; no separate empty `program` message is needed. The panel clears cached text on live unload or when path/revision differs. Program text may arrive before its matching status. Stale/down placeholder status does not erase historical text, but text is shown as current only when both path and revision match.

Directory paths and parents are canonical paths within the configured program root. Relative requests resolve from that root. A limited listing need not be a globally sorted prefix; entries omitted because of the enumeration limit may sort before those displayed. Canonical pathname validation alone does not establish immutable file identity between validation, preview and LinuxCNC opening the file.

## Preview identity

`preview_context` is an opaque integer token, compared only for equality. It must change when any input used to compute the path changes: program identity/reload, work offsets and rotations, G92, relevant tool data, block delete, and the interpreter's start state. That start snapshot includes active WCS, units, distance mode, loaded tool/active tool offset and any other modal, parameter or initial-position input consumed by the preview interpreter. Freeze the start snapshot while the corresponding program runs; live progress through its modal changes is not a new starting condition. The earlier list in the TypeScript comment was illustrative, not an exhaustive restriction on invalidation inputs.

## Cutting cycle status

The optional `status.program.cycle` reports `running` during a cutting cycle (including pauses), `completed` after a verified finish, and `idle` otherwise. It is independent of `restart_hint`, which may arrive after Stop once the preview is ready.

A `status.paused_z` record may include `localization` for its `reason`. It uses the same catalog metadata as a rejected paused-Z jog. The English reason remains available for older panels.

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

### Applying an edge angle

A successful `fixed_setter` `angle` reports `setter.edge_angle` in degrees
counterclockwise and an optional `setter.edge_rotation` with a `measurement_id`
and `wcs`. After confirmation, send `fixed_setter` with `action: "apply_angle"`,
the measurement ID and the probe tool. The controller accepts the measurement
once, in the same operator session and work frame. The applied result reports
the verified rotation in `setter.edge_angle`.

## HAL diagnostics

`hello.diagnostics: true` enables two additive v1 commands and one message. Absent or false: the panel offers no diagnostics view. The LAN screen's hello never carries it.

| Command | Fields | Answer |
| --- | --- | --- |
| `watch_diagnostics` | `on` (boolean) | `accepted` at once, in every machine state. While `on`, the page is sent `diagnostics` about ten times a second; `on: false` or the end of the page stops it. One page at a time |
| `launch_hal_tool` | `tool`: `halshow`, `halmeter` or `halscope` | `accepted` when the program was started; `rejected` with the reason when it is not installed, already open or could not start |

`diagnostics` has `inputs`, one per row in order, and `tools`. An input has `role` (`min_limit`, `max_limit`, `home_switch`, `amp_fault`, `estop_chain`, `estop_request`, `probe`, `spindle_on`, `spindle_at_speed`, `spindle_speed_out`, `spindle_speed_in`, `spindle_fault`, `configured`), `name` (the HAL pin or signal), `kind` (`pin` or `signal`), `state` (`missing`, `unwired`, `undriven`, `live`) and `value` (boolean, number or null). Joint rows add `joint` and, where the joint moves a known axis, `axis`; configured rows add `label`. `spindle.0.speed-in` is in revolutions per second. A tool has `tool`, `available` (installed) and `open` (started by CoffeeCNC and not ended).
