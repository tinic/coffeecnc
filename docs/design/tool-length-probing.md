# Fixed-setter tool-length measurement

The **Teach setter** and **Touch off at setter** buttons are beside the loaded tool in Manual and Tools. Both run on the current homed machine datum.

## Teach once after homing

1. Load the reference tool. On the G0759 this is T98, the flat-faced 6.35 mm blank. Leave it clamped during teaching.
2. Jog to the center of the setter with a clear Z search path. Check that the tool-change height and the XY travel plane clear the longest tool, work and clamps.
3. Press **Teach setter**. The cycle teaches machine XY and the reference latch Z. CoffeeCNC subtracts the existing T98 table length to establish the setter plane. The tool table and work offsets stay as they were.

For a profile with `SETTER_CLEARANCE_MM`, the cycle ends at reference latch Z plus that height. Other profiles use the taught starting Z. The G0759 homes in place, so teach again after homing or loss of the machine datum.

## Touch off a tool

Load a tool and press **Touch off at setter**. CoffeeCNC raises Z to clearance if needed, verifies it reached that plane, traverses XY to the taught setter, and probes down within the configured travel. The cycle retracts to the tool-change height and writes the measured Z offset with G10 L1. It applies G43 H and reads back both the table and active compensation.

The tool length is the new latched machine Z minus the reference latch Z plus the reference tool length. Use the normal work touch-off for work Z. The result beside the button includes Z travel after first contact, measured from the first latch to settled Z before retract. Peak excursion during deceleration requires separate observation.

[LinuxCNC probing](https://linuxcnc.org/docs/2.9/html/gcode/g-code.html#gcode:g38), [G10 L1](https://linuxcnc.org/docs/2.9/html/gcode/g-code.html#gcode:g10-l1).

## G0759 inputs and limits

The fixed setter has two normally closed contacts. Contact opens Mesa input 19. Further travel opens input 17, which drops the LinuxCNC external E-stop chain alongside input 16. The Ruby stylus uses input 18 and is tool T99. A fixed-setter cycle must select input 19 alone, including when T99 is loaded. An OR of the inputs would let a Ruby fault look like successful setter contact. The agreed interface is the HAL output `coffeecnc.probe-setter-select` and the input `coffeecnc.probe-setter-selected`, reporting the selector actually in use. Normal routing selects the Ruby input for T99 and the fixed setter for other tools. During calibration and measurement, require confirmed setter selection throughout the cycle. Restore normal routing after motion has stopped. The encoder-backed `coffeecnc.spindle-rpm` supplies measured spindle speed.

The physical profile has placeholder soft limits. The cycle bounds the descent by its configured search travel from the operator's start position. Commission that travel, the probe feed ceiling, permitted contact area and usable overtravel between the two contacts before enabling it. Check the stop and valid contact independently, including disconnected inputs and stopped spindle feedback. Teach XY and trigger-plane Z in CoffeeCNC after homing. Installation and physical tests belong to the mill operator and Claude.

On November 20, 2026, the G0759 operator measured **1.915 mm** from first contact to the overtravel E-stop trip. Use this as the measured separation of the two contacts. Before enabling probing, verify the stopping distance from first contact at the chosen search feed, including detection delay and deceleration, and establish a margin inside that separation.

The external E-stop remains active while the setter is held in overtravel. Set up and verify a machine-side release procedure with spindle and drive power isolated. Keep automatic probing disabled until the operator can clear a held setter safely with the E-stop circuit intact. After recovery, rehome and recalibrate before running another setter cycle. [LinuxCNC external E-stop input](https://linuxcnc.org/docs/2.9/html/man/man1/io.1.html).

Set `FIXED_SETTER = yes` under `[COFFEECNC]` after commissioning. Supply `SETTER_MAX_TRAVEL_MM`, `SETTER_SEARCH_FEED_MM_MIN` and `SETTER_XY_TOLERANCE_MM`, the allowable XY distance from the taught center. The previous `SETTER_MAX_FEED_MM_MIN` key remains a fallback for search feed. These settings use millimetres and millimetres per minute across both unit systems. `SETTER_REFERENCE_TOOL` defaults to 98. An enabled profile requires both probe and spindle-RPM feedback. Set `SETTER_CLEARANCE_MM` to the lift from the reference latch to tool-change height. Total search travel must extend below the reference latch from clearance. With `SETTER_LATCH_FEED_MM_MIN`, the setter backs off by `SETTER_LATCH_BACKOFF_MM` (1 mm by default), confirms release, and latches again at the slower feed inside the original search envelope.

[G0759 setter wiring](https://github.com/tinic/linuxcnc/commit/c0a779c), [Ruby routing](https://github.com/tinic/linuxcnc/commit/bc47f10), [G0759 configuration](https://github.com/tinic/linuxcnc/tree/claude/coffeecnc-g0759/configs/G0759).

## Native operation

Use one native worker and one operation ID for each calibration or measurement. Before dispatch, require a live link, machine power on, stationary and homed axes, a tool matching the confirmation, a stopped spindle with live encoder feedback, an idle controller, supported interpreter state, in-bounds travel and a clear selected setter. Reject cutter compensation, active external offsets and unsupported kinematics. Above the setter, confirm XY within the commissioned contact area before measurement. Take a fresh reading again before each move.

The Z lift and XY traverse run with the same motion permit as probing. A bounded Z-only G38.2 uses the commissioned feed, with feed override and adaptive feed disabled for the move and retract. Verify the modal changes before moving. The worker associates the probe with its exact command and observes a fresh probe cycle before consuming the latch. A new probe clears the previous trip, but the previous trip can remain set during unrelated moves. A no-contact probe can report a completed command together with a LinuxCNC operator error and a cleared trip latch. Therefore a completed MDI command alone never counts as contact. A valid latch must be finite, within the commanded segment and associated with the expected tool, frame and setter selection.

After valid contact, return only Z at a controlled feed to the tool-change height. Wait for the controller to settle and the setter to clear before saving a tool length. An interrupted or failed probe or retract invalidates the result. Use the existing stop actions, keep E-stop available and prevent ordinary commands from competing with the operation. Any loss of the active worker or a controller-state change that invalidates the cycle must stop the motion. The worker restores non-motion modal values only once it has confirmed the controller is live and compatible with the captured setup. Reconnect starts a fresh operation.

LinuxCNC 2.9.4 simulation stopped coordinated motion when the realtime watchdog lowered `motion.enable`. To bring task out of its machine-on state too, gate the same permit into `iocontrol.0.emc-enable-in` alongside the physical E-stop and overtravel inputs. In simulation, releasing this gate after a trip left the machine disabled and the interrupted move did not resume. Machine installation must preserve both physical inputs. The permit is high when `coffeecnc.probe-cycle-active` is low or when both `coffeecnc.probe-cycle-lease` and the existing watchdog output are high. Read the actual permit back on `coffeecnc.probe-motion-permitted`. The worker cannot renew an expired lease, and holds the interlock active until the controller confirms that motion has stopped.

The LinuxCNC checks live under [tool_probe_facts.py](../../backend/tests/sim/tool_probe_facts.py). [fixed_setter_tests.cpp](../../backend/tests/sim/fixed_setter_tests.cpp) exercises teaching through automatic positioning and verified writeback on simulated metric and inch machines. [tools-workflow.test.ts](../../ui/test/tools-workflow.test.ts) exercises the one-button Tools workflow and result. The G0759 requires physical commissioning before enabling the controls.
