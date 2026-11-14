# Panel protocol

The authoritative TypeScript contract is [`ui/src/protocol.ts`](../ui/src/protocol.ts). C++ request fixtures are in [`fixtures/commands.json`](fixtures/commands.json). Lengths are millimetres, speeds mm/min, spindle speed rpm and overrides factors. The native core validates every requested action independently of panel controls.

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

Homing policy does not redefine `AxisStatus.homed`, establish a physical origin, or permit actions while homing is active. Work-coordinate displays, STEP registration and loaded-tool placement keep their independent freshness/reference rules. The loaded-tool body remains hidden until the axes are homed even if the machine permits unhomed execution.

Signal availability must come from explicit configuration, not the existence/default zero value of an unwired HAL pin. A stale link makes readings stale even when their numeric/bool fields remain populated. The browser simulator emits explicit null for all three feedback fields; it does not synthesize measurements from spindle commands. Sensor UI presentation and native signal mapping are subsequent increments.

The browser simulator supports policy-only capability overrides for tests. They exercise asymmetric jog rates and homing requirements without claiming to simulate G0759 mechanics or its hardware. The real machine-definition review is in [`docs/machines/g0759.md`](../docs/machines/g0759.md).
