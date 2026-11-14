#!/bin/bash
# Checks that run.sh never leaves a LinuxCNC behind: after a timeout, and when it is interrupted.
#   runner_tests.sh <build-directory>
# One `check=<name> result=pass|fail detail=...` line per check; exit code 0 only if all pass.
set -u
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
build=$(realpath "${1:?build directory}")
work=$build/sim/runner
mkdir -p "$work" || exit 125
# A DISPLAY program that never finishes on its own.
printf '#!/bin/sh\nsleep 600\n' >"$work/never" && chmod +x "$work/never" || exit 125

failed=0
check() { # name, 0 for pass, detail
  if [ "$2" = 0 ]; then
    echo "check=$1 result=pass detail=$3"
  else
    echo "check=$1 result=fail detail=$3"
    failed=1
  fi
}
gone() {
  ! pgrep -x linuxcncsvr >/dev/null && ! pgrep -x milltask >/dev/null &&
    ! pgrep -x rtapi_app >/dev/null
}

started=$SECONDS
"$here/run.sh" -t 8 -d "$work/timeout" "$work/never" >"$work/timeout.out" 2>&1
code=$?
took=$((SECONDS - started))
[ "$code" = 124 ] && grep -q '^sim=done exit=124 printed_exit=none child_status=none processes_left=0 ' "$work/timeout.out" && gone
check timeout_ends_every_process $? "exit=${code}_seconds=$took"
[ "$took" -lt 90 ]
check timeout_is_bounded $? "seconds=$took"

# Not INT: bash starts background jobs with SIGINT ignored, so it would never arrive.
for signal in TERM HUP; do
  "$here/run.sh" -t 300 -d "$work/interrupt" "$work/never" >"$work/interrupt.out" 2>&1 &
  runner=$!
  for _ in $(seq 100); do
    pgrep -x milltask >/dev/null && break
    sleep 0.2
  done
  started=$SECONDS
  kill -"$signal" "$runner"
  wait "$runner"
  code=$?
  took=$((SECONDS - started))
  [ "$code" = 130 ] && grep -q '^sim=interrupted' "$work/interrupt.out" &&
    grep -q '^sim=done exit=130 printed_exit=none child_status=none processes_left=0 ' "$work/interrupt.out" && gone
  check "interrupt_${signal}_ends_every_process" $? "exit=${code}_seconds=$took"
done

# How the program really ended decides, not the `exit=` line it printed.
summary() { grep '^sim=done' "$1" | cut -d' ' -f2-4 | tr ' ' '_'; }
printf '#!/bin/sh\necho exit=0\nexit 7\n' >"$work/late-failure" && chmod +x "$work/late-failure" || exit 125
"$here/run.sh" -t 60 -d "$work/late" "$work/late-failure" >"$work/late.out" 2>&1
code=$?
[ "$code" = 7 ] && grep -q '^sim=done exit=7 printed_exit=0 child_status=7 processes_left=0 ' "$work/late.out"
check failure_after_printed_exit_0_fails $? "exit=${code}_$(summary "$work/late.out")"

printf '#!/bin/bash\necho exit=0\nkill -SEGV $$\n' >"$work/late-signal" && chmod +x "$work/late-signal" || exit 125
"$here/run.sh" -t 60 -d "$work/signal" "$work/late-signal" >"$work/signal.out" 2>&1
code=$?
[ "$code" = 139 ] && grep -q '^sim=done exit=139 printed_exit=0 child_status=signal_SEGV processes_left=0 ' "$work/signal.out"
check signal_after_printed_exit_0_fails $? "exit=${code}_$(summary "$work/signal.out")"

printf '#!/bin/sh\necho exit=3\nexit 0\n' >"$work/printed-failure" && chmod +x "$work/printed-failure" || exit 125
"$here/run.sh" -t 60 -d "$work/printed" "$work/printed-failure" >"$work/printed.out" 2>&1
code=$?
[ "$code" = 3 ] && grep -q '^sim=done exit=3 printed_exit=3 child_status=0 ' "$work/printed.out"
check printed_failure_with_exit_0_fails $? "exit=${code}_$(summary "$work/printed.out")"

# A program that says nothing: its recorded exit code is the result.
printf '#!/bin/sh\nexit 0\n' >"$work/silent" && chmod +x "$work/silent" || exit 125
"$here/run.sh" -t 60 -d "$work/silent-run" "$work/silent" >"$work/silent.out" 2>&1
code=$?
[ "$code" = 0 ] && grep -q '^sim=done exit=0 printed_exit=none child_status=0 processes_left=0 ' "$work/silent.out"
check silent_program_passes_on_its_exit_code $? "exit=${code}_$(summary "$work/silent.out")"

"$here/run.sh" -d "$work/with space" "$work/never" >"$work/space.out" 2>&1
code=$?
[ "$code" = 125 ] && grep -q 'unsupported-characters' "$work/space.out"
check unsupported_path_is_refused $? "exit=$code"

exit $failed
