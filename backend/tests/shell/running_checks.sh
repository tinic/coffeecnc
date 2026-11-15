#!/bin/bash
# Checks on a running coffeecnc from outside, for under_xvfb.sh --check:
#   running_checks.sh <process ID> <INI file> <program> [installed]
# The program is the DISPLAY program of a LinuxCNC that tests/sim/run.sh started; its output
# is in linuxcnc.log beside the INI file. Checked:
#   - with `installed`: the process runs <program> itself, has no --ui option and no
#     COFFEECNC_* variable but run.sh's process marker, and says at start-up that its panel is
#     <prefix>/share/coffeecnc/ui and its preview helper <prefix>/bin/coffeecnc-preview, where
#     <prefix> is the directory above the program's `bin`
#   - always: a program this script writes into [DISPLAY] PROGRAM_PREFIX and has LinuxCNC open
#     (another client, as an operator's own tool would) gets a preview the helper computed
#     whole: the line `coffeecnc: preview state=ready ... program=<file>`
# One `check=<name> result=pass|fail detail=...` line per check; exit code 0 only if all pass.
set -u
pid=$1
ini=$2
program=$(realpath "$3")
installed=${4:-}
directory=$(dirname "$ini")
log=$directory/linuxcnc.log
failed=0
check() { # name, 0 for pass, detail
  if [ "$2" = 0 ]; then
    echo "check=$1 result=pass detail=$3"
  else
    echo "check=$1 result=fail detail=$3"
    failed=1
  fi
}
# Waits until the log has a line that matches; up to $2 seconds.
logged() {
  local _
  for _ in $(seq $(($2 * 10))); do
    grep -qE -- "$1" "$log" && return 0
    sleep 0.1
  done
  return 1
}
started=$(grep -m1 '^coffeecnc: panel=' "$log" | tr ' ' '_')
if [ "$installed" = installed ]; then
  prefix=$(dirname "$(dirname "$program")")
  exe=$(readlink "/proc/$pid/exe")
  [ "$exe" = "$program" ]
  check installed_executable_runs $? "exe=$exe"
  variables=$(tr '\0' '\n' <"/proc/$pid/environ" | grep '^COFFEECNC_' | cut -d= -f1 | sort | tr '\n' ',')
  [ "$variables" = COFFEECNC_SIM_RUN, ]
  check no_coffeecnc_setting_in_the_environment $? "variables=${variables:-none}"
  ! tr '\0' '\n' <"/proc/$pid/cmdline" | grep -qx -- --ui
  check no_ui_option $? "arguments=$(tr '\0' ',' <"/proc/$pid/cmdline")"
  grep -qxF "coffeecnc: panel=$prefix/share/coffeecnc/ui preview_helper=$prefix/bin/coffeecnc-preview" "$log"
  check installed_panel_and_helper_are_used $? "${started:-no-start-line}"
else
  [ -n "$started" ]
  check panel_and_helper_named_at_start $? "${started:-no-start-line}"
fi

prefix_key=$(sed -n 's/^PROGRAM_PREFIX *= *//p' "$ini" | head -1)
case $prefix_key in
/*) programs=$prefix_key ;;
*) programs=$directory/$prefix_key ;;
esac
sample=$programs/coffeecnc_running_check.ngc
printf 'G21 G90 G17 G54 G40 G49\nG0 X10 Y0 Z5\nG1 Z-1 F300\nG1 X30 F600\nG2 X40 Y-10 I0 J-10\nG1 Y-20\nG0 Z5\nM2\n' >"$sample"
python3 -c "
import linuxcnc, sys
c = linuxcnc.command()
c.mode(linuxcnc.MODE_AUTO)
c.wait_complete()
c.program_open(sys.argv[1])
sys.exit(0 if c.wait_complete() == linuxcnc.RCS_DONE else 1)" "$sample"
opened=$?
logged "^coffeecnc: preview state=ready segments=[1-9][0-9]* program=$sample\$" 30
ready=$?
[ "$opened" = 0 ] && [ "$ready" = 0 ]
check preview_of_a_sample_program_is_ready $? "opened=${opened}_$(grep '^coffeecnc: preview ' "$log" | tail -1 | tr ' ' '_')"
exit $failed
