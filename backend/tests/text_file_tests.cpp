// read_text_file: whole files in blocks, the bound, and what cannot be read.
//   coffeecnc_text_file_tests <scratch directory>

#include <coffeecnc/core/text_file.hpp>

#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace coffeecnc;

namespace {

int checks = 0;

void require(bool condition, const std::string& message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

auto written(const std::filesystem::path& file, const std::string& text) -> std::string {
  std::ofstream out{file, std::ios::binary | std::ios::trunc};
  out << text;
  return file.string();
}

} // namespace

auto main(int argc, char** argv) -> int {
  try {
    if (argc != 2)
      throw std::runtime_error("usage: coffeecnc_text_file_tests <scratch directory>");
    const auto scratch = std::filesystem::path(argv[1]) / "text_file";
    std::filesystem::create_directories(scratch);
    constexpr std::size_t block = 65536;

    const auto empty = read_text_file(written(scratch / "empty", ""), 10);
    require(empty && empty->empty(), "an empty file is an empty text");
    require(read_text_file(written(scratch / "none", ""), 0).has_value(),
            "an empty file fits a limit of 0");

    const std::string line{"T1 P1 Z50 D6 ;6 mm end mill\n"};
    require(read_text_file(written(scratch / "short", line), 1000) == line, "a short file");
    require(read_text_file(scratch / "short", line.size()) == line, "exactly the limit fits");
    require(read_text_file(scratch / "short", line.size() - 1) ==
                std::unexpected(TextFileError::too_large),
            "one byte above the limit is too large");

    // Across block boundaries, with every byte value: one block, one more byte, several.
    for (const std::size_t size : {block - 1, block, block + 1, 3 * block + 17}) {
      std::string bytes(size, '\0');
      for (std::size_t i = 0; i < size; ++i)
        bytes[i] = static_cast<char>(i * 31 + i / block);
      const auto file = written(scratch / "blocks", bytes);
      require(read_text_file(file, size) == bytes, "a file of " + std::to_string(size));
      require(read_text_file(file, size - 1) == std::unexpected(TextFileError::too_large),
              "the limit below " + std::to_string(size));
      require(read_text_file(file, block / 2) == std::unexpected(TextFileError::too_large),
              "a limit inside the first block of " + std::to_string(size));
    }

    require(read_text_file(scratch / "missing", 10) == std::unexpected(TextFileError::unreadable),
            "a missing file is unreadable");
    require(read_text_file(scratch, 10) == std::unexpected(TextFileError::unreadable),
            "a directory is unreadable");
    std::cout << checks << " text file checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Check " << checks << " failed: " << error.what() << '\n';
    return 1;
  }
}
