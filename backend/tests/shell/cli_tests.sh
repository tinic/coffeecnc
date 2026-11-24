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
[ "$code" = 1 ] && said 'coffeecnc: \[TRAJ\] COORDINATES is "X Z": X, Y and Z are all required'
check unsupported_machine_is_refused $? "$(detail)"

# --check: a report on the INI file, no window and no LinuxCNC (Ferrum-style config check, #166).
run --check "$work/sim/coffeecnc_sim.ini"
[ "$code" = 0 ] && said 'CoffeeCNC starts with this file' && said 'Axes  *X (joint 0), Y (joint 1), Z (joint 2)' &&
  said 'Fixed tool setter  *off' && said 'Manual tool change prompt'
check check_reports_a_good_file $? "$(detail)"
# The sim's post-GUI file sources a fragment that wires the prompt itself.
said 'Manual tool change prompt *on  (wired in the post-GUI HAL file)'
check check_finds_tool_change_wired_in_hal $? "$(grep 'Manual tool change' "$work/out" | tr ' ' '_')"
said 'Reference marks *off  (\[COFFEECNC\] REFERENCE_MARKS = yes'
check check_reports_reference_marks_off $? "$(grep 'Reference marks' "$work/out" | tr ' ' '_')"
said 'Macros *off  (\[COFFEECNC\] MACRO = <label> | <MDI block>)'
check check_reports_macros_off $? "$(grep 'Macros' "$work/out" | tr ' ' '_')"
# Macros (#211): the labels in the check, and a call to a missing subroutine refused.
printf 'o<clamp> sub\no<clamp> endsub\nM2\n' >"$work/sim/nc_files/clamp.ngc"
cp "$work/sim/coffeecnc_sim.ini" "$work/sim/macros.ini" || exit 125
printf '\n[COFFEECNC]\nMACRO = Park | G53 G0 Z0\nMACRO = Clamp check | O<clamp> call\n' >>"$work/sim/macros.ini"
run --check "$work/sim/macros.ini"
[ "$code" = 0 ] && said 'Macros *2  (Park, Clamp check)'
check check_reports_macros $? "$(grep 'Macros' "$work/out" | tr ' ' '_')"
printf 'MACRO = Missing | O<nowhere> call\n' >>"$work/sim/macros.ini"
run --check "$work/sim/macros.ini"
[ "$code" = 1 ] && said 'MACRO "Missing" calls O<nowhere>, but nowhere.ngc is not in'
check check_refuses_macro_without_subroutine $? "$(detail)"
run --check "$work/sim/lathe.ini"
[ "$code" = 1 ] && said 'CoffeeCNC does not start with this file' && said 'COORDINATES'
check check_reports_why_a_file_is_refused $? "$(detail)"

run --write-ini -ini "$work/sim/coffeecnc_sim.ini"
[ "$code" = 1 ] && said 'requires --check'
check write_requires_check $? "$(detail)"
run --check "$work/sim/coffeecnc_sim.ini" --write-ini -ini "$work/sim/lathe.ini"
[ "$code" = 1 ] && said 'Specify one INI file'
check write_refuses_two_ini_paths $? "$(detail)"

repair="$work/sim/repair.ini"
before="$work/sim/repair.before"
mkdir -p "$work/sim/checker" || exit 125
cat >"$work/sim/checker-base.hal" <<'EOF'
source "checker/encoder setup.hal"
EOF
cat >"$work/sim/checker/encoder setup.hal" <<'EOF'
net measured-rpm encoder.scaled-velocity
EOF
cat >"$work/sim/checker-postgui.hal" <<'EOF'
    source "checker/nested post.hal" # quoted path
net measured-rpm => \
    coffeecnc.spindle-rpm
# halcmd -f joins comments too \
net hidden encoder.scaled-velocity => coffeecnc.probe-input
EOF
cat >"$work/sim/checker/nested post.hal" <<'EOF'
# halcmd source parses comments one line at a time \
source coffeecnc_postgui.hal
net at-speed spindle.0.at-speed => coffeecnc.spindle-at-speed
net ruby ruby.out => coffeecnc.probe-input
unlinkp coffeecnc.probe-input
# net ruby ruby.out => coffeecnc.probe-input
# net fake iocontrol.0.tool-change => coffeecnc.tool-change
EOF
# The checker must never execute any directive in either HAL phase.
echo "loadusr /usr/bin/touch $work/would-have-run" >>"$work/sim/checker/nested post.hal"
sed -e '/^DISPLAY *=/d' \
    -e '/^HALFILE *=.*coffeecnc_sim_home.hal$/a HALFILE = checker-base.hal' \
    -e 's/^POSTGUI_HALFILE *=.*/POSTGUI_HALFILE = checker-postgui.hal/' \
    "$work/sim/coffeecnc_sim.ini" >"$repair"
printf '\n[COFFEECNC]\n# Keep this explicit opt-out.\nSPINDLE_AT_SPEED_SIGNAL = no\n' >>"$repair"
chmod 640 "$repair" && cp "$repair" "$before" || exit 125
rm -f "$repair.bak" "$repair.bak.1" "$work/would-have-run"

run --check "$repair"
[ "$code" = 0 ] && cmp -s "$repair" "$before" && [ ! -e "$repair.bak" ] &&
  said '\[DISPLAY\] DISPLAY = coffeecnc' &&
  said '^  \[COFFEECNC\] SPINDLE_RPM_SIGNAL = yes$' &&
  said 'CoffeeCNC HAL .*coffeecnc_postgui.hal' &&
  said 'Manual tool change prompt *off' && [ ! -e "$work/would-have-run" ]
check check_traces_nested_hal_without_running_it $? "$(detail)"

run --check "$repair" --write-ini
[ "$code" = 0 ] && cmp -s "$before" "$repair.bak" && [ "$(stat -c %a "$repair")" = 640 ] &&
  grep -q '^DISPLAY = coffeecnc$' "$repair" &&
  grep -q '^SPINDLE_RPM_SIGNAL = yes$' "$repair" &&
  grep -q '^SPINDLE_AT_SPEED_SIGNAL = no$' "$repair" &&
  grep -q '^# Keep this explicit opt-out\.$' "$repair" &&
  ! grep -q '^PROBE_SIGNAL *=' "$repair" && [ ! -e "$work/would-have-run" ] &&
  said 'Measured spindle speed *on'
check write_adds_only_missing_settings_with_backup $? "$(detail)"

cp "$repair" "$work/sim/repair.after" || exit 125
run --write-ini --check "$repair"
[ "$code" = 0 ] && said 'No INI settings to add' &&
  cmp -s "$repair" "$work/sim/repair.after" && cmp -s "$repair.bak" "$before" && [ ! -e "$repair.bak.1" ]
check repeated_write_is_a_noop $? "$(detail)"

cp "$before" "$repair" || exit 125
printf 'Do not replace this older backup.\n' >"$repair.bak"
run --check "$repair" --write-ini
[ "$code" = 0 ] && grep -qx 'Do not replace this older backup.' "$repair.bak" &&
  cmp -s "$repair.bak.1" "$before"
check write_preserves_earlier_backups $? "$(detail)"

cycle="$work/sim/check-cycle.ini"
sed 's/POSTGUI_HALFILE = checker-postgui.hal/POSTGUI_HALFILE = checker-cycle.hal/' "$before" >"$cycle"
printf 'source checker-cycle.hal\n' >"$work/sim/checker-cycle.hal"
run --check "$cycle"
[ "$code" = 1 ] && said 'HAL source cycle' && ! said '^  \[COFFEECNC\] SPINDLE_RPM_SIGNAL = yes$'
check check_reports_source_cycles $? "$(detail)"

missing="$work/sim/missing-postgui.ini"
sed 's/POSTGUI_HALFILE = checker-postgui.hal/POSTGUI_HALFILE = absent.hal/' "$before" >"$missing"
cp "$missing" "$work/sim/missing-postgui.before" && rm -f "$missing.bak" || exit 125
run --check "$missing" --write-ini
[ "$code" = 1 ] && said 'post-GUI HAL sources could not be loaded' && said 'Cannot read HAL file .*absent.hal' &&
  cmp -s "$missing" "$work/sim/missing-postgui.before" && [ ! -e "$missing.bak" ]
check missing_postgui_blocks_the_write $? "$(detail)"

invalid="$work/sim/check-invalid.ini"
sed 's/^COORDINATES = X Y Z/COORDINATES = X Z/' "$before" >"$invalid"
cp "$invalid" "$work/sim/check-invalid.before" && rm -f "$invalid.bak" || exit 125
run --check "$invalid" --write-ini
[ "$code" = 1 ] && said 'Could not repair the INI: .*COORDINATES' &&
  cmp -s "$invalid" "$work/sim/check-invalid.before" && [ ! -e "$invalid.bak" ]
check invalid_ini_blocks_the_write $? "$(detail)"

locked="$work/sim/check-lock.ini"
cp "$before" "$locked" && rm -f "$locked.bak" || exit 125
exec 9<"$locked"
flock -n 9 || exit 125
run --check "$locked" --write-ini
flock -u 9
exec 9<&-
[ "$code" = 1 ] && said 'could not be locked' &&
  cmp -s "$locked" "$before" && [ ! -e "$locked.bak" ]
check concurrent_ini_write_is_refused $? "$(detail)"

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

# --apply-restore: the helper a restore leaves behind waits for the process it is told (the
# linuxcnc script) and for LinuxCNC to stop, then writes the staged file and keeps the one it
# replaces. The data folder is named by the machine ID the plan carries.
data="$work/restore-data"
rm -rf "$data" && mkdir -p "$data/restore" "$work/restore-config" || exit 125
now=$'T1 P1 Z10 ;now\n'
printf '%s' "$now" >"$work/restore-config/tool.tbl"
echo 'T1 P1 Z48.2 ;backup' >"$data/restore/0.staged"
# FNV-1a, 64 bits, of what the target holds when the restore is prepared (bash wraps at 2^64).
fnv() {
  local hash=-3750763034362895579 i byte
  for ((i = 0; i < ${#1}; i++)); do
    printf -v byte '%d' "'${1:i:1}"
    hash=$(((hash ^ byte) * 1099511628211))
  done
  printf '%016x' "$hash"
}
# Staged now, by page 1: a plan staged more than 2 minutes before the helper starts is refused.
# Read the same boot clock as the restore helper. Containers may virtualize /proc/uptime.
booted=$(python3 -c 'import time; print(time.clock_gettime_ns(time.CLOCK_BOOTTIME) // 1000000)') || exit 125
printf '{"version":4,"machine_id":"restore-data","staged":%s,"boot_id":"%s","booted":%s,"page":1,"source":"x","ini":"","files":[{"role":"tool_table","name":"tool-table/tool.tbl","from":"%s","root":"%s","relative":"tool.tbl","held":"%s"}]}\n' \
  "$(date +%s%3N)" "$(cat /proc/sys/kernel/random/boot_id)" "$booted" \
  "$data/restore/0.staged" "$work/restore-config" "$(fnv "$now")" >"$data/restore/plan.json"
# Review #274, 6: a parent of PID 1 tells nothing about LinuxCNC: refused, the plan is left.
run --apply-restore "$data" --after 1
[ "$code" = 1 ] && said 'coffeecnc: restore=not_written reason=the parent process is 1' &&
  [ -e "$data/restore/plan.json" ] && grep -q ';now' "$work/restore-config/tool.tbl"
check apply_restore_refuses_parent_pid_1 $? "$(detail)"
# The helper also waits for every LinuxCNC on the host: with one running (another checkout's
# simulation) the write is not tested here.
if pgrep -x milltask >/dev/null || pgrep -x rtapi_app >/dev/null || [ -e /tmp/linuxcnc.lock ]; then
  echo "fact=apply_restore_write_not_tested reason=linuxcnc_runs_on_this_host"
else
  sleep 2 &
  waited=$!
  start=$(date +%s)
  run --apply-restore "$data" --after "$waited"
  end=$(date +%s)
  [ "$code" = 0 ] && said 'coffeecnc: restore=written files=1' && [ $((end - start)) -ge 1 ] &&
    grep -q 'Z48.2 ;backup' "$work/restore-config/tool.tbl" && [ ! -e "$data/restore/plan.json" ] &&
    grep -q '"ok":true' "$data/restore/result.json" && ls "$data"/pre-restore-*.tar.gz >/dev/null 2>&1
  check apply_restore_waits_and_writes $? "$(detail)_waited_$((end - start))s"
  run --apply-restore "$data" --after "$waited"
  [ "$code" = 0 ] && said 'coffeecnc: restore=written files=0'
  check apply_restore_without_a_plan_does_nothing $? "$(detail)"
fi
run --apply-restore "$data"
[ "$code" = 1 ] && said 'usage: coffeecnc --apply-restore'
check apply_restore_usage $? "$(detail)"

exit $failed
