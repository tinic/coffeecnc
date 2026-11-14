#include <coffeecnc/app/program_store.hpp>

#include "fake_file_system.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using namespace coffeecnc;

namespace {
using Lines = std::vector<std::string>;

int checks = 0;
void require(bool condition, const std::string& message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

constexpr auto root = "/home/cnc/nc_files";
constexpr auto outside_reason = "Path is outside the program directory.";

auto in_root(std::string_view name) -> std::string {
  return std::string(root) + '/' + std::string(name);
}

auto names(const Directory& directory) -> Lines {
  Lines found;
  for (const auto& entry : directory.entries)
    found.push_back(entry.name);
  return found;
}

// A program directory with everything a listing must order, skip or refuse, in a file system that
// also holds what must stay out of reach.
auto shop() -> FakeFileSystem {
  FakeFileSystem files;
  files.file(in_root("part.ngc"), "G0 X1\nM2\n", 1700000123456);
  files.file(in_root("Zebra.ngc"), "M2\n", 5);
  files.file(in_root("apple.ngc"), "", -1);
  files.file(in_root("same.ngc"), "b");
  files.file(in_root("Same.ngc"), "a");
  files.directory(in_root("cherry"), 1700000000000);
  files.directory(in_root("Banana"));
  files.file(in_root("cherry/deep.ngc"), "G4 P1\n", 7);
  files.file(in_root(".hidden.ngc"), "M2\n");
  files.directory(in_root(".git"));
  files.other(in_root("pipe"));
  files.file(in_root("caf\xE9.ngc"), "M2\n");    // Latin-1, not UTF-8
  files.file(in_root("two\nlines.ngc"), "M2\n"); // a control character
  files.link(in_root("inside-link"), in_root("cherry/deep.ngc"));
  files.link(in_root("inside-dir"), in_root("cherry"));
  files.link(in_root("escape-file"), "/etc/passwd");
  files.link(in_root("escape-dir"), "/etc");
  files.link(in_root("dangling"), "/nowhere");
  files.link(in_root("loop"), in_root("loop"));
  files.file("/etc/passwd", "root:x:0:0\n");
  files.file("/home/cnc/secret.ngc", "M2\n");
  // A sibling whose name starts with the root's.
  files.file("/home/cnc/nc_files_other/x.ngc", "M2\n");
  return files;
}

void listing() {
  const auto files = shop();
  const ProgramStore store{files, root};

  const auto top = store.list(std::nullopt);
  require(top.has_value(), "The program directory lists: " + top.error_or(""));
  if (!top)
    return;
  require(top->path == root && !top->parent, "A null path is the root, which has no parent");
  require(names(*top) == Lines{"Banana", "cherry", "inside-dir", "apple.ngc", "inside-link",
                               "part.ngc", "Same.ngc", "same.ngc", "Zebra.ngc"},
          "Directories first, then by name without regard to case, then by name; hidden names, "
          "other kinds, names that are not UTF-8 or hold control characters, and links that "
          "dangle or leave the root are not listed");
  require(!top->truncated, "A short listing is complete");
  const auto& cherry = top->entries.at(1);
  require(cherry.kind == EntryKind::directory && cherry.bytes == 0 &&
              cherry.modified == 1700000000000,
          "A directory has no size, and its modification time");
  const auto& part = top->entries.at(5);
  require(part.kind == EntryKind::file && part.bytes == 9 && part.modified == 1700000123456,
          "A file has its size in bytes and its modification time in ms");
  require(top->entries.at(3).modified == -1 && top->entries.at(3).bytes == 0,
          "An empty file from before 1970 is listed as it is");
  const auto& link = top->entries.at(4);
  require(link.kind == EntryKind::file && link.bytes == 6 && link.modified == 7,
          "A link inside the root is listed under its own name as what it leads to");
  require(top->entries.at(2).kind == EntryKind::directory, "A linked directory is a directory");

  const auto below = store.list(in_root("cherry"));
  require(below && below->path == in_root("cherry") && below->parent == root &&
              names(*below) == Lines{"deep.ngc"},
          "A directory below the root names the root as its parent");
  for (const auto* path : {"cherry", "cherry/", "./cherry", "Banana/../cherry", "inside-dir"}) {
    const auto same = store.list(std::string(path));
    require(same && same->path == in_root("cherry") && same->parent == root,
            std::string("Relative to the root, and resolved: ") + path);
  }
  for (const auto* path : {".", "cherry/..", "/home/cnc/nc_files/", "/home/cnc/../cnc/nc_files"}) {
    const auto same = store.list(std::string(path));
    require(same && same->path == root && !same->parent,
            std::string("Another spelling of the root is the root: ") + path);
  }
  const auto empty = store.list(std::string("Banana"));
  require(empty && empty->entries.empty() && !empty->truncated, "An empty directory lists empty");
}

void listing_refusals() {
  auto files = shop();
  files.unreadable.emplace_back(in_root("Banana"));
  const ProgramStore store{files, root};
  const std::vector<std::pair<std::string, std::string>> rows{
      {"..", outside_reason},
      {"../..", outside_reason},
      {"cherry/../..", outside_reason},
      {"../nc_files_other", outside_reason},
      {"/", outside_reason},
      {"/home/cnc", outside_reason},
      {"/etc", outside_reason},
      {"/home/cnc/nc_files_other", outside_reason},
      {"/home/cnc/nc_files/../nc_files_other", outside_reason},
      {"escape-dir", outside_reason},
      {in_root("escape-dir"), outside_reason},
      {"", "Path is empty."},
      {std::string("cherry\0/..", 10), "Path contains a NUL byte."},
      {"missing", "Path is not available: No such file or directory"},
      {"dangling", "Path is not available: No such file or directory"},
      {"loop", "Path is not available: Too many levels of symbolic links"},
      {"part.ngc/..", "Path is not available: Not a directory"},
      {"part.ngc", "Path is not a directory."},
      {"pipe", "Path is not a directory."},
      {"Banana", "Directory cannot be read: Permission denied"},
      {std::string(5000, 'a'), "Path is not available: No such file or directory"},
  };
  for (const auto& [path, reason] : rows) {
    const auto refused = store.list(path);
    require(!refused.has_value() && refused.error_or("") == reason,
            std::format("Listing \"{}\" is refused with \"{}\" but got \"{}\"", path, reason,
                        refused.error_or("a listing")));
  }
  for (const auto& call : files.calls)
    require(!call.starts_with("names /etc") && !call.starts_with("names /home/cnc/nc_files_other"),
            "Nothing outside the root is read: " + call);
}

void roots() {
  auto files = shop();
  require(ProgramStore{files, ""}.list(std::nullopt).error_or("") ==
                  "No program directory is configured." &&
              ProgramStore{files, ""}.resolve_program(in_root("part.ngc")).error_or("") ==
                  "No program directory is configured.",
          "Without a configured directory everything is refused");
  require(ProgramStore{files, "/gone"}.list(std::nullopt).error_or("") ==
              "The program directory is not available: No such file or directory",
          "A root that does not exist refuses everything");
  require(ProgramStore{files, in_root("part.ngc")}.list(std::nullopt).error_or("") ==
              "Path is not a directory.",
          "A root that is a file lists nothing");

  files.link("/nc", root);
  const ProgramStore linked{files, "/nc"};
  const auto top = linked.list(std::nullopt);
  require(top && top->path == root && !top->parent, "A root that is a link is named by its target");
  require(linked.resolve_program("/nc/part.ngc") == in_root("part.ngc") &&
              linked.resolve_program(in_root("part.ngc")) == in_root("part.ngc"),
          "Below a linked root, both spellings resolve to the target");
  require(linked.list(std::string("/home/cnc")).error_or("") == outside_reason,
          "The linked root's real parent is outside");

  const ProgramStore whole{files, "/"};
  const auto slash = whole.list(std::nullopt);
  require(slash && slash->path == "/" && !slash->parent &&
              names(*slash) == Lines{"etc", "home", "nc"},
          "A root of / lists, and a link out of nothing is not possible");
  const auto home = whole.list(std::string("/home"));
  require(home && home->parent == "/", "Directly below / the parent is /");

  // The root's own name is not UTF-8: the panel could not name anything below it.
  files.file("/caf\xE9/part.ngc", "M2\n");
  require(ProgramStore{files, "/caf\xE9"}.list(std::nullopt).error_or("") ==
              "Path is not UTF-8 text.",
          "A resolved path that is not UTF-8 is refused");
  files.link(in_root("latin"), "/caf\xE9");
  require(ProgramStore{files, "/"}.resolve_program(in_root("latin/part.ngc")).error_or("") ==
              "Path is not UTF-8 text.",
          "Also when a UTF-8 request resolves to it through a link");
}

void listing_caps() {
  const auto fill = [](FakeFileSystem& files, std::size_t count) -> void {
    for (std::size_t i = 0; i < count; ++i)
      files.file(in_root("many/f" + std::to_string(100000 + i) + ".ngc"), "M2\n");
  };
  {
    FakeFileSystem files;
    fill(files, ProgramStore::max_directory_entries);
    const auto full = ProgramStore{files, root}.list(std::string("many"));
    require(full && full->entries.size() == ProgramStore::max_directory_entries && !full->truncated,
            "A directory with exactly the cap is complete");
  }
  {
    FakeFileSystem files;
    fill(files, ProgramStore::max_directory_entries + 5);
    files.directory(in_root("many/zz-last"));
    const auto cut = ProgramStore{files, root}.list(std::string("many"));
    require(cut && cut->entries.size() == ProgramStore::max_directory_entries && cut->truncated,
            "A directory with more entries than the cap is listed truncated");
    require(cut && cut->entries.front().name == "zz-last" &&
                cut->entries.back().name == "f100998.ngc",
            "The cap keeps the first entries of the sorted listing");
  }
  {
    FakeFileSystem files;
    // More names than are read; all but a few are hidden.
    for (std::size_t i = 0; i < ProgramStore::max_directory_names; ++i)
      files.file(in_root("many/.h" + std::to_string(i)), "");
    files.file(in_root("many/a.ngc"), "");
    files.file(in_root("many/b.ngc"), "");
    const auto cut = ProgramStore{files, root}.list(std::string("many"));
    require(cut && cut->truncated && cut->entries.size() < 2,
            "Names beyond the read cap are not listed, and the listing says it is truncated");
  }
}

void open_rules() {
  auto files = shop();
  const ProgramStore store{files, root};
  const auto part = in_root("part.ngc");
  for (const auto* path : {"part.ngc", "/home/cnc/nc_files/part.ngc", "cherry/../part.ngc",
                           "./part.ngc", "/home/cnc/nc_files/../nc_files/part.ngc"}) {
    const auto resolved = store.resolve_program(path);
    require(resolved == part, std::string("Resolves to the absolute path: ") + path);
  }
  require(store.resolve_program("inside-link") == in_root("cherry/deep.ngc") &&
              store.resolve_program("inside-dir/deep.ngc") == in_root("cherry/deep.ngc"),
          "A link inside the root resolves to its target");

  const std::vector<std::pair<std::string, std::string>> rows{
      {"/etc/passwd", outside_reason},
      {"../secret.ngc", outside_reason},
      {"/home/cnc/secret.ngc", outside_reason},
      {"cherry/../../secret.ngc", outside_reason},
      {"/home/cnc/nc_files_other/x.ngc", outside_reason},
      {"../nc_files_other/x.ngc", outside_reason},
      {"escape-file", outside_reason},
      {"escape-dir/passwd", outside_reason},
      {in_root("escape-dir/passwd"), outside_reason},
      {"cherry", "Path is not a regular file."},
      {".", "Path is not a regular file."},
      {"pipe", "Path is not a regular file."},
      {"missing.ngc", "Path is not available: No such file or directory"},
      {"dangling", "Path is not available: No such file or directory"},
      {"part.ngc/x", "Path is not available: Not a directory"},
      {"", "Path is empty."},
      {std::string("part.ngc\0x", 10), "Path contains a NUL byte."},
      {std::string(5000, 'a'), "Path is not available: No such file or directory"},
  };
  for (const auto& [path, reason] : rows) {
    const auto refused = store.resolve_program(path);
    require(!refused.has_value() && refused.error_or("") == reason,
            std::format("Opening \"{}\" is refused with \"{}\" but got \"{}\"", path, reason,
                        refused.error_or("a path")));
    const auto unread = store.read_program(path, 1);
    require(!unread.has_value() && unread.error_or("") == reason,
            "Reading \"" + path + "\" is refused the same way");
  }
  for (const auto& call : files.calls)
    require(!call.starts_with("read "), "A refused program is never read: " + call);
  // Hidden and oddly named files are not listed, but they are inside the root.
  require(store.resolve_program(".hidden.ngc") == in_root(".hidden.ngc"),
          "A hidden file inside the root can be opened by name");

  // LinuxCNC takes 254 bytes: the resolved path counts, however short the request.
  const std::string prefix = in_root("long/");
  const auto longest =
      prefix + std::string(ProgramStore::max_program_path_bytes - prefix.size(), 'n');
  files.file(longest, "M2\n");
  files.file(longest + 'x', "M2\n");
  files.link(in_root("short"), longest + 'x');
  require(longest.size() == 254 && store.resolve_program(longest) == longest,
          "A resolved path of 254 bytes is accepted");
  for (const auto& path : std::vector<std::string>{longest + 'x', "short"})
    require(store.resolve_program(path).error_or("") == "Program path is longer than 254 bytes.",
            "A resolved path of 255 bytes is refused: " + path);
  const auto listed = store.list(std::string("long"));
  require(listed && listed->entries.size() == 2,
          "A file too long to open is still listed; opening it says why it cannot be");
}

void program_text() {
  FakeFileSystem files;
  const ProgramStore store{files, root};
  const auto read = [&files, &store](std::string content) -> ProgramText {
    files.file(in_root("p.ngc"), std::move(content));
    const auto program = store.read_program("p.ngc", 9);
    require(program.has_value(), "The program reads: " + program.error_or(""));
    return program ? *program : ProgramText{};
  };

  const auto plain = read("G0 X1\nM2\n");
  require(plain.revision == 9 && plain.file == in_root("p.ngc") && !plain.truncated,
          "The text carries the caller's revision and the resolved path");
  const std::vector<std::pair<std::string, Lines>> splits{
      {"", {}},
      {"M2", {"M2"}},
      {"M2\n", {"M2"}},
      {"G0 X1\nM2\n", {"G0 X1", "M2"}},
      {"G0 X1\r\nM2\r\n", {"G0 X1", "M2"}},
      {"G0 X1\rM2\r", {"G0 X1", "M2"}},
      {"G0 X1\r\nM2", {"G0 X1", "M2"}},
      {"a\n\nb\n", {"a", "", "b"}},
      {"a\n\n", {"a", ""}},
      {"\n", {""}},
      {"\r\n\r\n", {"", ""}},
      {"a\n\rb", {"a", "", "b"}},
      {"a\r\r\nb", {"a", "", "b"}},
      {" a \t\n", {" a \t"}},
      {std::string("a\0b\n", 4), {std::string("a\0b", 3)}},
      {"(pi\xC3\xA8"
       "ce)\n",
       {"(pi\xC3\xA8"
        "ce)"}},
  };
  for (const auto& [content, expected] : splits)
    require(read(content).lines == expected, "Lines of: " + content);

  constexpr std::string_view bad = "\xEF\xBF\xBD";
  const auto fffd = [bad](std::size_t count) -> std::string {
    std::string text;
    for (std::size_t i = 0; i < count; ++i)
      text += bad;
    return text;
  };
  const std::vector<std::pair<std::string, std::string>> repairs{
      {"Fr\xE4ser", "Fr" + fffd(1) + "ser"}, // Latin-1
      {"\x80", fffd(1)},                     // a continuation byte alone
      {"\xC3", fffd(1)},                     // a sequence cut short by the end
      {"\xE2\x82(", fffd(2) + "("},          // cut short by another character
      {"\xC0\xAF", fffd(2)},                 // overlong /
      {"\xE0\x80\xAF", fffd(3)},             // overlong /
      {"\xED\xA0\x80", fffd(3)},             // a surrogate
      {"\xF4\x90\x80\x80", fffd(4)},         // above U+10FFFF
      {"\xF8\x88\x80\x80\x80", fffd(5)},     // a five-byte form
      {"\xFF\xFE", fffd(2)},                 // a UTF-16 byte order mark
      {"\xE2\x82\xAC \xF0\x9F\x98\x80", "\xE2\x82\xAC \xF0\x9F\x98\x80"}, // valid: kept
      {"\xEF\xBB\xBFG0", "\xEF\xBB\xBFG0"}, // a UTF-8 byte order mark is text
  };
  for (const auto& [content, expected] : repairs) {
    const auto first = read(content + "\nM2\n").lines;
    require(first == Lines{expected, "M2"}, "One U+FFFD per byte that is not UTF-8: " + expected);
    require(read(content + "\nM2\n").lines == first, "And the same every time");
  }

  // The cap. Lines of 16 bytes with their line end.
  constexpr auto cap = ProgramStore::max_program_bytes;
  std::string big;
  while (big.size() < cap)
    big += "G1 X1.0 Y2.0 F3\n";
  require(big.size() == cap, "The test text is exactly the cap");
  const auto whole = read(big);
  require(!whole.truncated && whole.lines.size() == cap / 16,
          "A program of exactly the cap is whole");
  const auto cut = read(big + "G1 X5\nM2\n");
  require(cut.truncated && cut.lines.size() == cap / 16 && cut.lines.back() == "G1 X1.0 Y2.0 F3",
          "A longer program is cut at the cap and says so");
  const auto partial = read(big.substr(8) + "G1 X5 Y6 Z7 F8 S9\nM2\n");
  require(partial.truncated && partial.lines.size() == cap / 16 &&
              partial.lines.back() == "G1 X1.0 Y2.0 F3",
          "The line the cap falls in is left out, not shown cut");
  const auto one_line = read(std::string(cap + 100, 'X'));
  require(one_line.truncated && one_line.lines.size() == 1 && one_line.lines.front().size() == cap,
          "A first line longer than the cap is the one exception");
  for (const auto& call : files.calls)
    require(!call.starts_with("read ") || call == "read " + in_root("p.ngc"),
            "Only the resolved program is read: " + call);
}

// The same rules on the host's file system, in a directory made for the test.
void on_the_host(const std::filesystem::path& scratch) {
  namespace fs = std::filesystem;
  const auto base = scratch / "program_store_tests.dir";
  fs::remove_all(base);
  fs::create_directories(base / "root" / "sub");
  fs::create_directories(base / "root" / ".hidden");
  fs::create_directories(base / "outside");
  const auto write = [](const fs::path& path, std::string_view content) -> void {
    std::ofstream{path, std::ios::binary} << content;
  };
  write(base / "root" / "part.ngc", "G0 X1\r\nFr\xE4ser\nM2\n");
  write(base / "root" / "Apple.ngc", "M2\n");
  write(base / "root" / "sub" / "deep.ngc", "G4 P1\n");
  write(base / "root" / ".dotfile", "M2\n");
  write(base / "outside" / "secret.ngc", "M2\n");
  fs::create_symlink(fs::path{"sub"} / "deep.ngc", base / "root" / "inside-link");
  fs::create_symlink(fs::path{".."} / "outside" / "secret.ngc", base / "root" / "escape-file");
  fs::create_directory_symlink(fs::path{".."} / "outside", base / "root" / "escape-dir");
  fs::create_symlink(base / "outside", base / "root" / "escape-absolute");
  fs::create_symlink("nowhere", base / "root" / "dangling");
  fs::create_directory_symlink(base / "root", base / "linked-root");
  const auto stamp =
      std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{1700000123000}};
  fs::last_write_time(base / "root" / "part.ngc", std::chrono::file_clock::from_sys(stamp));

  const auto top = fs::canonical(base / "root").string();
  const auto outside = fs::canonical(base / "outside").string();
  const LocalFileSystem files;
  const ProgramStore store{files, (base / "root").string()};

  const auto listed = store.list(std::nullopt);
  require(listed.has_value(), "The host directory lists: " + listed.error_or(""));
  if (listed) {
    require(listed->path == top && !listed->parent, "The root is reported resolved");
    require(names(*listed) == Lines{"sub", "Apple.ngc", "inside-link", "part.ngc"},
            "Sorted; hidden names, dangling links and links out of the root are left out");
    const auto& part = listed->entries.back();
    require(part.kind == EntryKind::file && part.bytes == 17 && part.modified == 1700000123000,
            "Size in bytes and modification time in ms since the epoch, from the host");
    require(listed->entries.at(2).kind == EntryKind::file && listed->entries.at(2).bytes == 6,
            "A link inside the root is listed as its target");
  }
  const auto below = store.list(std::string("sub"));
  require(below && below->path == top + "/sub" && below->parent == top &&
              names(*below) == Lines{"deep.ngc"},
          "A directory below the root");

  for (const auto& path :
       std::vector<std::string>{"..", "sub/../..", outside, "/", "escape-dir", "escape-absolute",
                                top + "/escape-dir", top + "/../outside"})
    require(store.list(path).error_or("") == outside_reason,
            "Listing outside the root is refused: " + path);
  for (const auto& path : std::vector<std::string>{
           "../outside/secret.ngc", outside + "/secret.ngc", "escape-file", "escape-dir/secret.ngc",
           "escape-absolute/secret.ngc", top + "/escape-absolute/secret.ngc",
           "sub/../../outside/secret.ngc", "/etc/hosts"}) {
    require(store.resolve_program(path).error_or("") == outside_reason,
            "Opening outside the root is refused: " + path);
    require(store.read_program(path, 1).error_or("") == outside_reason,
            "Reading outside the root is refused: " + path);
  }
  require(store.resolve_program("part.ngc") == top + "/part.ngc" &&
              store.resolve_program("sub/../part.ngc") == top + "/part.ngc" &&
              store.resolve_program("inside-link") == top + "/sub/deep.ngc",
          "Programs inside the root resolve to their real absolute path");
  require(!store.resolve_program("sub").has_value() &&
              !store.resolve_program("dangling").has_value() &&
              !store.resolve_program("missing.ngc").has_value() &&
              !store.resolve_program(std::string(5000, 'a')).has_value() &&
              !store.list(std::string("part.ngc")).has_value(),
          "A directory, a dangling link, a missing file and an impossible name are refused");

  const auto text = store.read_program("part.ngc", 4);
  require(text && text->lines == Lines{"G0 X1", "Fr\xEF\xBF\xBDser", "M2"} && text->revision == 4 &&
              text->file == top + "/part.ngc" && !text->truncated,
          "A program is read from the host: CRLF, LF and a byte that is not UTF-8");

  const ProgramStore linked{files, (base / "linked-root").string()};
  const auto through = linked.list(std::nullopt);
  require(through && through->path == top && !through->parent,
          "A root given as a link is its target");
  require(linked.list(fs::canonical(base).string()).error_or("") == outside_reason,
          "And its real parent is outside");

  // The adapter itself.
  require(files.read(top + "/part.ngc", 5) == "G0 X1" &&
              files.read(top + "/part.ngc", 1000).value_or("").size() == 17 &&
              files.read(top + "/part.ngc", 0) == "" &&
              !files.read(top + "/missing", 10).has_value(),
          "read returns at most the limit, and fails for a missing file");
  const auto few = files.names(top, 2);
  const auto all = files.names(top, 100);
  require(few && few->names.size() == 2 && few->more && all && !all->more &&
              all->names.size() == 10 && !files.names(top + "/missing", 10).has_value() &&
              !files.names(top + "/part.ngc", 10).has_value(),
          "names stops at the limit and says there are more; it fails for anything else");
  const auto sub = files.info(top + "/sub");
  const auto escape = files.info(top + "/escape-dir");
  require(sub && sub->kind == FileKind::directory && escape &&
              escape->kind == FileKind::directory && !files.info(top + "/dangling").has_value() &&
              !files.canonical(top + "/dangling").has_value(),
          "info follows links; a dangling link has none");

  // A name as long as the host allows: listed, and too long to open once the root is added.
  const std::string long_name(200, 'n');
  fs::create_directories(base / "root" / "deep" / long_name);
  write(base / "root" / "deep" / long_name / "p.ngc", "M2\n");
  const auto deep = store.list("deep/" + long_name);
  require(deep && names(*deep) == Lines{"p.ngc"}, "A very long directory name lists");
  const auto long_path = top + "/deep/" + long_name + "/p.ngc";
  require(store.resolve_program(long_path).has_value() ==
              (long_path.size() <= ProgramStore::max_program_path_bytes),
          "A real path is accepted up to 254 bytes and refused beyond");

  // Not every host stores a name that is not UTF-8 (APFS refuses it).
  std::ofstream latin1{base / "root" / "caf\xE9.ngc", std::ios::binary};
  if (latin1.is_open()) {
    latin1.close();
    const auto again = store.list(std::nullopt);
    require(again && std::ranges::none_of(
                         again->entries,
                         [](const DirectoryEntry& e) -> bool { return e.name.starts_with("caf"); }),
            "A host name that is not UTF-8 is not listed");
  }
  fs::remove_all(base);
}
} // namespace

// The only argument is a directory the test may write below (the build directory).
auto main(int argc, char** argv) -> int {
  try {
    listing();
    listing_refusals();
    roots();
    listing_caps();
    open_rules();
    program_text();
    if (argc > 1)
      on_the_host(argv[1]);
    else
      throw std::runtime_error("Usage: coffeecnc_program_store_tests <writable directory>");
    std::cout << checks << " program store checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Check " << checks << " failed: " << error.what() << '\n';
    return 1;
  }
}
