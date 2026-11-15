#!/bin/bash
# run.sh -m refuses a machine INI file whose [COFFEECNC] DATA_DIRECTORY is outside the run
# directory, before it locks, makes or starts anything. No LinuxCNC is started here.
#
#   data_directory_tests.sh <scratch directory>
#
# Output: `check=<name> result=pass|fail detail=...` lines; exit code 0 only when all pass.
set -u
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
work=$1/data_directory
rm -rf "$work"
mkdir -p "$work" || exit 125
work=$(realpath "$work")
failures=0
check() {
  if [ "$2" = 0 ]; then
    echo "check=$1 result=pass detail=${3:--}"
  else
    echo "check=$1 result=fail detail=${3:--}"
    failures=$((failures + 1))
  fi
}
touch "$work/layer.hal"
printf '#!/bin/sh\nexit 0\n' >"$work/program"
chmod +x "$work/program"
# An INI file with this [COFFEECNC] section; run with the options given. Sets $code and $out.
attempt() {
  local name=$1 section=$2
  shift 2
  printf '[EMC]\nMACHINE = test\n[OTHER]\nDATA_DIRECTORY = /not/this/section\n%b' "$section" \
    >"$work/$name.ini"
  out=$("$here/run.sh" -m "$work/$name.ini" -s "$work/layer.hal" -w 1 -d "$work/$name-run" "$@" \
    "$work/program" 2>&1)
  code=$?
}
refused() { [ "$code" = 122 ] && echo "$out" | grep -q "^sim=refused reason=data_directory_outside_run "; }

for mode in run assemble; do
  option=
  [ "$mode" = run ] || option=-n
  attempt "outside-$mode" "[COFFEECNC]\nMACHINE_ID = m\nDATA_DIRECTORY = $work/elsewhere-$mode\n" $option
  refused && [ ! -e "$work/elsewhere-$mode" ] && [ ! -e "$work/outside-$mode-run" ]
  check "data_directory_outside_refused_$mode" $? "exit=$code $(echo "$out" | tail -1 | tr ' ' '_')"
done
attempt spaced "  [COFFEECNC]  \n  DATA_DIRECTORY   =   $work/elsewhere-spaced  \n" -n
refused && [ ! -e "$work/elsewhere-spaced" ]
check data_directory_outside_refused_with_white_space $? "exit=$code"
attempt relative "[COFFEECNC]\nDATA_DIRECTORY = data\n" -n
refused
check data_directory_relative_refused $? "exit=$code"
attempt parent "[COFFEECNC]\nDATA_DIRECTORY = $work/parent-run/../elsewhere-parent\n" -n
refused && [ ! -e "$work/elsewhere-parent" ]
check data_directory_leaving_the_run_directory_refused $? "exit=$code"
attempt prefix "[COFFEECNC]\nDATA_DIRECTORY = $work/prefix-run-other/data\n" -n
refused
check data_directory_beside_the_run_directory_refused $? "exit=$code"

# Not refused: inside the run directory, no key, the key of another section. (The files are
# no machine configuration, so the assembly that follows fails; that is not this refusal.)
attempt inside "[COFFEECNC]\nDATA_DIRECTORY = $work/inside-run/data\n" -n
! refused && [ "$code" != 122 ]
check data_directory_inside_not_refused $? "exit=$code"
attempt none "[COFFEECNC]\nMACHINE_ID = m\n" -n
! refused && [ "$code" != 122 ]
check data_directory_absent_not_refused $? "exit=$code"
attempt other "[COFFEECNC_SIM]\nDATA_DIRECTORY = /another/section\n" -n
! refused && [ "$code" != 122 ]
check data_directory_of_another_section_not_refused $? "exit=$code"

echo "data_directory=done failures=$failures"
[ "$failures" = 0 ]
