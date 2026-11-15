# Native tool-model storage

Proposed first increment, 15 November 2026. The current implementation still uses browser defaults and page-local assignments. This contract is being agreed with the backend before protocol changes.

## Scope and identity

Persist the seven family defaults and explicit per-tool model assignments. Exclude STEP files, placement, jobs, work offsets, LinuxCNC tool-table edits, and camera state. The native WebKit context stays ephemeral. Browser demonstration defaults keep their existing localStorage key; they are never silently imported into a native machine.

Native persistence requires an explicit `[COFFEECNC] MACHINE_ID`. No display-name slug, hostname or configuration-path fallback: two machines must not share settings accidentally. Missing identity means session-only models, with that lifetime stated in the editor. An invalid configured identity disables storage with an explanation; it must not resolve to another machine's directory. The G0759 integration configuration will set an explicit ID.

The backend owns the directory, document and write lifecycle. Proposed default: `$XDG_DATA_HOME/coffeecnc/<machine_id>/` (normal per-user fallback when unset), with `[COFFEECNC] DATA_DIRECTORY` as an explicitly configured data root and the machine ID still appended. IDs are case-sensitive ASCII `[A-Za-z0-9][A-Za-z0-9_-]{0,63}`. The panel never supplies a filesystem path.

## Document

One versioned document contains `version: 1`, `defaults` and `assignments`. The backend also stores its revision for compare-and-swap writes. Revision and machine identity are authoritative backend values, not editable model fields.

`defaults` has exactly the existing families: `end-mill`, `ball-mill`, `drill`, `spot-drill`, `chamfer`, `face-mill`, `generic`. Each has the existing six fields: `diameter`, `cuttingLength`, `shankDiameter`, `overallLength`, `stickout`, `tipAngle`. Lengths are millimetres and the angle is degrees, independent of display units.

`assignments` is an array of unique tool numbers. Each entry contains:

- `tool: { number, pocket, diameter, comment }`, recording the reported row used when the operator assigned the model;
- `family`, one of the seven IDs;
- `overrides`, an optional subset of `cuttingLength`, `shankDiameter`, `overallLength`, `stickout`, `tipAngle`; an empty object means inheritance.

The structured `tool` binding replaces the UI's current serialized identity string at the storage boundary. Compare its field values, not C++ versus JavaScript JSON formatting. Z offset is deliberately excluded: touching off does not change physical geometry. Tool-table diameter still supplies cutting diameter; the visual editor cannot override it.

Proposed limits: 1 MiB encoded document, 1,024 assignments, unique positive 32-bit tool numbers, nonnegative 32-bit pockets, finite nonnegative binding diameter, and at most 1,024 UTF-8 bytes per comment. Reject embedded NUL/newlines and unknown family/dimension fields. These limits must also be enforced before unbounded parsing/allocation in the native request path.

Use the existing `toolDimensionError` rules on both sides: lengths 0.001–10,000 mm, angle 1–179 degrees, cutting length within stick-out within overall length, some shank remaining, and the ball/cone fitting its cutting length. Validate complete family defaults. Validate override fields individually; validate resolved models against the current table before rendering. A later default change can make a particular assignment require review without corrupting the saved document.

## Restoration and presentation

Wait for both settings and a current matching tool table before exposing restored assignments. Match number, pocket, diameter and description; never match T number alone. A missing/stale table withholds the rendered model and must not itself delete saved assignments. Keep the saved document separate from the currently usable assignment map, so saving another model does not accidentally discard an unmatched saved entry.

A changed or deleted row withholds its old model; the editor offers assigning the current row explicitly. A matching row is not proof of physical tool identity if the cutter changed without a table edit. The model remains a declared visual envelope, with the existing nominal-diameter and no-holder qualifications. No restored value changes controller state or claims collision clearance.

Changing machine identity clears loaded settings, pending saves and active assignments. Old-page or old-machine responses cannot populate the new view. A new page loads the backend document afresh. Existing dimensions remain in mm when the operator switches display units.

## Proposed bridge additions

Names below are proposals, pending agreement; preserve protocol v1 compatibility through explicit optional capability detection.

- `Hello.machine_id?: string | null`, plus an optional tool-model-storage capability/version. An older backend without the capability remains session-only; the panel sends it no new commands.
- A `tool_models` snapshot after `ready` and after an accepted change: machine ID, revision, availability/reason, and the validated document. Unavailable storage has a reason; missing, corrupt or future-version data must not masquerade as successfully loaded preferences.
- `set_tool_models` with the expected machine ID, base revision and complete proposed document. Reject a stale revision and return the current snapshot so an older editor cannot overwrite newer preferences. Save actions carry the normal command ID and page identity.

The initial loading state does not count as an empty saved document. The editor retains its draft until a confirmed result, distinguishes saving from saved, and reports failures without claiming success. Session-only use stays available with explicit wording. Unsupported/corrupt documents are not overwritten by startup defaults. A future-version document is read-only until an explicit migration/reset policy exists.

The settings writer performs no NML/HAL action and no filesystem work on the GTK/control/status threads. Saving these visual preferences is independent of machine power and execution state. One writer serializes bounded work, validates the expected revision again at commit, and uses a same-directory temporary file, fsync, rename and directory fsync. A second process using the same store must not create a lost-update race; lock it or make that second writer unavailable.

Only a confirmed durable save reports success. If an error occurs after replacement, report that the result is uncertain and refresh the authoritative snapshot before retrying; do not describe it as an untouched file. A local reply timeout or page close is not proof that the write was cancelled. New pages recover the committed state through the initial snapshot.

## Checks before shipping

Cover machine isolation, absent identity, unsupported/corrupt documents, exact numeric and size limits, and restart persistence. Exercise stale revisions, failed writes and page replacement during a save. UI checks should cover restored row matching, Z-only touch-off, changed/deleted rows, unavailable tables, saving errors and older-backend fallback. Native simulation must demonstrate a saved default and assignment surviving a page/application restart without changing LinuxCNC offsets. No hardware operation is part of this increment.
