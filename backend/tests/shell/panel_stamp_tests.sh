#!/bin/bash
# The panel stamp checker (tests/sim/check-panel-stamp.mjs) against a copy of the built panel
# and its inputs: it must accept the copy as built and refuse it after each kind of change.
#   panel_stamp_tests.sh <repository root> <scratch directory>
# The panel must have been built (build_panel.sh). Nothing in the checkout is changed.
# One `check=<name> result=pass|fail detail=...` line per check; exit code 0 only if all pass.
set -u
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(cd "${1:?repository root}" && pwd) || exit 125
work=${2:?scratch directory}/panel-stamp
checker=$here/../sim/check-panel-stamp.mjs
if [ -s "$HOME/.nvm/nvm.sh" ]; then
  . "$HOME/.nvm/nvm.sh" >/dev/null 2>&1
fi
failed=0
check() { # name, 0 for pass, detail
  if [ "$2" = 0 ]; then
    echo "check=$1 result=pass detail=$3"
  else
    echo "check=$1 result=fail detail=$3"
    failed=1
  fi
}
# A copy of the repository as far as the stamp names it: every input, and the bundle.
copy() {
  rm -rf "$work" && mkdir -p "$work/ui" || exit 125
  node -e '
    const stamp = JSON.parse(require("fs").readFileSync(process.argv[1] + "/ui/dist/build-stamp.json", "utf8"))
    for (const path of Object.keys(stamp.inputs)) console.log(path)
  ' "$root" | while read -r path; do
    mkdir -p "$work/$(dirname "$path")" && cp "$root/$path" "$work/$path" || exit 125
  done
  cp -r "$root/ui/dist" "$work/ui/dist" || exit 125
}
verdict() { node "$checker" "$work" 2>&1; }

copy
out=$(verdict)
[ $? = 0 ] && case $out in "panel=match inputs="*" assets="*) true ;; *) false ;; esac
check built_panel_matches $? "$(echo "$out" | cut -c1-60)"

printf ' ' >>"$work/ui/src/main.ts"
out=$(verdict)
[ $? = 1 ] && echo "$out" | grep -q 'detail=input_ui/src/main.ts_has_changed'
check changed_source_byte_is_a_mismatch $? "$(echo "$out" | sed 's/.*detail=//')"

copy
asset=$(ls "$work/ui/dist/assets/"index-*.js | head -1)
printf ' ' >>"$asset"
out=$(verdict)
[ $? = 1 ] && echo "$out" | grep -q 'detail=the_bundle_is_not_the_one'
check changed_asset_is_a_mismatch $? "$(echo "$out" | sed 's/.*detail=//')"

copy
echo 'export const added = 1' >"$work/ui/src/added-after-the-build.ts"
out=$(verdict)
[ $? = 1 ] && echo "$out" | grep -q 'detail=ui/src/added-after-the-build.ts_was_not_part_of_the_build'
check unbuilt_source_file_is_a_mismatch $? "$(echo "$out" | sed 's/.*detail=//')"

copy
rm "$work/ui/package-lock.json"
out=$(verdict)
[ $? = 1 ] && echo "$out" | grep -q 'detail=input_ui/package-lock.json_is_missing'
check missing_input_is_a_mismatch $? "$(echo "$out" | sed 's/.*detail=//')"

copy
rm "$work/ui/dist/build-stamp.json"
out=$(verdict)
[ $? = 1 ] && echo "$out" | grep -q 'detail=no_build-stamp.json'
check unstamped_bundle_is_a_mismatch $? "$(echo "$out" | sed 's/.*detail=//' | cut -c1-40)"

copy
out=$(verdict)
check untouched_copy_still_matches $? "$(echo "$out" | cut -c1-60)"
exit $failed
