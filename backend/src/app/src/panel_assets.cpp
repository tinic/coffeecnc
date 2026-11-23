#include <coffeecnc/app/panel_assets.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <system_error>
#include <utility>

namespace coffeecnc {
namespace {

constexpr int forbidden = 403;
constexpr int not_found = 404;

constexpr std::array<std::pair<std::string_view, std::string_view>, 17> content_types{{
    {".html", "text/html"},
    {".js", "text/javascript"},
    {".mjs", "text/javascript"},
    {".css", "text/css"},
    {".json", "application/json"},
    {".map", "application/json"},
    {".wasm", "application/wasm"},
    {".svg", "image/svg+xml"},
    {".png", "image/png"},
    {".jpg", "image/jpeg"},
    {".ico", "image/x-icon"},
    {".woff2", "font/woff2"},
    {".woff", "font/woff"},
    {".ttf", "font/ttf"},
    {".txt", "text/plain"},
    {".step", "model/step"},
    {".stp", "model/step"},
}};

auto hex(char digit) -> std::optional<int> {
  if (digit >= '0' && digit <= '9')
    return digit - '0';
  if (digit >= 'a' && digit <= 'f')
    return digit - 'a' + 10;
  if (digit >= 'A' && digit <= 'F')
    return digit - 'A' + 10;
  return {};
}

// The path with its percent escapes resolved; nothing for a malformed escape.
auto unescaped(std::string_view path) -> std::optional<std::string> {
  std::string text;
  for (std::size_t i = 0; i < path.size(); ++i) {
    if (path[i] != '%') {
      text.push_back(path[i]);
      continue;
    }
    const auto high = i + 2 < path.size() ? hex(path[i + 1]) : std::nullopt;
    const auto low = high ? hex(path[i + 2]) : std::nullopt;
    if (!low)
      return {};
    text.push_back(static_cast<char>(*high * 16 + *low));
    i += 2;
  }
  return text;
}

// The path after `coffeecnc://panel`, starting with `/`; nothing for another scheme or host.
auto path_of(std::string_view url) -> std::optional<std::string_view> {
  if (!url.starts_with(panel_origin))
    return {};
  url.remove_prefix(panel_origin.size());
  url = url.substr(0, url.find_first_of("?#"));
  if (url.empty())
    return std::string_view{"/"};
  if (url.front() != '/')
    return {}; // `coffeecnc://panel:80/`, `coffeecnc://panelx/`
  return url;
}

} // namespace

auto content_type_of(std::string_view file) -> std::string_view {
  const auto dot = file.find_last_of("./");
  if (dot == std::string_view::npos || file[dot] != '.')
    return "application/octet-stream";
  std::string extension(file.substr(dot));
  std::ranges::transform(extension, extension.begin(), [](unsigned char c) -> char {
    return static_cast<char>(std::tolower(c));
  });
  const auto* const found = std::ranges::find(
      content_types, std::string_view{extension},
      &std::pair<std::string_view, std::string_view>::first);
  return found == content_types.end() ? std::string_view{"application/octet-stream"}
                                      : found->second;
}

auto is_panel_url(std::string_view url) -> bool { return path_of(url).has_value(); }

auto resolve_program_part(std::string_view url, std::string_view loaded, ProgramPartLinks links)
    -> std::optional<std::string> {
  if (path_of(url) != program_part_path)
    return {};
  const auto query = url.find('?');
  if (query == std::string_view::npos)
    return {};
  auto parameters = url.substr(query + 1);
  parameters = parameters.substr(0, parameters.find('#'));
  constexpr std::string_view key = "program=";
  if (!parameters.starts_with(key) || parameters.find('&') != std::string_view::npos)
    return {};
  const auto program = unescaped(parameters.substr(key.size()));
  if (!program || program->empty() || program->front() != '/' ||
      program->find('\0') != std::string::npos || *program != loaded)
    return {};
  std::error_code error;
  const std::filesystem::path path{*program};
  if (!std::filesystem::is_regular_file(path, error))
    return {};
  for (const auto* extension : {".step", ".stp", ".STEP", ".STP"}) {
    auto candidate = path;
    candidate.replace_extension(extension);
    if (links == ProgramPartLinks::skip) {
      if (std::filesystem::is_regular_file(std::filesystem::symlink_status(candidate, error)))
        return candidate.string();
      continue;
    }
    if (!std::filesystem::is_regular_file(candidate, error))
      continue;
    auto canonical = std::filesystem::canonical(candidate, error);
    if (error)
      return {};
    return canonical.string();
  }
  return {};
}

auto resolve_asset(const std::string& root, std::string_view url) -> std::expected<Asset, int> {
  const auto escaped = path_of(url);
  if (!escaped)
    return std::unexpected(not_found);
  const auto path = unescaped(*escaped);
  if (!path)
    return std::unexpected(forbidden);
  std::string_view rest{*path};
  rest.remove_prefix(1);
  std::filesystem::path relative;
  if (rest.empty()) {
    relative = "index.html";
  } else {
    if (std::ranges::any_of(rest, [](unsigned char c) -> bool { return c < 0x20 || c == '\\'; }))
      return std::unexpected(forbidden);
    while (!rest.empty()) {
      const auto end = std::min(rest.find('/'), rest.size());
      const auto segment = rest.substr(0, end);
      if (segment.empty() || segment == "." || segment == "..")
        return std::unexpected(forbidden);
      relative /= segment;
      rest.remove_prefix(std::min(end + 1, rest.size()));
    }
    // A trailing slash names a directory.
    if (path->back() == '/')
      return std::unexpected(not_found);
  }
  std::error_code error;
  const auto base = std::filesystem::canonical(root, error);
  if (error)
    return std::unexpected(not_found);
  const auto file = std::filesystem::canonical(base / relative, error);
  if (error)
    return std::unexpected(not_found);
  // Through a symbolic link the file may be anywhere: it must still be below the directory.
  const auto inside = file.lexically_relative(base);
  if (inside.empty() || *inside.begin() == "..")
    return std::unexpected(forbidden);
  if (!std::filesystem::is_regular_file(file, error) || error)
    return std::unexpected(not_found);
  return Asset{file.string(), content_type_of(file.string())};
}

} // namespace coffeecnc
