#ifndef COFFEECNC_APP_MESSAGE_KEY_JSON_HPP
#define COFFEECNC_APP_MESSAGE_KEY_JSON_HPP

#include <coffeecnc/core/message_key.hpp>

#include <nlohmann/json.hpp>

namespace coffeecnc {

inline auto message_parameter_json(const MessageParameterKey& value) -> nlohmann::json {
  return {{"key", value.key}, {"params", value.params}};
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
  nlohmann::json result{{"key", value.key}, {"params", value.params}};
  for (const auto& [name, parameter] : value.params_localization)
    result["params_localization"][name] = message_parameter_json(parameter);
  for (const auto& [name, items] : value.params_lists)
    result["params_lists"][name] = message_list_json(items);
  return result;
}

inline auto message_parameter_from_json(const nlohmann::json& metadata)
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
