#!/bin/bash
# under_xvfb.sh's exit code: every failure it prints is also a nonzero exit, because
# tests/sim/run.sh goes by the exit code and reads no `result=fail` line.
#   wrapper_tests.sh <scratch directory>
# Needs Xvfb, no LinuxCNC. One `check=<name> result=pass|fail detail=...` line per check; exit
# code 0 only if all pass.
set -u
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
work=$1/wrapper
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
script() { # name, body
  printf '#!/bin/bash\n%s\n' "$2" >"$work/$1" && chmod +x "$work/$1" || exit 125
}
# Runs the wrapper as LinuxCNC would; its exit code in $code, its output in $work/out.
wrapper() {
  timeout 60 "$here/under_xvfb.sh" -ini "$work/machine.ini" "$@" >"$work/out" 2>&1
  code=$?
}
said() { grep -q -- "$1" "$work/out"; }
summary() { echo "exit=${code}_$(grep -c 'result=fail' "$work/out")_fail_lines"; }

# Programs: one that ends at once with 0, one that runs until SIGTERM and then exits 0, one
# that ends by itself with 0 after 2 s.
script ends-at-once 'exit 0'
script runs 'trap "exit 0" TERM; sleep 30 & wait'
script ends-soon 'sleep 2; exit 0'
script check-passes 'exit 0'
script check-fails 'exit 3'
script check-outlives-the-program 'sleep 3; exit 0'

wrapper --terminate-after=1 "$work/runs"
[ "$code" = 0 ] && said '^check=program_ran_until_terminated result=pass' && said '^exit=0$'
check program_terminated_passes $? "$(summary)"

wrapper --terminate-after=1 "$work/ends-at-once"
[ "$code" != 0 ] && said '^check=program_ran_until_terminated result=fail' && ! said '^exit=0$'
check program_that_ended_early_with_0_fails $? "$(summary)"

wrapper --terminate-after=1 --check="$work/check-passes" "$work/runs"
[ "$code" = 0 ] && said '^check=program_ran_through_its_checks result=pass'
check program_with_passing_check_passes $? "$(summary)"

wrapper --terminate-after=1 --check="$work/check-fails" "$work/runs"
[ "$code" != 0 ] && said 'checks_exit=3' && ! said '^exit=0$'
check failing_check_fails $? "$(summary)"

wrapper --terminate-after=1 --check="$work/check-outlives-the-program" "$work/ends-soon"
[ "$code" != 0 ] && said '^check=program_ran_through_its_checks result=fail' && ! said '^exit=0$'
check program_that_ended_with_0_during_its_check_fails $? "$(summary)"

wrapper --check="$work/check-passes" "$work/runs"
[ "$code" = 125 ] && said 'check-needs-terminate-after'
check check_without_terminate_after_is_refused $? "$(summary)"

wrapper --terminate-after=1 --check="$work/no-such-script" "$work/runs"
[ "$code" != 0 ] && ! said '^exit=0$'
check missing_check_script_fails $? "$(summary)"

exit $failed
