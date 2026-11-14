#include <coffeecnc/app/request.hpp>

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

using namespace coffeecnc;

namespace {
using Json = nlohmann::json;

int checks = 0;
void require(bool condition, const std::string& message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

// Protocol v1 `type` of every Command alternative, in the variant's order. A command added to
// the variant without a name here, or without an example in the fixture file, fails below; so
// does an example the decoder does not know.
constexpr std::array<std::string_view, std::variant_size_v<Command>> wire_types{
    "ready",       "set_machine",    "home",
    "jog",         "jog_step",       "jog_stop_all",
    "mdi",         "list_directory", "open_program",
    "run",         "pause",          "resume",
    "step",        "abort",          "set_override",
    "set_spindle", "set_coolant",    "set_program_option",
    "select_wcs",  "touch_off",      "tool_touch_off",
    "set_tool",    "remove_tool",    "tool_changed"};

void every_panel_command_decodes() {
  std::ifstream file{COFFEECNC_COMMAND_FIXTURES};
  require(file.is_open(), std::string("Fixture file opens: ") + COFFEECNC_COMMAND_FIXTURES);
  const auto fixtures = Json::parse(file);
  require(fixtures.is_array() && !fixtures.empty(), "Fixture file is a nonempty array");

  std::array<bool, wire_types.size()> covered{};
  for (const auto& fixture : fixtures) {
    const auto wire = fixture.dump();
    const auto decoded = decode_request(wire);
    const auto* request = std::get_if<Request>(&decoded);
    require(request != nullptr, "Panel command decodes: " + wire);
    if (request == nullptr)
      continue;
    require(request->id == fixture.at("id").get<RequestId>(), "Request ID preserved: " + wire);
    const auto alternative = request->command.index();
    require(wire_types.at(alternative) == fixture.at("type").get<std::string>(),
            "Decodes to the alternative of its type: " + wire);
    require(!covered.at(alternative), "One example per command: " + wire);
    covered.at(alternative) = true;
  }
  for (std::size_t i = 0; i < covered.size(); ++i) {
    require(!wire_types.at(i).empty(),
            "Command alternative " + std::to_string(i) + " has a wire type");
    require(covered.at(i), "Fixture file has an example of " + std::string(wire_types.at(i)));
  }
}
} // namespace

auto main() -> int {
  try {
    every_panel_command_decodes();
    std::cout << checks << " protocol fixture checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
