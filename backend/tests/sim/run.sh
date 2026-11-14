#!/bin/bash
# Runs a program as the DISPLAY program of a real LinuxCNC on CoffeeCNC's simulated mill.
# No hardware is involved: the HAL configuration loops commanded positions back as feedback.
#
#   run.sh [-p sim|g0759] [-u mm|inch] [-t seconds] [-d run-directory] [-n] <program> [arguments...]
#
# Profiles (-p):
#   sim    the test mill of coffeecnc_sim.ini.in, in millimetres or inches (-u); home switches
#   g0759  a mill with the G0759's INI values, coffeecnc_g0759.ini.in; millimetres only
#
# LinuxCNC starts <program> as `<program> -ini <run-directory>/coffeecnc_<profile>.ini
# [arguments...]` and shuts down when it exits. This script assembles the run directory, waits for
# the program with a hard timeout, makes sure every process it started is gone, and exits with the
# program's exit code (124 after a timeout, 125 when LinuxCNC could not be run or did not shut
# down). With -n it only assembles the run directory, prints `sim=assembled ini=<file>` and
# starts nothing.
#
# Output is the program's output followed by `sim=<key> ...` lines of this script.
set -u

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
profile=sim
units=mm
timeout=300
directory=
assemble_only=
while getopts "p:u:t:d:n" option; do
  case $option in
  p) profile=$OPTARG ;;
  u) units=$OPTARG ;;
  t) timeout=$OPTARG ;;
  d) directory=$OPTARG ;;
  n) assemble_only=1 ;;
  *) exit 125 ;;
  esac
done
shift $((OPTIND - 1))
if [ $# -lt 1 ] || { [ "$units" != mm ] && [ "$units" != inch ]; } ||
  { [ "$profile" != sim ] && [ "$profile" != g0759 ]; } ||
  { [ "$profile" = g0759 ] && [ "$units" != mm ]; }; then
  echo "usage: run.sh [-p sim|g0759] [-u mm|inch] [-t seconds] [-d run-directory] [-n]" \
    "<program> [arguments...]  (g0759 is millimetres only)" >&2
  exit 125
fi
program=$(realpath "$1") || exit 125
shift
# LinuxCNC splits [DISPLAY] DISPLAY at white space and its scripts pass paths through the shell,
# so only plain path characters can work. Refuse anything else instead of failing later.
plain() {
  case $1 in
  *[!A-Za-z0-9_./+:=,@%-]*) return 1 ;;
  esac
}
if [ "$profile" = sim ]; then
  flavour=$units
  fragments="coffeecnc_postgui.hal coffeecnc_sim_home.hal"
else
  flavour=$profile
  fragments="coffeecnc_postgui.hal coffeecnc_${profile}_postgui.hal coffeecnc_${profile}_sim.hal"
fi
ini=coffeecnc_$profile.ini
[ -n "$directory" ] || directory=$here/../../build/sim/$(basename "$program")-$flavour
mkdir -p "$directory/nc_files" || exit 125
directory=$(realpath "$directory")
for word in "$program" "$directory" "$@"; do
  if ! plain "$word"; then
    echo "sim=error detail=unsupported-characters-in-path-or-argument"
    exit 125
  fi
done

# The run directory holds only generated files.
perl -pe '
  BEGIN { $inch = shift eq "inch"; $display = shift; }
  s/\@UNITS\@/$inch ? "inch" : "mm"/ge;
  s/\@DISPLAY\@/$display/g;
  s/\@L:([-0-9.]+)\@/$inch ? sprintf("%.6f", $1 \/ 25.4) : $1/ge;
' "$units" "$program $*" "$here/coffeecnc_$profile.ini.in" >"$directory/$ini" || exit 125
for fragment in $fragments; do
  cp "$here/$fragment" "$directory/" || exit 125
done
rm -f "$directory/sim.var" "$directory/sim.var.bak" "$directory/linuxcnc.log"
# Tool table in machine units: T number, P pocket, Z length, D diameter.
if [ "$profile" = g0759 ]; then
  # As the machine's table: no length, no diameter, no description.
  printf 'T0 P0 ;\nT1 P1 ;\nT2 P2 ;\nT3 P3 ;\n'
elif [ "$units" = inch ]; then
  printf 'T1 P1 Z2.0 D0.25 ;quarter inch end mill\nT2 P2 Z1.5 D0.125 ;eighth inch end mill\n'
else
  printf 'T1 P1 Z50 D6 ;6 mm end mill\nT2 P2 Z40 D3 ;3 mm end mill\n'
fi >"$directory/tool.tbl"
if [ -n "$assemble_only" ]; then
  echo "sim=assembled ini=$directory/$ini"
  exit 0
fi

command -v linuxcnc >/dev/null || { echo "sim=error detail=linuxcnc-not-installed"; exit 125; }

# One LinuxCNC per host: instances share the NML and HAL shared memory segments.
for name in linuxcncsvr milltask rtapi_app; do
  if pgrep -x "$name" >/dev/null; then
    echo "sim=error detail=another-linuxcnc-is-running process=$name"
    exit 125
  fi
done
if [ -e /tmp/linuxcnc.lock ]; then
  echo "sim=error detail=linuxcnc-lock-file-exists file=/tmp/linuxcnc.lock"
  exit 125
fi

# Every process of this run carries the marker in its environment, however it detaches.
export COFFEECNC_SIM_RUN="$directory"
ours() {
  local pid
  for pid in $(pgrep -u "$(id -u)"); do
    [ "$pid" != $$ ] && [ "$pid" != "$BASHPID" ] &&
      tr "\\0" "\\n" 2>/dev/null <"/proc/$pid/environ" |
      grep -qxF "COFFEECNC_SIM_RUN=$directory" && echo "$pid"
  done
}

linuxcnc_pid=
# Ends the run: every process it started is gone when this returns, however it is reached
# (program finished, timeout, or this script interrupted). Bounded: it never waits on a process
# that may survive.
finish() {
  trap - INT TERM HUP
  local status=$1 left
  if [ -n "$linuxcnc_pid" ] && kill -0 "$linuxcnc_pid" 2>/dev/null; then
    # LinuxCNC's start script shuts everything down on SIGTERM.
    kill -TERM "$linuxcnc_pid" 2>/dev/null
    for _ in $(seq 150); do
      kill -0 "$linuxcnc_pid" 2>/dev/null || break
      sleep 0.2
    done
  fi
  # Whatever is left was not shut down properly: end it, then say so.
  left=$(ours)
  if [ -n "$left" ]; then
    echo "sim=leftover pids=$(echo $left | tr ' ' ',') names=$(ps -o comm= -p $left | tr '\n' ',')"
    kill -TERM $left 2>/dev/null
    sleep 2
    left=$(ours)
    [ -z "$left" ] || kill -KILL $left 2>/dev/null
    sleep 1
    rm -f /tmp/linuxcnc.lock
    [ "$status" != 0 ] || status=125
  fi
  # rtapi_app is setuid root: its environment cannot be read, so it is never in `ours`, and it
  # cannot be signalled. No other LinuxCNC was running when this run started, so one that is
  # still here belongs to it; halrun -U unloads the realtime side.
  if pgrep -x rtapi_app >/dev/null; then
    echo "sim=leftover names=rtapi_app"
    # Bounded: TERM after 20 s, KILL 5 s later. If rtapi_app survives it is counted below.
    timeout -k 5 20 halrun -U >/dev/null 2>&1
    sleep 1
    [ "$status" != 0 ] || status=125
  fi
  left="$(ours) $(pgrep -x rtapi_app)"
  echo "sim=done exit=$status processes_left=$(echo $left | wc -w) log=$directory/linuxcnc.log"
  [ -z "${left// /}" ] || status=125
  exit "$status"
}
trap 'echo "sim=interrupted"; finish 130' INT TERM HUP

cd "$directory" || exit 125
# No DISPLAY: LinuxCNC's start script must not try to open dialog windows. -r keeps its output
# and the program's on stdout instead of a temporary file.
env -u DISPLAY linuxcnc -r "$directory/$ini" >"$directory/linuxcnc.log" 2>&1 &
linuxcnc_pid=$!

status=
deadline=$((SECONDS + timeout))
while kill -0 "$linuxcnc_pid" 2>/dev/null; do
  if [ $SECONDS -ge $deadline ]; then
    echo "sim=timeout seconds=$timeout"
    grep -E '^(check|fact|exit)=' "$directory/linuxcnc.log"
    finish 124
  fi
  sleep 0.2
done
wait "$linuxcnc_pid" 2>/dev/null # It has exited: this only collects it.
# The start script ends with its cleanup, so its own exit code says nothing about the program:
# the program reports its exit code in the log.
status=$(sed -n 's/^exit=\([0-9]*\)$/\1/p' "$directory/linuxcnc.log" | tail -1)
[ -n "$status" ] || status=125

grep -E '^(check|fact|exit)=' "$directory/linuxcnc.log"
finish "$status"
