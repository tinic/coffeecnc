#!/bin/bash
# The coffeecnc executable's command line and the start-up failures that need no LinuxCNC:
#   cli_tests.sh <coffeecnc> <tests/sim/run.sh> <scratch directory>
# One `check=<name> result=pass|fail detail=...` line per check; exit code 0 only if all pass.
set -u
coffeecnc=$1
runner=$2
work=$3
mkdir -p "$work" || exit 125
failed=0
check() { # name, 0 for pass, detail
  if [ "$2" = 0 ]; then
    echo "check=$1 result=pass detail=$3"
  else
    echo "check=$1 result=fail detail=$3"
    failed=1
  fi
}
# Runs coffeecnc without a display; leaves its exit code in $code and its output in $work/out.
run() {
  env -u DISPLAY -u WAYLAND_DISPLAY timeout 20 "$coffeecnc" "$@" >"$work/out" 2>&1
  code=$?
}
said() { grep -q -- "$1" "$work/out"; }
detail() { echo "exit=${code}_$(head -1 "$work/out" | tr ' ' '_')"; }

run --help
[ "$code" = 0 ] && said '^usage: coffeecnc -ini <file>' && said -- '--windowed' && said -- '--ui'
check help $? "$(detail)"

run
[ "$code" = 1 ] && said 'coffeecnc: No INI file'
check no_ini_is_refused $? "$(detail)"

run -ini
[ "$code" = 1 ] && said 'coffeecnc: -ini needs a value'
check ini_without_value_is_refused $? "$(detail)"

run -ini "$work/x.ini" --frobnicate
[ "$code" = 1 ] && said 'coffeecnc: Unknown option --frobnicate'
check unknown_option_is_refused $? "$(detail)"

run -ini "$work/missing.ini"
[ "$code" = 1 ] && said 'coffeecnc: Cannot read INI file'
check missing_ini_is_reported $? "$(detail)"

# A machine the adapter does not drive: the simulated mill as a lathe.
"$runner" -n -d "$work/sim" /bin/true >/dev/null || exit 125
sed 's/^COORDINATES = X Y Z/COORDINATES = X Z/' "$work/sim/coffeecnc_sim.ini" >"$work/sim/lathe.ini"
run -ini "$work/sim/lathe.ini"
[ "$code" = 1 ] && said 'coffeecnc: \[TRAJ\] COORDINATES is "XZ": only an X Y Z mill is supported'
check unsupported_machine_is_refused $? "$(detail)"

run -ini "$work/sim/coffeecnc_sim.ini" --ui "$work/no-panel"
[ "$code" = 1 ] && said 'coffeecnc: No panel in .*index.html is missing'
check missing_panel_is_reported $? "$(detail)"

mkdir -p "$work/panel" && echo '<!doctype html>' >"$work/panel/index.html"
run -ini "$work/sim/coffeecnc_sim.ini" --ui "$work/panel"
[ "$code" = 1 ] && said 'coffeecnc: Cannot open a display'
check no_display_is_reported $? "$(detail)"

exit $failed
