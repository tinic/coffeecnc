#!/bin/bash
# Runs a DISPLAY program of LinuxCNC on an X display of its own (Xvfb, 1280x800): for
# tests/sim/run.sh, which starts LinuxCNC without one.
#
#   run.sh [...] under_xvfb.sh [--terminate-after=<seconds>] [--check=<script>[,<word>]]
#     <program> [arguments...]
#
# LinuxCNC starts this script as `under_xvfb.sh -ini <file> [options] <program>
# [arguments...]`; the program is started as `<program> -ini <file> [arguments...]`.
# With --terminate-after the program is sent SIGTERM after that time and this script prints
# an `exit=<code>` line with the program's exit code; otherwise the program prints it. Either
# way this script exits with the program's exit code. With --check (and --terminate-after) the
# script is run as `<script> <process ID> <INI file> <program> [<word>]` while the program
# still runs, just before the SIGTERM. --check without --terminate-after is refused (125).
#
# tests/sim/run.sh goes by this script's exit code and reads no `result=fail` line. So every
# failure printed here is a nonzero exit code too: 1 where the program's own was 0, for a
# program that ended by itself before the SIGTERM, a check script that failed, and a program
# that was gone when its check script returned. Then the `exit=` line carries 1 as well.
# Xvfb is started here and ended here, by its process ID.
set -u
[ "${1:-}" = -ini ] && [ $# -ge 3 ] || { echo "exit=125"; exit 125; }
ini=$2
shift 2
terminate_after=
check=
while :; do
  case $1 in
  --terminate-after=*) terminate_after=${1#--terminate-after=} ;;
  --check=*) check=${1#--check=} ;;
  *) break ;;
  esac
  shift
done
program=$1
shift
if [ -n "$check" ] && [ -z "$terminate_after" ]; then
  echo "check=options result=fail detail=check-needs-terminate-after"
  echo "exit=125"
  exit 125
fi
directory=$(dirname "$ini")

rm -f "$directory/xvfb.display"
Xvfb -displayfd 3 -screen 0 1280x800x24 -nolisten tcp 3>"$directory/xvfb.display" \
  >"$directory/xvfb.log" 2>&1 &
xvfb=$!
for _ in $(seq 100); do
  [ -s "$directory/xvfb.display" ] && break
  kill -0 "$xvfb" 2>/dev/null || break
  sleep 0.1
done
if ! [ -s "$directory/xvfb.display" ]; then
  echo "check=xvfb_started result=fail detail=$(tr ' \n' '__' <"$directory/xvfb.log")"
  kill "$xvfb" 2>/dev/null
  echo "exit=125"
  exit 125
fi
export DISPLAY=:$(cat "$directory/xvfb.display")
# No accessibility bridge on this display: nothing here reads the screen, and GTK's AT-SPI
# bridge (atk_bridge_adaptor_init) opens a D-Bus connection whose messages libdbus still holds
# at exit, which LeakSanitizer reports (4588b3b: 800 to 1216 bytes in 4 to 6 allocations, full
# stacks in build/sim-history/4588b3b/asan-dbus/). This is the test environment's setting only:
# the coffeecnc executable does not set it.
export NO_AT_BRIDGE=1

if [ -z "$terminate_after" ]; then
  # The program prints its own `exit=` line. How the process really ends is this script's
  # exit code, which run.sh records and goes by.
  "$program" -ini "$ini" "$@"
  code=$?
else
  "$program" -ini "$ini" "$@" &
  pid=$!
  sleep "$terminate_after"
  checked=0
  lifecycle=0
  if kill -0 "$pid" 2>/dev/null; then
    echo "check=program_ran_until_terminated result=pass detail=seconds=$terminate_after"
    if [ -n "$check" ]; then
      # shellcheck disable=SC2086  # <script>,<word>: two words on purpose.
      "${check%%,*}" "$pid" "$ini" "$program" $(case $check in *,*) echo "${check#*,}" ;; esac)
      checked=$?
      if kill -0 "$pid" 2>/dev/null; then
        echo "check=program_ran_through_its_checks result=pass detail=checks_exit=$checked"
      else
        echo "check=program_ran_through_its_checks result=fail detail=ended-by-itself_checks_exit=$checked"
        lifecycle=1
      fi
      [ "$checked" = 0 ] || echo "check=checks_passed result=fail detail=checks_exit=$checked"
    fi
    kill -TERM "$pid"
  else
    echo "check=program_ran_until_terminated result=fail detail=ended-by-itself"
    lifecycle=1
  fi
  wait "$pid"
  code=$?
  if [ "$code" = 0 ]; then
    echo "check=sigterm_exit_code result=pass detail=0"
  else
    echo "check=sigterm_exit_code result=fail detail=$code"
  fi
  # A failure printed above with a program that exited 0 is still a failure of this script.
  if [ "$code" = 0 ] && { [ "$checked" != 0 ] || [ "$lifecycle" != 0 ]; }; then
    code=1
  fi
  echo "exit=$code"
fi
kill "$xvfb" 2>/dev/null
wait "$xvfb" 2>/dev/null
exit "$code"
