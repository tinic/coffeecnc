# Parametric tool models

The Tools workspace has **Model defaults** and a **Set model** action on each tool. The editor renders a simplified 3D envelope while dimensions change. Families cover flat end mills, ball-nose end mills, drills, spot drills, chamfer mills, face mills and a generic cylinder. Family selection is explicit; comments and T numbers do not determine a tool's shape automatically.

## Dimensions and inheritance

Each family has a reference cutting diameter, cutting length, shank diameter, overall length, stick-out and included tip angle. These are nominal starting proportions, not a manufacturer's specification. A positive tool-table diameter scales all default lengths proportionally; the angle stays fixed. A missing/zero diameter uses the reference diameter, with a visible nominal-diameter notice. Per-tool overrides supply absolute lengths in millimetres. The editor converts from the selected mm/inch display unit and keeps that unit fixed while open.

The LinuxCNC table's Z offset (`Tool.length`) never supplies physical length. Touching off a tool changes that offset without changing its visual model. The editor cannot change table diameter, controller offsets or send a motion command.

The current envelope constraints require finite positive dimensions, cutting length no greater than stick-out, stick-out no greater than overall length, and some shank beyond the cutting section. Ball ends must fit a full radius in the cutting length. Pointed families must fit the cone implied by the included angle. Unsupported tools such as tapered, bull-nose, thread, dovetail and form cutters need additional profiles; use an explicitly nominal generic envelope until those profiles exist.

## Geometry contract

The cutting tip is `(0, 0, 0)`; the body extends along **+Z**, in millimetres. `toolProfiles()` supplies radius/height sections independently of the renderer. The Three.js [LatheGeometry](https://threejs.org/docs/pages/LatheGeometry.html) renderer rotates its native +Y axis to +Z. Cutting and shank surfaces use distinct materials. The blue ring is a holder-face datum at `Z = stickout`, not a holder mesh.

The full physical tool is displayed, including the section above that datum. Flutes, individual inserts and holders are not represented. Drill/spot/chamfer tips are ideal sharp cones; ball ends are hemispherical. These are visual envelopes, not collision or material-removal simulation. The Machine workspace now places the configured loaded-tool envelope at the reported tip in the shared STEP scene. The toolpath and tip marker remain on top, so this is not an occlusion test.

## Placement in the Machine workspace

The body uses `Status.tool.in_spindle`, never a prepared T word or the selected tool-sequence section. Its anchor is `Axis.machine - Status.offsets.tool` in millimetres, exactly once, matching the toolpath's tip convention. G5x, G92 and work rotation do not transform this machine-space point. The model's physical overall length and table Z offset never supply an additional translation. This first renderer assumes the supported XYZ mill with its spindle along machine Z.

The body is hidden when the link is stale/down, an axis is unhomed/homing, a manual tool change is in progress, the loaded row is missing/unassigned, or its dimensions are invalid. The existing tip marker remains available where position is known. **Tool** toggles the envelope independently of **Part**. **Fit**, named views and initial view sizing include the visible body; live motion does not refit the camera. Tool and part share one WebGL renderer and camera. Geometry rebuilds only when its model changes; arriving positions translate the existing mesh.

The banner identifies the loaded T number and absent holder, and flags a nominal diameter if the table has none. The current protocol describes controller coordinates, not an independently measured cutter position. This display does not establish the physical length, offset correctness or collision clearance of the mounted assembly.

## Storage and lifetime

Validated family defaults persist in browser storage under `coffeecnc.tool-model-defaults.v1`. Corrupt or unsupported preferences fall back to built-in defaults. Storage failure is reported and retains defaults only in memory.

Per-tool assignments are deliberately page-local while native machine-scoped configuration remains pending. Assignments are keyed by tool number and fingerprinted with pocket, diameter and description. A changed/deleted row removes its assignment; a new connection hello clears all assignments. An open editor rejects saving against a changed tool. Z offset changes alone preserve the assignment. This does not establish physical tool identity when an operator replaces a cutter without changing its table row.

Changing defaults affects tools that inherit them; overridden lengths remain fixed. A combination made invalid by a later family-default change is labeled **Review model** and must be corrected before rendering.

## Sample-driven follow-up

The expanded corpus supplies useful next cases without justifying automatic tool-table population:

- Camcraft's `ops_3d_ramp*.nc` names a D6/R3 ball end mill; its chamfer and engraving fixtures exercise 90° pointed cutters. These are good visual family/shape references.
- EngiWorld's `tool_candidates.csv` names D16/D10/D8 end mills and D14/D12 drills, but provides no shank or stick-out dimensions. Missing geometry must remain nominal.
- Trebuchet's `tools.json` includes cutting and shank diameters, flute/overall length and a holder with its own inch units beside metric tools. An importer needs explicit unit conversion and tool/holder assembly handling.
- SM-101's tool JSON conflicts with its NC revision for T4, as documented in the sample catalog. Metadata import needs conflict review, not silent assignment.

A later native configuration format should persist machine-scoped assignments and setup-specific stick-out, support manufacturer/CAM imports with provenance, and introduce toolholders separately. The loaded-tool renderer follows that reported-tool/tip convention; future holders and other machine kinematics must retain explicit coordinate and unit contracts.
