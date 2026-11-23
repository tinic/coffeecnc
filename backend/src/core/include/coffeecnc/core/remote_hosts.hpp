#ifndef COFFEECNC_CORE_REMOTE_HOSTS_HPP
#define COFFEECNC_CORE_REMOTE_HOSTS_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace coffeecnc {

// Host tokens only. Ports and schemes are parsed by the HTTP server, outside this grammar.
[[nodiscard]] inline auto normalize_remote_host(std::string_view input)
    -> std::optional<std::string> {
  if (input.ends_with('.'))
    input.remove_suffix(1);
  if (input.empty() || input.size() > 253)
    return std::nullopt;
  std::string name;
  name.reserve(input.size());
  std::size_t length = 0;
  for (const char c : input) {
    if (c == '.') {
      if (length == 0 || name.back() == '-')
        return std::nullopt;
      length = 0;
    } else {
      const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
      if ((!alpha && !(c >= '0' && c <= '9') && c != '-') ||
          (length == 0 && c == '-'))
        return std::nullopt;
      ++length;
      if (length > 63)
        return std::nullopt;
    }
    name.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
  }
  if (length == 0 || name.back() == '-')
    return std::nullopt;
  return name;
}

} // namespace coffeecnc

#endif
