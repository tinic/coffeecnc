// LeakSanitizer suppressions for the programs that open the GTK/WebKit window, in
// AddressSanitizer builds only. Leak detection stays on.
//
// Three libraries of the distribution keep allocations for the life of the process, and
// LeakSanitizer reports them at exit and fails the program. Observed with coffeecnc after a
// clean shutdown (WebKitGTK 2.54.0, Mesa 25.0.7, fontconfig 2.15; commit ca88a0a, log kept in
// build/sim-history/ca88a0a/asan/):
//
//   libfontconfig.so.1               18049 bytes in 643 allocations
//   libGLX_mesa.so.0                108124 bytes in 481 allocations
//   libjavascriptcoregtk-4.1.so.0      360 bytes in  15 allocations
//
// No frame of any of them was in CoffeeCNC's code. A pattern suppresses every leak with a frame
// in that library, so a leak this code causes through one of the three (an object of theirs it
// fails to release) is hidden as well: with these in place the shell's own use of them is not
// leak-checked. Nothing else is suppressed: not GTK, not GLib, not the rest of WebKit.

// COFFEECNC_ADDRESS_SANITIZER is defined by cmake/ProjectOptions.cmake for builds with
// -fsanitize=address, whatever the compiler: GCC and Clang name that condition differently.
#if defined(COFFEECNC_ADDRESS_SANITIZER)

extern "C" auto __lsan_default_suppressions() -> const char*;
extern "C" auto __lsan_default_suppressions() -> const char* {
  return "leak:libfontconfig.so\n"
         "leak:libGLX_mesa.so\n"
         "leak:libjavascriptcoregtk-4.1.so\n";
}

#endif
