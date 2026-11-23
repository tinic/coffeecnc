#!/usr/bin/python3
"""Facts about LinuxCNC's parameter file as a source of the inactive work offsets.

Run with tests/sim/run.sh on the sim profile. Each `fact=` line is one observation; each
`check=` line a requirement CoffeeCNC's reading of the file relies on (NmlPanelSource, the
synch in NmlPort::idle_maintenance). A failed requirement ends the run with exit code 1.
"""

import os
import sys
import time
from pathlib import Path

import linuxcnc


def near(value, expected):
    # The file has six decimals in machine units: 0.000001 in is 0.0000254 mm.
    return abs(value - expected) < 1e-4


def require(condition, description):
    if not condition:
        print(f"failed={description}", flush=True)
        raise SystemExit(1)
    print(f"check={description}", flush=True)


def main():
    ini = Path(sys.argv[2]).resolve()
    run = os.environ.get("COFFEECNC_SIM_RUN")
    if sys.argv[1] != "-ini" or not run or ini != Path(run) / "coffeecnc_sim.ini":
        raise RuntimeError("Use tests/sim/run.sh with the sim profile.")
    var = ini.parent / "sim.var"

    status = linuxcnc.stat()
    command = linuxcnc.command()
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        status.poll()
        if status.linear_units > 0:
            break
        time.sleep(0.01)
    scale = 1 / status.linear_units  # millimetres per machine unit

    errors = linuxcnc.error_channel()

    def done():
        result = command.wait_complete(5)
        if result != linuxcnc.RCS_DONE:
            said = errors.poll()
            raise RuntimeError(f"The controller did not complete the command: rcs={result} "
                               f"error={said[1] if said else 'none'}")

    def idle():
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            status.poll()
            if status.interp_state == linuxcnc.INTERP_IDLE and status.queued_mdi_commands == 0:
                return
            time.sleep(0.005)
        raise RuntimeError("interpreter did not become idle")

    def mdi(block):
        command.mdi(block)
        done()
        idle()

    def parameters():
        values = {}
        for line in var.read_text().splitlines():
            words = line.split()
            if len(words) == 2:
                values[int(words[0])] = float(words[1])
        return values

    def stamp():
        info = var.stat()
        return (info.st_ino, info.st_mtime_ns)

    def lines():
        status.poll()
        return (status.current_line, status.motion_line, status.read_line)

    def synch():
        command.task_plan_synch()
        done()

    def g55_x():
        return parameters()[5241] * scale

    command.state(linuxcnc.STATE_ESTOP_RESET)
    done()
    # E-stop reset: task synchs the interpreter and so writes the file.
    command.state(linuxcnc.STATE_ON)
    done()
    command.home(-1)
    done()
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        status.poll()
        if all(status.homed[:3]):
            break
        time.sleep(0.01)
    require(all(status.homed[:3]), "the simulation is homed")
    command.mode(linuxcnc.MODE_MDI)
    done()
    mdi("G21 G90 G54")

    # 1. G10 L2 on an inactive system: the file does not have it, the block moves a line counter,
    #    and a synch writes the file without moving the counters.
    before_stamp = stamp()
    before_lines = lines()
    mdi("G10 L2 P2 X11 Y12 Z13 R0")
    time.sleep(0.5)
    after_lines = lines()
    print(f"fact=g10_l2 lines_before={before_lines} lines_after={after_lines} "
          f"file_g55_x={g55_x():.6f} rewritten={stamp() != before_stamp}", flush=True)
    require(not near(g55_x(), 11), "the parameter file lacks the last MDI block's G10 L2")
    synch_stamp = stamp()
    t0 = time.monotonic()
    synch()
    print(f"fact=synch_mdi seconds={time.monotonic() - t0:.4f} rewritten={stamp() != synch_stamp} "
          f"lines={lines()}", flush=True)
    require(stamp() != synch_stamp, "task_plan_synch in MDI mode rewrites the file before it is done")
    require(near(g55_x(), 11), "after the synch the file has G55 X")
    require(near(parameters()[5242] * scale, 12) and near(parameters()[5243] * scale, 13),
            "the file has G55 Y and Z in machine units")

    # 2. Rotation: G10 L2 R on an inactive system is parameter 5250.
    mdi("G10 L2 P2 R15")
    synch()
    require(near(parameters()[5250], 15), "G55 rotation is parameter 5250, in degrees")
    require(near(g55_x(), 11), "G10 L2 with R alone keeps X")
    mdi("G10 L2 P2 R0")

    # 3. A touch-off (G10 L20 P0) in another system, then back to G54: no block writes the file;
    #    a synch writes the value of the system that was left.
    mdi("G55")
    status.poll()
    machine_x = status.actual_position[0] * scale
    mdi("G10 L20 P0 X5")
    status.poll()
    live_x = status.g5x_offset[0] * scale
    require(abs(live_x - (machine_x - 5)) < 1e-6, "G10 L20 sets the active origin from the position")
    stamp_before = stamp()
    mdi("G54")
    print(f"fact=touch_off_then_g54 live_g55_x={live_x:.6f} file_g55_x={g55_x():.6f} "
          f"rewritten={stamp() != stamp_before}", flush=True)
    require(stamp() == stamp_before, "an MDI block does not write the file (no synch before it)")
    synch()
    require(near(g55_x(), live_x), "a synch after G54 writes the G55 touch-off into the file")

    # 4. Manual mode and E-stop: a synch is accepted and writes the file.
    command.mode(linuxcnc.MODE_MANUAL)
    done()
    stamp_before = stamp()
    synch()
    require(stamp() != stamp_before, "task_plan_synch in manual mode rewrites the file")
    command.state(linuxcnc.STATE_ESTOP)
    done()
    stamp_before = stamp()
    synch()
    require(stamp() != stamp_before, "task_plan_synch in E-stop rewrites the file")
    command.state(linuxcnc.STATE_ESTOP_RESET)
    done()
    command.state(linuxcnc.STATE_ON)
    done()

    # 5. A program that writes G56 and ends with M2.
    program = ini.parent / "nc_files" / "wcs_facts.ngc"
    program.parent.mkdir(exist_ok=True)
    program.write_text("G10 L2 P3 X21\nG4 P0.3\nM2\n")
    command.mode(linuxcnc.MODE_AUTO)
    done()
    command.program_open(str(program))
    done()
    before_lines = lines()
    stamp_before = stamp()
    command.auto(linuxcnc.AUTO_RUN, 0)
    done()
    print(f"fact=auto_run_in_auto rewritten={stamp() != stamp_before}", flush=True)
    stamp_before = stamp()
    deadline = time.monotonic() + 5
    saw_running = False
    while time.monotonic() < deadline:
        status.poll()
        if status.interp_state != linuxcnc.INTERP_IDLE:
            saw_running = True
        elif saw_running:
            break
        time.sleep(0.005)
    time.sleep(0.2)
    print(f"fact=program_m2 file_g56_x={parameters()[5261] * scale:.6f} rewritten={stamp() != stamp_before} "
          f"lines_before={before_lines} lines_after={lines()}", flush=True)
    # Not relied on: whether the file has the program's G10 depends on how far the
    # interpreter had read ahead when the run's own synch wrote it.
    require(stamp() == stamp_before, "a program's end (M2) does not write the file")

    # 6. An abort of a running program: task queues a synch of its own.
    idle()
    second = program.with_name("wcs_facts_abort.ngc")
    second.write_text("G10 L2 P4 X31\nG4 P5\nM2\n")
    command.program_open(str(second))
    done()
    # Not waited for: the run is complete when the program ends.
    command.auto(linuxcnc.AUTO_RUN, 0)
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        status.poll()
        if status.interp_state != linuxcnc.INTERP_IDLE:
            break
        time.sleep(0.005)
    require(status.interp_state != linuxcnc.INTERP_IDLE, "the second program runs")
    time.sleep(0.5)
    stamp_before = stamp()
    command.abort()
    done()
    idle()
    time.sleep(0.2)
    print(f"fact=abort file_g57_x={parameters()[5281] * scale:.6f} rewritten={stamp() != stamp_before}", flush=True)
    require(near(parameters()[5281] * scale, 31), "an abort leaves the program's G10 in the file")

    # 7. A change to MDI mode synchs (writes); the block after it is not in the file.
    command.mode(linuxcnc.MODE_MDI)
    done()
    stamp_before = stamp()
    mdi("G10 L2 P5 X41")
    print(f"fact=mode_mdi_then_block rewritten={stamp() != stamp_before}", flush=True)
    require(not near(parameters()[5301] * scale, 41), "the file lacks the block after a mode change")
    print("exit=0", flush=True)


if __name__ == "__main__":
    main()
