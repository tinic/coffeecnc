# Operator settings

Settings opens from the app menu. Startup units (machine default, mm, inch), visible work systems G54–G59.3, default spindle RPM and default jog speed are machine-scoped operator preferences. They do not edit LinuxCNC offsets, INI limits, program units or issue motion/spindle commands. A zero RPM default leaves the speed unset. Speeds apply at startup; shortcut visibility updates when saved. All numeric settings accept formulas and units.

Native storage follows [native-tool-model-storage.md](native-tool-model-storage.md), with the WebKit context remaining ephemeral: explicit MACHINE_ID, backend-owned separate operator-settings.json document, revision compare-and-swap, bounded reads/writes, durable replacement and page identity. Reuse the machine directory and existing writer/lock rather than acquiring the same lock a second time. Missing identity is session-only; invalid/corrupt/locked storage is unavailable and never silently overwritten. Browser demo preferences may use localStorage and never import into native mode.

## Protocol v1 addition

Hello adds optional operator_settings_storage: 1 and optional max_spindle_rpm (finite positive INI ceiling). The operator_settings snapshot is sent after hello on ready, and before the accepted result after a durable save. Fields: type, machine_id, revision, availability, reason, document. Availability is available/session_only/unavailable, with the same rules as tool_models. Available carries valid machine_id, safe integer revision and the document below; unavailable/session_only carry revision 0/document null.

```
{ version: 1,
  startup_units: 'machine' | 'mm' | 'inch',
  enabled_wcs: number[],
  default_spindle_rpm: number,
  default_jog_velocity: number }
```

Document fields are exact. enabled_wcs contains 1–9 unique integers in the inclusive range 1–9, ascending when saved. At least one shortcut is required. RPM is finite 0..1e9; jog velocity finite >0..1e9 mm/min. Native saves additionally reject values above the configured spindle/jog limits. Preserve saved values if INI limits change; startup use clamps to current machine limits. Missing file defaults: machine units, all nine systems, spindle 0, jog min(600, configured ceiling). Revision0 is unsaved factory defaults.

Command: `{type:'set_operator_settings', id, machine_id, base_revision, document}`. UI/protocol.ts and ui/operator-settings.ts define exact shapes. Shared command fixture id28. Save is independent of machine power and has no controller side effects. Older peers without capability offer session-only work-system shortcuts and receive no new command; startup fields are disabled because they cannot survive restart. Pending or stale saves do not claim success. New sessions discard old pending edits and load their own document.

Startup defaults apply once after the initial settings snapshot, not every time settings are saved or status arrives. Preserve live programmed spindle RPM and user edits. A display-unit preference does not send G20/G21. User-selected live display units remain session-local. Jog speeds remain mm/min internally.

## Work system presentation

Enabled/disabled controls govern operator shortcuts, not the interpreter: G-code can still select any work system. Always show the actual active system, even if omitted from shortcuts. Keep at most five primary buttons; put remaining enabled systems behind More. The full offset table stays available in Tools & offsets.

G92 is an additional shift, not a tenth selectable work system. LinuxCNC G52 shares its offset registers. Show an active G92/G52 shift separately and never hide an applied shift with shortcut preferences. Ordinary Zero/Half keeps changing the active G5x system. Do not persist a second copy of G92 or automatically clear it at startup. Details may offer an explicit confirmed G92.1 clear while MDI is allowed; no G92 creation controls in this increment.

## Implementation and verification

The existing ToolModelStore owns one writer and one directory lock for both documents. OperatorSettingsArchive reads and replaces operator-settings.json under that lock; a corrupt tool-model file does not poison the separate operator document, and a failed directory lock blocks both. The file envelope is {machine_id, revision, document}; reads are bounded to 16 KiB and reject duplicate keys, unsupported versions, malformed fields and mismatched identity. Saved values survive reduced INI limits; only effective startup values are clamped.

Browser checks at 1280 × 720: save inch startup units, 2400/2 RPM and 7in/2 jog speed; reload shows inch, 1200 RPM and 3.5 in/min without starting the spindle. Disable G59.3, select it through simulated MDI, and verify it remains visible. Apply simulated G92 X0 and inspect the separate G92/G52 details. Demo defaults were restored after checking. Screenshots: ui/screenshots/settings-1280.png and settings-g92-shift-1280.png.
