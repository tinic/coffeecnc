# Tools workflow

The Tools table uses one 44 px Load button per row. Tapping anywhere else in the row opens its editor. The header contains the reported spindle tool and applied Length; G43/G49 comes from active interpreter codes. Model assignment, model defaults and confirmed deletion live in the editor. Descriptions wrap without hover-only content or ellipses; exceptionally long descriptions can grow beyond two lines rather than hiding text.

Load first presents the selected tool, table length and diameter. Confirmation sends one `T<n> M6 G43 H<n>` MDI block through the existing backend command checks. It requires live status, machine on, interpreter idle and the configured homing policy. The native tool-change prompt remains authoritative. Connection, current-tool or MDI availability changes invalidate pending confirmation; a changed table row blocks it. The UI does not invent a loaded-tool state or acknowledge the manual-change prompt automatically.

M6 can invoke configured axis motion and does not itself apply length compensation, so the confirmation explains the tool-change sequence and G43 is explicit. See the primary [LinuxCNC M6 documentation](https://linuxcnc.org/docs/2.9/html/gcode/m-code.html#mcode:m6) and [G43 documentation](https://linuxcnc.org/docs/2.9/html/gcode/g-code.html#gcode:g43). CoffeeCNC neither adds a positioning move nor promises that a machine's configured M6 is stationary.

Length consistently means the LinuxCNC table/applied Z offset in the operator table, tool editor and Manual DRO. Preview-model physical dimensions remain separate. Editing a description preserves unchanged numeric values exactly, including in inch display. Concurrent row or connection changes block stale edits. Deleting the spindle tool is disabled.

The configurable contact-height preset is documented in [operator settings](operator-settings.md). A fixed setter at an absolute machine Z needs a separate measurement design; its physical height cannot be treated as a work-coordinate reference without calibration.
