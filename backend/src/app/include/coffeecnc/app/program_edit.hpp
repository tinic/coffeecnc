#ifndef COFFEECNC_APP_PROGRAM_EDIT_HPP
#define COFFEECNC_APP_PROGRAM_EDIT_HPP

#include <coffeecnc/app/program_store.hpp>
#include <coffeecnc/app/tool_model_store.hpp>

#include <expected>
#include <string>
#include <string_view>

namespace coffeecnc {

// Whether `after` is `before` with only the number of one F (feed) or S (spindle speed) word
// changed, outside comments, to a plain decimal number. Nothing else of the line may differ, so
// an edit can never add motion, a tool change or any other code.
[[nodiscard]] auto feed_or_speed_edit(std::string_view before, std::string_view after) -> bool;

// Replaces one line of a program below the program directory, durably: a temporary file in the
// same directory, fsync, rename over the program, fsync of the directory. The line must still
// read `before` and the change must be a feed_or_speed_edit. Line ends are kept as they are.
// The replaced file is readable and writable by its owner only (mode 0600).
class ProgramWriter {
public:
  // Both must outlive the writer. One caller at a time.
  ProgramWriter(const ProgramStore& programs, ToolModelFiles& files);

  // The resolved path of the program written, or why nothing was written.
  [[nodiscard]] auto replace_line(std::string_view path, int line, std::string_view before,
                                  std::string_view after) -> std::expected<std::string, std::string>;

private:
  const ProgramStore& programs_;
  ToolModelFiles& files_;
};

} // namespace coffeecnc

#endif // COFFEECNC_APP_PROGRAM_EDIT_HPP
