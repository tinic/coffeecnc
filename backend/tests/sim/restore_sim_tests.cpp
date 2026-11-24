// Backup and restore on a simulated machine's own files (review #274): the steps of
// restore_tests.sh that need CoffeeCNC's code. Output is key=value; exit code 0 on success.
//
//   coffeecnc_restore_sim_tests backup <ini> <folder>
//   coffeecnc_restore_sim_tests stage <ini> <archive> <after pid> <coffeecnc executable>
//   coffeecnc_restore_sim_tests report <ini>
//
// `backup` writes a backup of the machine to <folder>. `stage` checks and stages a restore of
// <archive> and starts `<coffeecnc> --apply-restore <data> --after <pid>` as the application
// does when it ends, without the run's COFFEECNC_SIM_RUN (run.sh ends every process that has
// it). `report` is what the next start of CoffeeCNC says about it.
#include <coffeecnc/app/machine_data.hpp>
#include <coffeecnc/app/restore_helper.hpp>
#include <coffeecnc/app/tool_model_store.hpp>
#include <coffeecnc/linuxcnc/machine_config.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <format>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace coffeecnc;

namespace {

auto environment(const char* name) -> std::optional<std::string> {
  const char* value = std::getenv(name);
  return value != nullptr ? std::optional<std::string>{value} : std::nullopt;
}

auto unix_now() -> std::chrono::milliseconds {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch());
}

// The machine's files as the application takes them from its INI file.
auto machine_paths(const std::string& ini) -> MachineDataPaths {
  const auto config = load_machine_config(ini);
  if (!config)
    throw std::runtime_error("the INI file does not load: " + config.error());
  const auto location =
      resolve_tool_model_location({config->machine_id, config->data_directory,
                                   environment("XDG_DATA_HOME"), environment("HOME")});
  if (location.availability != ToolModelAvailability::available)
    throw std::runtime_error("the machine has no data folder: " + location.reason);
  MachineDataPaths paths;
  paths.machine_id = location.machine_id.value_or("");
  paths.ini_file = config->ini_file;
  paths.config_directory = config->directory;
  if (const auto inspected = inspect_machine_ini(config->ini_file))
    paths.hal_files = inspected->halfiles;
  if (!config->postgui_halfile.empty())
    paths.hal_files.push_back(config->postgui_halfile);
  paths.tool_table = config->tool_table;
  paths.parameter_file = config->parameter_file;
  paths.data_directory = location.directory;
  return paths;
}

auto run(std::span<char*> arguments) -> int {
  const std::vector<std::string_view> words(arguments.begin(), arguments.end());
  if (words.size() < 3)
    throw std::runtime_error("usage: coffeecnc_restore_sim_tests backup|stage|report <ini> ...");
  const auto paths = machine_paths(std::string(words[2]));
  LocalToolModelFiles files;
  MachineData machine{files, paths, unix_now};
  if (words[1] == "report" && words.size() == 3) {
    const auto notice = machine.take_notice();
    std::cout << "notice_level="
              << (!notice                               ? "none"
                  : notice->first == MessageLevel::info ? "info"
                                                        : "error")
              << " notice=" << (notice ? notice->second : std::string{}) << '\n';
    return notice ? 0 : 1;
  }
  if (words[1] == "backup" && words.size() == 4) {
    const auto done = machine.backup(std::string(words[3]));
    if (done.outcome != CommandOutcome::accepted || done.messages.empty())
      throw std::runtime_error("the backup is not written: " + done.reason);
    const auto written = nlohmann::json::parse(done.messages.front());
    std::cout << "backup=" << written["path"].get<std::string>()
              << " files=" << written["files"].get<std::size_t>() << '\n';
    return 0;
  }
  if (words[1] == "stage" && words.size() == 6) {
    const std::string archive{words[3]};
    const auto checked = machine.check_restore(archive);
    if (checked.outcome != CommandOutcome::accepted || checked.messages.empty())
      throw std::runtime_error("the backup does not check: " + checked.reason);
    const auto summary = nlohmann::json::parse(checked.messages.front());
    for (const auto& file : summary["files"])
      std::cout << "planned role=" << file["role"].get<std::string>()
                << " change=" << file["change"].get<std::string>()
                << " target=" << file["target"].get<std::string>() << '\n';
    const auto staged = machine.restore(archive, summary["token"].get<std::string>(), 1,
                                        [] { return std::string{}; });
    if (staged.outcome != CommandOutcome::accepted || !staged.quit || !machine.staged())
      throw std::runtime_error("the restore is not staged: " + staged.reason);
    // As start_restore_helper does it; run.sh ends what carries its run's variable.
    static_cast<void>(::unsetenv("COFFEECNC_SIM_RUN"));
    const auto child = spawn_detached({std::string(words[5]), "--apply-restore",
                                       paths.data_directory, "--after", std::string(words[4])});
    if (!child)
      throw std::runtime_error("the helper does not start: " + child.error());
    std::cout << "staged=yes helper=" << *child << " data=" << paths.data_directory << '\n';
    return 0;
  }
  throw std::runtime_error("usage: coffeecnc_restore_sim_tests backup|stage|report <ini> ...");
}

} // namespace

auto main(int argc, char** argv) -> int {
  try {
    return run(std::span<char*>(argv, static_cast<std::size_t>(argc)));
  } catch (const std::exception& error) {
    std::cout << "error=" << error.what() << '\n';
    return 1;
  }
}
