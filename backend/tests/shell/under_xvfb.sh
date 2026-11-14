#!/bin/bash
# Runs a DISPLAY program of LinuxCNC on an X display of its own (Xvfb, 1280x800): for
# tests/sim/run.sh, which starts LinuxCNC without one.
#
#   run.sh [...] under_xvfb.sh [--terminate-after=<seconds>] <program> [arguments...]
#
# LinuxCNC starts this script as `under_xvfb.sh -ini <file> [--terminate-after=<seconds>]
# <program> [arguments...]`; the program is started as `<program> -ini <file> [arguments...]`.
# With --terminate-after the program is sent SIGTERM after that time and this script prints
# an `exit=<code>` line with the program's exit code; otherwise the program prints it. Either
# way this script exits with the program's exit code. Xvfb is started here and ended here, by
# its process ID.
set -u
[ "${1:-}" = -ini ] && [ $# -ge 3 ] || { echo "exit=125"; exit 125; }
ini=$2
shift 2
terminate_after=
case $1 in
--terminate-after=*)
  terminate_after=${1#--terminate-after=}
  shift
  ;;
esac
program=$1
shift
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
  if kill -0 "$pid" 2>/dev/null; then
    echo "check=program_ran_until_terminated result=pass detail=seconds=$terminate_after"
    kill -TERM "$pid"
  else
    echo "check=program_ran_until_terminated result=fail detail=ended-by-itself"
  fi
  wait "$pid"
  code=$?
  if [ "$code" = 0 ]; then
    echo "check=sigterm_exit_code result=pass detail=0"
  else
    echo "check=sigterm_exit_code result=fail detail=$code"
  fi
  echo "exit=$code"
fi
kill "$xvfb" 2>/dev/null
wait "$xvfb" 2>/dev/null
exit "$code"
