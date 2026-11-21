# G0759 air-cut programs

These LinuxCNC programs exercise CoffeeCNC's Machine tab in the air. The programmed paths stay within 30 mm of G54 X0 and Y0 and at or above G54 Z+5 mm. The paths finish at G54 Z+30 mm with the spindle and coolant off.

| Program | Exercises | Approximate run time |
| --- | --- | --- |
| [01 Square](01-square.ngc) | Rapids, linear feeds and live program progress | 30 s |
| [02 Arcs](02-arcs.ngc) | G2/G3, I/J and R arcs, and a descending helix | 45 s |
| [03 Pocket](03-pocket.ngc) | Raster passes at three heights, feed changes and overrides | 2 min |
| [04 Drill](04-drill.ngc) | G81 drilling, G83 pecking, G98 and G99 retract planes | 35 s |
| [05 Pauses](05-pauses.ngc) | Operator messages, M0, optional M1 and a dwell | 25 s plus stops |
| [06 Tool change](06-toolchange.ngc) | Manual T1/T98 changes and their length offsets | 25 s plus changes |

Run in a clear area with no material under the path. Set G54 X0/Y0 at the center of that area and G54 Z0 at the intended stock-top plane. Verify that the full XY area and Z+5 to Z+30 clear the table, fixtures and both tool lengths. Confirm there is no G54 rotation or active local or temporary offset.

Verify the T1 length in the tool table and the active length of the tool in the spindle. With the spindle and coolant stopped, bring the tool to a clear starting position near G54 X0 Y0 Z+30 before Cycle start. For program 06 verify T98's length and clearance at the manual exchange position at G54 X0 Y0 Z+30 as well. T98 moves with the spindle stopped. Enable Optional stop for program 05 to exercise M1.
