# Review of the first milling concept

The [interactive concept](../concepts/coffeecnc-mill.html) established a visual direction. The [workflow research](../research/machinist-workflows.md) identifies where it is not yet a practical operator interface.

**Implementation correction, 14 November 2026:** the user rejected the first panel's visual departure from this concept. Preserve the original composition, typography and surface treatment while correcting workflow behavior. The [implementation alignment brief](implementation-alignment.md) records the required corrections and visual review criteria; the workflow proposals below do not replace the visual reference.

## Keep as design hypotheses

- Legible coordinate readouts and a stable location for run controls.
- Restrained color, clear typography and useful spacing.
- A contextual panel beside the main workspace.
- Optional visual association between a program, its part and its tooling.
- Layouts that can emphasize either geometry or numerical information.

## Replace or substantiate

| Concept behavior | Problem | Proposed replacement |
|---|---|---|
| Clicking a stock-origin point changes displayed coordinates | Illustrative interaction confuses view state with the machine's work offset | A setup-datum preview separate from measured touch-off and live position |
| Face / pocket / drill operations always exist | Arbitrary NC may contain no usable CAM metadata | Program-centric fallback; imported operation names labeled with their source |
| A perfect solid and vise are always visible | No actual model, stock registration or fixture dimensions have been loaded | Optional geometry with explicit setup association and missing-data states |
| Tool appears measured and setup checks appear green | The prototype has no measurements or controller connection | Evidence-based fields that can be unknown, stale or conflicting |
| A scripted marker and timer look like machining progress | They do not represent LinuxCNC execution, material removal or realistic timing | Separate preview playback from actual program/backplot state; label estimates |
| Selecting an operation changes the simulated cutter/DRO | Fine for exploring a mockup, misleading for a connected UI | Selection changes inspection context only |
| Setup / Machine / Inspect dominates navigation | Manual motion, MDI, tool offsets and interruptions are underdeveloped | Stable machine status/controls plus task workspaces chosen through workflow testing |
| Feed and spindle sliders are the main controls | First-part proving and manual motion require more context | Separate configured overrides, jog controls, single block, stops and hardware-control feedback |

## Proposed next layout to test

Keep a persistent machine-status region with controller mode, position basis, WCS, loaded tool, connection freshness and current message. Keep the actions needed during execution in stable positions. Use the remaining workspace for **manual/setup**, **program/run**, and **tools/offsets**, with drawing/model inspection available as context rather than a mandatory final stage.

Within program/run, allow switching emphasis between NC text, path view and DRO without changing machine mode or losing execution state. A setup panel should focus on the selected datum/tool task while maintaining visibility of actual coordinates and the controller's response.

These names and arrangements are proposals. Before drawing the next polished screen, walk through the sample scenarios and a real operator's manual tool-change and interruption sequences.
