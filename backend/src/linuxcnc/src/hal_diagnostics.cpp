#include <coffeecnc/linuxcnc/hal_diagnostics.hpp>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <signal.h> // kill, SIGTERM: POSIX, not in <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <hal.h>

namespace coffeecnc {
namespace {

// Pin and signal memory is shared with LinuxCNC's realtime thread and every other component:
// one atomic load of the whole value, as in hal_component.cpp.
template <typename Value> auto get(const volatile Value* place) -> Value {
  Value value;
  __atomic_load(place, &value, __ATOMIC_RELAXED);
  return value;
}

auto value_of(hal_type_t type, const hal_data_u* data) -> DiagnosticValue {
  if (data == nullptr)
    return {};
  switch (type) {
  case HAL_BIT:
    return get(&data->b);
  case HAL_FLOAT:
    return get(&data->f);
  case HAL_S32:
    return static_cast<double>(get(&data->s));
  case HAL_U32:
    return static_cast<double>(get(&data->u));
  default:
    return {};
  }
}

// Reads a pin of a standard row or a configured one.
auto pin_reading(DiagnosticReading reading, HalLookup& hal, const PinSignals& wiring)
    -> DiagnosticReading {
  const auto pin = hal.pin(reading.input.name);
  if (!pin) {
    reading.state = DiagnosticState::missing;
    return reading;
  }
  reading.value = pin->value;
  if (!pin->linked) {
    reading.state = DiagnosticState::unwired;
    return reading;
  }
  reading.state = DiagnosticState::live;
  if (const auto found = wiring.find(reading.input.name); found != wiring.end())
    if (const auto signal = hal.signal(found->second); signal && !signal->driven)
      reading.state = DiagnosticState::undriven;
  return reading;
}

} // namespace

auto diagnostic_inputs(const MachineConfig& config) -> std::vector<DiagnosticInput> {
  // The axis letter of each joint, as trivkins assigns them: X Y Z, then A B C.
  std::map<int, char> letters;
  constexpr std::string_view linear = "xyz";
  constexpr std::string_view rotary = "abc";
  for (std::size_t i = 0; i < config.axis_joints.size(); ++i)
    for (const int joint : config.axis_joints.at(i))
      letters.emplace(joint, linear.at(i));
  for (std::size_t i = 0; i < config.rotary_joints.size(); ++i)
    for (const int joint : config.rotary_joints.at(i))
      letters.emplace(joint, rotary.at(i));
  const int joints = letters.empty() ? 0 : letters.rbegin()->first + 1;
  const auto letter = [&](int joint) -> std::optional<char> {
    const auto found = letters.find(joint);
    return found == letters.end() ? std::nullopt : std::optional{found->second};
  };
  std::vector<DiagnosticInput> inputs;
  struct Pin {
    DiagnosticRole role;
    const char* name;
  };
  constexpr std::array joint_pins{Pin{DiagnosticRole::min_limit, "neg-lim-sw-in"},
                                  Pin{DiagnosticRole::max_limit, "pos-lim-sw-in"},
                                  Pin{DiagnosticRole::home_switch, "home-sw-in"},
                                  Pin{DiagnosticRole::amp_fault, "amp-fault-in"}};
  for (int joint = 0; joint < joints; ++joint)
    for (const auto& pin : joint_pins)
      inputs.push_back({.role = pin.role,
                        .joint = joint,
                        .axis = letter(joint),
                        .name = std::format("joint.{}.{}", joint, pin.name)});
  constexpr std::array machine_pins{
      Pin{DiagnosticRole::estop_chain, "iocontrol.0.emc-enable-in"},
      Pin{DiagnosticRole::estop_request, "iocontrol.0.user-enable-out"},
      Pin{DiagnosticRole::probe, "motion.probe-input"},
      Pin{DiagnosticRole::spindle_on, "spindle.0.on"},
      Pin{DiagnosticRole::spindle_at_speed, "spindle.0.at-speed"},
      Pin{DiagnosticRole::spindle_speed_out, "spindle.0.speed-out"},
      Pin{DiagnosticRole::spindle_speed_in, "spindle.0.speed-in"},
      Pin{DiagnosticRole::spindle_fault, "spindle.0.amp-fault-in"}};
  for (const auto& pin : machine_pins)
    inputs.push_back({.role = pin.role, .name = pin.name});
  for (const auto& signal : config.diagnostics)
    inputs.push_back(
        {.role = DiagnosticRole::configured, .label = signal.label, .name = signal.name});
  return inputs;
}

auto HalApiLookup::pin(const std::string& name) -> std::optional<PinReading> {
  hal_type_t type{};
  hal_data_u* data{};
  bool connected{};
  if (hal_get_pin_value_by_name(name.c_str(), &type, &data, &connected) != 0)
    return {};
  return PinReading{value_of(type, data), connected};
}

auto HalApiLookup::signal(const std::string& name) -> std::optional<SignalReading> {
  hal_type_t type{};
  hal_data_u* data{};
  bool writers{};
  if (hal_get_signal_value_by_name(name.c_str(), &type, &data, &writers) != 0)
    return {};
  return SignalReading{value_of(type, data), writers};
}

auto pin_signals(std::string_view shown) -> PinSignals {
  PinSignals signals;
  std::istringstream lines{std::string(shown)};
  for (std::string line; std::getline(lines, line);) {
    std::istringstream words{line};
    const std::vector<std::string> word{std::istream_iterator<std::string>{words}, {}};
    if (word.size() < 5 || word.size() % 2 != 1)
      continue;
    const bool arrows = [&]() -> bool {
      for (std::size_t i = 3; i < word.size(); i += 2)
        if (word[i] != "<==" && word[i] != "==>" && word[i] != "<=>")
          return false;
      return true;
    }();
    if (!arrows)
      continue;
    for (std::size_t i = 4; i < word.size(); i += 2)
      signals.insert_or_assign(word[i], word[2]);
  }
  return signals;
}

auto read_diagnostics(std::span<const DiagnosticInput> inputs, HalLookup& hal,
                      const PinSignals& wiring) -> std::vector<DiagnosticReading> {
  std::vector<DiagnosticReading> readings;
  readings.reserve(inputs.size());
  for (const auto& input : inputs) {
    DiagnosticReading reading{.input = input};
    if (input.role == DiagnosticRole::configured) {
      if (const auto signal = hal.signal(input.name)) {
        reading.signal = true;
        reading.value = signal->value;
        reading.state = signal->driven ? DiagnosticState::live : DiagnosticState::undriven;
        readings.push_back(std::move(reading));
        continue;
      }
    }
    readings.push_back(pin_reading(std::move(reading), hal, wiring));
  }
  return readings;
}

HalToolLauncher::HalToolLauncher(std::string search_path, std::vector<std::string> environment)
    : search_path_(std::move(search_path)), environment_(std::move(environment)) {}

HalToolLauncher::~HalToolLauncher() {
  const std::scoped_lock lock{mutex_};
  reap();
  for (const pid_t child : children_) {
    if (child <= 0)
      continue;
    // The tool leads a session and a process group of its own. It is not waited for: init
    // takes it over when CoffeeCNC exits.
    kill(-child, SIGTERM);
  }
}

auto HalToolLauncher::program(HalTool tool) const -> std::optional<std::string> {
  const auto name = hal_tool_name(tool);
  for (std::size_t at = 0; at <= search_path_.size();) {
    const auto end = std::min(search_path_.find(':', at), search_path_.size());
    const std::string_view directory = std::string_view(search_path_).substr(at, end - at);
    at = end + 1;
    // An empty entry of PATH is the working directory: never a place for these tools here.
    if (directory.empty() || directory.front() != '/')
      continue;
    auto candidate = (std::filesystem::path(directory) / name).string();
    std::error_code error;
    if (std::filesystem::is_regular_file(candidate, error) && access(candidate.c_str(), X_OK) == 0)
      return candidate;
  }
  return {};
}

void HalToolLauncher::reap() {
  for (auto& child : children_) {
    if (child <= 0)
      continue;
    int status{};
    pid_t got{};
    do {
      got = waitpid(child, &status, WNOHANG);
    } while (got < 0 && errno == EINTR);
    // Ended (got == child), or not ours to wait for any more (ECHILD).
    if (got == child || (got < 0 && errno == ECHILD))
      child = 0;
  }
}

auto HalToolLauncher::launch(HalTool tool) -> std::optional<std::string> {
  const auto name = hal_tool_name(tool);
  const auto path = program(tool);
  if (!path)
    return std::format("{} is not installed: it is not in the search path ({}).", name,
                       search_path_);
  const std::scoped_lock lock{mutex_};
  reap();
  auto& child = children_.at(static_cast<std::size_t>(tool));
  if (child > 0)
    return std::format("{} is already open.", name);

  std::string program_path = *path;
  std::array<char*, 2> argv{program_path.data(), nullptr};
  std::vector<std::string> environment = environment_;
  std::vector<char*> envp;
  envp.reserve(environment.size() + 1);
  std::ranges::transform(environment, std::back_inserter(envp),
                         [](std::string& entry) -> char* { return entry.data(); });
  envp.push_back(nullptr);

  posix_spawn_file_actions_t actions;
  posix_spawnattr_t attributes;
  if (posix_spawn_file_actions_init(&actions) != 0)
    return std::format("{} could not be started.", name);
  if (posix_spawnattr_init(&attributes) != 0) {
    posix_spawn_file_actions_destroy(&actions);
    return std::format("{} could not be started.", name);
  }
  int error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  if (error == 0)
    error = posix_spawn_file_actions_addclosefrom_np(&actions, STDERR_FILENO + 1);
  // Its own session: the terminal's signals and CoffeeCNC's process group are not its.
  if (error == 0)
    error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
  pid_t started{};
  if (error == 0)
    error = posix_spawn(&started, program_path.c_str(), &actions, &attributes, argv.data(),
                        envp.data());
  posix_spawnattr_destroy(&attributes);
  posix_spawn_file_actions_destroy(&actions);
  if (error != 0)
    return std::format("{} could not be started: {}.", name,
                       std::generic_category().message(error));
  child = started;
  return {};
}

auto HalToolLauncher::states() -> std::vector<HalToolState> {
  std::array<bool, hal_tools.size()> open{};
  {
    const std::scoped_lock lock{mutex_};
    reap();
    for (std::size_t i = 0; i < children_.size(); ++i)
      open.at(i) = children_.at(i) > 0;
  }
  std::vector<HalToolState> states;
  states.reserve(hal_tools.size());
  for (const auto tool : hal_tools)
    states.push_back({.tool = tool,
                      .available = program(tool).has_value(),
                      .open = open.at(static_cast<std::size_t>(tool))});
  return states;
}

auto hal_tool_environment(const std::string& ini_file) -> std::vector<std::string> {
  std::vector<std::string> environment;
  bool ini = false;
  for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
    environment.emplace_back(*entry);
    ini = ini || environment.back().starts_with("INI_FILE_NAME=");
  }
  if (!ini)
    environment.push_back("INI_FILE_NAME=" + ini_file);
  return environment;
}

} // namespace coffeecnc
