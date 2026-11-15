#!/bin/bash
# `cmake --install` of a build directory into a prefix below it, and what is in the prefix.
#   install_tree.sh <build directory> <prefix> <backend source directory> development|installed-only
# The prefix is emptied first. One `check=<name> result=pass|fail detail=...` line per check;
# exit code 0 only if all pass.
set -u
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
build=$(realpath "$1") || exit 125
prefix=$2
source=$(realpath "$3") || exit 125
failed=0
check() { # name, 0 for pass, detail
  if [ "$2" = 0 ]; then
    echo "check=$1 result=pass detail=$3"
  else
    echo "check=$1 result=fail detail=$3"
    failed=1
  fi
}
. "$here/layout_checks.sh"
# Only a directory of the build is ever removed here.
case $prefix in
"$build"/*) ;;
*) echo "check=prefix_is_below_the_build_directory result=fail detail=$prefix"; exit 125 ;;
esac
rm -rf "$prefix"
mkdir -p "$(dirname "$prefix")" || exit 125
cmake --install "$build" --prefix "$prefix" >"$(dirname "$prefix")/install.log" 2>&1
code=$?
check cmake_install $code "exit=${code}_log=$(dirname "$prefix")/install.log"
layout_checks "$prefix" "$source" "$build" "$4"
exit $failed
