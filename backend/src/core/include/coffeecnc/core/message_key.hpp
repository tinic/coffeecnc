#ifndef COFFEECNC_CORE_MESSAGE_KEY_HPP
#define COFFEECNC_CORE_MESSAGE_KEY_HPP

#include <map>
#include <string>

namespace coffeecnc {

// A catalog key and named arguments, carried alongside English fallback text.
struct MessageKey {
  std::string key;
  std::map<std::string, std::string> params{};
  friend auto operator==(const MessageKey&, const MessageKey&) -> bool = default;
};

} // namespace coffeecnc

#endif // COFFEECNC_CORE_MESSAGE_KEY_HPP
