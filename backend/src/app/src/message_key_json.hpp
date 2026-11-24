#ifndef COFFEECNC_APP_MESSAGE_KEY_JSON_HPP
#define COFFEECNC_APP_MESSAGE_KEY_JSON_HPP

#include <coffeecnc/core/message_key.hpp>

#include <nlohmann/json.hpp>

namespace coffeecnc {

inline auto message_key_json(const MessageKey& value) -> nlohmann::json {
  nlohmann::json result{{"key", value.key}, {"params", value.params}};
  for (const auto& [name, parameter] : value.params_localization)
    result["params_localization"][name] = {{"key", parameter.key}, {"params", parameter.params}};
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

} // namespace coffeecnc

#endif
