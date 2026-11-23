// Sanitizer suppressions for the programs that open the GTK/WebKit window, in sanitizer builds
// only. Every check stays on for CoffeeCNC's own code.
//
// LeakSanitizer (AddressSanitizer builds). Three libraries of the distribution keep allocations
// for the life of the process, and LeakSanitizer reports them at exit and fails the program.
// Observed with coffeecnc after a clean shutdown (WebKitGTK 2.54.0, Mesa 25.0.7, fontconfig
// 2.15; commit ca88a0a, log kept in build/sim-history/ca88a0a/asan/):
//
//   libfontconfig.so.1               18049 bytes in 643 allocations
//   libGLX_mesa.so.0                108124 bytes in 481 allocations
//   libjavascriptcoregtk-4.1.so.0      360 bytes in  15 allocations
//
// No frame of any of them was in CoffeeCNC's code. A pattern suppresses every leak with a frame
// in that library, so a leak this code causes through one of the three (an object of theirs it
// fails to release) is hidden as well: with these in place the shell's own use of them is not
// leak-checked. Nothing else is suppressed: not GTK, not GLib, not the rest of WebKit.
//
// ThreadSanitizer. GLib and the libraries of GTK and WebKitGTK are not instrumented:
// ThreadSanitizer sees only the calls they make into its interceptors (malloc, free, string
// functions, mutexes), not their atomics, so it reports races between their own threads that
// their own synchronisation orders. Observed in quit_delivery: 116 reports, in every one the
// first frame above the runtime of both accesses in GLib, GIO, GObject, Pango, fontconfig,
// WebKitGTK, JavaScriptCore or libc called from them.
//
// `called_from_lib` ignores the interceptor calls made from inside the named library only. Our
// code is instrumented, and every access it makes is still checked, including accesses to
// memory those libraries allocated. The list is GLib, fontconfig, and every library of the
// window that links GLib: g_rec_mutex_unlock tail-calls pthread_mutex_unlock, so an unlock is
// seen from the library that called GLib (with only GLib listed, GDK's, GdkPixbuf's and Pango's
// unlocks of a GRecMutex GLib had locked were reported as unlocks of an unlocked mutex).
// CoffeeCNC's code takes no GLib lock. Each entry must match one loaded library only.

// The COFFEECNC_*_SANITIZER macros are defined by cmake/ProjectOptions.cmake, whatever the
// compiler: GCC and Clang name those conditions differently.
#if defined(COFFEECNC_ADDRESS_SANITIZER)

extern "C" auto __lsan_default_suppressions() -> const char*;
extern "C" auto __lsan_default_suppressions() -> const char* {
  return "leak:libfontconfig.so\n"
         "leak:libGLX_mesa.so\n"
         "leak:libjavascriptcoregtk-4.1.so\n";
}

#endif

#if defined(COFFEECNC_THREAD_SANITIZER)

extern "C" auto __tsan_default_suppressions() -> const char*;
extern "C" auto __tsan_default_suppressions() -> const char* {
  return "called_from_lib:/libglib-2.0.so\n"
         "called_from_lib:/libgio-2.0.so\n"
         "called_from_lib:/libgobject-2.0.so\n"
         "called_from_lib:/libgmodule-2.0.so\n"
         "called_from_lib:/libfontconfig.so\n"
         "called_from_lib:/libgtk-3.so\n"
         "called_from_lib:/libgdk-3.so\n"
         "called_from_lib:/libgdk_pixbuf-2.0.so\n"
         "called_from_lib:/libatk-1.0.so\n"
         "called_from_lib:/libatk-bridge-2.0.so\n"
         "called_from_lib:/libatspi.so\n"
         "called_from_lib:/libcairo-gobject.so\n"
         "called_from_lib:/libpango-1.0.so\n"
         "called_from_lib:/libpangocairo-1.0.so\n"
         "called_from_lib:/libpangoft2-1.0.so\n"
         "called_from_lib:/libharfbuzz.so\n"
         "called_from_lib:/libcloudproviders.so\n"
         "called_from_lib:/libwebkit2gtk-4.1.so\n"
         "called_from_lib:/libjavascriptcoregtk-4.1.so\n"
         "called_from_lib:/libsoup-3.0.so\n"
         "called_from_lib:/libsecret-1.so\n"
         "called_from_lib:/libenchant-2.so\n"
         "called_from_lib:/libmanette-0.2.so\n"
         "called_from_lib:/libgudev-1.0.so\n"
         "called_from_lib:/libgstreamer-1.0.so\n"
         "called_from_lib:/libgstbase-1.0.so\n"
         "called_from_lib:/libgstallocators-1.0.so\n"
         "called_from_lib:/libgstapp-1.0.so\n"
         "called_from_lib:/libgstaudio-1.0.so\n"
         "called_from_lib:/libgstfft-1.0.so\n"
         "called_from_lib:/libgstgl-1.0.so\n"
         "called_from_lib:/libgstpbutils-1.0.so\n"
         "called_from_lib:/libgsttag-1.0.so\n"
         "called_from_lib:/libgstvideo-1.0.so\n";
}

#endif
