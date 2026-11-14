# Product brief

Recorded 2026-11-14.

## Product

CoffeeCNC is a complete new LinuxCNC milling frontend. The name and independent product direction come from the project owner. Elle supplies architectural experience, not a companion-product requirement or a codebase to fork automatically.

The starting audience is an operator of a three-axis mill, including machines with manual tool changes. This is a research assumption to validate, not a settled restriction on future machines.

## Architectural direction

Use a C++ core and a GTK-hosted web interface, following the architectural concepts that have worked in Elle. Native code handles controller integration; the UI presents state and requests actions. Motion remains in LinuxCNC. UI framework and exact native library versions remain implementation decisions.

## Design intent

Build an interface that is calm, readable at the machine, and efficient during repetitive work. Modern styling must support the operator's decisions. Large visualizations are useful when they explain a toolpath or setup; they must yield space to jogging, offsets, program text or recovery when those are the task.

The original concept's Setup / Machine / Inspect arrangement, illustrated vise, operation names, origin picker and green readiness indicators are proposals, not approved machine behavior. Research must decide which survive.

## First useful product

A useful first release needs file loading, controller state, homing, manual motion and MDI, work and tool offsets, tool-change handling, preview/backplot, program execution controls, overrides, messages and recovery states. It must remain useful with only an NC file and a configured machine.

STEP display, CAM operation metadata, inspection records and material-removal simulation can extend this foundation. They are not prerequisites for operating an ordinary job.

## How we evaluate the design

Use complete scenarios: receive a program; set up stock and tools; establish work coordinates; prove the first part; run repeated parts; handle a tool change, hold, abort or alarm; resume or restart deliberately. Evaluate both richly described jobs and sparse G-code. Include physical pendant and keyboard interaction in addition to touch.

No machinist interviews or shop observations have been conducted for this repository yet. The next research stage combines primary manuals, inspectable GitHub jobs and direct operator feedback.
