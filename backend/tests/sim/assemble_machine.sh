#!/bin/bash
# Assembles, in a run directory, a simulation of a machine's own LinuxCNC configuration: its
# INI file and HAL files as they are, with the hardware taken out. For run.sh -m.
#
#   assemble_machine.sh <machine INI file> <simulated hardware HAL file> <run directory>
#     <DISPLAY program> <CoffeeCNC's post-GUI fragment>
#
# Nothing is written outside the run directory; the machine's files are only read. What the
# run directory gets, and every way it differs from the machine's files:
#
#   <INI file>     the machine's, with exactly these lines different (anything else fails):
#                    [DISPLAY] DISPLAY          -> <DISPLAY program>       (the test's program)
#                    [DISPLAY] PROGRAM_PREFIX   -> nc_files                (in the run directory)
#                    [HAL] one line added after the last HALFILE:
#                          HALFILE = <simulated hardware HAL file's name>
#   [HAL] HALFILE  each file of the machine without the lines that match a `#drop: <regex>`
#                  line of the simulated hardware file (the drivers and every line that names
#                  one of their pins or parameters); all other lines unchanged, in place.
#                  The dropped lines are listed in dropped-lines.txt.
#   the simulated hardware HAL file, loaded last: what stands in for the dropped lines
#   [HAL] POSTGUI_HALFILE, [HAL] SHUTDOWN, [EMCIO] TOOL_TABLE   byte for byte copies
#   [RS274NGC] PARAMETER_FILE   not copied: the interpreter starts without saved offsets
#
# Also checked: every command of CoffeeCNC's post-GUI fragment is in the machine's post-GUI
# file, in the fragment's order (a machine file carries the commands itself; this is what
# says they have not drifted from the fragment the package ships).
#
# Prints `machine=<step> ...` lines, `machine=assembled ini=<file>` last. Exit code 0, or 125
# with `machine=error detail=...`.
set -u
machine=$(realpath "$1") || exit 125
layer=$(realpath "$2") || exit 125
directory=$3
display=$4
fragment=$5
source=$(dirname "$machine")
ini=$(basename "$machine")
fail() {
  echo "machine=error detail=$1"
  exit 125
}

perl -e '
  my ($display, $layer, $in, $out) = @ARGV;
  open(my $i, "<", $in) or exit 2;
  my @lines = <$i>;
  my ($section, $displays, $prefixes, $last) = ("", 0, 0, -1);
  for my $n (0 .. $#lines) {
    $section = $1 if $lines[$n] =~ /^\[(\w+)\]/;
    if ($section eq "DISPLAY" && $lines[$n] =~ /^DISPLAY\s*=/) {
      $lines[$n] = "DISPLAY = $display\n";
      $displays++;
    }
    if ($section eq "DISPLAY" && $lines[$n] =~ /^PROGRAM_PREFIX\s*=/) {
      $lines[$n] = "PROGRAM_PREFIX = nc_files\n";
      $prefixes++;
    }
    $last = $n if $section eq "HAL" && $lines[$n] =~ /^HALFILE\s*=/;
  }
  exit 3 unless $displays == 1 && $prefixes == 1 && $last >= 0;
  splice(@lines, $last + 1, 0, "HALFILE = $layer\n");
  open(my $o, ">", $out) or exit 2;
  print $o @lines;
' "$display" "$(basename "$layer")" "$machine" "$directory/$ini" ||
  fail "ini-needs-one-DISPLAY-one-PROGRAM_PREFIX-and-a-HALFILE"
# Two lines replaced (each counts twice in a diff) and one added.
changed=$(diff "$machine" "$directory/$ini" | grep -c '^[<>]')
[ "$changed" = 5 ] || fail "ini-differs-in-${changed}-diff-lines-not-5"
echo "machine=ini file=$ini from=$machine substituted=2 added=1"

# Every value of a key in a section of the assembled INI file, one per line: as LinuxCNC's
# own reader (inivar) gives them, so that what is copied and filtered here is what LinuxCNC
# will load.
command -v inivar >/dev/null || fail inivar-not-found
values() {
  local number=1 value
  while value=$(inivar -ini "$directory/$ini" -sec "$1" -var "$2" -num "$number" 2>/dev/null); do
    echo "$value"
    number=$((number + 1))
  done
}
# The substitutions above were made on lines found by pattern. LinuxCNC must read them so:
# the test's program, the run directory's programs, and the simulated hardware loaded last.
[ "$(values DISPLAY DISPLAY | head -1)" = "$display" ] &&
  [ "$(values DISPLAY PROGRAM_PREFIX | head -1)" = nc_files ] &&
  [ "$(values HAL HALFILE | tail -1)" = "$(basename "$layer")" ] ||
  fail "ini-substitutions-are-not-what-LinuxCNC-reads"
plain() {
  case $1 in
  */* | *[!A-Za-z0-9_.+-]*) fail "file-name-not-plain_$1" ;;
  esac
}

patterns=$directory/dropped-patterns.txt
sed -n 's/^#drop: //p' "$layer" >"$patterns"
[ -s "$patterns" ] || fail "no-drop-lines-in-$(basename "$layer")"
: >"$directory/dropped-lines.txt"
for file in $(values HAL HALFILE); do
  plain "$file"
  if [ "$file" = "$(basename "$layer")" ]; then
    cp "$layer" "$directory/$file" || fail "copy_$file"
    continue
  fi
  [ -f "$source/$file" ] || fail "missing_$file"
  grep -vE -f "$patterns" "$source/$file" >"$directory/$file"
  grep -nE -f "$patterns" "$source/$file" | sed "s|^|$file:|" >>"$directory/dropped-lines.txt"
  echo "machine=hal file=$file lines=$(wc -l <"$source/$file") dropped=$(grep -cE -f "$patterns" "$source/$file")"
done

for key in "HAL POSTGUI_HALFILE" "HAL SHUTDOWN" "EMCIO TOOL_TABLE"; do
  # shellcheck disable=SC2086  # section and key: two words on purpose.
  for file in $(values $key); do
    plain "$file"
    cp "$source/$file" "$directory/$file" && cmp -s "$source/$file" "$directory/$file" ||
      fail "copy_$file"
    echo "machine=unchanged key=${key#* } file=$file sha256=$(sha256sum <"$directory/$file" | cut -c1-16)"
  done
done
for file in $(values RS274NGC PARAMETER_FILE); do
  plain "$file"
  rm -f "$directory/$file" "$directory/$file.bak"
done

postgui=$(values HAL POSTGUI_HALFILE | head -1)
[ -n "$postgui" ] || fail no-POSTGUI_HALFILE
result=$(perl -e '
  sub commands { open(my $f, "<", shift) or exit 2;
    return map { s/\s+/ /g; s/ $//; $_ } grep { !/^\s*(#|$)/ } <$f>; }
  my @wanted = commands(shift);
  my @have = commands(shift);
  my $at = 0;
  for my $line (@have) { $at++ if $at < @wanted && $line eq $wanted[$at]; }
  print "fragment_commands=" . scalar(@wanted) . " in_order=$at";
  print " first_missing=" . ($wanted[$at] =~ s/ /_/gr) if $at < @wanted;
  exit($at == @wanted ? 0 : 1);
' "$fragment" "$directory/$postgui")
code=$?
echo "machine=postgui file=$postgui $result"
[ "$code" = 0 ] || fail "post-gui-file-does-not-have-the-fragment's-commands"
echo "machine=assembled ini=$directory/$ini"
