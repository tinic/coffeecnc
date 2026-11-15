#ifndef COFFEECNC_LINUXCNC_PROGRAM_WATCH_HPP
#define COFFEECNC_LINUXCNC_PROGRAM_WATCH_HPP

#include <optional>
#include <string>
#include <string_view>

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

} // namespace coffeecnc

#endif // COFFEECNC_LINUXCNC_PROGRAM_WATCH_HPP
