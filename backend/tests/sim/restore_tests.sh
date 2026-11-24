#!/bin/bash
# A restore on a real LinuxCNC (review #274): stage it while LinuxCNC runs, LinuxCNC stops and
# writes its .var file, the helper writes the backup's files after that, and the next start of
# CoffeeCNC says so. The simulation runs once, through run.sh and its host lock.
#
#   restore_tests.sh <scratch directory> <coffeecnc_restore_sim_tests> <coffeecnc executable>
#
# Output: `check=<name> result=pass|fail detail=...` lines; exit code 0 only when all pass.
set -u
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
program=$2
coffeecnc=$3
run=$1/linuxcnc_restore
backups=$1/linuxcnc_restore_backups
rm -rf "$backups"
mkdir -p "$backups" || exit 125
failures=0
check() {
  if [ "$2" = 0 ]; then
    echo "check=$1 result=pass detail=${3:--}"
  else
    echo "check=$1 result=fail detail=${3:--}"
    failures=$((failures + 1))
  fi
}

out=$("$here/run.sh" -i SIMRESTORE -t 120 -d "$run" "$here/restore_display.py" "$program" \
  "$coffeecnc" "$backups" 2>&1)
code=$?
echo "$out"
check restore_staged_and_linuxcnc_stopped "$code" "exit=$code"
[ "$code" = 0 ] || { echo "restore=done failures=$failures"; exit 1; }
archive=$(echo "$out" | sed -n 's/^fact=archive path=//p' | tail -1)
data=$run/coffeecnc-data/SIMRESTORE

# The helper waits until LinuxCNC has stopped; it writes the result last.
for _ in $(seq 300); do
  [ -e "$data/restore/result.json" ] && break
  sleep 0.2
done
result=$(cat "$data/restore/result.json" 2>/dev/null)
echo "$result" | grep -q '"ok":true' && [ ! -e "$data/restore/plan.json" ] &&
  [ ! -e "$data/restore/applying.json" ]
check helper_wrote_after_linuxcnc_stopped $? "result=$(echo "$result" | tr ' ' '_')"

g54_x() { awk '$1 == 5221 { print $2 }' "$1"; }
[ "$(g54_x "$run/sim.var")" = 7.000000 ]
check var_is_the_backups_not_what_linuxcnc_wrote_at_exit $? "g54_x=$(g54_x "$run/sim.var")"
! grep -q 'added after the backup' "$run/tool.tbl" && grep -q '^T1 ' "$run/tool.tbl"
check tool_table_is_the_backups $? "$(tr '\n' '|' <"$run/tool.tbl" | tr ' ' '_')"
pre=$(ls "$data"/pre-restore-*.tar.gz 2>/dev/null | head -1)
saved=$(tar -xzOf "$pre" parameters/sim.var 2>/dev/null | awk '$1 == 5221 { print $2 }')
[ -n "$pre" ] && [ "$saved" = 12.500000 ] && tar -tzf "$pre" | grep -q '^tool-table/tool.tbl$'
check pre_restore_copy_has_what_linuxcnc_left "$?" "pre=$pre g54_x=$saved"
[ -n "$archive" ] && [ -e "$archive" ]
check backup_archive_written "$?" "archive=$archive"

# The next start: what came of it, once.
report=$("$program" report "$run/coffeecnc_sim.ini" 2>&1)
echo "$report" | grep -q '^notice_level=info notice=The backup was restored: 2 files were written.' &&
  echo "$report" | grep -qF "$pre" &&
  echo "$report" | grep -qx 'notice_key=backend.machineData.restored'
check next_start_reports_the_restore $? "$(echo "$report" | tr ' ' '_')"
again=$("$program" report "$run/coffeecnc_sim.ini" 2>&1)
echo "$again" | grep -q '^notice_level=none'
check next_start_reports_it_once $? "$(echo "$again" | tr ' ' '_')"

echo "restore=done failures=$failures"
[ "$failures" = 0 ]
