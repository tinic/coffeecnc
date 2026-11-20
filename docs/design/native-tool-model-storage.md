# Native tool-model storage

First increment, implemented on 15 November 2026. The panel and native backend implement the messages, validation, machine isolation and save lifecycle below. Editor saves and full LinuxCNC/application restart pass native simulation checks; see the [integration review and qualification limits](../research/evidence/native-tool-models-06ae603-review.txt).

## Scope and identity

Persist family defaults and explicit per-tool model assignments. Exclude STEP files, placement, jobs, work offsets, LinuxCNC tool-table edits, and camera state. The native WebKit context stays ephemeral. Browser demonstration defaults keep their existing localStorage key; they are never silently imported into a native machine.

Native persistence requires an explicit `[COFFEECNC] MACHINE_ID`. No display-name slug, hostname or configuration-path fallback: two machines must not share settings accidentally. Missing or empty identity means session-only models, with that lifetime stated in the editor. An invalid configured identity disables storage with an explanation; it must not resolve to another machine's directory. The draft G0759 integration configuration sets the explicit ID `g0759`.

The backend owns the directory, document and write lifecycle. Default: `$XDG_DATA_HOME/coffeecnc/<machine_id>/` (normal per-user fallback when unset), with `[COFFEECNC] DATA_DIRECTORY` as an absolute configured data root and the machine ID still appended. A relative DATA_DIRECTORY disables persistence with an explanation. Use an absolute XDG_DATA_HOME, otherwise the per-user ~/.local/share fallback; do not resolve a relative environment value against the process working directory. IDs are case-sensitive ASCII `[A-Za-z0-9][A-Za-z0-9_-]{0,63}`. The panel never supplies a filesystem path.

## Document

One versioned document contains `version: 1`, `defaults` and `assignments`. The disk envelope contains machine_id, revision and document; document is the versioned object described here. The machine ID must match the configured store. Revision and machine identity are authoritative backend values, not editable model fields. Revisions are integers from 0 to 2^53 - 1; refuse further writes at exhaustion rather than wrapping.

The seven cutter defaults are `end-mill`, `ball-mill`, `drill`, `spot-drill`, `chamfer`, `face-mill` and `generic`. Each has `diameter`, `cuttingLength`, `shankDiameter`, `overallLength`, `stickout` and `tipAngle`. The `touch-probe` default has `diameter`, `shankDiameter`, `bodyDiameter`, `bodyLength` and `stylusLength`. Legacy version 1 documents with seven families get the factory probe template. Lengths are millimetres and the angle is degrees, independent of display units.

`assignments` is an array of unique tool numbers. Each entry contains:

- `tool: { number, pocket, diameter, comment }`, recording the reported row used when the operator assigned the model;
- `family`, one of the default IDs;
- `overrides`, a subset of the chosen family's fields with diameter supplied by the tool binding. An empty object means inheritance.
- Optional `holder`, one of `r8`, `tts-4` through `tts-12`, `tts-er16`, `tts-er20` or `tts-er32`. An absent holder means None. Existing assignments load with that default.

The structured `tool` binding replaces the UI's current serialized identity string at the storage boundary. Compare its field values, not C++ versus JavaScript JSON formatting. Z offset is deliberately excluded: touching off does not change physical geometry. Tool-table diameter still supplies cutting diameter; the visual editor cannot override it. Persist the exact reported tool_table diameter in millimetres: loaded_tools already converts raw machine units to mm. Do not convert through display units or compare serialized floating-point text.

Limits: 1 MiB encoded disk envelope/outbound snapshot, 1,001 assignments, unique positive 32-bit tool numbers, nonnegative 32-bit pockets, finite nonnegative binding diameter, and at most 255 UTF-8 bytes per binding comment. Reject oversized bindings rather than truncating them into another identity. Reject embedded NUL/newlines and unknown family/dimension fields. Keep the existing native request limit of 16,384 bytes and nesting depth 8. Requests update defaults or one assignment, never transmit the whole saved catalog. Bound disk reads and snapshot construction as well.

Use the existing `toolDimensionError` rules on both sides: lengths 0.001–10,000 mm, angle 1–179 degrees, cutting length within stick-out within overall length, some shank remaining, and the ball/cone fitting its cutting length. Validate complete family defaults. Validate override fields individually; validate resolved models against the current table before rendering. A later default change can make a particular assignment require review without corrupting the saved document.

## Restoration and presentation

Wait for both settings and a current matching tool table before exposing restored assignments. Match number, pocket, diameter and description; never match T number alone. A missing/stale table withholds the rendered model and must not itself delete saved assignments. Keep the saved document separate from the currently usable assignment map, so saving another model does not accidentally discard an unmatched saved entry.

An unreadable description is not a confirmed empty description. If the native publisher cannot read the comments for an accepted table, withhold its published rows and report the failure; never retain earlier text or substitute blank strings as binding evidence. Retry and republish when the file becomes readable, even if the numeric table is unchanged. Saved assignments and the preview's raw numeric tool inputs remain intact.

A changed or deleted row withholds its old model; the editor offers assigning the current row explicitly. A matching row is not proof of physical tool identity if the cutter changed without a table edit. The model remains a declared visual envelope. Selected holders use nominal geometry for the preview. No restored value changes controller state or claims collision clearance.

Changing machine identity clears loaded settings, pending saves and active assignments. Old-page or old-machine responses cannot populate the new view. A new page loads the backend document afresh. Existing dimensions remain in mm when the operator switches display units.

## Bridge additions

Protocol v1 additions use explicit optional capability detection. New backends send `Hello.machine_id: string | null` and `Hello.tool_model_storage: 1`. Older backends without that capability remain session-only; the panel sends them no new commands. A missing or invalid configured ID is reported as null; availability/reason distinguishes absent identity from invalid configuration.

The snapshot shape is:

```ts
{
  type: 'tool_models',
  machine_id: string | null,
  revision: number,
  availability: 'available' | 'session_only' | 'unavailable',
  reason: string,
  document: { version: 1, defaults: ToolDefaults, assignments: Assignment[] } | null
}
```

An available snapshot has a valid machine ID and document. Session-only/unavailable snapshots have revision 0 and document null. Missing MACHINE_ID is session_only; invalid configuration, a locked store, corrupt data or unsupported file version is unavailable with an explanation. A missing settings file is normal: available, revision 0, factory defaults and no assignments, without automatically creating a document on disk. Revision 0 is not a previously saved preference set.

After `ready`, send the snapshot for the current page. After a durable write, send the new snapshot **before** its accepted CommandResult. For a stale base revision, send the current snapshot before the rejected result. Reject snapshots not belonging to the current hello identity and capability. Machine ID alone does not replace the shell's page identity checks.

Two commands use the existing command ID and page identity:

```ts
{ type: 'set_tool_model_defaults', id, machine_id, base_revision, defaults }
{ type: 'set_tool_model_assignment', id, machine_id, base_revision,
  tool: { number, pocket, diameter, comment }, family, overrides, holder? }
```

`set_tool_model_defaults` replaces the family defaults together, preserving assignments. This fits within 16 KiB and preserves the editor's multi-family Save action. The assignment command replaces one T entry; `family: null` with `overrides: {}` removes that T entry. It preserves every other assignment and the defaults. Both require the expected configured machine ID and compare-and-swap revision. The full catalog travels only from backend to panel.

The initial loading state does not count as an empty saved document. The editor retains its draft until a confirmed result, distinguishes saving from saved, and reports failures without claiming success. Session-only use stays available with explicit wording. Unsupported/corrupt documents are not overwritten by startup defaults. Corrupt/future-version documents make the store unavailable. This increment has no automatic repair or reset: the operator must explicitly move/remove the file outside the application to recover, and restart. A normal editor Save or Reset family never overwrites that file.

The settings writer performs no NML/HAL action and no filesystem work on the GTK/control/status threads. Saving these visual preferences is independent of machine power and execution state. One writer serializes bounded work, validates the expected revision again at commit, and uses a same-directory temporary file, fsync, rename and directory fsync. Hold flock on <machine-directory>/lock for the process lifetime. A second process gets unavailable with an in-use reason; it does not write. Create the private directory with mode 0700 and files with mode 0600. Existing filesystem access errors disable storage without preventing the operator panel from running.

If the pre-commit reread finds an external change to the held document or revision, reject the write and make storage unavailable until restart. This includes deletion, a lower revision, and different content at the same revision. Do not silently replace the current catalog or publish a regressing available revision. Reading back the writer's own uncertain replacement, described below, is a separate case.

Only a confirmed durable save reports success. If an error occurs after replacement, report that the result is uncertain and refresh the authoritative snapshot before retrying; do not describe it as an untouched file. A local reply timeout or page close is not proof that the write was cancelled. New pages recover the committed state through the initial snapshot.

## Checks before shipping

Cover machine isolation, absent identity, unsupported/corrupt documents, exact numeric and size limits, and restart persistence. Exercise stale revisions, failed writes and page replacement during a save. UI checks should cover restored row matching, Z-only touch-off, changed/deleted rows, unavailable tables, saving errors and older-backend fallback. Native simulation must demonstrate a saved default and assignment surviving a page/application restart without changing LinuxCNC offsets. No hardware operation is part of this increment.
