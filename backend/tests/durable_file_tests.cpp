// The adapter's file replacement (tool table and sidecar records): whole contents, the mode,
// never through a link planted at the temporary name, and what a failure leaves.
//   coffeecnc_durable_file_tests <scratch directory>

#include "../src/linuxcnc/src/durable_file.hpp"

#include <coffeecnc/core/text_file.hpp>

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include <sys/stat.h>

using namespace coffeecnc;

namespace {

int checks = 0;

void require(bool condition, const std::string& message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

auto text(const std::filesystem::path& file) -> std::string {
  return read_text_file(file.string(), 1 << 20).value_or("<unreadable>");
}

void write(const std::filesystem::path& file, const std::string& bytes) {
  std::ofstream out{file, std::ios::binary | std::ios::trunc};
  out << bytes;
}

} // namespace

auto main(int argc, char** argv) -> int {
  try {
    if (argc != 2)
      throw std::runtime_error("usage: coffeecnc_durable_file_tests <scratch directory>");
    const auto scratch = std::filesystem::path(argv[1]) / "durable_file";
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch);
    const auto table = scratch / "tool.tbl";

    ::umask(022);
    require(replace_synced(table.string(), "T1 P1 Z50\n").has_value() &&
                text(table) == "T1 P1 Z50\n" && !std::filesystem::exists(scratch / "tool.tbl.new"),
            "A new file holds what was written; no temporary is left");
    struct stat info{};
    require(::stat(table.c_str(), &info) == 0 && (info.st_mode & 07777) == 0644,
            "Mode 0666 less the umask, as before");
    require(replace_synced(table.string(), "T2 P2 Z40\n").has_value() &&
                text(table) == "T2 P2 Z40\n",
            "An existing file is replaced whole");

    // Not through a link planted at the temporary name: its target stays as it was, and so
    // does the file.
    const auto victim = scratch / "victim";
    write(victim, "keep\n");
    std::filesystem::create_symlink(victim, scratch / "tool.tbl.new");
    const auto refused = replace_synced(table.string(), "T3 P3 Z30\n");
    require(!refused && refused.error().find("tool.tbl.new") != std::string::npos,
            "A link at the temporary name is refused, named: " +
                (refused ? std::string() : refused.error()));
    require(text(victim) == "keep\n" && text(table) == "T2 P2 Z40\n",
            "Neither the link's target nor the file is touched");
    std::filesystem::remove(scratch / "tool.tbl.new");

    // The two steps apart, as the tool table load uses them.
    const auto temporary = (scratch / "tool.tbl.coffeecnc").string();
    require(write_synced(temporary, "T4 P4 Z20\n").has_value() && text(table) == "T2 P2 Z40\n",
            "Writing the temporary leaves the file alone");
    require(rename_synced(temporary, table.string()).has_value() && text(table) == "T4 P4 Z20\n" &&
                !std::filesystem::exists(temporary),
            "The rename puts it in place");

    require(!write_synced((scratch / "missing" / "tool.tbl").string(), "x") &&
                !rename_synced((scratch / "none").string(), table.string()) &&
                text(table) == "T4 P4 Z20\n",
            "A file that cannot be made, or a rename without a source, fails and changes nothing");
    std::cout << checks << " durable file checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Check " << checks << " failed: " << error.what() << '\n';
    return 1;
  }
}
