#!/usr/bin/python3
"""Check the probe latch and coordinated-motion stop on CoffeeCNC's simulated mill."""

import math
import os
import subprocess
import sys
import time
from pathlib import Path

import hal
import linuxcnc


def require(condition, description):
    if not condition:
        raise RuntimeError(description)
    print(f"check={description}", flush=True)


def hal_command(*args):
    subprocess.run(["halcmd", *args], check=True)


def wait_for(status, predicate, description, tick=lambda: None, seconds=5):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        tick()
        status.poll()
        if predicate():
            return
        time.sleep(0.005)
    raise RuntimeError(description)


def check_latch_and_stop():
    # This test owns the probe and motion-enable pins. Run only on its generated simulator.
    ini = Path(sys.argv[2]).resolve()
    run = os.environ.get("COFFEECNC_SIM_RUN")
    if sys.argv[1] != "-ini" or not run or ini != Path(run) / "coffeecnc_sim.ini":
        raise RuntimeError("Use tests/sim/run.sh with the sim profile.")

    status = linuxcnc.stat()
    command = linuxcnc.command()
    wait_for(status, lambda: status.linear_units > 0, "LinuxCNC status did not come up")
    mm_per_machine_unit = 1 / status.linear_units

    # The probe closes below a fixed Z on the simulated axis. No G-code offset affects it.
    hal_command("loadrt", "wcomp", "names=sim-setter")
    hal_command("addf", "sim-setter", "servo-thread")
    hal_command("setp", "sim-setter.min", str(-100 / mm_per_machine_unit))
    hal_command("setp", "sim-setter.max", str(10 / mm_per_machine_unit))
    hal_command("net", "sim-setter-z", "axis.z.pos-cmd", "sim-setter.in")
    hal_command("net", "sim-setter-tripped", "sim-setter.out", "motion.probe-input")

    def done():
        if command.wait_complete(5) != linuxcnc.RCS_DONE:
            raise RuntimeError("The controller failed to complete the command.")

    def mdi(block):
        command.mdi(block)
        done()

    command.state(linuxcnc.STATE_ESTOP_RESET)
    done()
    command.state(linuxcnc.STATE_ON)
    done()
    # The contact plane is clear during the Z homing sequence.
    hal_command("setp", "sim-setter.max", str(-45 / mm_per_machine_unit))
    command.home(-1)
    done()
    wait_for(status, lambda: all(status.homed[:3]), "simulation failed to home")
    command.mode(linuxcnc.MODE_MDI)
    done()
    mdi("G21 G90 G94 G40 G49 G92.1")
    mdi("M61 Q1")
    mdi("G43 H1")
    mdi("G10 L2 P1 Z-7")
    mdi("G54")
    mdi("G53 G0 Z20")
    mdi("G92 Z12")
    hal_command("setp", "sim-setter.max", str(10 / mm_per_machine_unit))

    # Cross machine and program units to distinguish the NML frame from #5063.
    program_inch = mm_per_machine_unit < 2
    program_scale = 25.4 if program_inch else 1
    mdi(("G20" if program_inch else "G21") + " G91 M49")
    mdi("M52 P0")
    status.poll()
    require(not status.probe_val, "probe starts clear")
    frame_z = (status.g5x_offset[2] + status.g92_offset[2]
               + status.tool_offset[2]) * mm_per_machine_unit
    require(abs(frame_z) > 1, "nonzero combined offset")
    mdi(f"G38.2 Z{-15 / program_scale:.8f} F{240 / program_scale:.8f}")
    status.poll()
    contact_mm = status.probed_position[2] * mm_per_machine_unit
    stop_mm = status.actual_position[2] * mm_per_machine_unit
    print(f"fact=machine_scale={mm_per_machine_unit} program_scale={program_scale} "
          f"combined_offset_mm={frame_z:.6f} contact_mm={contact_mm:.6f} "
          f"stopped_mm={stop_mm:.6f}", flush=True)
    require(status.probe_tripped and not status.probing, "completed probe reports a trip")
    require(math.isclose(contact_mm, 10, abs_tol=0.03), "latch is in machine coordinates and units")
    require(contact_mm - stop_mm > 0.001, "contact latch precedes the stopped position")
    mdi(f"G90 G53 G1 Z{20 / program_scale:.8f} F{240 / program_scale:.8f}")
    status.poll()
    require(not status.probe_val, "successful retract releases the setter")
    require(status.probe_tripped, "the old trip persists through retract")

    # With a previously calibrated setter plane, a length comes from the latched machine Z.
    # Use a fictitious plane 52 mm below the simulation's trigger to exercise a changed Z offset.
    setter_z = -42
    measured_length = contact_mm - setter_z
    previous_row = next(tool for tool in status.tool_table if tool.id == 1)
    mdi(f"G10 L1 P1 Z{measured_length / program_scale:.8f}")
    mdi("G43 H1")
    status.poll()
    updated_row = next(tool for tool in status.tool_table if tool.id == 1)
    require(math.isclose(updated_row.zoffset * mm_per_machine_unit, measured_length, abs_tol=0.001)
            and math.isclose(status.tool_offset[2] * mm_per_machine_unit, measured_length, abs_tol=0.001),
            "apply uses the latch after retract and reads back table and compensation")
    require(previous_row.diameter == updated_row.diameter, "apply preserves the tool diameter")

    errors = linuxcnc.error_channel()
    command.mdi(f"G91 G38.2 Z{-5 / program_scale:.8f} F{240 / program_scale:.8f}")
    result = command.wait_complete(5)
    require(result in (linuxcnc.RCS_DONE, linuxcnc.RCS_ERROR), "no-contact move ended")

    def missed_contact():
        error = errors.poll()
        return error is not None and "without making contact" in error[1]

    wait_for(status, missed_contact, "missing no-contact error text")
    print(f"fact=no_contact_rcs={result}", flush=True)
    status.poll()
    require(not status.probe_tripped, "a new probe clears the earlier trip")
    row_after_miss = next(tool for tool in status.tool_table if tool.id == 1)
    require(updated_row == row_after_miss, "missed contact leaves the tool table unchanged")
    mdi(f"G90 G53 G0 Z{20 / program_scale:.8f}")

    # Simulate a live probe heartbeat, then stop renewing it during a coordinated descent.
    # The servo thread drops motion enable and the simulated E-stop chain together. The latter
    # latches the interruption at the task level even if the watchdog path recovers.
    component = hal.component("sim-probe-client")
    component.newpin("active", hal.HAL_BIT, hal.HAL_OUT)
    component.newpin("heartbeat", hal.HAL_BIT, hal.HAL_OUT)
    component.ready()
    hal_command("loadrt", "watchdog", "num_inputs=1")
    hal_command("loadrt", "lut5", "names=sim-probe-permit,sim-probe-estop")
    hal_command("addf", "watchdog.process", "servo-thread")
    hal_command("addf", "watchdog.set-timeouts", "servo-thread")
    hal_command("addf", "sim-probe-permit", "servo-thread")
    hal_command("addf", "sim-probe-estop", "servo-thread")
    hal_command("setp", "watchdog.timeout-0", "0.3")
    # Permit = watchdog healthy OR cycle inactive (inputs 0 and 1).
    hal_command("setp", "sim-probe-permit.function", "0xB")
    hal_command("setp", "sim-probe-estop.function", "0x8")
    hal_command("net", "sim-probe-beat", "sim-probe-client.heartbeat", "watchdog.input-0")
    hal_command("net", "sim-probe-active", "sim-probe-client.active", "watchdog.enable-in", "sim-probe-permit.in-1")
    hal_command("net", "sim-probe-healthy", "watchdog.ok-out", "sim-probe-permit.in-0")
    hal_command("net", "sim-probe-permitted", "sim-probe-permit.out", "motion.enable", "sim-probe-estop.in-0")
    hal_command("unlinkp", "iocontrol.0.emc-enable-in")
    hal_command("net", "estop-loop", "sim-probe-estop.in-1")
    hal_command("net", "sim-probe-estop-chain", "sim-probe-estop.out", "iocontrol.0.emc-enable-in")

    def beat():
        component["heartbeat"] = not component["heartbeat"]

    def arm_watchdog():
        beat()
        component["active"] = True

    def expire_watchdog(phase):
        wait_for(status, lambda: status.task_state == linuxcnc.STATE_ESTOP and not status.enabled,
                 f"watchdog did not stop {phase} and assert E-stop")
        stop_z = status.actual_position[2]
        component["active"] = False
        time.sleep(0.25)
        status.poll()
        print(f"fact=phase={phase} after_release_state={status.task_state} "
              f"enabled={status.enabled} z_mm={stop_z * mm_per_machine_unit:.6f}", flush=True)
        require(status.task_state != linuxcnc.STATE_ON and not status.enabled,
                f"releasing the watchdog after {phase} leaves motion disabled")
        require(math.isclose(status.actual_position[2], stop_z, abs_tol=0.01 / mm_per_machine_unit),
                f"the interrupted {phase} does not resume")
        return stop_z * mm_per_machine_unit

    feed_word = f"F{240 / program_scale:.8f}"
    probe_block = f"G91 G38.2 Z{-15 / program_scale:.8f} {feed_word}"
    arm_watchdog()
    command.mdi(probe_block)
    wait_for(status, lambda: status.probing, "probe failed to start under watchdog", tick=beat)
    require(expire_watchdog("descent") > 10, "watchdog stopped the descent before contact")

    # Only the simulator resets power here. Start again clear, reach contact with a healthy
    # heartbeat, then let the watchdog expire during the upward G53 return.
    command.state(linuxcnc.STATE_ESTOP_RESET)
    done()
    command.state(linuxcnc.STATE_ON)
    done()
    command.mode(linuxcnc.MODE_MDI)
    done()
    mdi(("G20" if program_inch else "G21") + " G90 G94 M49")
    mdi("M52 P0")
    mdi(f"G53 G0 Z{20 / program_scale:.8f}")
    arm_watchdog()
    command.mdi(probe_block)
    wait_for(status, lambda: status.probing, "second probe did not start", tick=beat)
    wait_for(status, lambda: status.probe_tripped and not status.probing and status.inpos
             and status.interp_state == linuxcnc.INTERP_IDLE,
             "second probe did not finish at contact", tick=beat)
    command.mdi(f"G90 G53 G1 Z{20 / program_scale:.8f} {feed_word}")
    wait_for(status, lambda: status.current_vel > 0, "retract failed to start under watchdog", tick=beat)
    require(10 < expire_watchdog("retract") < 19, "watchdog stopped the retract before its endpoint")
    require(next(tool for tool in status.tool_table if tool.id == 1) == updated_row,
            "interruptions leave the tool table unchanged")


if __name__ == "__main__":
    check_latch_and_stop()
