# Air-cut programs

Use these programs to run a job on the G0759 without material. They test the Machine tab: preview, progress, overrides, pauses and tool changes.

| Program | What it tests | Run time |
| --- | --- | --- |
| [01 Square](01-square.ngc) | Rapids, straight feeds, program progress | 30 s |
| [02 Arcs](02-arcs.ngc) | G2/G3 arcs with I/J and R, a helix | 45 s |
| [03 Pocket](03-pocket.ngc) | Passes at three heights, feed changes, overrides, feed hold | 2 min |
| [04 Drill](04-drill.ngc) | G81 and G83 cycles, G98 and G99 retract | 35 s |
| [05 Pauses](05-pauses.ngc) | Messages, M0, optional stop M1, dwell | 25 s and the stops |
| [06 Tool change](06-toolchange.ngc) | Manual change from T1 to T98, length offsets | 25 s and the changes |

## Limits

All moves stay between Z+5 and Z+30 in G54, and within 30 mm of G54 X0 Y0. The spindle turns at 1500 rpm. At the end, the spindle is off and the tool is at Z+30.

The machine soft limits do not protect this area. The program is the only limit.

## Before you start

1. Set G54 X0 Y0 at the center of a clear area of at least 60 × 60 mm.
2. Set G54 Z0 on the stock top, or on a surface below the path.
3. Make sure that G54 has no rotation and that no G52 or G92 offset is active. These offsets move the full path.
4. Load T1 with a valid length.
5. Move the tool to a clear position near G54 X0 Y0 Z+30. Each program first moves straight to Z+30, then to the start point in X and Y.
6. For program 05, turn on Optional stop to test M1.
7. For program 06, make sure that T98 has a valid length and clears the area at Z+30. T98 moves with the spindle stopped.
