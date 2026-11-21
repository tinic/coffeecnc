#!/bin/bash
# Runs a program as the DISPLAY program of a real LinuxCNC on CoffeeCNC's simulated mill.
# No hardware is involved: the HAL configuration loops commanded positions back as feedback.
#
#   run.sh [-p sim|g0759|gantry] [-u mm|inch] [-i machine-id] [-k] [-t seconds] [-w seconds]
#          [-d run-directory] [-n] <program> [arguments...]
#   run.sh -m <machine INI file> -s <simulated hardware HAL file> [-k] [-t ...] [-d ...] [-n] <program> ...
#
# Profiles (-p):
#   sim    the test mill of coffeecnc_sim.ini.in, in millimetres or inches (-u); home switches
#   g0759  a mill with the G0759's INI values, coffeecnc_g0759.ini.in; millimetres only
#   gantry a router with two motors on Y (joints X Y Y Z), coffeecnc_gantry.ini.in; millimetres
#          only; home switches
# With -m, no profile: a machine's own INI file and HAL files with the hardware replaced by the
# -s file; see assemble_machine.sh for exactly what differs from the machine's files. The INI
# file in the run directory then has the machine file's name.
#
# @RUN@ in an INI template is the run directory. With -i the sim profile, which names no
# machine, gets `[COFFEECNC] MACHINE_ID = <machine-id>` and `DATA_DIRECTORY =
# <run-directory>/coffeecnc-data` (the g0759 profile has its own).
#
# What CoffeeCNC saves for a machine never goes to the user's own data directory from here:
# the program is started with XDG_DATA_HOME=<run-directory>/xdg-data, which is where a machine
# with a MACHINE_ID and no DATA_DIRECTORY keeps it (a machine's own INI file with -m: nothing is
# added to that file for it). Both data directories are emptied before every run, except with
# -k: a run that is to find what the run before it in the same run directory saved. A machine
# INI file (-m) whose own [COFFEECNC] DATA_DIRECTORY is not inside the run directory is refused:
# `sim=refused reason=data_directory_outside_run ...`, exit code 122, also with -n; nothing
# was started, locked or made. The key is read with LinuxCNC's inivar; a run whose INI file
# cannot be read that way is refused with reason=data_directory_cannot_be_checked (122).
#
# LinuxCNC starts <program> as `<program> -ini <run-directory>/coffeecnc_<profile>.ini
# [arguments...]` and shuts down when it exits. (The arguments are in the wrapper this script
# writes, not in the INI file: LinuxCNC reads at most 255 characters of an INI line and cuts a
# longer [DISPLAY] DISPLAY short without a word.) This script assembles the run directory, waits for
# the program with a hard timeout, makes sure every process it started is gone, and exits with the
# program's exit code (124 after a timeout, 125 when LinuxCNC could not be run or did not shut
# down). With -n it only assembles the run directory, prints `sim=assembled ini=<file>` and
# starts nothing.
#
# The exit code is the program's real one. LinuxCNC's start script does not pass it on, so the
# DISPLAY program LinuxCNC is given is a wrapper this script writes into the run directory
# (display.sh): it runs the program, waits for it and records how it ended in display.status,
# as `<code>` or `signal_<NAME>`. That record decides. The `exit=<code>` line a test program
# prints is only compared with it: a program that prints `exit=0` and then fails (a sanitizer's
# report at exit, a destructor, a signal) has failed. The result is
#
#   the recorded exit code            when it is not 0
#   128 + the signal's number         when the program was ended by a signal
#   the printed code                  when the program exited 0 but printed another code
#   125                               when nothing was recorded
#   0                                 otherwise; a program need not print an `exit=` line
#
# One simulation per host and user at a time, whatever checkout starts it: instances share the
# NML and HAL shared memory, and LinuxCNC's start script, finding another instance, shuts that
# one down. Before it touches the run directory this script takes an exclusive lock (flock) on
#
#   ~/ai/coffeecnc-sim-runner-<uid>.lock      (/tmp/... where there is no ~/ai)
#
# and keeps it until it exits, cleanup included, on every path; LinuxCNC does not inherit it.
# Who holds it is in <lock>.info, and `sim=locked lock=<file> at=<seconds since the epoch>` is
# printed when it is taken. A held lock is waited for, at most -w seconds (300): then
# `sim=refused reason=locked ...` and exit code 123, and nothing was started or changed. A
# LinuxCNC found running while this script holds the lock is not one of these runs: it is never
# cleaned up from here; the run is refused (125). -n takes no lock: it starts nothing.
#
# Output is the program's output followed by `sim=<key> ...` lines of this script; the last is
# `sim=done exit=<result> printed_exit=<code|none> child_status=<code|signal_NAME|none> ...`.
set -u

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
profile=sim
units=mm
timeout=300
directory=
assemble_only=
machine=
keep_data=
identity=
layer=
lock_wait=300
while getopts "p:u:t:d:nm:s:w:i:k" option; do
  case $option in
  w) lock_wait=$OPTARG ;;
  m) machine=$OPTARG ;;
  s) layer=$OPTARG ;;
  p) profile=$OPTARG ;;
  u) units=$OPTARG ;;
  t) timeout=$OPTARG ;;
  d) directory=$OPTARG ;;
  i) identity=$OPTARG ;;
  k) keep_data=1 ;;
  n) assemble_only=1 ;;
  *) exit 125 ;;
  esac
done
shift $((OPTIND - 1))
if [ $# -lt 1 ] || { [ "$units" != mm ] && [ "$units" != inch ]; } ||
  { [ "$profile" != sim ] && [ "$profile" != g0759 ] && [ "$profile" != gantry ]; } ||
  { [ "$profile" != sim ] && [ "$units" != mm ]; } ||
  { [ -n "$machine" ] && { [ ! -f "$machine" ] || [ ! -f "$layer" ]; }; } ||
  { [ -n "$identity" ] && { [ "$profile" != sim ] || [ -n "$machine" ]; }; }; then
  echo "usage: run.sh [-p sim|g0759|gantry] [-u mm|inch] [-i machine-id] [-k] [-t seconds]" \
    "[-d run-directory] [-n] <program> [arguments...]  (g0759 and gantry are millimetres only;" \
    "g0759 has its own machine ID), or" \
    "run.sh -m <machine INI file> -s <simulated hardware HAL file> ..." >&2
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
  fragments="coffeecnc_postgui.hal coffeecnc_sim_postgui.hal coffeecnc_sim_home.hal"
elif [ "$profile" = gantry ]; then
  flavour=$profile
  fragments="coffeecnc_postgui.hal coffeecnc_sim_postgui.hal coffeecnc_gantry_sim.hal"
else
  flavour=$profile
  fragments="coffeecnc_postgui.hal coffeecnc_sim_postgui.hal coffeecnc_${profile}_postgui.hal coffeecnc_${profile}_sim.hal"
fi
ini=coffeecnc_$profile.ini
if [ -n "$machine" ]; then
  flavour=machine
  ini=$(basename "$machine")
fi
[ -n "$directory" ] || directory=$here/../../build/sim/$(basename "$program")-$flavour

# A machine's own INI file may name a data directory of its own. Nothing of a test is saved
# outside its run directory: such a run is refused, before the lock, before anything is made.
# The value is read by LinuxCNC's own reader (inivar), the one the application's adapter uses
# as a library: whatever that reader makes of comments, white space, case and repeated
# sections is what the application will be given. Without it, or when it fails, the run is
# refused too.
if [ -n "$machine" ]; then
  if ! command -v inivar >/dev/null; then
    echo "sim=refused reason=data_directory_cannot_be_checked detail=inivar-not-found ini=$machine"
    exit 122
  fi
  # inivar's exit code 1 is "not found" (key or section absent; seen with 2.9.4, where a file it
  # cannot open gives 255): that the file can be read is therefore established here first.
  if [ ! -f "$machine" ] || [ ! -r "$machine" ] || ! head -c 1 "$machine" >/dev/null 2>&1; then
    echo "sim=refused reason=data_directory_cannot_be_checked detail=ini-not-readable ini=$machine"
    exit 122
  fi
  data_directory=$(inivar -ini "$machine" -sec COFFEECNC -var DATA_DIRECTORY 2>/dev/null)
  case $? in
  0)
    case $(realpath -m -- "$data_directory") in
    "$(realpath -m "$directory")"/*) ;;
    *)
      echo "sim=refused reason=data_directory_outside_run data_directory=$data_directory" \
        "run_directory=$(realpath -m "$directory") ini=$machine"
      exit 122
      ;;
    esac
    ;;
  1) ;; # The key is not set.
  *)
    echo "sim=refused reason=data_directory_cannot_be_checked detail=inivar-failed ini=$machine"
    exit 122
    ;;
  esac
fi

locked=
release() {
  # The lock itself ends with this process. The note about its holder goes when it is ours.
  [ -z "$locked" ] || ! grep -q "^pid=$$ " "$lock.info" 2>/dev/null || rm -f "$lock.info"
}
if [ -z "$assemble_only" ]; then
  lock_directory=$HOME/ai
  [ -d "$lock_directory" ] || lock_directory=/tmp
  lock=$lock_directory/coffeecnc-sim-runner-$(id -u).lock
  exec 9>>"$lock" || { echo "sim=refused reason=lock-file-cannot-be-opened lock=$lock"; exit 123; }
  if ! flock -n 9; then
    holder() { sed 's/\(^\| \)\([a-z_]*\)=/\1holder_\2=/g' "$lock.info" 2>/dev/null || echo holder=unknown; }
    echo "sim=waiting reason=locked lock=$lock at_most_seconds=$lock_wait $(holder)"
    if ! flock -w "$lock_wait" 9; then
      echo "sim=refused reason=locked lock=$lock waited_seconds=$lock_wait $(holder)"
      exit 123
    fi
  fi
  locked=1
  trap release EXIT
  echo "pid=$$ checkout=$(cd "$here/../../.." && pwd) run_directory=$directory" \
    "program=$(basename "$program") since=$(date -Is)" >"$lock.info"
  # From here the time is this run's own, not the queue's (seconds since the epoch).
  echo "sim=locked lock=$lock at=$(date +%s)"
fi
mkdir -p "$directory/nc_files" || exit 125
directory=$(realpath "$directory")
for word in "$program" "$directory" "$machine" "$identity" "$@"; do
  if ! plain "$word"; then
    echo "sim=error detail=unsupported-characters-in-path-or-argument"
    exit 125
  fi
done

if [ $((${#directory} + 30)) -gt 250 ]; then
  echo "sim=error detail=run-directory-path-too-long-for-an-ini-line"
  exit 125
fi

# The DISPLAY program LinuxCNC starts: the real one with its arguments, and a record of how it
# ended. LinuxCNC calls it as `display.sh -ini <file>`.
rm -f "$directory/display.status"
cat >"$directory/display.sh" <<EOF || exit 125
#!/bin/bash
"$program" "\$@" $*
status=\$?
if [ \$status -gt 128 ] && name=\$(kill -l \$status 2>/dev/null); then
  echo "signal_\$name" >"$directory/display.status"
else
  echo \$status >"$directory/display.status"
fi
exit \$status
EOF
chmod +x "$directory/display.sh" || exit 125

# What an earlier run saved for its machine.
[ -n "$keep_data" ] || rm -rf "$directory/coffeecnc-data" "$directory/xdg-data"
# The post-GUI fragment the package installs: the tests run that very file.
shipped_postgui=$here/../../share/hal/coffeecnc_postgui.hal
if [ -n "$machine" ]; then
  # A machine's own configuration: assembled from its files, never written to.
  rm -f "$directory/linuxcnc.log"
  "$here/assemble_machine.sh" "$machine" "$layer" "$directory" "$directory/display.sh" \
    "$shipped_postgui" || exit 125
else
# The run directory holds only generated files.
perl -pe '
  BEGIN { $inch = shift eq "inch"; $display = shift; $run = shift; }
  s/\@UNITS\@/$inch ? "inch" : "mm"/ge;
  s/\@DISPLAY\@/$display/g;
  s/\@RUN\@/$run/g;
  s/\@L:([-0-9.]+)\@/$inch ? sprintf("%.6f", $1 \/ 25.4) : $1/ge;
' "$units" "$directory/display.sh" "$directory" "$here/coffeecnc_$profile.ini.in" >"$directory/$ini" || exit 125
if [ -n "$identity" ]; then
  printf '\n[COFFEECNC]\nMACHINE_ID = %s\nDATA_DIRECTORY = %s/coffeecnc-data\n' \
    "$identity" "$directory" >>"$directory/$ini" || exit 125
fi
for fragment in $fragments; do
  source_file=$here/$fragment
  [ "$fragment" = coffeecnc_postgui.hal ] && source_file=$shipped_postgui
  cp "$source_file" "$directory/" || exit 125
done
rm -f "$directory/sim.var" "$directory/sim.var.bak" "$directory/linuxcnc.log" \
  "$directory/coffeecnc-spindle-tool.txt"
# Tool table in machine units: T number, P pocket, Z length, D diameter.
if [ "$profile" = g0759 ]; then
  # As the machine's table: no length, no diameter, no description.
  printf 'T0 P0 ;\nT1 P1 ;\nT2 P2 ;\nT3 P3 ;\n'
elif [ "$units" = inch ]; then
  printf 'T1 P1 Z2.0 D0.25 ;quarter inch end mill\nT2 P2 Z1.5 D0.125 ;eighth inch end mill\n'
else
  printf 'T1 P1 Z50 D6 ;6 mm end mill\nT2 P2 Z40 D3 ;3 mm end mill\n'
fi >"$directory/tool.tbl"
fi
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
# Where a machine without [COFFEECNC] DATA_DIRECTORY keeps its data: here, not under $HOME.
export XDG_DATA_HOME="$directory/xdg-data"
ours() {
  local pid
  for pid in $(pgrep -u "$(id -u)"); do
    [ "$pid" != $$ ] && [ "$pid" != "$BASHPID" ] &&
      tr "\\0" "\\n" 2>/dev/null <"/proc/$pid/environ" |
      grep -qxF "COFFEECNC_SIM_RUN=$directory" && echo "$pid"
  done
}

linuxcnc_pid=
printed=
child=
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
    timeout -k 5 20 halrun -U >/dev/null 2>&1 9>&-
    sleep 1
    [ "$status" != 0 ] || status=125
  fi
  left="$(ours) $(pgrep -x rtapi_app)"
  echo "sim=done exit=$status printed_exit=${printed:-none} child_status=${child:-none}" \
    "processes_left=$(echo $left | wc -w) log=$directory/linuxcnc.log"
  [ -z "${left// /}" ] || status=125
  exit "$status"
}
trap 'echo "sim=interrupted"; finish 130' INT TERM HUP

cd "$directory" || exit 125
# No DISPLAY: LinuxCNC's start script must not try to open dialog windows. -r keeps its output
# and the program's on stdout instead of a temporary file.
# Without the lock's descriptor: the lock is this script's, not LinuxCNC's.
env -u DISPLAY linuxcnc -r "$directory/$ini" >"$directory/linuxcnc.log" 2>&1 9>&- &
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
# the wrapper recorded how the program really ended, and that decides. What the program printed
# as its exit code must agree with it.
printed=$(sed -n 's/^exit=\([0-9]*\)$/\1/p' "$directory/linuxcnc.log" | tail -1)
child=$(cat "$directory/display.status" 2>/dev/null)
case $child in
"") status=125 ;;
signal_*) status=$((128 + $(kill -l "${child#signal_}" 2>/dev/null || echo 0))) ;;
0) status=${printed:-0} ;;
*) status=$child ;;
esac

grep -E '^(check|fact|exit)=' "$directory/linuxcnc.log"
finish "$status"
