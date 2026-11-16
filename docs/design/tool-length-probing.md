# Tool-length probing proposal

Status: design for review, 16 November 2026. No probing command or automatic motion is implemented by this document. Claude owns physical G0759 commissioning and deployment. The initial implementation should stay disabled until the reference method, probe polarity and permitted travel/feed have been commissioned.

## Operator workflow

The first supported operation is a single Z-down measurement from the operator's current XY. The operator positions the tool above a contact plate on a surface whose active work Z is already known, loads the tool and applies its table length, then chooses **Measure length**. The dialog states the loaded tool, work system, reference height, downward travel limit and feed, and shows the current probe state. A separate **Start measurement** confirmation authorizes that one bounded descent and its successful-contact retract. There is no automatic XY positioning.

On successful contact, retract only Z to the starting machine Z at a controlled feed, with the old length still applied. Show the measured length and the difference from the table. A second **Apply length** action writes that specific tool's Z length and reloads its compensation; dismissing the result leaves the table unchanged. The extra review is intentional for this first implementation. Stop and E-stop remain available throughout.

## Two different reference methods

The existing Tool reference height is a work-coordinate contact height. A 6.35 mm plate on work Z=0 has H=6.35 mm. The same plate on work Z=12 has H=18.35 mm. This assumes work Z was independently established; a measurement cannot determine both an unknown work origin and an unknown tool length.

A fixed setter instead needs a calibrated trigger-plane Z in machine coordinates, established using a known reference tool and repeatable machine datum. Its body thickness is not that coordinate. Give fixed-setter calibration a separate machine-scoped record with reference tool, measured datum, uncertainty/repeatability and homing/configuration identity. Do not silently reinterpret the existing height preference or reserve G59.3. Leave fixed-setter mode out of the first implementation until that calibration workflow is agreed.

LinuxCNC records probe contact in #5061–#5069 in the current work frame and success in #5070. The trajectory stops under acceleration constraints, so its final position can differ from contact. G10 L10 instead derives a length from the current position. G10 L11 uses G59.3 without G92/G52; that is a distinct reference method. [LinuxCNC probing](https://linuxcnc.org/docs/2.9/html/gcode/g-code.html#gcode:g38), [calculated tool offsets](https://linuxcnc.org/docs/2.9/html/gcode/g-code.html#gcode:g10-l10).

## Contact calculation

For the first mode, require ordinary G43 for the loaded tool, matching the table's Z length; reject dynamic/additive length offsets, nonzero tool X/Y offsets, cutter compensation, rotated coordinate systems and unsupported kinematics. Preserve the current G5x and G92/G52 values and bind them to the operation.

With this restricted model, the proposed equation is:

`new_length = old_applied_length + latched_contact_work_Z - reference_height`

Example: old length 50 mm, contact work Z 8 mm, reference H=6 mm gives new length 52 mm. This is a design derivation from `work_Z = machine_Z - work_origin_Z - shift_Z - applied_length`, to be verified against native simulations before implementation is accepted. A final stopped position 0.1 mm lower must not reduce the result by 0.1 mm. Retracting before applying must not change it either.

Prefer a typed C++ probe-result record from the NML probe latch, normalized to millimetres and correlated with this operation. LinuxCNC exposes separate latched-contact, input-value and probing-state fields. The exact NML coordinate frame, units and freshness must be confirmed in source and a millimetre/inch simulation before relying on them. Do not infer contact from a browser readout, assume an old latch is fresh, or read a parameter file that may be stale. [NML status documentation](https://linuxcnc.org/docs/2.9/html/config/python-interface.html#_reading_linuxcnc_status).

## Backend-owned sequence

Proposed states: ready → confirming → probing → contact captured → retracting → review → applying → complete, with interrupted/failed terminal states. One operation ID binds the measurement and result. One application worker owns the sequence; browser timers never issue successive motion commands. A new cycle always requires a new confirmation, including after reconnect.

Before starting, validate live controller/probe status, machine on, idle and at rest, loaded tool/table/compensation agreement, no pending tool change, stopped spindle and configured stopped-speed feedback, supported modal state, homed axes and travel limits. For this cycle require a valid homed datum even when ordinary manual work permits NO_FORCE_HOMING. Persisted preferences do not waive those conditions.

The machine profile must explicitly enable probing and identify commissioned probe polarity, valid travel direction, travel margin, feed ceiling and usable overtravel. Require the probe to be clear at start. An unavailable or tripped input disables Start. Spindle command zero alone cannot establish that a coasting spindle is stopped; the profile must supply a validated stopped-speed signal or an agreed machine-specific stop policy.

Settings provide maximum downward travel and probe feed, with formula entry and canonical mm/mm-min storage. They are bounded by commissioned machine limits and remaining Z travel, including stopping distance and setter overtravel. No universal travel/feed preset is safe to activate by default. Show effective values on every confirmation. Feed/adaptive overrides must not increase the commissioned probing speed; the implementation must define and verify scoped override handling, including restoration.

Capture the starting machine Z, tool/table revision, applied offsets, work system, frame values, homing identity, units, distance/feed modes, feed value and relevant override state. Use an explicitly bounded Z-only G38.2 move. A success requires successful command completion, a fresh correlated trip, finite contact coordinates inside the expected segment and unchanged context. No-contact, stale status, disconnect, Stop, E-stop, limit/following errors, spindle change, tool change, or frame/configuration change invalidates the result. Feed hold interrupts the sequence; it does not leave an automatic measurement resume queued.

After valid contact only, perform a controlled Z-only return to the starting machine Z, with old compensation unchanged. Check travel and release of the probe. A stuck probe or retract error invalidates the result and prevents table writes. Never append a retract after a failed/no-contact probe or after an abort. Recovery then belongs to the operator.

The application rejects competing ordinary motion, MDI, offset, tool, mode and spindle commands while a cycle owns execution. Stops retain priority. Unexpected state changes by another LinuxCNC client terminate the operation; this is detection, not a promise of a system-wide lock. On application loss, the native watchdog must stop this bounded coordinated probe/retract, not merely jog motion. That watchdog path needs new proof: the existing jog deadman is insufficient evidence.

Apply consumes the exact pending measurement once, revalidates context and writes only the selected tool's Z offset (G10 L1), then G43 H for that tool. Do not use the current-position tool_touch_off command after retract. Report partial failure explicitly if the table write succeeds but compensation reload fails; no optimistic table update or automatic retry. Read back table and applied offset before reporting completion. Preserve diameter, description and preview model.

Restore non-motion modal values/overrides on normal completion or cancellation only through validated native commands. Never restore a motion mode by sending a block that could move, automatically restart the spindle, or send recovery MDI while status is lost or E-stop is active. If restoration cannot be confirmed, show the actual resulting state and require a fresh setup; reconnect must not resume a saved cycle.

## Acceptance before enabling

Simulation must cover mm and inch machines; G20/G21 and G90/G91 entry; existing G43 length and G92 shifts; known contact versus deceleration overrun; retract-before-apply; no-contact; initially tripped/missing/stale probe; stuck probe on retract; limits; paused/interrupted motion; watchdog/browser/application loss; competing commands and changed frames/tools; duplicate Apply; partial write/reload failures; and modal restoration. Fault injection must prove no tool-table write and no automatic recovery move after an invalid result.

For G0759, the recorded first launch showed Tripped and the machine notes leave physical polarity unresolved. Establish idle/tripped/disconnected behavior and fault detection, datum repeatability, stopped-spindle proof, probe overtravel and bounded feed/travel with Claude and the machine operator before enabling the capability. A disconnected normally-open contact may look clear; maximum travel alone does not make an unwired setter safe. [Current G0759 evidence](../machines/g0759.md#first-g0759-controller-launch).

Review decisions: agree the initial work-relative contact-plate mode; agree review-before-Apply; define fixed-setter calibration separately; and choose the native correlated latch/watchdog mechanism. This proposal changes no deployed behavior.
