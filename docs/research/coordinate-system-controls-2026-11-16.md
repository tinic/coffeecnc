# Coordinate-system control patterns

Research on 16 November 2026, from manufacturer/project documentation. These are interface patterns, not interchangeable controller semantics.

| Control | Documented treatment | Implication for CoffeeCNC |
| --- | --- | --- |
| Haas mill | G92 shift is shown separately at the bottom of the Work Offsets page and can be cleared there. Reset/power-up behavior depends on Haas conventions. | Separate shift display; don't copy Haas clearing behavior into LinuxCNC. |
| LinuxCNC QtDragon | Offsets tab has dedicated controls to zero rotation, G92 and the current G5x offset. | Distinguish actions by what they change. |
| gmoccapy | Source has a dedicated Zero G92/G52 action issuing G92.1; normal axis touch-off uses G10 L20 P0. | Keep normal zeroing on the selected work system. |
| Avid Mach4 | Common G54–G59 controls plus a Full Fixture Table for extended offsets; current offset visibly marked. | Common shortcuts plus overflow/full-table access. |
| PathPilot | Offset table highlights active system and supports many more offsets than should be permanent buttons. | Keep active identity visible; use table/overflow for larger sets. |
| MASSO | Documentation distinguishes work offsets from temporary offsets and G92/G92.1 set/cancel. | Separate concepts rather than adding G92 to the work-system selector. |

Sources:
- https://legacy-aem.haascnc.com/content/haascnc/en/service/codes-settings.type%3Dgcode.machine%3Dmill.value%3DG92.html
- https://linuxcnc.org/docs/stable/html/gui/qtdragon.html#_offsets_tab
- https://github.com/LinuxCNC/linuxcnc/blob/master/src/emc/usr_intf/gmoccapy/gmoccapy.py
- https://www.avidcnc.com/support/instructions/software/mach4UsersGuide/#offsets-tab
- https://knowledgebase.tormach.com/15l/pathpilot-tools-and-features-15l
- https://docs.masso.com.au/getting-started-guides/machining-with-masso/work-offsets
- https://www.linuxcnc.org/docs/stable/html/gcode/coordinates.html

LinuxCNC-specific details: nine G54–G59.3 systems; G92 shifts all of them; G52 shares the G92 registers. G92.1 clears, G92.2 suspends and G92.3 reapplies saved values. LinuxCNC normally persists these values, so "temporary" must not imply they disappear on restart. Current status exposes applied G92 offsets, not a complete stored/suspended history.

Recommendation: configurable work-system shortcuts with a More menu, always retain the actual active system, and show an active G92/G52 shift as a separate inspectable indicator. No G92 creation controls or automatic clearing. Settings describe visibility as shortcuts, not disabling G-code support.
