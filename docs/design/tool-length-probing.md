# Fixed-setter tool-length measurement

The approved first workflow uses the G0759's fixed tool setter. CoffeeCNC will measure from the operator's current XY and move only Z. The application has no automatic probing control yet. The metric and inch LinuxCNC simulations in this PR exercise the underlying motion, contact latch, offset writeback and watchdog.

## Calibration after homing

The G0759 homes in place. Repeatable machine XY and Z across homing cycles would require a repeatable physical reference. The operator teaches the setter during setup after each homing. CoffeeCNC holds the result in memory and invalidates it on a new homing operation on any axis, controller restart or loss of controller status.

1. Load a reference tool with a known length from the spindle datum to the point that contacts the setter.
2. Jog above the center of the setter, within the displayed search travel. **Calibrate setter** shows the reference tool and length, machine XY, maximum downward travel and feed.
3. **Start calibration** makes one bounded Z-only descent and a controlled return to the original Z after a valid trip. **Save calibration** records the starting machine XY and the trigger-plane Z, calculated as the latched machine Z minus the known reference length.

A fixed setter and a plate on the workpiece use different references. The existing **Tool reference height** remains the work-coordinate contact height for manual touch-off. Fixed-setter calibration uses the machine datum. It consumes no work coordinate system and does not reserve G59.3. A move from the current position to the stored setter XY stays manual.

## Measuring a tool

Load the tool to measure, jog above the setter at the taught XY and select **Measure length**. The confirmation names the tool, shows the calibration and displays the effective downward travel and feed. A separate **Start measurement** action authorizes the descent and contact retract.

The controller reports the contact in machine coordinates and machine units. Convert the latch to millimetres and subtract the calibrated trigger-plane Z. For example, a contact at machine Z = 15 mm and a calibrated plane at Z = −35 mm give a length of 50 mm. Work origin, G92/G52 and the applied tool length do not enter the fixed-setter calculation.

After a valid contact and completed retract, show the current table length, measured length and difference. **Apply length** writes just the tool's Z offset with G10 L1 and applies G43 H for that tool. Verify the table row and compensation by reading both back. Dismiss leaves the table unchanged. If the table write completes but applying compensation fails, report that distinction and leave retry to the operator.

[LinuxCNC probing](https://linuxcnc.org/docs/2.9/html/gcode/g-code.html#gcode:g38), [G10 L1](https://linuxcnc.org/docs/2.9/html/gcode/g-code.html#gcode:g10-l1), [NML status](https://linuxcnc.org/docs/2.9/html/config/python-interface.html#_reading_linuxcnc_status).

## G0759 inputs and limits

The fixed setter has two normally closed contacts. Contact opens Mesa input 19. Further travel opens input 17, which drops the LinuxCNC external E-stop chain alongside input 16. The Ruby stylus uses input 18 and is tool T99. A fixed-setter cycle must select input 19 alone, including when T99 is loaded. An OR of the inputs would let a Ruby fault look like successful setter contact. The agreed interface is the HAL output `coffeecnc.probe-setter-select` and the input `coffeecnc.probe-setter-selected`, reporting the selector actually in use. Normal routing selects the Ruby input for T99 and the fixed setter for other tools. During calibration and measurement, require confirmed setter selection throughout the cycle. Restore normal routing after motion has stopped. The encoder-backed `coffeecnc.spindle-rpm` supplies measured spindle speed.

The physical profile has placeholder soft limits. Commission the maximum downward travel, probe feed ceiling, minimum starting clearance, permitted contact area and usable overtravel between the two contacts before enabling automated measurement. Check the stop and valid contact independently, including disconnected inputs and stopped spindle feedback. Teach XY and trigger-plane Z in CoffeeCNC after homing. Installation and physical tests belong to the mill operator and Claude.

[G0759 setter wiring](https://github.com/tinic/linuxcnc/commit/c0a779c), [Ruby routing](https://github.com/tinic/linuxcnc/commit/bc47f10), [G0759 configuration](https://github.com/tinic/linuxcnc/tree/claude/coffeecnc-g0759/configs/G0759).

## Native operation

Use one native worker and one operation ID for each calibration or measurement. Before dispatch, require a live link, machine power on, stationary and homed axes, a tool matching the confirmation, a stopped spindle with live encoder feedback, an idle controller, supported interpreter state, in-bounds travel and a clear selected setter. Reject cutter compensation, active external offsets and unsupported kinematics. Above the setter, confirm XY within the commissioned contact area before measurement. Take a fresh reading again before each move.

An explicitly bounded Z-only G38.2 uses the commissioned feed, with feed override and adaptive feed disabled for the move and retract. Verify the modal changes before moving. The worker associates the probe with its exact command and observes a fresh probe cycle before consuming the latch. A new probe clears the previous trip, but the previous trip can remain set during unrelated moves. A no-contact probe can report a completed command together with a LinuxCNC operator error and a cleared trip latch. Therefore a completed MDI command alone never counts as contact. A valid latch must be finite, within the commanded segment and associated with the expected tool, frame and setter selection.

After valid contact, return only Z at a controlled feed to the captured start position. Wait for the controller to settle and the setter to clear before offering the result. An interrupted or failed probe or retract invalidates the result. Use the existing stop actions, keep E-stop available and prevent ordinary commands from competing with the operation. Any loss of the active worker or a controller-state change that invalidates the cycle must stop the motion. The worker restores non-motion modal values only once it has confirmed the controller is live and compatible with the captured setup. Reconnect starts a fresh operation.

LinuxCNC 2.9.4 simulation stopped coordinated motion when the realtime watchdog lowered `motion.enable`. To bring task out of its machine-on state too, gate the same permit into `iocontrol.0.emc-enable-in` alongside the physical E-stop and overtravel inputs. In simulation, releasing this gate after a trip left the machine disabled and the interrupted move did not resume. Machine installation must preserve both physical inputs. The existing jog watchdog only controls jogs.

The simulation checks live under [tool_probe_facts.py](../../backend/tests/sim/tool_probe_facts.py) and are registered as `linuxcnc_tool_probe_mm` and `linuxcnc_tool_probe_inch`. Physical commissioning and tests through CoffeeCNC's eventual command path are separate from these controller-level checks.
