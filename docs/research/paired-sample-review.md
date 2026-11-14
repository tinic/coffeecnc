# Reviewing paired CAD and NC jobs

Inspected 2026-11-14 against the pinned [sample corpus](../../samples/README.md). This review combines the actual STEP importer results, posted NC text and static FreeCAD document properties. It does not execute NC, regenerate CAM or establish collision clearance.

## Camcraft: a useful first part overlay

The [fixture documentation](../../samples/upstream/camcraft/fixtures/step/README.md) associates `ops_block.step` with face, pocket, contour and drill programs. The real importer produces bounds `[-50, -35, 0]` to `[50, 35, 20]` mm from its centimetre source units.

| Evidence | Observed value | Consequence |
|---|---|---|
| [Face NC](../../samples/upstream/camcraft/fixtures/nc/ops_block_face.nc) | G21, G54; facing at Z20; T1 D6 flat end mill | The model's top plane and programmed facing plane agree with a bottom-of-part Z origin. T1 is not the simulator's T1 face mill. |
| [Pocket NC](../../samples/upstream/camcraft/fixtures/nc/ops_block_pocket.nc) | G54; helical entry; final cutting Z12.5 | A useful next registration check against the pocket floor and cutter radius. This review has not measured that floor from the B-rep. |
| [Drill NC](../../samples/upstream/camcraft/fixtures/nc/ops_block_drill.nc) | T2 D6, 118° drill; X35/X15, Y−22; G81 Z8 R25 | Tests a different cutter family, hole locations, and canned-cycle interpretation. |
| [3D ramp NC](../../samples/upstream/camcraft/fixtures/nc/ops_3d_ramp.nc) | T2 D6/R3 ball end mill | The same T number has a different meaning in another setup. Tool requirements need a job association. |

For the block family, identity CAD-to-work placement is a plausible initial candidate: the measured model coordinates and facing plane agree. This is an inference, not a completed registration check. Preserve the distinction between the CAD-to-work transform and the operator's G54-to-machine offset.

The browser interpreter currently rejects the Camcraft programs at their G28 reference-return block; facing also uses G18 arcs and drilling uses G81/G98. Do not delete these blocks or silently approximate them to produce an attractive preview. The native interpreter preview is the appropriate next place to evaluate these original programs with an explicit machine/tool/parameter context.

## EngiWorld task 17: source stock and two setups

The [task specification](../../samples/upstream/engiworld-cam/task/single-software-execution/cli/freecad-path/task-17/task-17.json), [input STEP](../../samples/upstream/engiworld-cam/task/single-software-execution/cli/freecad-path/task-17/init_file/two_sided_part.step), [native CAM document](../../samples/upstream/engiworld-cam/task/single-software-execution/cli/freecad-path/task-17/ground_truth/task-17_real.fcstd) and [Side A](../../samples/upstream/engiworld-cam/task/single-software-execution/cli/freecad-path/task-17/ground_truth/task-17_A.nc)/[Side B](../../samples/upstream/engiworld-cam/task/single-software-execution/cli/freecad-path/task-17/ground_truth/task-17_B.nc) programs form an explicit two-setup example.

The STEP is the **initial solid plate**, not the finished pocketed/drilled part. It has six `ADVANCED_FACE` records; the importer produces one mesh with 24 vertices and 12 triangles, bounded by `[-60, -40, 0]` and `[60, 40, 18]` mm. A translucent overlay of that file cannot be described as finished-part verification or current remaining stock after Side A.

Static inspection of `Document.xml` inside the FCStd ZIP finds:

| Object | Recorded placement |
|---|---|
| `SourcePart_A`, `Clone001` (`Model-SourcePart_A`) | Identity |
| `SourcePart_B`, `Clone` (`Model-SourcePart_B`) | 180° about X, translated +18 mm in Z |
| `Job_SideA`, `Job_SideB` | Identity job placement; distinct labeled jobs |

The recorded B-side transform corresponds to `x′ = x`, `y′ = −y`, `z′ = 18 − z`. It leaves this symmetric plate's bounding box unchanged. A bounding-box agreement therefore cannot detect a missing flip. These are recorded object placements; they must not be multiplied together blindly as if source and clone were two consecutive transforms.

Side A cuts its pocket in G54 with T1. Side B cuts in G55 with T2/T3, but its postamble switches back to G54. Consequently, the controller's active WCS at program end is not necessarily the setup frame used for its cutting moves. Automatically binding the CAD model to whatever WCS happens to be active when a file opens would lose this distinction.

The browser interpreter also refuses this FreeCAD preamble's G40/G80; Side B adds G81/G98. Native preview coverage is needed before these become full UI demonstration jobs.

## Implications for the next workflow

1. Give attachments explicit roles: finished part, initial stock, fixture, or reference. A file extension and project association do not establish a role.
2. Store a CAD placement per setup, including the designated WCS and source revision. Record an explicit flip transform; do not infer it from symmetric bounding boxes or the final active WCS.
3. Present program tool requirements beside the actual tool table for operator reconciliation. T numbers, diameters and cutter families from different jobs must not silently overwrite one another.
4. Preserve the original NC and its post dialect. Unsupported interpreter features should remain visible failures until the native preview supports them.
5. Review a complete sequence: load a job, identify model roles, reconcile tools, establish the first setup, review the original path, then establish and review the second setup. A STEP attachment alone is only one part of that workflow.
