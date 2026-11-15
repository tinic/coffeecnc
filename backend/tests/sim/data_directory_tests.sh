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

# Whatever LinuxCNC's reader takes for the key of [COFFEECNC] is refused: a header with a
# comment behind it, a value with a comment behind it.
attempt hash "[COFFEECNC] # settings\nDATA_DIRECTORY = $work/elsewhere-hash\n" -n
refused && [ ! -e "$work/elsewhere-hash" ]
check data_directory_under_a_header_with_hash_comment_refused $? "exit=$code"
attempt semicolon "[COFFEECNC] ; settings\nDATA_DIRECTORY = $work/elsewhere-semicolon\n" -n
refused && [ ! -e "$work/elsewhere-semicolon" ]
check data_directory_under_a_header_with_semicolon_comment_refused $? "exit=$code"
attempt commented "[COFFEECNC]\nDATA_DIRECTORY = $work/elsewhere-commented # there\n" -n
refused && [ ! -e "$work/elsewhere-commented" ]
check data_directory_with_a_trailing_comment_refused $? "exit=$code"
attempt first "[COFFEECNC]\nDATA_DIRECTORY = $work/elsewhere-first\nDATA_DIRECTORY = $work/first-run/in\n" -n
refused
check data_directory_set_twice_the_first_counts $? "exit=$code"

# What that reader does not take for the key is not the application's data directory either
# (the adapter reads through the same library): another case of section or key, and a second
# [COFFEECNC] section. Each is judged here exactly as inivar judges it, whichever way that is.
judged_like_inivar() {
  local name=$1
  inivar -ini "$work/$name.ini" -sec COFFEECNC -var DATA_DIRECTORY >/dev/null 2>&1
  local read=$?
  if [ "$read" = 0 ]; then refused; else ! refused && [ "$code" != 122 ]; fi &&
    [ ! -e "$work/elsewhere-$name" ]
}
attempt lowersection "[coffeecnc]\nDATA_DIRECTORY = $work/elsewhere-lowersection\n" -n
judged_like_inivar lowersection
check data_directory_lower_case_section_as_inivar_reads_it $? "exit=$code"
attempt lowerkey "[COFFEECNC]\ndata_directory = $work/elsewhere-lowerkey\n" -n
judged_like_inivar lowerkey
check data_directory_lower_case_key_as_inivar_reads_it $? "exit=$code"
attempt second "[COFFEECNC]\nMACHINE_ID = m\n[HAL]\nHALFILE = a.hal\n[COFFEECNC]\nDATA_DIRECTORY = $work/elsewhere-second\n" -n
judged_like_inivar second
check data_directory_in_a_second_section_as_inivar_reads_it $? "exit=$code"

# A file that cannot be read: refused, whatever inivar would say about it. What inivar itself
# answers for an absent key, an absent section and an unreadable file is put on record.
printf '[COFFEECNC]\nDATA_DIRECTORY = %s\n' "$work/elsewhere-unreadable" >"$work/unreadable.ini"
chmod 000 "$work/unreadable.ini"
out=$("$here/run.sh" -n -m "$work/unreadable.ini" -s "$work/layer.hal" -d "$work/unreadable-run" \
  "$work/program" 2>&1)
code=$?
if [ -r "$work/unreadable.ini" ]; then
  echo "fact=unreadable_ini_not_testable detail=this-user-reads-a-mode-000-file"
else
  [ "$code" = 122 ] && echo "$out" | grep -q "^sim=refused reason=data_directory_cannot_be_checked detail=ini-not-readable " &&
    [ ! -e "$work/unreadable-run" ] && [ ! -e "$work/elsewhere-unreadable" ]
  check data_directory_unreadable_ini_refused $? "exit=$code $(echo "$out" | tail -1 | tr ' ' '_')"
fi
codes=
printf '[COFFEECNC]\nMACHINE_ID = m\n' >"$work/absentkey.ini"
printf '[EMC]\nMACHINE = test\n' >"$work/nosection.ini"
for name in absentkey nosection unreadable; do
  inivar -ini "$work/$name.ini" -sec COFFEECNC -var DATA_DIRECTORY >/dev/null 2>&1
  codes="$codes ${name}_exit=$?"
done
echo "fact=inivar$codes"
chmod 600 "$work/unreadable.ini"

# Without LinuxCNC's reader nothing can be said about the file: refused.
mkdir -p "$work/bin"
for tool in bash dirname basename realpath cat head; do
  ln -sf "$(command -v "$tool")" "$work/bin/$tool"
done
printf '[COFFEECNC]\nMACHINE_ID = m\n' >"$work/blind.ini"
out=$(PATH=$work/bin "$work/bin/bash" "$here/run.sh" -n -m "$work/blind.ini" -s "$work/layer.hal" \
  -d "$work/blind-run" "$work/program" 2>&1)
code=$?
[ "$code" = 122 ] && echo "$out" | grep -q "^sim=refused reason=data_directory_cannot_be_checked " &&
  [ ! -e "$work/blind-run" ]
check data_directory_unchecked_without_inivar_refused $? "exit=$code $(echo "$out" | tail -1 | tr ' ' '_')"

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
