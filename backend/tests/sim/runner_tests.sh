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
[ "$code" = 124 ] && grep -q '^sim=done exit=124 processes_left=0 ' "$work/timeout.out" && gone
check timeout_ends_every_process $? "exit=${code}_seconds=$took"
[ "$took" -lt 90 ]
check timeout_is_bounded $? "seconds=$took"

for signal in TERM INT; do
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
    grep -q '^sim=done exit=130 processes_left=0 ' "$work/interrupt.out" && gone
  check "interrupt_${signal}_ends_every_process" $? "exit=${code}_seconds=$took"
done

"$here/run.sh" -d "$work/with space" "$work/never" >"$work/space.out" 2>&1
code=$?
[ "$code" = 125 ] && grep -q 'unsupported-characters' "$work/space.out"
check unsupported_path_is_refused $? "exit=$code"

exit $failed
