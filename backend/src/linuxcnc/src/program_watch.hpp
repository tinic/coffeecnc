#ifndef COFFEECNC_LINUXCNC_PROGRAM_WATCH_HPP
#define COFFEECNC_LINUXCNC_PROGRAM_WATCH_HPP

#include <cstdint>
#include <cstddef>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace coffeecnc {

// Comments removed, lower case, no white space: a block as the interpreter's reader sees it.
// Comments are `( ... )` and everything from `;` on.
[[nodiscard]] auto bare_block(std::string_view line) -> std::string;

// Why the preview does not go past this line of a program, if it does not: the text of a line
// the interpreter has read, judged before the line is executed. Pure: no LinuxCNC.
//
// The preview's interpreter starts with the parameters of the file LinuxCNC saved last; task's
// holds what programs and MDI blocks have set since. Which of them a line reads, and whether
// the line runs at all, is known only inside the interpreter, and its public interface does
// not say. So nothing with a parameter is previewed: a line is refused when, outside its
// comments, it has
//
//   a `#`: a parameter read or assigned, numbered, named or computed; also inside a
//   (msg, ...), (debug, ...), (print, ...) or (abort, ...) comment, which the interpreter
//   expands. A `#` in an ordinary comment is text
//   a comment the interpreter acts on by running Python or writing a file: (py, ...),
//   (pyrun, ...), (pyreload), a line `;py,...`, (log, ...), (logopen, ...), (logappend, ...),
//   (logclose), (PROBEOPEN ...), (PROBECLOSE); recognised as the interpreter does, in any
//   case and after white space
//   an O word call with arguments (`o100 call [5]`): they become #1 and up of the subroutine
//   a computed O word (`o[...]`) or G word (`g[...]`)
//   G28, G30 or G92.3: they use positions saved in the parameter file without naming one
//
// A line in a branch that is not taken, or in a subroutine that is never called, is read by
// the interpreter too and is refused all the same.
[[nodiscard]] auto refused_block(std::string_view line) -> std::optional<std::string>;

// A comment that makes the interpreter run Python or write a file, anywhere in a text: the
// first line that has one, from 1, and what it is. The forms, as LinuxCNC 2.9.4 recognises
// them:
//
//   in parentheses, acted on when the block is EXECUTED (Interp::convert_comment,
//   interp_convert.cc:1499: leading white space skipped, any case, the word then its comma):
//   (py,...) (pyrun,...) (pyreload...) run Python; (log,...) (logopen,...) (logappend,...)
//   (logclose) write a file through the canon. (PROBEOPEN ...) and (PROBECLOSE) open and
//   close a file in task's canon (emccanon.cc:2236), not in the interpreter
//   a line that begins `;py,`, acted on when the line is READ (Interp::read_semicolon,
//   interp_read.cc:316), before any check between read and execute could see it
//
// Recognised here a little wider than there: `;py,` also after white space and in any case.
//
// And an O word whose name is not plainly a name. The interpreter looks for the file of a
// called subroutine as `<name>.ngc`, first as given (an absolute path opens that path) and
// after shell-style expansion of the name (wordexp: `~`, `$VAR`, globs, command substitution;
// Interp::find_ngc_file, rs274ngc_pre.cc:2672). A name is therefore only letters, digits, `_`,
// `-` and `.` (after the interpreter's own lower-casing and removal of white space), without
// `..`; a number is a name; a computed O word (`o[...]`, `o#...`) is not; and the name is
// followed by one of the interpreter's words for an O block (sub, call, if, ...). This holds
// for every O word of the text, a definition as much as a call.
//
// A program is to be scanned whole with this before the interpreter reads any of it: the
// first line, from 1, that must not reach the interpreter, and why.
struct Forbidden {
  int line{};
  std::string what;
};
[[nodiscard]] auto first_forbidden_line(std::string_view text) -> std::optional<Forbidden>;

// The name of the O word a line begins with, as the interpreter takes it: lower case, without
// white space; a number as its digits. Nothing when the line has no O word or its name is
// not plainly a name (first_forbidden_line refuses those).
[[nodiscard]] auto o_word_name(std::string_view line) -> std::optional<std::string>;
// Whether that O word is a call.
[[nodiscard]] auto is_call(std::string_view line) -> bool;

// The names a text calls as subroutines (`o<name> call`, `o123 call`) that are not in `known`,
// each once, in the order of their first call. The collection is bounded while it runs: it
// stops at the first name beyond `room` (`too_many`), and when `cancelled`, asked every
// 1024 lines, says so. `examined` is how many bytes of the text it had looked at when it
// stopped: all of them only when it ran to the end. The text has passed first_forbidden_line.
struct CalledNames {
  std::vector<std::string> names;
  bool too_many{};
  bool cancelled{};
  std::size_t examined{};
};
[[nodiscard]] auto called_names(std::string_view text, const std::set<std::string>& known,
                                std::size_t room, const std::function<bool()>& cancelled)
    -> CalledNames;

// What a line does about the motion mode, for a program that starts in a mode the preview
// could not put in place: it names a motion code of its own (G0, G1, G2, G3, G33, G38.x, a
// canned cycle, G80), or it has axis words and none, and so moves by the mode it inherits.
enum class MotionUse : std::uint8_t { none, sets, inherits };
[[nodiscard]] auto motion_use(std::string_view line) -> MotionUse;

} // namespace coffeecnc

#endif // COFFEECNC_LINUXCNC_PROGRAM_WATCH_HPP
