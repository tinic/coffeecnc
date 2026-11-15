#!/bin/bash
# The coffeecnc executable's command line and the start-up failures that need no LinuxCNC:
#   cli_tests.sh <coffeecnc> <tests/sim/run.sh> <scratch directory> development|installed-only
# The last word says how the executable was built (COFFEECNC_DEVELOPMENT_PATHS).
# One `check=<name> result=pass|fail detail=...` line per check; exit code 0 only if all pass.
set -u
coffeecnc=$1
runner=$2
work=$3
paths=$4
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

# The executable alone in a prefix, no panel installed beside it. A build for packaging looks
# nowhere else and says where it looked; a development build falls back to the checkout's
# panel and gets as far as the display.
rm -rf "$work/alone"
mkdir -p "$work/alone/bin" && cp "$coffeecnc" "$work/alone/bin/coffeecnc" || exit 125
alone=$(realpath "$work/alone")
coffeecnc=$alone/bin/coffeecnc
run -ini "$work/sim/coffeecnc_sim.ini"
if [ "$paths" = installed-only ]; then
  [ "$code" = 1 ] && said "coffeecnc: No panel in $alone/share/coffeecnc/ui: index.html is missing" &&
    ! said 'coffeecnc: panel='
  check installed_only_build_has_no_checkout_fallback $? "$(detail)"
else
  [ "$code" = 1 ] && said '^coffeecnc: panel=.*/ui/dist preview_helper=' &&
    said 'coffeecnc: Cannot open a display'
  check development_build_falls_back_to_the_checkout $? "$(detail)"
fi
# With a panel installed beside it, that one is used, in either build.
mkdir -p "$alone/share/coffeecnc/ui" && echo '<!doctype html>' >"$alone/share/coffeecnc/ui/index.html"
run -ini "$work/sim/coffeecnc_sim.ini"
[ "$code" = 1 ] && said "^coffeecnc: panel=$alone/share/coffeecnc/ui preview_helper=" &&
  said 'coffeecnc: Cannot open a display'
check installed_panel_beside_the_executable_is_used $? "$(detail)"
# No helper beside it either: a build for packaging says so and names none; a development
# build names the build tree's.
if [ "$paths" = installed-only ]; then
  said ' preview_helper=none$' && said 'coffeecnc: No preview helper: coffeecnc-preview is not beside the executable'
  check installed_only_build_says_the_helper_is_missing $? "$(detail)"
else
  said ' preview_helper=/.*/coffeecnc-preview$' && ! said 'coffeecnc: No preview helper'
  check development_build_falls_back_to_the_built_helper $? "$(detail)"
fi
# With a helper beside the executable, that one is named, in either build.
cp /bin/true "$alone/bin/coffeecnc-preview" || exit 125
run -ini "$work/sim/coffeecnc_sim.ini"
said " preview_helper=$alone/bin/coffeecnc-preview\$" && ! said 'coffeecnc: No preview helper'
check installed_helper_beside_the_executable_is_used $? "$(detail)"

exit $failed
