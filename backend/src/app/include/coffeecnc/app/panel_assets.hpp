#ifndef COFFEECNC_APP_PANEL_ASSETS_HPP
#define COFFEECNC_APP_PANEL_ASSETS_HPP

#include <expected>
#include <string>
#include <string_view>

namespace coffeecnc {

// Where the panel is served from, and the only origin there is.
inline constexpr std::string_view panel_scheme = "coffeecnc";
inline constexpr std::string_view panel_origin = "coffeecnc://panel";
inline constexpr std::string_view panel_url = "coffeecnc://panel/index.html";

// One file of the built panel as a request for it is answered.
struct Asset {
  std::string file;              // resolved, inside the panel directory
  std::string_view content_type; // without parameters: `application/wasm` must be exactly that
};

// The media type by file name extension, lower or upper case; `application/octet-stream` for
// anything unknown.
[[nodiscard]] auto content_type_of(std::string_view file) -> std::string_view;

// The file a URL on the panel's scheme names, or the HTTP status that refuses it:
//
//   404  another scheme, another host than `panel` (also with a port or user), a file that does
//        not exist, a directory (nothing is listed); a missing file is never answered with the
//        entry page
//   403  a path that tries to leave the panel directory: a `.` or `..` segment or an empty one
//        (`//etc/passwd`), a backslash or control character, a malformed percent escape, and a
//        symbolic link that resolves outside the directory
//
// The query and fragment are ignored. Only `/` itself is answered with `index.html`. `root` is
// the panel directory; it is resolved on every call. Reads the file system (names only).
[[nodiscard]] auto resolve_asset(const std::string& root, std::string_view url)
    -> std::expected<Asset, int>;

// The URL is one of the panel's own: the only place the window may navigate to.
[[nodiscard]] auto is_panel_url(std::string_view url) -> bool;

} // namespace coffeecnc

#endif // COFFEECNC_APP_PANEL_ASSETS_HPP
