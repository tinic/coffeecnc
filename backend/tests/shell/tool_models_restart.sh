#!/bin/bash
# The tool models through the real panel, across a restart: LinuxCNC and the application are
# run twice in one run directory, on a sim machine that is given an identity. The first run
# saves family defaults and a tool's model through the editor; the second, a new LinuxCNC and
# a new process of the application, must be shown them (coffeecnc_shell_tests tool-models
# [second-run]).
#
#   tool_models_restart.sh <run.sh> <under_xvfb.sh> <coffeecnc_shell_tests> <run-directory>
#     <lock wait, seconds> <run time, seconds>
#
# Each of the two runs may wait that long for the host's runner lock (run.sh -w) and run that
# long (run.sh -t): the caller's own time limit has to hold both, twice.
#
# Output: the check lines of both runs. Exit code: the first run's when it failed, else the
# second's. The first run's LinuxCNC log is kept as first-run.log.
set -u
[ $# -eq 6 ] || { echo "usage: tool_models_restart.sh <run.sh> <under_xvfb.sh> <program> <directory> <lock wait> <run time>" >&2; exit 125; }
runner=$1
xvfb=$2
program=$3
directory=$4
lock_wait=$5
run_time=$6

"$runner" -u mm -i shell-mill -w "$lock_wait" -t "$run_time" -d "$directory" "$xvfb" "$program" tool-models
first=$?
cp "$directory/linuxcnc.log" "$directory/first-run.log" 2>/dev/null
echo "restart=first_run exit=$first"
[ "$first" = 0 ] || exit "$first"
# run.sh takes the host's runner lock for each run and waits for it when another checkout
# holds it in between.
"$runner" -k -u mm -i shell-mill -w "$lock_wait" -t "$run_time" -d "$directory" "$xvfb" "$program" tool-models second-run
second=$?
echo "restart=second_run exit=$second"
exit "$second"
