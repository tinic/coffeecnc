#!/bin/bash
# Makes sure ui/dist is the bundle of the ui/ sources of this checkout, for the shell tests:
# builds the panel unless tests/sim/check-panel-stamp.mjs says the bundle there already matches.
#
#   build_panel.sh <repository root>
#
# `npm ci` runs when package-lock.json is not the one node_modules was installed from. Prints
# the checker's line, `panel=match inputs=<digest> assets=<digest>`, and `built=yes|no`. Exit
# code 0 only when the bundle matches in the end.
set -u
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(cd "${1:?repository root}" && pwd) || exit 125
checker=$here/../sim/check-panel-stamp.mjs
# A Node the panel's tools accept: the user's nvm one where there is one.
if [ -s "$HOME/.nvm/nvm.sh" ]; then
  . "$HOME/.nvm/nvm.sh" >/dev/null 2>&1
fi
command -v node >/dev/null || { echo "panel=unknown built=no detail=node-not-found"; exit 125; }
if state=$(node "$checker" "$root"); then
  echo "$state built=no"
  exit 0
fi
command -v npm >/dev/null || { echo "$state built=no detail=npm-not-found"; exit 125; }
cd "$root/ui" || exit 125
mkdir -p .scratch
lock=$(sha256sum package-lock.json | cut -d' ' -f1)
if [ ! -d node_modules ] || [ "$(cat node_modules/.coffeecnc-lock 2>/dev/null)" != "$lock" ]; then
  npm ci >.scratch/npm-ci.log 2>&1 || { echo "panel=mismatch built=no detail=npm-ci-failed_log=ui/.scratch/npm-ci.log"; exit 1; }
  echo "$lock" >node_modules/.coffeecnc-lock
fi
npm run build >.scratch/npm-build.log 2>&1 || { echo "panel=mismatch built=no detail=build-failed_log=ui/.scratch/npm-build.log"; exit 1; }
state=$(node "$checker" "$root")
code=$?
echo "$state built=yes"
exit $code
