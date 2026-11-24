#!/usr/bin/python3
"""The DISPLAY program of the first run of restore_tests.sh (review #274).

    restore_display.py -ini <file> <coffeecnc_restore_sim_tests> <coffeecnc> <backup folder>

On the sim profile with a machine ID: sets G54 X 7 and has LinuxCNC write it to the .var
file, backs the machine up, then sets G54 X 12.5 (which LinuxCNC writes only as it stops) and
edits the tool table, and stages a restore of the backup with the helper waiting for the
linuxcnc script. It then exits, and LinuxCNC stops. Output: `check=` lines; exit code 0.
"""

import os
import subprocess
import sys
import time
from pathlib import Path

import linuxcnc


def require(condition, description):
    if not condition:
        print(f"failed={description}", flush=True)
        raise SystemExit(1)
    print(f"check={description}", flush=True)


def parent_of(pid):
    # The fourth field of /proc/<pid>/stat, after the name in parentheses.
    text = Path(f"/proc/{pid}/stat").read_text()
    return int(text[text.rindex(")") + 2:].split()[1])


def main():
    if len(sys.argv) != 6 or sys.argv[1] != "-ini":
        raise RuntimeError("Use tests/sim/restore_tests.sh.")
    ini = Path(sys.argv[2]).resolve()
    program, coffeecnc, folder = sys.argv[3], sys.argv[4], sys.argv[5]
    var = ini.parent / "sim.var"
    tools = ini.parent / "tool.tbl"

    status = linuxcnc.stat()
    command = linuxcnc.command()
    errors = linuxcnc.error_channel()

    def done():
        result = command.wait_complete(5)
        if result != linuxcnc.RCS_DONE:
            said = errors.poll()
            raise RuntimeError(f"The controller did not complete the command: rcs={result} "
                               f"error={said[1] if said else 'none'}")

    def mdi(block):
        command.mdi(block)
        done()
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            status.poll()
            if status.interp_state == linuxcnc.INTERP_IDLE and status.queued_mdi_commands == 0:
                return
            time.sleep(0.005)
        raise RuntimeError("interpreter did not become idle")

    def g54_x():
        for line in var.read_text().splitlines():
            words = line.split()
            if len(words) == 2 and words[0] == "5221":
                return float(words[1])
        return None

    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        status.poll()
        if status.linear_units > 0:
            break
        time.sleep(0.01)
    command.state(linuxcnc.STATE_ESTOP_RESET)
    done()
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
    mdi("G10 L2 P1 X7")
    command.task_plan_synch()
    done()
    require(g54_x() is not None and abs(g54_x() - 7) < 1e-6, "the .var file has G54 X 7")

    backup = subprocess.run([program, "backup", str(ini), folder], capture_output=True, text=True)
    print(backup.stdout, end="", flush=True)
    require(backup.returncode == 0 and "backup=" in backup.stdout, "the machine is backed up")
    archive = backup.stdout.split("backup=")[1].split()[0]

    # After the backup: an offset LinuxCNC keeps in memory and writes as it stops, and a tool
    # table edited by hand.
    mdi("G10 L2 P1 X12.5")
    print(f"fact=var_g54_x_after_g10 value={g54_x()}", flush=True)
    tools.write_text(tools.read_text() + "T9 P9 Z1 D1 ;added after the backup\n")
    command.state(linuxcnc.STATE_OFF)
    done()

    linuxcnc_script = parent_of(os.getppid())  # display.sh is started by the linuxcnc script
    # Into a file, not a pipe: the helper keeps the descriptors 0, 1 and 2 it is started with,
    # and outlives this program.
    said = ini.parent / "stage.out"
    with open(said, "w") as out:
        staged = subprocess.run([program, "stage", str(ini), archive, str(linuxcnc_script), coffeecnc],
                                stdin=subprocess.DEVNULL, stdout=out)
    text = said.read_text()
    print(text, end="", flush=True)
    require(staged.returncode == 0 and "staged=yes" in text, "the restore is staged")
    require("role=parameters change=at_exit" in text,
            "the .var file is compared when LinuxCNC has stopped")
    require("role=tool_table change=changed" in text, "the tool table changes")
    print(f"fact=archive path={archive}", flush=True)
    print("exit=0", flush=True)


if __name__ == "__main__":
    main()
