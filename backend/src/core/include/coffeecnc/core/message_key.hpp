#ifndef COFFEECNC_CORE_MESSAGE_KEY_HPP
#define COFFEECNC_CORE_MESSAGE_KEY_HPP

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace coffeecnc {

// A named argument may itself come from the catalog. Its fallback remains a string in params.
struct MessageParameterKey {
  std::string key;
  std::map<std::string, std::string> params{};
  std::map<std::string, MessageParameterKey> params_localization{};
  friend auto operator==(const MessageParameterKey&, const MessageParameterKey&) -> bool = default;
};

// One item in a list argument. An opaque item can carry just its literal fallback.
struct MessageListItem {
  std::string text;
  std::optional<MessageParameterKey> localization{};
  friend auto operator==(const MessageListItem&, const MessageListItem&) -> bool = default;
};

// A catalog key and named arguments, carried alongside English fallback text.
struct MessageKey {
  std::string key;
  std::map<std::string, std::string> params{};
  std::map<std::string, MessageParameterKey> params_localization{};
  std::map<std::string, std::vector<MessageListItem>> params_lists{};
  friend auto operator==(const MessageKey&, const MessageKey&) -> bool = default;
};

[[nodiscard]] inline auto parameter_localization(std::string_view name,
                                                 const std::optional<MessageKey>& key)
    -> std::map<std::string, MessageParameterKey> {
  if (!key)
    return {};
  return {{std::string(name), {key->key, key->params, key->params_localization}}};
}

// A reason produced by the backend. Adapters may have only an opaque fallback string.
struct LocalizedReason {
  std::string text;
  std::optional<MessageKey> localization{};
  [[nodiscard]] auto empty() const -> bool { return text.empty(); }
  friend auto operator==(const LocalizedReason&, const LocalizedReason&) -> bool = default;
};

} // namespace coffeecnc

#endif // COFFEECNC_CORE_MESSAGE_KEY_HPP
