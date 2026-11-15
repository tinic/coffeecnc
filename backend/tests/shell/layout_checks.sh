# Sourced by install_tree.sh and package_deb.sh: the checks on an installed tree.
#   layout_checks <prefix> <backend source directory>
# Uses the caller's check().
layout_checks() {
  local prefix=$1 source=$2 extra
  [ -x "$prefix/bin/coffeecnc" ] && [ -x "$prefix/bin/coffeecnc-preview" ]
  check programs_installed $? "$(cd "$prefix/bin" 2>/dev/null && ls | tr '\n' ',')"
  [ -f "$prefix/share/coffeecnc/ui/index.html" ] &&
    diff -rq "$source/../ui/dist" "$prefix/share/coffeecnc/ui" >/dev/null
  check panel_installed_is_the_built_one $? "files=$(find "$prefix/share/coffeecnc/ui" -type f 2>/dev/null | wc -l)"
  # One fragment: the installed file is the one the simulation tests run.
  cmp -s "$source/tests/sim/coffeecnc_postgui.hal" "$prefix/share/coffeecnc/hal/coffeecnc_postgui.hal"
  check hal_fragment_is_the_tested_file $? "sha256=$(sha256sum <"$prefix/share/coffeecnc/hal/coffeecnc_postgui.hal" | cut -c1-16)"
  cmp -s "$source/share/coffeecnc.ini.example" "$prefix/share/coffeecnc/coffeecnc.ini.example"
  check ini_example_installed $? -
  extra=$(cd "$prefix" && find . -type f ! -path './bin/coffeecnc' ! -path './bin/coffeecnc-preview' \
    ! -path './share/coffeecnc/*' | head -5 | tr '\n' ',')
  [ -z "$extra" ]
  check nothing_else_installed $? "${extra:--}"
}
