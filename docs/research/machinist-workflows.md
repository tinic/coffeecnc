# Machinist workflows and implications for CoffeeCNC

Research date: 2026-11-14. Scope: three-axis milling, including manual tool changes. Evidence consists of primary operator documentation and inspected GitHub files. This is desk research, not a completed study of machinists at work.

## What the sources establish

| Evidence | Observed workflow | Consequence to investigate |
|---|---|---|
| [LinuxCNC AXIS manual](https://linuxcnc.org/docs/stable/html/gui/axis.html) | A typical session includes homing, loading and previewing a program, loading material, jogging/touching off and running. Repeated parts may reuse a fixture or need new offsets. Manual and MDI access coexist with execution. The manual distinguishes pause, stop, single step and optional stops, and explicitly warns against selected-line restart with subroutines. | Setup is revisited throughout work; preview selection and execution restart must remain separate actions. |
| [Haas part setup](https://legacy-aem.haascnc.com/service/online-operator-s-manuals/mill-operator-s-manual/mill---part-setup.html) | Workholding, jogging, tool offsets and work offsets are distinct setup tasks. Both manual measurement and probing are covered. | An origin marker cannot replace acquiring a datum or establishing tool length. |
| [LinuxCNC tool compensation](https://linuxcnc.org/docs/stable/html/gcode/tool-compensation.html) | Tool-table touch-off depends on a loaded tool and measured reference; compensation is applied separately. Tool tables apply to manual tools as well as tool changers. | Show tool selection, physical change state, stored offsets and active compensation as separate facts. |
| [Haas operation](https://legacy-aem.haascnc.com/service/online-operator-s-manuals/mill-operator-s-manual/mill---operation.html) | Graphics mode produces no machine movement. Run-stop-jog-continue is a specific recovery procedure; its return path and offset handling matter. Feed hold can leave the spindle turning. | Distinguish preview, actual running, holding and recovery. Haas behavior illustrates the problem; it is not a specification for LinuxCNC. |
| [Sienci running-jobs handbook on GitHub](https://github.com/Sienci-Labs/Resources/blob/main/cnc-fun/handbook/cnc-running-jobs.md) | The workflow checks the selected coordinate system, physical workholding, installed tool, CAM origin and project file. It covers jobs split into multiple toolpaths when tool changing is unavailable. | A job may span several NC files. Operator-confirmed setup facts need a different representation from controller telemetry. Router-specific instructions must not be copied into milling behavior wholesale. |
| [MakeIt Labs/Tormach pendant project](https://github.com/tangentaudio/tormach-pendant) | The documented physical controls include cycle start, feed hold, stop, maximum velocity, feed/spindle overrides and an axis jog wheel. | Evaluate the UI while the operator is using hardware controls, not only clicking the screen. |
| [Collected GitHub jobs](../../samples/README.md) | Associated CAD, CAM, NC and setup data can disagree about material, tool identity, revision or placement. Some programs have no model at all. | Expose provenance and conflicts; avoid manufacturing a confident “ready” state from imported metadata. |

## Proposed workflow model

The following is a product interpretation of the evidence. It is a set of scenarios to validate with operators, not a universal machining procedure.

| Situation | Operator's question | CoffeeCNC should make available |
|---|---|---|
| Machine startup | Which machine/configuration am I using, and what prevents motion? | Connection freshness, machine mode, enable/estop state, homing by axis/joint as applicable, relevant limits and messages. |
| Receive or revise a job | Is this the intended program and revision? | File identity, modification/reload state, units, controller dialect clues, referenced tools, preview status, and optional associated documents. |
| Establish workholding | How is this stock held, and where is its datum? | Setup notes and optional geometry beside accessible manual controls; a way to record operator declarations without presenting them as sensed facts. |
| Locate the work | Which coordinate system and offset am I changing? | Jog mode/increment/rate, MDI, machine/work coordinates, explicit G54–G59.x selection, touch-off or configured probing, and review of the resulting values. |
| Prepare tools | Which tool is physically loaded, and which length is active? | Tool table, selected versus loaded tool, compensation state and configured measurement workflow. Manual-change acknowledgement and ATC progress need different presentations. |
| Prove the first part | What will happen next, and how do I control it? | Program text/current execution context, path preview, distance-to-go, single block, optional stop/block delete, separate relevant overrides and immediately available hold/abort. |
| Routine running | Is the cut proceeding as intended? | Large stable DRO, actual versus commanded feed/spindle where feedback exists, current tool, program state, messages and physical-control feedback. |
| Change a tool or pause | What is the controller waiting for? | A specific waiting reason and valid next actions. Keep spindle/coolant state visible during a hold. |
| Abort, alarm or lose connection | What stopped, and what information can I still trust? | Reason, timestamp/freshness, controller state after reconnection, and a deliberate recovery path. Do not display cached telemetry as live. |
| Run the next part or setup | What must be repeated and what remains valid? | Reload/re-run clarity, fixture/WCS identification, changed tool or offset information, and optional measurement notes. Avoid forcing every repeated part through every setup page. |

These situations overlap. An operator may return from a program to jogging, inspect an offset during setup, change stock between cycles, or consult a drawing while stopped. The navigation should support those returns without concealing the controller's real mode.

## Changes required before a second concept

1. **Give manual operation a complete workspace.** The first concept has no serious jog, MDI, touch-off or tool-table interaction. Those are central milling tasks, not secondary settings.
2. **Keep live state stable.** Selecting a model origin, CAM operation or preview time must never alter a live DRO. Show preview coordinates separately. Display the chosen work coordinate system and whether position is actual or commanded.
3. **Make program text a peer of the picture.** Plain NC without a model needs an equally useful layout. Keep the current execution context and messages visible when geometry is unavailable or preview generation is incomplete.
4. **Replace unconditional green checks with evidence.** Distinguish controller-reported state, imported setup data, measured values and operator confirmation. Surface the material/tool discrepancies found in the samples.
5. **Separate browsing from starting.** Clicking a recognized operation should inspect it. Restarting there is a different action requiring controller-supported semantics and validated context. A progress scrubber cannot serve as a restart mechanism.
6. **Treat hold, abort and emergency stop as different states.** Their appearance and available actions must follow the configured LinuxCNC behavior. A browser animation stopping says nothing about the real spindle or motion subsystem.
7. **Design for hands occupied at the machine.** Include pendant/MPG, keyboard focus, gloves, deliberate touch targets and glanceable readouts in evaluations. A responsive phone layout is not evidence of shop-floor usability.
8. **Allow inspection without making it a mandatory final stage.** Measurements can occur between operations, during first-part proving and between repeated parts. A fixture swap or second setup may follow instead of “job complete.”

## Concrete walkthroughs using the corpus

| Walkthrough | Fixture | Pass condition for a future prototype |
|---|---|---|
| Load a sparse program | LinuxCNC hole circle | Shows inches, drilling-cycle context and preview status without requiring STEP or inventing named operations. |
| Prepare a multi-tool job | LinuxCNC compensated circles | Distinguishes selected/loaded tools and active length/radius compensation; represents M1 and a tool-change wait explicitly. |
| Import several files | CAMotics heart/cutout | Preserves file order and per-tool units; does not equate T words with physical tool changes. |
| Associate a model | Trebuchet left/right | Requires a supported setup transform; does not assume STEP origin equals NC origin or that left/right means flipping the stock. |
| Check inconsistent metadata | Trebuchet setup sheet | Shows the MDF/stainless discrepancy and earlier document date; does not copy the material field into a trusted readiness check. |
| Check tool revisions | SM-101 | Shows the T4 tool mismatch between JSON and NC and keeps both source references available. |
| Preview programmable motion | LinuxCNC grid probe | Recognizes probe-dependent execution and loops; does not render an invented deterministic measured surface. |
| Lose controller updates during a hold | Simulated controller scenario, not an upstream file | Marks state stale and avoids inferring that the machine stopped from a disconnected UI. |

## Questions for operator observation

Ask machinists to walk through a recent part using their actual files, fixture, tools and control. Record where they look, what they touch, what they remember and what they distrust. Do not begin by asking whether they like the screenshot.

- What arrives at the machine: NC only, a setup sheet, a drawing, a CAM project, STEP, or a mixture? How are revisions distinguished?
- How do they establish and verify work zero and tool length? Which tools repeat accurately in holders, and which need measurement after each change?
- What do they watch during first-part proving versus the tenth part? Which controls are physical?
- What causes the most frequent interruption? What do they do after hold, abort, tool failure or a power cycle?
- How do multiple files, fixtures, work offsets and second operations fit together?
- Which data can be absent without blocking work, and which omissions should block starting?

Seek at least three different setups initially: a manual-tool-change mill, an ATC machine, and a router-style machine. The latter is useful for contrast, not evidence that all three need identical defaults. The current research has not yet recruited or observed these operators.
