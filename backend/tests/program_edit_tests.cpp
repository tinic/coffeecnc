#include <coffeecnc/app/file_system.hpp>
#include <coffeecnc/app/program_edit.hpp>
#include <coffeecnc/app/program_store.hpp>
#include <coffeecnc/app/tool_model_store.hpp>
#include <coffeecnc/core/command_policy.hpp>

#include "fake_file_system.hpp"
#include "fake_tool_model_files.hpp"

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
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
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
  require(!feed_or_speed_edit("G1 Z#<pos1> F500", "G1 Z#<pos2> F500"), "A named parameter");
  require(!feed_or_speed_edit("O<pass1> call", "O<pass2> call"), "A subroutine name");
  require(feed_or_speed_edit("G1 Z#<pos1> F500", "G1 Z#<pos1> F250"), "A real F word after a name");
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

void creates_new_programs(const fs::path& base) {
  const fs::path root = base / "program_create_root";
  fs::remove_all(root);
  fs::create_directories(root / "sub");
  { std::ofstream(root / "part.ngc", std::ios::binary) << "M2\n"; }
  const LocalFileSystem files;
  const ProgramStore programs{files, "/", root.string()};
  LocalToolModelFiles writes;
  ProgramWriter writer{programs, writes};
  const std::string text = "(Facing)\nG21\nM2\n";

  const auto made = writer.create("facing.ngc", text, false);
  require(made && *made == fs::canonical(root).string() + "/facing.ngc",
          "A new program is written in the program directory: " + made.error_or(""));
  require(read(root / "facing.ngc") == text, "The text is written exactly");
  require(!fs::exists(root / ".facing.ngc.coffeecnc-new"), "No temporary file is left");
  require((fs::status(root / "facing.ngc").permissions() & fs::perms::others_all) == fs::perms::none,
          "Only its owner can read it");

  const auto again = writer.create("facing.ngc", "M2\n", false);
  require(!again && again.error() == "A program named facing.ngc exists already.",
          "An existing program is not written over");
  require(read(root / "facing.ngc") == text, "It keeps its text");
  require(writer.create("facing.ngc", "M30\n", true).has_value() && read(root / "facing.ngc") == "M30\n",
          "Asked to, it is replaced");
  require(writer.create((root / "sub" / "deep.ngc").string(), text, false).has_value() &&
              read(root / "sub" / "deep.ngc") == text,
          "Below the program directory");
  require(!writer.create((base / "outside.ngc").string(), text, false) &&
              !fs::exists(base / "outside.ngc"),
          "Outside the program directory nothing is written");
  // A dangling link already occupies this name.
  fs::create_symlink(root / "nowhere" / "target.ngc", root / "dangling.ngc");
  const auto through = writer.create("dangling.ngc", text, false);
  require(!through && through.error().find("exists already") != std::string::npos &&
              fs::is_symlink(root / "dangling.ngc") && !fs::exists(root / "nowhere"),
          "A dangling link blocks non-replacing saves at the existence check");
  require(!fs::exists(root / ".dangling.ngc.coffeecnc-new"), "Nor is a temporary file left");
  fs::remove_all(root);
}

// The program editor's save over the loaded program (save_program with a stamp).
void saves_over_a_program_with_its_stamp(const fs::path& base) {
  const fs::path root = base / "program_replace_root";
  fs::remove_all(root);
  fs::create_directories(root);
  const fs::path part = root / "part.ngc";
  const std::string before = "G21\nG1 X10 F600\nM2\n";
  { std::ofstream(part, std::ios::binary) << before; }
  const LocalFileSystem files;
  const ProgramStore programs{files, root.string()};
  LocalToolModelFiles writes;
  ProgramWriter writer{programs, writes};

  const auto shown = programs.read_program(part.string(), 1);
  require(shown && shown->stamp == program_stamp(before) && shown->stamp.size() == 16,
          "The program message names the bytes it was read from");
  require(program_stamp(before) != program_stamp("G21\nG1 X10 F601\nM2\n") &&
              program_stamp("") == "cbf29ce484222325",
          "Another text has another stamp; the empty text has FNV-1a's offset basis");

  // A second name for the file as it was: the save makes a new file, it does not write into it.
  fs::create_hard_link(part, root / "kept.ngc");
  const std::string after = "G21\nG1 X12 F450\nG1 Y5\nM2\n";
  const auto saved = writer.replace(part.string(), after, shown->stamp);
  require(saved && *saved == fs::canonical(part).string(),
          "The edited text is saved over the program: " + saved.error_or(""));
  require(read(part) == after, "The new text is written exactly");
  require(read(root / "kept.ngc") == before,
          "The old file is replaced by a rename, never rewritten in place");
  require(!fs::exists(root / ".part.ngc.coffeecnc-edit"), "No temporary file is left");

  const auto stale = writer.replace(part.string(), "M2\n", shown->stamp);
  require(!stale && stale.error() ==
                        "The program file changed since it was opened. Open it again to edit it.",
          "The stamp of the text before the save no longer matches");
  require(read(part) == after, "A refused save writes nothing");

  // Someone else (CAM, another panel) writes the file while it is open in the editor.
  const auto opened = program_stamp(after);
  { std::ofstream(part, std::ios::binary | std::ios::trunc) << "G21\nG0 X0\nM2\n"; }
  require(!writer.replace(part.string(), "M2\n", opened),
          "A file changed on disk is not written over");
  require(read(part) == "G21\nG0 X0\nM2\n", "It keeps the other writer's text");

  const std::string cap(max_saved_program_bytes + 1, 'G');
  const auto large = writer.replace(part.string(), cap, program_stamp(read(part)));
  require(!large && large.error() == "The program text is longer than 512 KiB.",
          "A text above 512 KiB is refused");
  require(writer.replace(part.string(), std::string(max_saved_program_bytes, ';'),
                         program_stamp(read(part)))
                  .has_value() &&
              fs::file_size(part) == max_saved_program_bytes,
          "A text of exactly 512 KiB is saved");

  const fs::path windows = root / "windows.ngc";
  { std::ofstream(windows, std::ios::binary) << "G21\r\nM2\r\n"; }
  require(writer.replace(windows.string(), "G21\nG0 X1\r\nM2\n", program_stamp("G21\r\nM2\r\n"))
              .has_value(),
          "A program with CR LF line ends is saved");
  const std::string windows_text = "G21\r\nG0 X1\r\nM2\r\n";
  require(read(windows) == windows_text, "Its line ends stay CR LF, none doubled");
  const auto too_large = writer.replace(windows.string(), std::string(max_saved_program_bytes, '\n'),
                                       program_stamp(windows_text));
  require(!too_large && read(windows) == windows_text,
          "A text that exceeds the cap after restoring CR LF is refused before replacing the file");

  require(!writer.replace((root / "missing.ngc").string(), "M2\n", program_stamp("")),
          "A program that does not exist is refused");
  require(!writer.replace((base / "outside.ngc").string(), "M2\n", program_stamp("")),
          "A file outside the program directory is refused");
  fs::remove_all(root);
}

void keeps_external_changes_during_sync() {
  constexpr auto path = "/nc/part.ngc";
  const std::string before = "G0 X1\nM2\n";
  const std::string external = "G0 X9\nM2\n";
  FakeFileSystem files;
  files.file(path, before);
  const ProgramStore programs{files, "/nc"};
  FakeDisk disk;
  disk.files[path] = before;
  FakeToolModelFiles writes{disk};
  writes.during = [&](std::string_view step) {
    if (step == "sync")
      disk.files[path] = external;
  };
  ProgramWriter writer{programs, writes};
  const auto save = writer.replace(path, "G0 X2\nM2\n", program_stamp(before));
  require(!save && save.error().starts_with("The program file changed"),
          "The original is checked once more after the temporary file is synced");
  require(disk.files.at(path) == external && writes.count("rename") == 0,
          "The external writer's text remains on disk");
  require(!disk.files.contains("/nc/.part.ngc.coffeecnc-edit"), "The refused save discards its temporary file");
}

// Failures before rename keep the original. A directory sync failure reports the replacement.
void a_failed_save_keeps_the_program() {
  constexpr auto path = "/nc/part.ngc";
  const std::string before = "G0 X1\nM2\n";
  for (const std::string step : {"open_temporary", "write", "sync", "rename", "sync_directory"}) {
    FakeFileSystem files;
    files.file(path, before);
    const ProgramStore programs{files, "/nc"};
    FakeDisk disk;
    disk.files[path] = before;
    FakeToolModelFiles writes{disk};
    writes.failures[step] = 1;
    ProgramWriter writer{programs, writes};
    const auto saved = writer.replace(path, "G0 X2\nM2\n", program_stamp(before));
    if (step == "sync_directory") {
      require(!saved && saved.error().starts_with("The program was replaced"),
              "A directory sync failure reports that replacement already happened");
      require(disk.files.at(path) == "G0 X2\nM2\n", "The new text is visible after the rename");
      FakeToolModelFiles upload_writes{disk};
      upload_writes.failures.emplace(step, 1);
      ProgramWriter upload_writer{programs, upload_writes};
      const auto uploaded = upload_writer.receive("part.ngc", "M30\n", true, {});
      require(!uploaded && uploaded.error().starts_with("Saved on disk. Directory synchronization failed.") &&
                  disk.files.at(path) == "M30\n",
              "An upload reports a directory sync failure separately from publication");
    } else {
      require(!saved && saved.error().starts_with("The program could not be saved."),
              "A failed " + step + " is reported");
      require(disk.files.at(path) == before, "A failed " + step + " keeps the program");
    }
    require(!disk.files.contains("/nc/.part.ngc.coffeecnc-edit"),
            "A failed " + step + " leaves no temporary file");
  }
}

void manages_program_files(const fs::path& base) {
  const fs::path root = base / "program_file_management";
  fs::remove_all(root);
  fs::create_directories(root / "Programs");
  fs::create_directories(root / "USB");
  const auto usb = root / "USB" / "sample.ngc";
  const auto programs = root / "Programs";
  const auto copied = programs / "sample.ngc";
  const std::string bytes = "G21\r\nM2\r\n";
  std::ofstream(usb, std::ios::binary) << bytes;
  const LocalFileSystem files;
  const ProgramStore store{files, root.string(), programs.string()};
  LocalToolModelFiles writes;
  ProgramWriter writer{store, writes};
  using Op = ProgramFileOperation;

  require(writer.manage({Op::copy, usb.string()}, "").has_value() && read(copied) == bytes,
          "A USB program is copied byte for byte to Programs");
  require(!writer.manage({Op::copy, usb.string()}, ""), "Copy never replaces an existing file");
  require(!writer.manage({Op::remove, usb.string()}, "") && fs::exists(usb),
          "The browser cannot delete from the USB source");
  require(!writer.manage({Op::rename, copied.string(), "../away.ngc"}, ""),
          "Rename takes a file name only");
  require(!writer.manage({Op::remove, copied.string()}, copied.string()),
          "The loaded file cannot be deleted");
  require(writer.manage({Op::rename, copied.string(), "part.ngc"}, "").has_value() &&
              read(programs / "part.ngc") == bytes && !fs::exists(copied),
          "The program is renamed in place");
  const auto folder = programs / "new";
  require(writer.manage({Op::create_folder, folder.string()}, "").has_value() && fs::is_directory(folder),
          "The new folder is created");
  std::ofstream(folder / "kept.ngc") << bytes;
  require(!writer.manage({Op::remove, folder.string()}, ""), "A nonempty folder cannot be deleted");
  require(!writer.manage({Op::rename, folder.string(), "changed"}, (folder / "kept.ngc").string()),
          "The folder containing the loaded program cannot be renamed");
  require(writer.manage({Op::remove, (folder / "kept.ngc").string()}, "").has_value() &&
              writer.manage({Op::remove, folder.string()}, "").has_value() && !fs::exists(folder),
          "The file and the empty folder can be deleted");

  const auto refused = writer.receive("part.ngc", "M30\n", true,
      [](const std::string&, const ProgramWriter::Commit&) -> std::expected<void, std::string> {
        return std::unexpected("Stop the loaded program before replacing it.");
      });
  require(!refused && read(programs / "part.ngc") == bytes &&
              std::distance(fs::directory_iterator{programs}, fs::directory_iterator{}) == 1,
          "Refusal leaves old bytes and removes the staged upload");
  require(writer.receive("part.ngc", "M30\n", true,
      [](const std::string&, const ProgramWriter::Commit& commit) { return commit(); }).has_value() &&
              read(programs / "part.ngc") == "M30\n",
          "Accepted replacement publishes the new bytes");
  const auto reopened = writer.receive("part.ngc", bytes, true,
      [](const std::string&, const ProgramWriter::Commit& commit) -> std::expected<void, std::string> {
        if (auto saved = commit(); !saved)
          return saved;
        return std::unexpected("Saved on disk, but not reopened. LinuxCNC refused the open.");
      });
  require(!reopened && reopened.error() == "Saved on disk, but not reopened. LinuxCNC refused the open." &&
              read(programs / "part.ngc") == bytes,
          "A failed reopen preserves the published file and reports the failure accurately");
  fs::remove_all(root);
}

} // namespace

auto main(int argc, char** argv) -> int {
  edits_only_feed_or_speed_numbers();
  replaces_one_line_durably(argc > 1 ? fs::path{argv[1]} : fs::temp_directory_path());
  creates_new_programs(argc > 1 ? fs::path{argv[1]} : fs::temp_directory_path());
  manages_program_files(argc > 1 ? fs::path{argv[1]} : fs::temp_directory_path());
  saves_over_a_program_with_its_stamp(argc > 1 ? fs::path{argv[1]} : fs::temp_directory_path());
  keeps_external_changes_during_sync();
  a_failed_save_keeps_the_program();
  if (failures != 0)
    return EXIT_FAILURE;
  std::cout << "program_edit: all passed\n";
  return EXIT_SUCCESS;
}
