#ifndef COFFEECNC_APP_MESSAGE_KEY_JSON_HPP
#define COFFEECNC_APP_MESSAGE_KEY_JSON_HPP

#include <coffeecnc/core/message_key.hpp>

#include <nlohmann/json.hpp>

namespace coffeecnc {

// Storage diagnostics can wrap a file operation in a reader, then in an operator notice.
inline constexpr unsigned max_message_parameter_depth = 4;

inline auto message_parameter_json(const MessageParameterKey& value, unsigned depth = 0)
    -> nlohmann::json {
  nlohmann::json result{{"key", value.key}, {"params", value.params}};
  if (depth < max_message_parameter_depth)
    for (const auto& [name, parameter] : value.params_localization)
      result["params_localization"][name] = message_parameter_json(parameter, depth + 1);
  return result;
}

inline auto message_list_json(const std::vector<MessageListItem>& items) -> nlohmann::json {
  auto list = nlohmann::json::array();
  for (const auto& item : items) {
    nlohmann::json value{{"text", item.text}};
    if (item.localization)
      value["localization"] = message_parameter_json(*item.localization);
    list.push_back(std::move(value));
  }
  return list;
}

inline auto message_key_json(const MessageKey& value) -> nlohmann::json {
  auto result = message_parameter_json({value.key, value.params, value.params_localization});
  for (const auto& [name, items] : value.params_lists)
    result["params_lists"][name] = message_list_json(items);
  return result;
}

inline auto message_parameter_from_json(const nlohmann::json& metadata, unsigned depth = 0)
    -> std::optional<MessageParameterKey> {
  if (!metadata.is_object())
    return std::nullopt;
  const auto key = metadata.find("key");
  const auto params = metadata.find("params");
  if (key == metadata.end() || !key->is_string() || key->get<std::string_view>().empty() ||
      params == metadata.end() || !params->is_object())
    return std::nullopt;
  MessageParameterKey message{key->get<std::string>()};
  for (const auto& [name, arg] : params->items()) {
    if (!arg.is_string())
      return std::nullopt;
    message.params.emplace(name, arg.get<std::string>());
  }
  const auto keys = metadata.find("params_localization");
  if (depth < max_message_parameter_depth && keys != metadata.end() && keys->is_object())
    for (const auto& [name, child] : keys->items())
      if (message.params.contains(name))
        if (auto parameter = message_parameter_from_json(child, depth + 1))
          message.params_localization.emplace(name, std::move(*parameter));
  return message;
}

inline auto message_list_from_json(const nlohmann::json& value)
    -> std::optional<std::vector<MessageListItem>> {
  if (!value.is_array() || value.empty())
    return std::nullopt;
  std::vector<MessageListItem> list;
  list.reserve(value.size());
  for (const auto& item : value) {
    if (!item.is_object())
      return std::nullopt;
    const auto text = item.find("text");
    if (text == item.end() || !text->is_string())
      return std::nullopt;
    MessageListItem entry{text->get<std::string>()};
    if (const auto localization = item.find("localization"); localization != item.end())
      entry.localization = message_parameter_from_json(*localization);
    list.push_back(std::move(entry));
  }
  return list;
}

} // namespace coffeecnc

#endif
