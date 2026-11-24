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

} // namespace coffeecnc

#endif
