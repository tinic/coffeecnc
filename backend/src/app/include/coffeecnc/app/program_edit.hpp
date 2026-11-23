#ifndef COFFEECNC_APP_PROGRAM_EDIT_HPP
#define COFFEECNC_APP_PROGRAM_EDIT_HPP

#include <coffeecnc/app/program_store.hpp>
#include <coffeecnc/app/tool_model_store.hpp>

#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace coffeecnc {

// Whether `after` is `before` with only the number of one F (feed) or S (spindle speed) word
// changed, outside comments, to a plain decimal number. Nothing else of the line may differ, so
// an edit can never add motion, a tool change or any other code.
[[nodiscard]] auto feed_or_speed_edit(std::string_view before, std::string_view after) -> bool;

// Writes programs below the program directory.
//
// replace_line replaces one line of a program, durably: a temporary file in the
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

  // Writes a new program, durably as above: `text` exactly, to a path ProgramStore's
  // resolve_new_program accepts. Something of that name is replaced only with `replace`;
  // without it, a name taken in the meantime is not written over either (link, not rename).
  // The resolved path of the program written, or why nothing was written.
  [[nodiscard]] auto create(std::string_view path, std::string_view text, bool replace)
      -> std::expected<std::string, std::string>;

  // Writes the program editor's `text` over the existing program `path`, durably as
  // replace_line. Check `stamp` before writing and check the bytes again after syncing the
  // temporary file. An external writer can still change the file between the last check and
  // the rename. `text` has `\n` line ends; a file
  // whose first line ends in `\r\n` keeps that line end on every line. No backup is kept: the
  // rename leaves either the old file or the new one, never a part of either.
  // The final controller check returns a refusal reason or an empty string to proceed.
  [[nodiscard]] auto replace(std::string_view path, std::string_view text, std::string_view stamp,
                             const std::function<std::string()>& check_controller = {})
      -> std::expected<std::string, std::string>;

private:
  // Replaces the file `resolved` with `bytes` through `<directory>/.<name><suffix>`.
  [[nodiscard]] auto write_over(const std::string& resolved, std::string_view bytes,
                                std::string_view suffix,
                                std::optional<std::string_view> previous = std::nullopt,
                                const std::function<std::string()>& check_controller = {})
      -> std::expected<void, std::string>;

  const ProgramStore& programs_;
  ToolModelFiles& files_;
};

} // namespace coffeecnc

#endif // COFFEECNC_APP_PROGRAM_EDIT_HPP
