#!/bin/bash
# Checks that run.sh never leaves a LinuxCNC behind: after a timeout, and when it is interrupted.
#   runner_tests.sh <build-directory> [lock wait, seconds]
#
# The host's runner lock: eleven of the runs below (ten calls, one of them made twice) may each
# have to wait for a simulation of another checkout, at most the lock wait given (run.sh -w;
# 300 without one). The two waits on this test's own run are fixed, 3 s and 120 s. The
# caller's time limit is made of these.
# One `check=<name> result=pass|fail detail=...` line per check; exit code 0 only if all pass.
set -u
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
build=$(realpath "${1:?build directory}")
lock_wait=${2:-300}
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
# Everything here is judged on this test's own runs: another checkout may run a simulation of
# its own before, between and after them. A run's processes carry its run directory in their
# environment (COFFEECNC_SIM_RUN, set by run.sh).
of_run() { # run directory, process name: the process IDs of that run with that name
  local pid
  for pid in $(pgrep -u "$(id -u)" ${2:+-x "$2"}); do
    tr '\0' '\n' 2>/dev/null <"/proc/$pid/environ" | grep -qxF "COFFEECNC_SIM_RUN=$1" && echo "$pid"
  done
}
# No process of that run is left. (rtapi_app is setuid root: its environment cannot be read,
# so it cannot be told apart here. The run's own `processes_left=0`, counted while it still
# held the host's lock, is what the checks below read for it.)
gone() { [ -z "$(of_run "$1")" ]; }
# That run's own task has started; it may have had to wait for the host's lock first.
await_task() {
  for _ in $(seq $(((lock_wait + 30) * 5))); do
    [ -z "$(of_run "$1" milltask)" ] || return 0
    sleep 0.2
  done
  return 1
}
# The host's lock is not held by a run of this test.
lock_not_ours() { ! grep -q " run_directory=$work/" "$lock.info" 2>/dev/null; }
lock_directory=$HOME/ai
[ -d "$lock_directory" ] || lock_directory=/tmp
lock=$lock_directory/coffeecnc-sim-runner-$(id -u).lock

"$here/run.sh" -w "$lock_wait" -t 8 -d "$work/timeout" "$work/never" >"$work/timeout.out" 2>&1
code=$?
# From the moment this run had the lock: the time it waited for another checkout is not its.
locked_at=$(sed -n 's/^sim=locked .* at=\([0-9]*\)$/\1/p' "$work/timeout.out" | head -1)
took=$(($(date +%s) - ${locked_at:-0}))
[ "$code" = 124 ] && grep -q '^sim=done exit=124 printed_exit=none child_status=none processes_left=0 ' "$work/timeout.out" && gone "$work/timeout"
check timeout_ends_every_process $? "exit=${code}_seconds=$took"
[ -n "$locked_at" ] && [ "$took" -lt 90 ]
check timeout_is_bounded $? "seconds_since_locked=${took}_locked_at=${locked_at:-none}"

# Not INT: bash starts background jobs with SIGINT ignored, so it would never arrive.
for signal in TERM HUP; do
  "$here/run.sh" -w "$lock_wait" -t 300 -d "$work/interrupt" "$work/never" >"$work/interrupt.out" 2>&1 &
  runner=$!
  await_task "$work/interrupt"
  begun=$?
  started=$SECONDS
  kill -"$signal" "$runner"
  wait "$runner"
  code=$?
  took=$((SECONDS - started))
  [ "$begun" = 0 ] && [ "$code" = 130 ] && grep -q '^sim=interrupted' "$work/interrupt.out" &&
    grep -q '^sim=done exit=130 printed_exit=none child_status=none processes_left=0 ' "$work/interrupt.out" &&
    gone "$work/interrupt"
  check "interrupt_${signal}_ends_every_process" $? "started=${begun}_exit=${code}_seconds=$took"
done

# The host-wide lock: one simulation at a time, whatever checkout starts it.
lock_not_ours
check lock_is_free_after_timeout_and_interrupts $? "lock=$lock"
printf '#!/bin/sh\nexit 0\n' >"$work/quiet" && chmod +x "$work/quiet" || exit 125
rm -rf "$work/refused" "$work/waiter"
"$here/run.sh" -w "$lock_wait" -t 300 -d "$work/holder" "$work/never" >"$work/holder.out" 2>&1 &
holder=$!
await_task "$work/holder"
check lock_holder_started $? "run=$work/holder"
# Assembly only starts nothing and waits for nobody.
started=$SECONDS
"$here/run.sh" -n -d "$work/assembled" "$work/quiet" >"$work/assembled.out" 2>&1
code=$?
[ "$code" = 0 ] && grep -q '^sim=assembled ' "$work/assembled.out" && [ $((SECONDS - started)) -lt 5 ]
check assembly_only_takes_no_lock $? "exit=${code}_seconds=$((SECONDS - started))"
# A second run waits its bound, then is refused: it names the holder and has started and
# written nothing.
started=$SECONDS
"$here/run.sh" -w 3 -t 60 -d "$work/refused" "$work/quiet" >"$work/refused.out" 2>&1
code=$?
took=$((SECONDS - started))
[ "$code" = 123 ] && grep -q "^sim=waiting reason=locked .*holder_pid=$holder " "$work/refused.out" &&
  grep -q "^sim=refused reason=locked .*waited_seconds=3 holder_pid=$holder .*holder_run_directory=$work/holder " "$work/refused.out" &&
  [ ! -e "$work/refused" ] && [ "$took" -ge 3 ] && [ "$took" -lt 30 ] && kill -0 "$holder"
check bounded_wait_ends_in_a_refusal $? "exit=${code}_seconds=${took}_$(grep -c . "$work/refused.out")_lines"
# A second run with time to wait runs once the first has ended, here by SIGTERM.
"$here/run.sh" -w 120 -t 60 -d "$work/waiter" "$work/quiet" >"$work/waiter.out" 2>&1 &
waiter=$!
sleep 3
grep -q '^sim=waiting reason=locked ' "$work/waiter.out" && [ ! -e "$work/waiter" ] && kill -0 "$waiter"
waiting=$?
kill -TERM "$holder"
wait "$holder"
held=$?
wait "$waiter"
code=$?
[ "$waiting" = 0 ] && [ "$held" = 130 ] &&
  grep -q '^sim=done exit=130 .* processes_left=0 ' "$work/holder.out" && [ "$code" = 0 ] &&
  grep -q '^sim=done exit=0 printed_exit=none child_status=0 processes_left=0 ' "$work/waiter.out"
check second_run_waits_then_runs $? "waiting=${waiting}_holder_exit=${held}_waiter_exit=$code"
lock_not_ours && gone "$work/holder" && gone "$work/waiter"
check lock_is_released_after_the_runs $? "info=$(cat "$lock.info" 2>/dev/null | tr ' ' '_')"

# How the program really ended decides, not the `exit=` line it printed.
summary() { grep '^sim=done' "$1" | cut -d' ' -f2-4 | tr ' ' '_'; }
printf '#!/bin/sh\necho exit=0\nexit 7\n' >"$work/late-failure" && chmod +x "$work/late-failure" || exit 125
"$here/run.sh" -w "$lock_wait" -t 60 -d "$work/late" "$work/late-failure" >"$work/late.out" 2>&1
code=$?
[ "$code" = 7 ] && grep -q '^sim=done exit=7 printed_exit=0 child_status=7 processes_left=0 ' "$work/late.out"
check failure_after_printed_exit_0_fails $? "exit=${code}_$(summary "$work/late.out")"

printf '#!/bin/bash\necho exit=0\nkill -SEGV $$\n' >"$work/late-signal" && chmod +x "$work/late-signal" || exit 125
"$here/run.sh" -w "$lock_wait" -t 60 -d "$work/signal" "$work/late-signal" >"$work/signal.out" 2>&1
code=$?
[ "$code" = 139 ] && grep -q '^sim=done exit=139 printed_exit=0 child_status=signal_SEGV processes_left=0 ' "$work/signal.out"
check signal_after_printed_exit_0_fails $? "exit=${code}_$(summary "$work/signal.out")"

printf '#!/bin/sh\necho exit=3\nexit 0\n' >"$work/printed-failure" && chmod +x "$work/printed-failure" || exit 125
"$here/run.sh" -w "$lock_wait" -t 60 -d "$work/printed" "$work/printed-failure" >"$work/printed.out" 2>&1
code=$?
[ "$code" = 3 ] && grep -q '^sim=done exit=3 printed_exit=3 child_status=0 ' "$work/printed.out"
check printed_failure_with_exit_0_fails $? "exit=${code}_$(summary "$work/printed.out")"

# A program that says nothing: its recorded exit code is the result.
printf '#!/bin/sh\nexit 0\n' >"$work/silent" && chmod +x "$work/silent" || exit 125
"$here/run.sh" -w "$lock_wait" -t 60 -d "$work/silent-run" "$work/silent" >"$work/silent.out" 2>&1
code=$?
[ "$code" = 0 ] && grep -q '^sim=done exit=0 printed_exit=none child_status=0 processes_left=0 ' "$work/silent.out"
check silent_program_passes_on_its_exit_code $? "exit=${code}_$(summary "$work/silent.out")"

# Arguments longer than an INI line (255 characters in LinuxCNC) reach the program whole.
printf '#!/bin/sh\necho "arguments=$*"\nexit 0\n' >"$work/arguments" && chmod +x "$work/arguments" || exit 125
long=$(printf 'argument-%03d,' $(seq 40))
"$here/run.sh" -w "$lock_wait" -t 60 -d "$work/arguments-run" "$work/arguments" first "$long" last >"$work/arguments.out" 2>&1
code=$?
[ "$code" = 0 ] && grep -qxF "arguments=-ini $work/arguments-run/coffeecnc_sim.ini first $long last" "$work/arguments-run/linuxcnc.log"
check long_arguments_reach_the_program $? "exit=${code}_characters=${#long}"

# The loaded tool table is the run's own: an rs274 started with the caller's HOME while the run
# is going makes $HOME/.tool.mmap again with its own table (LinuxCNC's sim.tbl: T1 Z0.511
# D0.125, T2 Z0.1), and must not replace the run's (T1 Z50, T2 Z40). A caller HOME of the test's
# own, so that no other checkout's simulation is touched; its ai/ is the host's lock directory.
caller_home=$work/caller-home
rm -rf "$caller_home" && mkdir -p "$caller_home" || exit 125
ln -s "$lock_directory" "$caller_home/ai" || exit 125
cat >"$work/foreign-table" <<'EOF' || exit 125
#!/bin/bash
# foreign-table -ini <file> <caller HOME>
cd "$(dirname "$2")" || exit 1
printf 'T1 P1 D0.125 Z0.511\nT2 P2 D0.0625 Z0.1\n' >foreign.tbl
printf 'M2\n' >foreign.ngc
HOME=$3 rs274 -t foreign.tbl -g foreign.ngc >/dev/null 2>&1 </dev/null
python3 -c '
import linuxcnc, os
s = linuxcnc.stat()
s.poll()
tools = {t.id: t.zoffset for t in s.tool_table if t.id > 0}
print("tools=" + ",".join("T%d:%g" % (n, tools[n]) for n in sorted(tools)))
print("home=" + os.environ["HOME"])
'
EOF
chmod +x "$work/foreign-table" || exit 125
HOME=$caller_home "$here/run.sh" -w "$lock_wait" -t 60 -d "$work/foreign" "$work/foreign-table" \
  "$caller_home" >"$work/foreign.out" 2>&1
code=$?
[ "$code" = 0 ] && grep -qxF "tools=T1:50,T2:40" "$work/foreign/linuxcnc.log" &&
  grep -qxF "home=$work/foreign/home" "$work/foreign/linuxcnc.log" && [ -e "$caller_home/.tool.mmap" ]
check foreign_rs274_keeps_the_run_tool_table $? \
  "exit=${code}_$(grep -E '^(tools|home)=' "$work/foreign/linuxcnc.log" | tr '\n' '_')"
rm -rf "$caller_home"

"$here/run.sh" -w "$lock_wait" -d "$work/with space" "$work/never" >"$work/space.out" 2>&1
code=$?
[ "$code" = 125 ] && grep -q 'unsupported-characters' "$work/space.out"
check unsupported_path_is_refused $? "exit=$code"

exit $failed
