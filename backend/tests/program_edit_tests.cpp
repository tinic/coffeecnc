#include <coffeecnc/app/file_system.hpp>
#include <coffeecnc/app/program_edit.hpp>
#include <coffeecnc/app/program_store.hpp>
#include <coffeecnc/app/tool_model_store.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;
using namespace coffeecnc;

namespace {

int failures = 0; // NOLINT: one counter for the whole test

void require(bool condition, std::string_view message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

auto read(const fs::path& file) -> std::string {
  std::ifstream in(file, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void edits_only_feed_or_speed_numbers() {
  require(feed_or_speed_edit("G1 X10 F600", "G1 X10 F450"), "A feed number");
  require(feed_or_speed_edit("S1500 M3", "S2000 M3"), "A speed number");
  require(feed_or_speed_edit("g1 x1 f 300", "g1 x1 f 250.5"), "Lower case and a space after the letter");
  require(feed_or_speed_edit("G1 X10 F600 S1000", "G1 X10 F600 S900"), "The second of two words");
  require(feed_or_speed_edit("F.5", "F12."), "Numbers without a leading or trailing digit");
  require(!feed_or_speed_edit("G1 X10 F600", "G1 X10 F600"), "No change is no edit");
  require(!feed_or_speed_edit("G1 X10 F600", "G1 X11 F600"), "Another word");
  require(!feed_or_speed_edit("G1 X10 F600", "G1 X10 F600 G0 Z-5"), "Added code");
  require(!feed_or_speed_edit("G1 X10 F600", "G1 X10 F[600]"), "An expression");
  require(!feed_or_speed_edit("G1 X10 F600", "G1 X10 F-600"), "A negative number");
  require(!feed_or_speed_edit("G1 X10 F600", "G1 X10 F6e2"), "An exponent");
  require(!feed_or_speed_edit("G1 X10 F600", "G1 X10 F60 0"), "Digits split by a space");
  require(!feed_or_speed_edit("G1 X10 F600", "G1 X10 F6\n0"), "A line end");
  require(!feed_or_speed_edit("(F600) G1 X1", "(F500) G1 X1"), "A number in a comment");
  require(!feed_or_speed_edit("G1 X1 ; F600", "G1 X1 ; F500"), "A number after a semicolon");
  require(!feed_or_speed_edit("G1 X1 F#1", "G1 X1 F#2"), "A parameter");
}

void replaces_one_line_durably(const fs::path& base) {
  const fs::path root = base / "program_edit_root";
  fs::remove_all(root);
  fs::create_directories(root);
  const fs::path part = root / "part.ngc";
  { std::ofstream(part, std::ios::binary) << "G21\r\nG1 X10 F600\r\nS1500 M3\nM30"; }
  const LocalFileSystem files;
  const ProgramStore programs{files, root.string()};
  LocalToolModelFiles writes;
  ProgramWriter writer{programs, writes};

  const auto written = writer.replace_line(part.string(), 2, "G1 X10 F600", "G1 X10 F450");
  require(written.has_value(), "The feed is replaced: " + (written ? std::string{} : written.error()));
  require(read(part) == "G21\r\nG1 X10 F450\r\nS1500 M3\nM30", "Only that number changes; line ends are kept");
  require(!fs::exists(root / ".part.ngc.coffeecnc-edit"), "No temporary file is left");

  require(writer.replace_line(part.string(), 4, "M30", "M30").error().starts_with("Only the number"),
          "Anything but a feed or speed is refused");
  const auto stale = writer.replace_line(part.string(), 2, "G1 X10 F600", "G1 X10 F300");
  require(!stale && stale.error().starts_with("The program changed"), "A line that changed is refused");
  require(read(part) == "G21\r\nG1 X10 F450\r\nS1500 M3\nM30", "A refused edit writes nothing");
  require(!writer.replace_line(part.string(), 9, "S1500 M3", "S900 M3"), "A line that does not exist");
  require(writer.replace_line(part.string(), 3, "S1500 M3", "S900 M3").has_value(), "The last-but-one line");
  require(read(part) == "G21\r\nG1 X10 F450\r\nS900 M3\nM30", "Its own line end is kept");
  require(!writer.replace_line((base / "outside.ngc").string(), 1, "F1", "F2"),
          "A file outside the program directory is refused");
  fs::remove_all(root);
}

} // namespace

auto main(int argc, char** argv) -> int {
  edits_only_feed_or_speed_numbers();
  replaces_one_line_durably(argc > 1 ? fs::path{argv[1]} : fs::temp_directory_path());
  if (failures != 0)
    return EXIT_FAILURE;
  std::cout << "program_edit: all passed\n";
  return EXIT_SUCCESS;
}
