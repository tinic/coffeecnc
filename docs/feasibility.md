# What is realistic about the concept?

The presentation is realistic to build. The initial mockup does not establish that its implied machine behavior is correct. C++ plus a locally hosted web interface is a credible way to implement the product, and Elle provides relevant experience with that arrangement.

## Features and their actual prerequisites

| Feature shown or discussed | Feasibility | Required work/data |
|---|---|---|
| Modern layout, typography, theme and readable DRO | Ordinary interface work | Actual controller values, units, freshness and a stable layout under changing messages |
| Feed/spindle override controls and program actions | Normal frontend integration | Command lifecycle, mode eligibility, external-control synchronization and configured limits |
| Toolpath preview and live backplot | Feasible, substantial integration | Interpreter semantics, transforms, current tool/setup and separation of planned from executed motion |
| Named operations such as face/pocket/drill | Available when metadata exists | CAM/post integration, trusted comments or a CoffeeCNC-authored cycle; a fallback when names are absent |
| STEP part and fixture display | Feasible as optional context | Import/tessellation, units, coordinate registration and separate fixture geometry |
| Material-removal simulation | A distinct subsystem | Starting stock, tool geometry, interpreted motion, appropriate fidelity and performance |
| Collision checking | More demanding than drawing a model | Holders, fixtures, machine geometry, transforms, supported kinematics and clearly bounded checks |
| Tool/setup readiness | Only as good as its evidence | Controller state, measurement records, operator declarations and handling of stale/conflicting information |
| Cycle-time estimate | Useful when labeled as an estimate | Motion limits, blending/acceleration assumptions, tool changes and other non-cutting time |
| Inspection feedback | Feasible when actual measurements are supplied | Measurement source, nominal/tolerance data, traceability and a clear distinction from simulated values |

LinuxCNC exposes command/status/error channels, modes, positions, offsets and tool-related state. The integration still has to interpret those values correctly for the selected machine configuration. [LinuxCNC interface reference](https://linuxcnc.org/docs/stable/html/config/python-interface.html)

## AI's useful role

AI can accelerate visual iterations, adapters, import experiments and scenario scaffolding. It cannot supply missing shop-floor observations or make inconsistent source files trustworthy. Our [sample inspection](../samples/README.md) found mismatched material declarations, tool descriptions and revisions even in repositories that contain rich job data.

Treat a generated screen as a hypothesis to test. The harder product work lies in truthful state, manual operation, interruptions, setup changes and recovery. None of those becomes trivial because the initial interface can be generated quickly.

## Sensible sequence

1. Validate workflows against the sample corpus and operator observation.
2. Build the native shell, state contract and simulated controller; cover manual/setup and program-run scenarios.
3. Integrate LinuxCNC simulation, file loading, offsets, tool-change handling, execution controls and error states.
4. Add dependable interpreter-based preview and backplot with a useful NC-only experience.
5. Add optional STEP/setup metadata after the basic machine workflow is coherent.
6. Evaluate simulation, richer CAM integration and inspection as separate capabilities.

No calendar estimate is justified yet: supported machine configurations, preview integration and operator requirements remain open.
