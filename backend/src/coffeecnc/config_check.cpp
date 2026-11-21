#include "application.hpp"

#include <coffeecnc/app/tool_model_store.hpp>
#include <coffeecnc/core/text_file.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace coffeecnc {
namespace {

namespace fs = std::filesystem;
constexpr std::size_t max_file_bytes = 1 << 20;
constexpr std::array signal_pins{"coffeecnc.spindle-rpm", "coffeecnc.spindle-at-speed",
                                 "coffeecnc.probe-input"};
constexpr std::array signal_labels{"Measured spindle speed", "Spindle at speed", "Probe input"};

auto trimmed(std::string_view text) -> std::string_view {
  const auto first = text.find_first_not_of(" \t\r\n");
  return first == text.npos ? std::string_view{}
                            : text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

auto environment(const char* name) -> std::optional<std::string> {
  const char* value = std::getenv(name);
  return value && *value ? std::optional<std::string>{value} : std::nullopt;
}

auto hal_words(std::string_view line) -> std::vector<std::string> {
  // A # starts a HAL comment outside a quoted filename.
  bool quoted = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    if (quoted && line[i] == '\\' && i + 1 < line.size()) {
      ++i;
    } else if (line[i] == '"') {
      quoted = !quoted;
    } else if (!quoted && line[i] == '#') {
      line = line.substr(0, i);
      break;
    }
  }
  std::istringstream input{std::string(line)};
  std::vector<std::string> words;
  for (std::string word; input >> std::quoted(word);)
    words.push_back(std::move(word));
  return words;
}

struct HalInspection {
  struct Link {
    std::string signal;
    bool postgui;
  };
  fs::path directory;
  std::map<std::string, Link, std::less<>> links{};
  std::vector<fs::path> fragments{};
  std::vector<std::string> notes{};
  std::vector<fs::path> stack{};
  std::size_t files{};
  bool postgui{};
  bool postgui_error{};

  auto connected(std::string_view pin) const -> bool {
    const auto found = links.find(pin);
    return found != links.end() && (!pin.starts_with("coffeecnc.") || found->second.postgui) &&
           std::ranges::any_of(links, [&](const auto& link) {
             return link.first != pin && link.second.signal == found->second.signal;
           });
  }

  auto joined(std::string_view first, std::string_view second) const -> bool {
    const auto a = links.find(first);
    const auto b = links.find(second);
    return a != links.end() && a->second.postgui && b != links.end() &&
           a->second.signal == b->second.signal;
  }

  void read(const fs::path& path);
  void command(const std::vector<std::string>& words, const fs::path& path);
};

void HalInspection::command(const std::vector<std::string>& words, const fs::path& path) {
  if (words.size() < 2)
    return;
  const auto& name = words[1];
  if (words[0] == "source") {
    // halcmd keeps its original working directory even inside a sourced file.
    if (words.size() != 2 || name.find_first_of("$[") != name.npos)
      notes.push_back(std::format("{}: could not resolve source {}", path.string(), name));
    else
      read(directory / name);
  } else if (words[0] == "net") {
    for (std::size_t i = 2; i < words.size(); ++i)
      if (words[i] != "=>" && words[i] != "<=" && words[i] != "<=>")
        links[words[i]] = {name, postgui};
  } else if (words[0] == "linksp" && words.size() == 3) {
    links[words[2]] = {name, postgui};
  } else if (words[0] == "linkps" && words.size() == 3) {
    links[name] = {words[2], postgui};
  } else if (words[0] == "unlinkp") {
    links.erase(name);
  } else if (words[0] == "delsig") {
    std::erase_if(links, [&](const auto& link) { return link.second.signal == name; });
  }
}

void HalInspection::read(const fs::path& path) {
  std::error_code error;
  const auto file = fs::weakly_canonical(path, error);
  if (error || !fs::is_regular_file(file, error)) {
    notes.push_back(std::format("Cannot read HAL file {}.", path.string()));
    postgui_error = postgui_error || postgui;
    return;
  }
  if (std::ranges::find(stack, file) != stack.end()) {
    notes.push_back(std::format("HAL source cycle at {}.", file.string()));
    postgui_error = postgui_error || postgui;
    return;
  }
  if (++files > 128 || stack.size() >= 32) {
    notes.push_back(std::format("HAL source limit reached at {}.", file.string()));
    return;
  }
  if (file.extension() == ".tcl") {
    notes.push_back(std::format("Tcl HAL file {} needs runtime inspection.", file.string()));
    return;
  }
  const auto text = read_text_file(file.string(), max_file_bytes);
  if (!text) {
    notes.push_back(std::format("Cannot read HAL file {} ({}).", file.string(),
                                text.error() == TextFileError::too_large ? "file is too large"
                                                                         : "read failed"));
    postgui_error = postgui_error || postgui;
    return;
  }
  if ((path.filename() == "coffeecnc_postgui.hal" ||
       path.filename() == "coffeecnc_g0759_postgui.hal") &&
      std::ranges::find(fragments, path.lexically_normal()) == fragments.end())
    fragments.push_back(path.lexically_normal());
  stack.push_back(file);
  std::istringstream lines{*text};
  std::string logical;
  for (std::string line; std::getline(lines, line);) {
    const auto content = trimmed(line);
    if (content.ends_with('\\')) {
      logical.append(content.substr(0, content.size() - 1));
      logical += ' ';
    } else {
      logical.append(content);
      command(hal_words(logical), file);
      logical.clear();
    }
  }
  stack.pop_back();
}

auto scan_ini_hal(const MachineIniInspection& ini) -> HalInspection {
  HalInspection result{.directory = fs::path(ini.path).parent_path()};
  const auto library = fs::path("/usr/share/linuxcnc/hallib");
  for (const auto& setting : ini.halfiles) {
    const auto words = hal_words(setting);
    if (words.empty()) {
      result.notes.emplace_back("An empty HALFILE cannot be inspected.");
      continue;
    }
    const auto& filename = words.front();
    if (filename.starts_with("LIB:")) {
      result.read(library / filename.substr(4));
      continue;
    }
    const auto local = result.directory / filename;
    std::error_code error;
    if (fs::path(filename).is_absolute() || fs::exists(local, error))
      result.read(local);
    else if (fs::exists(library / filename, error))
      result.read(library / filename);
    else
      result.read(local);
  }
  if (std::ranges::any_of(result.links,
                          [](const auto& link) { return link.first.starts_with("coffeecnc."); }))
    result.notes.emplace_back("A HALFILE references CoffeeCNC pins before the panel creates them. "
                              "Put that wiring in POSTGUI_HALFILE.");
  result.postgui = true;
  if (ini.postgui && !ini.postgui->empty())
    result.read(result.directory / *ini.postgui);
  return result;
}

struct IniAddition {
  std::string_view section;
  std::string_view key;
  std::string_view value;
};

auto additions_for(const MachineIniInspection& ini, const HalInspection& hal)
    -> std::vector<IniAddition> {
  std::vector<IniAddition> additions;
  if (!ini.display)
    additions.push_back({"DISPLAY", "DISPLAY", "coffeecnc"});
  // An unreadable source could override a visible link. In that case report only.
  if (hal.notes.empty() && ini.postgui && !ini.postgui->empty()) {
    for (std::size_t i = 0; i < signal_pins.size(); ++i)
      if (!ini.signal_configured[i] && hal.connected(signal_pins[i]))
        additions.push_back({"COFFEECNC", coffeecnc_signal_keys[i], "yes"});
  }
  return additions;
}

void insert_section(std::string& ini, std::string_view section, std::string_view lines,
                    std::string_view newline) {
  const std::string header = std::format("[{}]", section);
  bool found = false;
  std::size_t at = 0;
  while (at < ini.size()) {
    const auto end = std::min(ini.find('\n', at), ini.size());
    const auto line = trimmed(std::string_view(ini).substr(at, end - at));
    if (line.starts_with('[')) {
      if (found)
        break;
      found = line.substr(0, line.find_first_of(";#")).starts_with(header);
    }
    at = std::min(end + 1, ini.size());
  }
  std::string entry;
  if (at > 0 && ini[at - 1] != '\n')
    entry += newline;
  if (!found) {
    if (!ini.empty())
      entry += newline;
    entry += header;
    entry += newline;
  }
  entry += lines;
  ini.insert(at, entry);
}

struct IniLock {
  int descriptor;
  ~IniLock() {
    if (descriptor >= 0)
      static_cast<void>(::close(descriptor));
  }
};

struct StagedIni {
  LocalToolModelFiles& files;
  std::string path;
  ~StagedIni() {
    if (!path.empty())
      files.discard(path);
  }
};

auto repair_ini(const MachineIniInspection& plan, const std::vector<IniAddition>& additions)
    -> std::expected<std::string, std::string> {
  const auto& path = plan.path;
  const IniLock lock{::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
  struct stat original{};
  if (lock.descriptor < 0 || ::fstat(lock.descriptor, &original) != 0 || !S_ISREG(original.st_mode))
    return std::unexpected("The INI must be a regular file to write it.");
  if (::flock(lock.descriptor, LOCK_EX | LOCK_NB) != 0)
    return std::unexpected("The INI could not be locked for writing.");
  LocalToolModelFiles files;
  const auto before = files.read(path, max_file_bytes);
  if (!before || !*before || (*before)->more)
    return std::unexpected("Cannot read the complete INI for its backup.");
  const auto latest = inspect_machine_ini(path);
  if (!latest || latest->display != plan.display || latest->postgui != plan.postgui ||
      latest->signal_configured != plan.signal_configured || latest->halfiles != plan.halfiles)
    return std::unexpected(
        "The INI changed during the check. Re-run the check on the current file.");
  auto replacement = (*before)->bytes;
  const auto first_newline = replacement.find('\n');
  const std::string_view newline = first_newline != 0 && first_newline != replacement.npos &&
                                           replacement[first_newline - 1] == '\r'
                                       ? "\r\n"
                                       : "\n";
  for (const auto section : {"DISPLAY", "COFFEECNC"}) {
    std::string lines;
    for (const auto& setting : additions)
      if (setting.section == section)
        lines += std::format("{} = {}{}", setting.key, setting.value, newline);
    if (!lines.empty())
      insert_section(replacement, section, lines, newline);
  }

  auto temporary = path + ".tmp.XXXXXX";
  const auto reserved = ::mkstemp(temporary.data());
  if (reserved < 0)
    return std::unexpected("Cannot create a temporary INI in the machine directory.");
  static_cast<void>(::close(reserved));
  StagedIni stage{files, temporary};
  if (auto step = files.open_temporary(stage.path); !step)
    return std::unexpected(step.error());
  if (::chown(stage.path.c_str(), original.st_uid, original.st_gid) != 0 ||
      ::chmod(stage.path.c_str(), original.st_mode & 0777) != 0)
    return std::unexpected("Cannot preserve the INI ownership and permissions.");
  if (auto step = files.write(replacement); !step)
    return std::unexpected(step.error());
  if (auto step = files.sync(); !step)
    return std::unexpected(step.error());

  // Check with the same parser and units/feature validation used at startup.
  const auto staged = load_machine_config(stage.path);
  if (!staged)
    return std::unexpected(staged.error());
  const auto verified = inspect_machine_ini(stage.path);
  if (!verified)
    return std::unexpected(verified.error());
  const std::array enabled{staged->signals.spindle_rpm, staged->signals.spindle_at_speed,
                           staged->signals.probe};
  for (const auto& setting : additions) {
    const auto key = std::ranges::find(coffeecnc_signal_keys, setting.key);
    if (setting.section == "DISPLAY"
            ? verified->display != setting.value
            : key == coffeecnc_signal_keys.end() ||
                  !enabled.at(static_cast<std::size_t>(key - coffeecnc_signal_keys.begin())))
      return std::unexpected("LinuxCNC could not read the added INI settings.");
  }

  struct stat current{};
  const auto unchanged = files.read(path, max_file_bytes);
  if (::lstat(path.c_str(), &current) != 0 || current.st_dev != original.st_dev ||
      current.st_ino != original.st_ino || !unchanged || !*unchanged || (*unchanged)->more ||
      (*unchanged)->bytes != (*before)->bytes)
    return std::unexpected("The INI changed while it was being prepared.");
  auto backup = path + ".bak";
  for (int suffix = 1; ::link(path.c_str(), backup.c_str()) != 0; ++suffix) {
    if (errno != EEXIST)
      return std::unexpected(std::format("Cannot create INI backup {}.", backup));
    backup = std::format("{}.bak.{}", path, suffix);
  }
  const auto directory = fs::path(path).parent_path().string();
  if (auto step = files.sync_directory(directory); !step)
    return std::unexpected(std::format("Could not save backup {}: {}.", backup, step.error()));
  if (auto step = files.rename(stage.path, path); !step)
    return std::unexpected(
        std::format("Could not replace the INI: {}. Backup: {}.", step.error(), backup));
  stage.path.clear();
  if (auto step = files.sync_directory(directory); !step)
    return std::unexpected(
        std::format("The INI was replaced but could not be synchronised. Backup: {}.", backup));
  return backup;
}

void show_additions(const std::vector<IniAddition>& additions, std::string_view heading) {
  std::cout << '\n' << heading << '\n';
  for (const auto& setting : additions)
    std::cout << std::format("  [{}] {} = {}\n", setting.section, setting.key, setting.value);
}

} // namespace

auto check_configuration(const Options& options) -> int {
  std::cout << "CoffeeCNC configuration check: " << options.ini_file << '\n';
  auto ini = inspect_machine_ini(options.ini_file);
  if (!ini) {
    std::cout << "\nCoffeeCNC does not start with this file:\n  " << ini.error() << '\n';
    return 1;
  }
  const auto hal = scan_ini_hal(*ini);
  const auto additions = additions_for(*ini, hal);
  if (!options.write_ini && !additions.empty())
    show_additions(additions, "Missing INI settings that --write-ini can add");
  if (options.write_ini) {
    if (additions.empty()) {
      std::cout << "\nNo INI settings to add.\n";
    } else if (!hal.postgui_error) {
      const auto backup = repair_ini(*ini, additions);
      if (!backup) {
        std::cout << "\nCould not repair the INI: " << backup.error() << '\n';
        return 1;
      }
      show_additions(additions, "Added INI settings");
      std::cout << "  Backup " << *backup << '\n';
      ini = inspect_machine_ini(options.ini_file);
      if (!ini) {
        std::cout << ini.error() << '\n';
        return 1;
      }
    }
  }

  const auto config = load_machine_config(options.ini_file);
  if (!config) {
    std::cout << "\nCoffeeCNC does not start with this file:\n  " << config.error() << '\n';
    return 1;
  }
  const auto file_state = [](const std::string& name) -> std::string {
    if (name.empty())
      return "not configured";
    std::error_code error;
    return fs::exists(name, error) ? name : name + " (missing)";
  };
  const auto on_off = [](bool on) -> std::string_view { return on ? "on" : "off"; };
  constexpr std::string_view axes{"XYZ"};
  std::string axis_list;
  for (std::size_t i = 0; i < axes.size(); ++i) {
    axis_list += std::format("{}{}", axis_list.empty() ? "" : ", ", axes[i]);
    const auto& joints = config->axis_joints[i];
    axis_list += joints.size() == 1
                     ? std::format(" (joint {})", joints.front())
                     : std::format(" (joints {} and {})", joints.front(), joints.back());
  }
  std::cout << (hal.postgui_error ? "\nCoffeeCNC does not start with this file: post-GUI HAL "
                                    "sources could not be loaded.\n"
                                  : "\nCoffeeCNC starts with this file.\n")
            << "\nMachine\n"
            << "  Axes              " << axis_list << '\n'
            << "  Units             " << (config->millimetres_per_unit == 1.0 ? "mm" : "inch")
            << '\n'
            << std::format("  Spindle maximum   {:.0f} rpm\n", config->limits.max_spindle_rpm)
            << "  Homing            "
            << (config->limits.homing_required ? "required before MDI and programs"
                                               : "not required (NO_FORCE_HOMING)")
            << '\n'
            << "  Home all          "
            << (config->home_all_configured ? "configured" : "not configured: home each axis")
            << '\n'
            << "  Display           " << ini->display.value_or("not configured");
  if (ini->display && *ini->display != "coffeecnc" && !ini->display->starts_with("coffeecnc ") &&
      !fs::path(*ini->display).filename().string().starts_with("coffeecnc"))
    std::cout << "  (to start the CoffeeCNC panel, set [DISPLAY] DISPLAY = coffeecnc)";
  std::cout << "\n\nFiles\n"
            << "  Programs          " << file_state(config->program_prefix) << '\n'
            << "  Tool table        " << file_state(config->tool_table) << '\n'
            << "  Post-GUI HAL      " << file_state(config->postgui_halfile) << '\n';
  for (const auto& fragment : hal.fragments)
    std::cout << "  CoffeeCNC HAL     " << fragment.string() << '\n';
  if (hal.fragments.empty()) {
    const bool core_links =
        hal.connected("coffeecnc.heartbeat") && hal.connected("motion.jog-stop");
    std::cout << "  CoffeeCNC HAL     "
              << (core_links ? "jog-stop and watchdog wiring found"
                             : "wiring not found ([HAL] POSTGUI_HALFILE = coffeecnc_postgui.hal)")
              << '\n';
  }
  for (const auto& note : hal.notes)
    std::cout << "  HAL inspection    " << note << '\n';

  const auto location =
      resolve_tool_model_location({config->machine_id, config->data_directory,
                                   environment("XDG_DATA_HOME"), environment("HOME")});
  std::cout << "  Saved data        "
            << (location.availability == ToolModelAvailability::available
                    ? location.directory
                    : std::format("not saved: {}", location.reason))
            << '\n';
  const bool wires_prompt = hal.joined("coffeecnc.tool-change", "iocontrol.0.tool-change") &&
                            hal.joined("coffeecnc.tool-changed", "iocontrol.0.tool-changed") &&
                            hal.joined("coffeecnc.tool-number", "iocontrol.0.tool-prep-number");
  const auto prompt = config->manual_tool_change ? "on"
                      : wires_prompt             ? "on  (wired in the post-GUI HAL file)"
                                                 : "off  ([COFFEECNC] MANUAL_TOOLCHANGE = yes)";
  std::cout << "\nOptional functions\n"
            << "  Manual tool change prompt    " << prompt << '\n';
  const std::array signals{config->signals.spindle_rpm, config->signals.spindle_at_speed,
                           config->signals.probe};
  for (std::size_t i = 0; i < signals.size(); ++i) {
    std::cout << std::format("  {:29} {}", signal_labels[i], on_off(signals[i]));
    if (!signals[i])
      std::cout << std::format("  ([COFFEECNC] {} = yes when {} is wired)",
                               coffeecnc_signal_keys[i], signal_pins[i]);
    else if (!hal.connected(signal_pins[i]))
      std::cout << std::format("  (the link to {} was not found in the inspected HAL)",
                               signal_pins[i]);
    std::cout << '\n';
  }
  std::cout << "  Touch probe tool             "
            << (config->touch_probe_tool ? std::format("T{}", *config->touch_probe_tool)
                                         : std::string("off  (TOUCH_PROBE_TOOL)"))
            << '\n'
            << "  Fixed tool setter            " << on_off(config->fixed_setter.has_value())
            << (config->fixed_setter ? "" : "  (FIXED_SETTER = yes and the SETTER_* keys)") << '\n'
            << "  Z jog during a pause         " << on_off(config->paused_z.has_value())
            << (config->paused_z ? ""
                                 : "  (PAUSED_Z_MAX_MM, OFFSET_AV_RATIO, both spindle signals)")
            << '\n'
            << "  Remembered spindle tool      " << on_off(config->remember_spindle_tool) << '\n'
            << "\nSee /usr/share/coffeecnc/coffeecnc.ini.example for every key.\n";
  return hal.postgui_error ? 1 : 0;
}

} // namespace coffeecnc
