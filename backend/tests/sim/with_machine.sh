#!/bin/bash
# Runs tests/sim/run.sh on a machine's own configuration from a checkout of the machine
# configurations (tinic/linuxcnc), which this repository does not carry.
#
#   with_machine.sh machines=<checkout or nothing> <INI file in the checkout> <run.sh>
#     [run.sh options and program...]
#
# The checkout is the CMake variable COFFEECNC_MACHINES_DIRECTORY, or, when that is empty, the
# environment variable of the same name when ctest runs. With neither the test is skipped:
# `skip=...`, exit code 77. A checkout without the INI file is an error (125).
set -u
machines=${1#machines=}
[ -n "$machines" ] || machines=${COFFEECNC_MACHINES_DIRECTORY:-}
relative=$2
runner=$3
shift 3
if [ -z "$machines" ]; then
  echo "skip=no-machine-configurations detail=COFFEECNC_MACHINES_DIRECTORY-is-not-set"
  exit 77
fi
if [ ! -f "$machines/$relative" ]; then
  echo "machine=error detail=no-file_$machines/$relative"
  exit 125
fi
echo "machine=source checkout=$machines commit=$(git -C "$machines" rev-parse --short HEAD 2>/dev/null || echo unknown) dirty=$(git -C "$machines" status --short 2>/dev/null | wc -l)"
exec "$runner" -m "$machines/$relative" "$@"
