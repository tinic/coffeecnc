# Restore the concept's visual design

The user's review of the first running panel on 14 November 2026 was: “this looks nothing like your proposal.” The implementation at `9392f3c` reproduced many controls but replaced the concept's composition and visual hierarchy. Workflow corrections do not authorize a different visual design.

The design reference is [the original CoffeeCNC concept](../concepts/coffeecnc-mill.html), with reusable styles and structure in [its fragment](../concepts/coffeecnc-mill.fragment.html). The [workflow review](concept-review.md) still governs truthful data and practical behavior. Both requirements apply together.

Claude owns the UI correction in PR #1. Codex owns comparison against the reference and review. Hold the UI merge until the revised screens have been visually reviewed. Functional tests alone cannot establish design fidelity.

## What drifted

| Reference | First implementation | Required correction |
|---|---|---|
| Branded header, centered pill navigation, quiet machine identity | Crowded toolbar of task state and machine controls | Restore the header's hierarchy while keeping power/E-stop and connection state visible |
| Clear job title and secondary setup context | Filename in a narrow utility toolbar | Restore a job/program title region; display only metadata actually available |
| Horizontal WCS and XYZ readout strip | Permanent 376 px left sidebar with three large boxed DROs | Restore the horizontal strip; retain position basis, units, machine/DTG access and touch-off affordances |
| Large main scene with a roughly 292 px contextual inspector | Default equal-width NC text and small path view | Make the scene the primary Program view; keep Text/Split as explicit choices |
| Soft neutral surfaces, restrained borders and spacious grouping | Heavy dark frames and many equally weighted gray buttons | Reuse the concept's surface, spacing, radius and type hierarchy |
| Readable contextual operations and selected-tool details | Legend floating over line geometry, information spread across bars | Restore the right inspector with genuine program/tool context; CAM operations remain optional |
| Deliberate bottom command bar and visually dominant main action | Uniform compact controls across the entire bottom edge | Keep stable stop/hold/start locations with clear priority and grouped overrides |

Reference screenshots: [concept](reference-concept.png) and [implementation before correction](implementation-before-correction.png). Their viewport heights differ; use these to compare composition, not to infer a pixel-perfect layout at one resolution.

## Visual requirements

- Use the concept's DM Sans and IBM Plex Mono, bundled locally with their licenses. Machine operation must not depend on fetching fonts.
- Preserve its 12 px principal surface radius, approximately 24 px page margins, subtle dividers, neutral canvas and restrained blue accent. Maintain both light and dark versions of this same composition.
- Keep the job title, positions, principal workspace and main actions visually distinct. Avoid turning every status value into a bordered card or button.
- At 1280 × 800, the primary workspace and essential stop/hold controls must fit without whole-page scrolling. The original concept's extra vertical spacing can compress at this height without replacing its arrangement.
- Manual motion, MDI and tool/offset editing must use the same design system. They can have task-specific contents while preserving the header, readout and command regions.
- A toolpath-only fallback still needs a useful camera framing, legible geometry and a calm canvas. Optional model/stock context is part of the product direction, not a reason to paint a fictional vise around arbitrary NC.

## Preserve truthful behavior

The concept's illustrative stock, named operations, measured-tool badges and estimated timings are not controller evidence. Do not reinstate these as unconditional UI facts. A loaded model needs an explicit source and setup association; unavailable geometry should be identified as unavailable. Program/tool metadata can occupy the right inspector when CAM operations are absent. File-line position must not be labeled as elapsed machining time or material-removal progress.

Keep actual controller state, stale-link handling, manual tool-change waits, coordinate transforms, jog releases, single block and state-dependent command validation. A scene selection changes inspection context, not machine mode or coordinates. Jog/MDI remain available through the Manual workspace. Persistent stop/hold and software E-stop must remain easy to locate.

## Review before merging

1. Compare a revised Program screen directly with the concept at the same desktop width, in light and dark themes. Review composition first, then type, spacing, surfaces and controls.
2. Check Manual and Tools for a consistent visual design and usable touch targets.
3. Check running, paused, manual tool-change, E-stop, stale status and failed-preview states. Important state changes must remain conspicuous without destabilizing the layout.
4. Run UI type, lint, behavioral and build checks after the changes. Then exercise view switching and stop/jog interactions in the rendered application.

The first correction should produce a reviewable Program screenshot before polishing every workspace. Commit and push small increments. Codex will review those screenshots and behavior before approving the UI merge.
