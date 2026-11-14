# Panel protocol

The authoritative TypeScript contract is [`ui/src/protocol.ts`](../ui/src/protocol.ts). Shared fixtures cover [panel commands](fixtures/commands.json) and [native messages](fixtures/messages.json). C++ tests verify decoding/encoding against them; TypeScript tests keep typed literal mirrors and require every message kind. The native fixture currently contains 25 cases, including missing feedback versus zero/false and complete versus partial snapshots. Lengths are millimetres, speeds mm/min, spindle speed rpm and overrides factors. The native core validates every requested action independently of panel controls.

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

## Program text and directory completeness

`program.truncated` and `directory.truncated` are additive optional booleans. New native encoders emit them explicitly. True means the supplied text/listing is partial; the panel keeps a visible notice beside the affected view. A partial program's supplied line count is not its total length, and text delivery limits do not shorten the program LinuxCNC executes. Preview generation must use the complete source independently. The existing simulator sends complete snapshots; absent/false retains the complete-snapshot presentation for older v1 peers.

A live `status.program.file: null` means no program is loaded; no separate empty `program` message is needed. The panel clears cached text on live unload or when path/revision differs. Program text may arrive before its matching status. Stale/down placeholder status does not erase historical text, but text is shown as current only when both path and revision match.

Directory paths and parents are canonical paths within the configured program root. Relative requests resolve from that root. A limited listing need not be a globally sorted prefix; entries omitted because of the enumeration limit may sort before those displayed. Canonical pathname validation alone does not establish immutable file identity between validation, preview and LinuxCNC opening the file.

## Preview identity

`preview_context` is an opaque integer token, compared only for equality. It must change when any input used to compute the path changes: program identity/reload, work offsets and rotations, G92, relevant tool data, block delete, and the interpreter's start state. That start snapshot includes active WCS, units, distance mode, loaded tool/active tool offset and any other modal, parameter or initial-position input consumed by the preview interpreter. Freeze the start snapshot while the corresponding program runs; live progress through its modal changes is not a new starting condition. The earlier list in the TypeScript comment was illustrative, not an exhaustive restriction on invalidation inputs.
