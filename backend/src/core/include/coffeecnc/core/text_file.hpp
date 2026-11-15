#ifndef COFFEECNC_CORE_TEXT_FILE_HPP
#define COFFEECNC_CORE_TEXT_FILE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <fstream>
#include <string>

namespace coffeecnc {

enum class TextFileError : std::uint8_t {
  unreadable, // cannot be opened, or reading failed
  too_large,  // has more than the limit; nothing of it is returned
};

// The whole contents of a file, when it has at most `limit` bytes. Every place that takes a
// file into memory reads through this: with a bound, and in blocks with read(). (A string built
// from std::istreambuf_iterator has neither a bound nor, with GCC 14 at -O2 and above, a clean
// -Wnull-dereference: the end iterator holds a null stream buffer by design.)
[[nodiscard]] inline auto read_text_file(const std::string& path, std::size_t limit)
    -> std::expected<std::string, TextFileError> {
  std::ifstream in{path, std::ios::binary};
  if (!in)
    return std::unexpected(TextFileError::unreadable);
  std::string text;
  std::array<char, 65536> block{};
  while (true) {
    in.read(block.data(), static_cast<std::streamsize>(block.size()));
    const auto got = static_cast<std::size_t>(in.gcount());
    if (in.bad())
      return std::unexpected(TextFileError::unreadable);
    if (got > limit - text.size())
      return std::unexpected(TextFileError::too_large);
    text.append(block.data(), got);
    if (in.eof())
      return text;
    // Not at the end and not a full block: the read failed.
    if (in.fail())
      return std::unexpected(TextFileError::unreadable);
  }
}

} // namespace coffeecnc

#endif // COFFEECNC_CORE_TEXT_FILE_HPP
