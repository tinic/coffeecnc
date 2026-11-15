#!/bin/bash
# Builds the Debian package with CPack, checks its control fields and contents, and unpacks it
# (dpkg-deb -x: nothing is installed on the host).
#   package_deb.sh <build directory> <work directory> <backend source directory> <version>
#     development|installed-only
# Leaves in the work directory: cpack.log, the .deb in cpack/, info.txt (dpkg-deb -I),
# contents.txt (dpkg-deb -c) and root/, the unpacked tree. One
# `check=<name> result=pass|fail detail=...` line per check; exit code 0 only if all pass.
set -u
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
build=$(realpath "$1") || exit 125
work=$2
source=$(realpath "$3") || exit 125
version=$4
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
case $work in
"$build"/*) ;;
*) echo "check=work_is_below_the_build_directory result=fail detail=$work"; exit 125 ;;
esac
rm -rf "$work"
mkdir -p "$work" || exit 125
(cd "$build" && cpack -G DEB -B "$work/cpack") >"$work/cpack.log" 2>&1
code=$?
deb=$(ls "$work"/cpack/*.deb 2>/dev/null)
[ "$code" = 0 ] && [ -f "$deb" ]
check cpack_deb $? "exit=${code}_file=$(basename "${deb:-none}")_log=$work/cpack.log"
[ -f "$deb" ] || exit 1
dpkg-deb -I "$deb" >"$work/info.txt" 2>&1
dpkg-deb -c "$deb" >"$work/contents.txt" 2>&1
field() { dpkg-deb -f "$deb" "$1"; }
architecture=$(dpkg --print-architecture)
[ "$(field Package)" = coffeecnc ] && [ "$(field Version)" = "$version" ] &&
  [ "$(field Architecture)" = "$architecture" ] &&
  [ "$(basename "$deb")" = "coffeecnc_${version}_$architecture.deb" ]
check package_identity $? "package=$(field Package)_version=$(field Version)_architecture=$(field Architecture)"
depends=$(field Depends)
missing=
# The LinuxCNC series, the window's libraries and the C and C++ runtimes. Library package
# names are dpkg-shlibdeps' (libgtk-3-0t64 on Debian 13).
for wanted in 'linuxcnc-uspace (>= ' 'linuxcnc-uspace (<< ' libwebkit2gtk-4.1-0 libgtk-3-0 \
  libglib2.0-0 libc6 libstdc++6; do
  case $depends in
  *"$wanted"*) ;;
  *) missing="$missing $wanted" ;;
  esac
done
[ -z "$missing" ]
check package_depends $? "missing=$(echo "${missing:- none}" | tr ' ' '_')_depends=$(echo "$depends" | tr ' ' '_')"
[ -n "$(field Maintainer)" ] && [ -n "$(field Description)" ]
check package_maintainer_and_description $? "maintainer=$(field Maintainer | tr ' ' '_')"
outside=$(awk '{print $6}' "$work/contents.txt" | grep -vE '^\./(usr/(bin/(coffeecnc(-preview)?)?|share/(coffeecnc/.*)?)?)?$' | head -5 | tr '\n' ',')
[ -z "$outside" ]
check package_files_only_in_usr_bin_and_share $? "${outside:--}"
writable=$(grep -cE '^.....w|^........w' "$work/contents.txt")
[ "$writable" = 0 ]
check package_nothing_group_or_world_writable $? "entries=$writable"
dpkg-deb -x "$deb" "$work/root"
check package_unpacks $? "root=$work/root"
layout_checks "$work/root/usr" "$source" "$build" "$5"
echo "fact=package file=$deb bytes=$(stat -c %s "$deb") installed_kib=$(field Installed-Size)"
exit $failed
