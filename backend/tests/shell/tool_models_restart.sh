#!/bin/bash
# The tool models through the real panel, across a restart: LinuxCNC and the application are
# run twice in one run directory, on a sim machine that is given an identity. The first run
# saves family defaults and a tool's model through the editor; the second, a new LinuxCNC and
# a new process of the application, must be shown them (coffeecnc_shell_tests tool-models
# [second-run]).
#
#   tool_models_restart.sh <run.sh> <under_xvfb.sh> <coffeecnc_shell_tests> <run-directory>
#
# Output: the check lines of both runs. Exit code: the first run's when it failed, else the
# second's. The first run's LinuxCNC log is kept as first-run.log.
set -u
[ $# -eq 4 ] || { echo "usage: tool_models_restart.sh <run.sh> <under_xvfb.sh> <program> <directory>" >&2; exit 125; }
runner=$1
xvfb=$2
program=$3
directory=$4

"$runner" -u mm -m shell-mill -t 200 -d "$directory" "$xvfb" "$program" tool-models
first=$?
cp "$directory/linuxcnc.log" "$directory/first-run.log" 2>/dev/null
echo "restart=first_run exit=$first"
[ "$first" = 0 ] || exit "$first"
# One LinuxCNC per host: when another user of the simulator took it meanwhile, wait for it,
# bounded; run.sh refuses anyway while one runs.
for _ in $(seq 300); do
  pgrep -x "linuxcncsvr|milltask|rtapi_app" >/dev/null || [ -e /tmp/linuxcnc.lock ] || break
  sleep 1
done
"$runner" -k -u mm -m shell-mill -t 200 -d "$directory" "$xvfb" "$program" tool-models second-run
second=$?
echo "restart=second_run exit=$second"
exit "$second"
