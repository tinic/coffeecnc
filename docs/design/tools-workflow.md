# Tools workflow

The Tools table uses outlined 44 px Edit and Load buttons per row. Tapping anywhere else in the row also opens its editor. The header contains the reported spindle tool and applied Length; G43/G49 comes from active interpreter codes. Model assignment, model defaults and confirmed deletion live in the editor. Descriptions wrap without hover-only content or ellipses; exceptionally long descriptions can grow beyond two lines rather than hiding text.

Load first presents the selected tool, table length and diameter. Confirmation sends one `T<n> M6 G43 H<n>` MDI block through the existing backend command checks. It requires live status, machine on, interpreter idle and the configured homing policy. The native tool-change prompt remains authoritative. Connection, current-tool or MDI availability changes invalidate pending confirmation; a changed table row blocks it. The UI does not invent a loaded-tool state or acknowledge the manual-change prompt automatically.

M6 can invoke configured axis motion and does not itself apply length compensation, so the confirmation explains the tool-change sequence and G43 is explicit. See the primary [LinuxCNC M6 documentation](https://linuxcnc.org/docs/2.9/html/gcode/m-code.html#mcode:m6) and [G43 documentation](https://linuxcnc.org/docs/2.9/html/gcode/g-code.html#gcode:g43). CoffeeCNC neither adds a positioning move nor promises that a machine's configured M6 is stationary.

Length consistently means the LinuxCNC table/applied Z offset in the operator table, tool editor and Manual DRO. Preview-model physical dimensions remain separate. Editing a description preserves unchanged numeric values exactly, including in inch display. Concurrent row or connection changes block stale edits. Deleting the spindle tool is disabled.

The configurable contact-height preset is documented in [operator settings](operator-settings.md). A fixed setter at an absolute machine Z needs a separate measurement design; its physical height cannot be treated as a work-coordinate reference without calibration.

## Work origins

Use Edit or tap a work-system row to edit its XYZ origins in machine coordinates. The existing Select button remains separate: opening or editing G55 does not select G55. Each entry accepts the shared unit/formula syntax. Values are canonical millimetres on the wire; unchanged entries retain their original precision even when the display is inches.

Review changes shows the current and proposed values and requires a fresh confirmation. Copy XYZ takes the entered values to the chosen destination; Clear XYZ proposes three zeros. Both use the same review step. These operations preserve the destination's XY rotation and the global G92/G52 shift. They do not copy or clear an entire rotated coordinate frame. An active-system write changes the work readout immediately without moving the machine.

Unavailable origins stay blank rather than inheriting placeholder zeros. Replacing an unavailable system requires three explicit values, or the operator can close the editor and use Select to read it first. The review labels unknown prior values Unavailable. There is no hidden coordinate-system selection when opening the editor.

The additive protocol command is `{type: 'set_work_offset', id, wcs: 1..9, offset: [X, Y, Z]}`. The native decoder requires exactly three finite numbers. The command policy requires live status, all three axes configured, machine on, idle/in-position, no tool-change handshake or homing, and the machine's homing policy. The session converts millimetres to current interpreter units and emits one explicit `G10 L2 P<n> X… Y… Z…` block. Existing dispatch checks and controller error handling apply. The backend does not select the destination or change G90/G91, units, R or G92.

This follows the primary [LinuxCNC G10 L2 definition](https://linuxcnc.org/docs/2.9/html/gcode/g-code.html#gcode:g10-l2): explicit P identifies the fixture, and omitted rotation is retained. The choice to expose XYZ separately is CoffeeCNC's design.

Drafts survive position feedback. Source-origin changes block editing; destination changes block a pending review. A work-system or MDI-availability change cancels confirmation while preserving the draft; unit or connection changes close the editor. The UI comparison is not an atomic controller lock: another LinuxCNC client can still race a write. Native command guards remain authoritative. Native NML only publishes the active system as available; the UI does not infer inactive values from a parameter file that may lag the interpreter.
