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

inline auto message_key_json(const MessageKey& value) -> nlohmann::json {
  return message_parameter_json({value.key, value.params, value.params_localization});
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

} // namespace coffeecnc

#endif
