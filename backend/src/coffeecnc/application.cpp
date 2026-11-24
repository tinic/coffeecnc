#include "application.hpp"
#include "remote_server.hpp"

#include <coffeecnc/app/alarm_log.hpp>
#include <coffeecnc/app/diagnostics_feed.hpp>
#include <coffeecnc/app/file_system.hpp>
#include <coffeecnc/app/machine_data.hpp>
#include <coffeecnc/app/restore_helper.hpp>
#include <coffeecnc/app/offset_history.hpp>
#include <coffeecnc/app/panel_feed.hpp>
#include <coffeecnc/app/preview.hpp>
#include <coffeecnc/app/program_edit.hpp>
#include <coffeecnc/app/program_progress.hpp>
#include <coffeecnc/app/program_store.hpp>
#include <coffeecnc/app/runtime.hpp>
#include <coffeecnc/app/tool_model_store.hpp>
#include <coffeecnc/linuxcnc/hal_component.hpp>
#include <coffeecnc/linuxcnc/hal_control.hpp>
#include <coffeecnc/linuxcnc/hal_diagnostics.hpp>
#include <coffeecnc/linuxcnc/loaded_tools.hpp>
#include <coffeecnc/linuxcnc/nml_panel_source.hpp>
#include <coffeecnc/linuxcnc/nml_port.hpp>
#include <coffeecnc/linuxcnc/nml_status_reader.hpp>
#include <coffeecnc/linuxcnc/panel_mapping.hpp>
#include <coffeecnc/linuxcnc/preview_engine.hpp>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace coffeecnc {
namespace {

constexpr std::string_view usage_text =
    "usage: coffeecnc -ini <file> [--windowed] [--ui <directory>] [--inspector]\n"
    "       coffeecnc --check <file> [--write-ini]\n"
    "\n"
    "The CoffeeCNC operator panel, a DISPLAY program of LinuxCNC: set\n"
    "[DISPLAY] DISPLAY = coffeecnc in the machine's INI file and start LinuxCNC.\n"
    "\n"
    "  -ini <file>       the machine's INI file, as LinuxCNC passes it\n"
    "  --windowed        a 1280 x 800 window instead of the full screen\n"
    "  --ui <directory>  the built panel (the directory with index.html)\n"
    "  --inspector       allow WebKit's developer tools\n"
    "  --check <file>    check the INI file: does CoffeeCNC start with it, and which\n"
    "                    optional functions are on; LinuxCNC need not run\n"
    "  --write-ini       with --check, add missing settings and save a .bak\n"
    "  --help            this text\n";

void say(std::string_view reason) { std::cerr << "coffeecnc: " << reason << '\n' << std::flush; }

// Runs `halcmd -i <ini> -f <file>` in `directory` and waits for it, without a shell.
auto run_halcmd(std::string ini, std::string file, const std::string& directory)
    -> std::expected<void, std::string> {
  std::string program{"halcmd"};
  std::string ini_option{"-i"};
  std::string file_option{"-f"};
  // posix_spawnp takes char*, which only a string that is not const gives.
  std::array<char*, 6> argv{program.data(),     ini_option.data(), ini.data(),
                            file_option.data(), file.data(),       nullptr};
  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0)
    return std::unexpected(std::string("cannot start halcmd"));
  int error = posix_spawn_file_actions_addchdir_np(&actions, directory.c_str());
  pid_t child{};
  if (error == 0)
    error = posix_spawnp(&child, argv.front(), &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (error != 0)
    return std::unexpected(
        std::format("cannot start halcmd: {}", std::generic_category().message(error)));
  int status{};
  if (waitpid(child, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    return std::unexpected(std::string("halcmd failed"));
  return {};
}

// Runs `halcmd -s show <what> <name>`. A failed probe is unknown, never "no output".
auto halcmd_show(std::string_view what, std::string_view name) -> std::optional<std::string> {
  std::string program{"halcmd"};
  std::string script{"-s"};
  std::string show{"show"};
  std::string item{what};
  std::string pattern{name};
  std::array<char*, 6> argv{program.data(), script.data(), show.data(),
                            item.data(), pattern.data(), nullptr};
  std::array<int, 2> pipe_ends{};
  if (pipe(pipe_ends.data()) != 0)
    return {};
  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) {
    close(pipe_ends[0]);
    close(pipe_ends[1]);
    return {};
  }
  int error = posix_spawn_file_actions_adddup2(&actions, pipe_ends[1], STDOUT_FILENO);
  if (error == 0)
    error = posix_spawn_file_actions_addclose(&actions, pipe_ends[0]);
  pid_t child{};
  if (error == 0)
    error = posix_spawnp(&child, argv.front(), &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(pipe_ends[1]);
  std::string output;
  bool read_ok = true;
  if (error == 0) {
    std::array<char, 4096> buffer{};
    for (;;) {
      const auto got = read(pipe_ends[0], buffer.data(), buffer.size());
      if (got < 0 && errno == EINTR)
        continue;
      if (got <= 0) {
        read_ok = got == 0;
        break;
      }
      output.append(buffer.data(), static_cast<std::size_t>(got));
    }
  }
  close(pipe_ends[0]);
  int status{};
  pid_t waited = -1;
  if (error == 0) {
    do {
      waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
  }
  if (error != 0 || !read_ok || waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    return {};
  return output;
}

// iocontrol's coolant pin drives an output: its signal has a reader.
auto drives_output(const std::string& pin) -> std::optional<bool> {
  const auto pin_output = halcmd_show("pin", pin);
  if (!pin_output)
    return std::nullopt;
  const auto signal = pin_signal(*pin_output, pin);
  if (!signal)
    return std::nullopt;
  if (signal->empty())
    return false;
  const auto signal_output = halcmd_show("sig", *signal);
  return signal_output ? signal_has_reader(*signal_output, *signal) : std::nullopt;
}

} // namespace

auto usage() -> std::string_view { return usage_text; }

void configure_environment() {
  if (::setenv("WEBKIT_DISABLE_DMABUF_RENDERER", "1", 0) != 0)
    say("Cannot configure WebKit's renderer.");
}

auto parse_options(std::span<const std::string_view> arguments)
    -> std::expected<Options, std::string> {
  Options options;
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    const auto argument = arguments[i];
    const auto value = [&]() -> std::expected<std::string, std::string> {
      if (i + 1 >= arguments.size())
        return std::unexpected(std::format("{} needs a value.", argument));
      return std::string(arguments[++i]);
    };
    if (argument == "-ini" || argument == "--ini") {
      if (!options.ini_file.empty())
        return std::unexpected(std::string("Specify one INI file."));
      auto file = value();
      if (!file)
        return std::unexpected(std::move(file.error()));
      options.ini_file = std::move(*file);
    } else if (argument == "--check") {
      if (!options.ini_file.empty())
        return std::unexpected(std::string("Specify one INI file."));
      auto file = value();
      if (!file)
        return std::unexpected(std::move(file.error()));
      options.ini_file = std::move(*file);
      options.check = true;
    } else if (argument == "--write-ini") {
      options.write_ini = true;
    } else if (argument == "--ui") {
      auto directory = value();
      if (!directory)
        return std::unexpected(std::move(directory.error()));
      options.ui_directory = std::move(*directory);
    } else if (argument == "--windowed") {
      options.windowed = true;
    } else if (argument == "--inspector") {
      options.developer_tools = true;
    } else if (argument == "--help" || argument == "-h") {
      options.help = true;
    } else {
      return std::unexpected(std::format("Unknown option {}.", argument));
    }
  }
  if (!options.help) {
    if (options.write_ini && !options.check)
      return std::unexpected(std::string("--write-ini requires --check <file>."));
    if (options.ini_file.empty())
      return std::unexpected(std::string("No INI file: -ini <file> is required."));
  }
  return options;
}

auto panel_directory(const Options& options) -> std::expected<std::string, std::string> {
  std::filesystem::path directory;
  std::error_code error;
  if (options.ui_directory) {
    directory = *options.ui_directory;
  } else {
    const auto self = std::filesystem::read_symlink("/proc/self/exe", error);
    directory = self.parent_path().parent_path() / "share" / "coffeecnc" / "ui";
#ifdef COFFEECNC_SOURCE_UI_DIRECTORY
    if (error || !std::filesystem::exists(directory / "index.html", error))
      directory = std::filesystem::path{COFFEECNC_SOURCE_UI_DIRECTORY};
#endif
  }
  if (!std::filesystem::is_regular_file(directory / "index.html", error))
    return std::unexpected(std::format(
        "No panel in {}: index.html is missing. Build it (npm run build in ui/) or pass --ui.",
        directory.string()));
  const auto resolved = std::filesystem::canonical(directory, error);
  return error ? directory.string() : resolved.string();
}

namespace {

// The preview helper: COFFEECNC_PREVIEW_HELPER when set, else the program beside this one,
// else, in a build with COFFEECNC_DEVELOPMENT_PATHS, the one of the build tree this was built
// in. Empty when there is none.
auto preview_helper() -> std::string {
  std::error_code error;
  if (const char* given = std::getenv("COFFEECNC_PREVIEW_HELPER"); given != nullptr && *given != 0)
    return given;
  const auto self = std::filesystem::read_symlink("/proc/self/exe", error);
  if (const auto beside = self.parent_path() / "coffeecnc-preview";
      !error && std::filesystem::exists(beside, error))
    return beside.string();
#ifdef COFFEECNC_PREVIEW_HELPER_BUILT
  if (std::filesystem::exists(COFFEECNC_PREVIEW_HELPER_BUILT, error))
    return COFFEECNC_PREVIEW_HELPER_BUILT;
#endif
  return {};
}

auto unix_now() -> std::chrono::milliseconds {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch());
}

// Starts `coffeecnc --apply-restore <directory> --after <parent>` in a session of its own, so
// that it outlives this process and the `linuxcnc` script that started it. It gets no
// descriptor of this process but 0, 1 and 2: the LAN screen's socket is not kept open.
void start_restore_helper(const std::string& directory) {
  std::error_code error;
  auto self = std::filesystem::read_symlink("/proc/self/exe", error).string();
  if (error) {
    say("A restore is staged, but the helper cannot be found. It is not written.");
    return;
  }
  const auto parent = std::to_string(getppid());
  const auto child = spawn_detached({self, "--apply-restore", directory, "--after", parent});
  if (!child)
    say(std::format("A restore is staged, but the helper cannot start: {}. It is not written.",
                    child.error()));
  else
    say(std::format("restore helper pid={} waits for pid={}", *child, parent));
}

// An environment variable; empty when it is not set.
auto environment(const char* name) -> std::optional<std::string> {
  const char* value = std::getenv(name);
  return value != nullptr ? std::optional<std::string>{value} : std::nullopt;
}

auto state_name(PreviewState state) -> std::string_view {
  switch (state) {
  case PreviewState::pending:
    return "pending";
  case PreviewState::ready:
    return "ready";
  case PreviewState::failed:
    return "failed";
  case PreviewState::unavailable:
    return "unavailable";
  }
  return "unknown";
}

// Says on stderr how each preview computation ended, one line each: what shows on a machine
// that the helper was found and works, and what a test of an installed tree reads. Called by
// the preview thread only, like the engine it wraps.
class ReportingEngine final : public PreviewEngine {
public:
  explicit ReportingEngine(PreviewEngine& engine) : engine_(&engine) {}
  [[nodiscard]] auto compute(const PreviewInputs& inputs, const std::function<bool()>& cancelled)
      -> PreviewComputation override {
    auto computation = engine_->compute(inputs, cancelled);
    if (!computation.cancelled && inputs.program_file)
      std::cerr << std::format("coffeecnc: preview state={} segments={} program={}\n",
                               state_name(computation.outcome.state),
                               computation.outcome.segments.size(), *inputs.program_file)
                << std::flush;
    return computation;
  }

private:
  PreviewEngine* engine_;
};

// A directory of this process for the preview's private files, removed with it.
class WorkDirectory {
public:
  WorkDirectory() {
    std::error_code error;
    std::string pattern =
        (std::filesystem::temp_directory_path(error) / "coffeecnc-XXXXXX").string();
    if (!error && mkdtemp(pattern.data()) != nullptr)
      path_ = pattern;
  }
  WorkDirectory(const WorkDirectory&) = delete;
  auto operator=(const WorkDirectory&) -> WorkDirectory& = delete;
  ~WorkDirectory() {
    std::error_code error;
    if (!path_.empty())
      std::filesystem::remove_all(path_, error);
  }
  [[nodiscard]] auto path() const -> const std::string& { return path_; }

private:
  std::string path_;
};

// Import once, while the runtime lock is held. A restart after an interrupted import keeps
// files already copied. A successful import can never restore a record later invalidated.
auto import_runtime_state(LocalToolModelFiles& files, const MachineConfig& config,
                          const std::string& directory) -> ToolModelFiles::Done {
  const auto marker = directory + "/.imported";
  const auto imported = files.read(marker, 0);
  if (!imported)
    return std::unexpected(imported.error());
  if (*imported)
    return {};

  const auto store = [&](const std::string& target, std::string_view bytes) -> ToolModelFiles::Done {
    const auto temporary = target + ".import";
    const auto write = [&]() -> ToolModelFiles::Done {
      if (auto result = files.open_temporary(temporary); !result)
        return result;
      if (auto result = files.write(bytes); !result)
        return result;
      if (auto result = files.sync(); !result)
        return result;
      return files.rename(temporary, target);
    };
    auto result = write();
    if (!result)
      files.discard(temporary);
    return result;
  };
  const std::filesystem::path ini_parent = config.directory;
  const auto tool_parent = std::filesystem::path(config.tool_table).parent_path();
  struct Record { std::filesystem::path parent; const char* name; std::size_t limit; };
  const std::array records{
      Record{ini_parent, ".coffeecnc-breakpoint.ngc", ProgramStore::max_program_bytes + 256},
      Record{ini_parent, ".coffeecnc-breakpoint.txt", 512},
      Record{ini_parent, ".coffeecnc-spindle-hold", 512},
      Record{tool_parent, "coffeecnc-spindle-tool.txt", 512},
      Record{tool_parent, "coffeecnc-touched-off.txt", ProgramStore::max_program_bytes}};
  for (const auto& record : records) {
    if (record.parent.empty())
      continue;
    const auto target = directory + '/' + record.name;
    const auto current = files.read(target, 0);
    if (!current)
      return std::unexpected(std::format("{}: {}", target, current.error()));
    if (*current)
      continue;
    const auto source = (record.parent / record.name).string();
    const auto old = files.read(source, record.limit);
    if (!old)
      return std::unexpected(std::format("{}: {}", source, old.error()));
    if (const auto& content = *old) {
      if (content->more)
        return std::unexpected(std::format("{} is too large to import", source));
      if (const auto copied = store(target, content->bytes); !copied)
        return std::unexpected(std::format("{}: {}", target, copied.error()));
    }
  }
  if (const auto saved = files.sync_directory(directory); !saved)
    return saved;
  if (const auto saved = store(marker, "1\n"); !saved)
    return saved;
  return files.sync_directory(directory);
}

} // namespace

auto run_application(const Options& options, const ApplicationHooks& hooks) -> int {
  auto config = load_machine_config(options.ini_file);
  if (!config) {
    say(config.error());
    return 1;
  }
  // Programs come from anywhere the operator can reach (turo: not only nc_files): the browser
  // starts in the program directory and offers it, the home folder and USB media as shortcuts.
  {
    std::error_code error;
    const auto add = [&](std::string name, const std::optional<std::string>& path) {
      if (path && !path->empty() && path->front() == '/' &&
          std::filesystem::is_directory(*path, error))
        config->program_places.push_back({std::move(name), *path});
    };
    add("Programs", config->program_prefix);
    add("Home", environment("HOME"));
    if (const auto user = environment("USER"); user && !user->empty()) {
      const auto before_usb = config->program_places.size();
      add("USB", "/media/" + *user);
      add(config->program_places.size() == before_usb ? "USB" : "USB (system)", "/run/media/" + *user);
    }
  }
  const auto ui = panel_directory(options);
  if (!ui) {
    say(ui.error());
    return 1;
  }
  // Which panel and which helper this process uses: an installed tree's own, or a checkout's.
  const auto helper = preview_helper();
  std::cerr << std::format("coffeecnc: panel={} preview_helper={}\n", *ui,
                           helper.empty() ? std::string_view{"none"} : std::string_view{helper})
            << std::flush;
  if (helper.empty())
    say("No preview helper: coffeecnc-preview is not beside the executable. Programs are shown "
        "without a toolpath preview.");
  if (!initialise_display()) {
    say("Cannot open a display.");
    return 1;
  }
  auto hal = HalComponent::create("coffeecnc", config->signals);
  if (!hal) {
    say(std::format("Cannot create the HAL component: {}. Is LinuxCNC running?", hal.error()));
    return 1;
  }
  if (!config->postgui_halfile.empty()) {
    const auto wired = run_halcmd(config->ini_file, config->postgui_halfile, config->directory);
    if (!wired) {
      say(std::format("[HAL] POSTGUI_HALFILE {}: {}.", config->postgui_halfile, wired.error()));
      return 1;
    }
  }
  if (config->manual_tool_change) {
    const auto wired = hal->connect_manual_tool_change();
    if (!wired) {
      say(std::format("[COFFEECNC] MANUAL_TOOLCHANGE: {}", wired.error()));
      return 1;
    }
  }
  const auto flood = drives_output("iocontrol.0.coolant-flood");
  const auto mist = drives_output("iocontrol.0.coolant-mist");
  if (flood && mist) {
    config->coolant = CoolantOutputs{.flood = *flood, .mist = *mist};
    say(std::format("coolant outputs: flood={} mist={}", *flood, *mist));
  } else {
    config->coolant.reset();
    say("coolant outputs unknown; offering both controls");
  }
  HalShared shared;
  NmlLoads loads;
  const auto location = resolve_tool_model_location({config->machine_id, config->data_directory,
                              environment("XDG_DATA_HOME"), environment("HOME")});
  LocalToolModelFiles runtime_files;
  if (location.availability == ToolModelAvailability::available) {
    const auto directory = location.directory + "/runtime";
    if (const auto prepared = prepare_data_directory(runtime_files, directory); !prepared) {
      say(std::format("Cannot prepare machine runtime data in {}: {}", directory, prepared.error()));
      return 1;
    }
    if (const auto locked = runtime_files.lock(directory); !locked) {
      say(std::format("Cannot lock machine runtime data in {}: {}", directory, locked.error().reason));
      return 1;
    }
    if (const auto moved = import_runtime_state(runtime_files, *config, directory); !moved) {
      say(std::format("Cannot import machine runtime data into {}: {}", directory, moved.error()));
      return 1;
    }
    config->runtime_directory = directory;
  }
  auto port = NmlPort::connect(*config, &shared, &loads);
  if (!port) {
    say(port.error());
    return 1;
  }
  NmlStatusMailbox mailbox{&loads};
  auto reader = NmlStatusReader::connect(*config, &shared, &mailbox);
  if (!reader) {
    say(reader.error());
    return 1;
  }
  HalControl control{*hal, shared};
  const LocalFileSystem files;
  const ProgramStore programs{files, "/", config->program_prefix};
  // LinuxCNC's io process made the tool table memory when it started.
  const auto tools = LoadedToolTable::attach(config->millimetres_per_unit);
  if (!tools) {
    say(tools.error());
    return 1;
  }
  // One pass over LinuxCNC's tool data per period, by the publisher thread: shown, and kept
  // with every field for the preview.
  std::vector<RawTool> pass;
  NmlPanelSource source{*config, mailbox,
                        [&]() -> std::optional<std::vector<LoadedTool>> {
                          pass = tools->read_raw();
                          return loaded_tools(pass, config->millimetres_per_unit);
                        },
                        [&programs](std::string_view path, std::uint64_t revision)
                            -> std::expected<ProgramText, std::string> {
                          return programs.read_program(path, revision);
                        },
                        &loads};
  source.raw_tools([&pass]() -> std::vector<PreviewTool> { return pass; });

  // The services outlive the GTK callbacks at close, and stop while the window can still
  // accept their final posts.
  // The preview: an engine that starts the helper, and the thread that computes with it. Both
  // outlive the publisher, which hands contexts over and takes results.
  const WorkDirectory work;
  const auto preview_settings = load_preview_settings(*config, helper, work.path());
  HelperPreviewEngine helper_engine{preview_settings};
  HelperPreviewEngine restart_engine{preview_settings};
  ReportingEngine engine{helper_engine};
  PreviewWorker preview{engine};
  // The tool models: saved by a thread of their own, which outlives the runtime that hands it
  // commands and the publisher that asks it for snapshots.
  LocalToolModelFiles model_files;
  LocalToolModelFiles progress_files;
  LocalToolModelFiles edit_files;
  LocalToolModelFiles copy_files;
  LocalToolModelFiles upload_files;
  LocalToolModelFiles history_files;
  // Backup and restore, on the worker thread.
  LocalToolModelFiles machine_data_files;
  std::unique_ptr<MachineData> machine_data;
  LocalToolModelFiles alarm_files;
  // Every operator message and refusal posted to the page is also kept in the alarm log.
  std::unique_ptr<AlarmLog> alarms;
  std::unique_ptr<ProgramProgress> progress;
  std::unique_ptr<OffsetHistoryStore> history;
  std::optional<ProgramWriter> program_writer;
  ProgramWriter copy_writer{programs, copy_files};
  ProgramWriter upload_writer{programs, upload_files};
  std::unique_ptr<ToolModelStore> models;
  // The diagnostics view: HAL is read on the feed's thread, and only while a page watches. The
  // pin-to-signal wiring is that thread's alone. The HAL tools get LinuxCNC's environment.
  HalApiLookup hal_lookup;
  HalToolLauncher launcher{environment("PATH").value_or("/usr/bin:/bin"),
                            hal_tool_environment(config->ini_file)};
  const auto diagnostic_rows = diagnostic_inputs(*config);
  PinSignals wiring;
  std::unique_ptr<DiagnosticsFeed> diagnostics;
  std::unique_ptr<Runtime> runtime;
  std::unique_ptr<PanelFeed> feed;
  std::atomic<Runtime*> upload_runtime{};
  std::unique_ptr<RemoteServer> remote;
  PanelWindow window{{.ui_directory = *ui,
                      .windowed = options.windowed,
                      .developer_tools = options.developer_tools},
                     {.begin_page = [&]() -> PageToken {
                        return runtime ? runtime->begin_page(true) : PageToken{};
                      },
                      .end_page = [&](PageToken page) -> void {
                        if (feed)
                          feed->page_ended(page);
                        if (runtime)
                          runtime->end_page(page);
                      },
                      .message = [&](PageToken page, std::string_view json,
                                     std::chrono::milliseconds rendered_at) -> void {
                        if (!runtime)
                          return;
                        if (alarms)
                          alarms->request(page, json);
                        auto outcome = runtime->submit(page, json, rendered_at);
                        const bool ready = outcome.send_snapshots;
                        if (alarms)
                          alarms->posted(outcome.page, outcome.messages, unix_now());
                        window.post(outcome.page, std::move(outcome.messages));
                        if (ready && feed)
                          feed->page_ready(page);
                      },
                      .frame = [&](PageToken page, std::chrono::milliseconds rendered_at) -> void {
                        if (runtime)
                          runtime->panel_frame(page, rendered_at);
                      },
                      .loaded_program = [&]() -> std::string {
                        return runtime ? runtime->loaded_program() : std::string{};
                      }}};
  const struct Shutdown {
    PanelWindow& window;
    std::unique_ptr<PanelFeed>& feed;
    std::unique_ptr<Runtime>& runtime;
    std::unique_ptr<ToolModelStore>& models;
    std::unique_ptr<RemoteServer>& remote;
    std::unique_ptr<AlarmLog>& alarms;
    std::unique_ptr<DiagnosticsFeed>& diagnostics;
    ~Shutdown() {
      window.close();
      feed.reset();
      if (remote)
        remote->stop();
      runtime.reset();
      // It posts through the alarm log too.
      diagnostics.reset();
      if (models)
        models->stop();
      // What the others posted while they stopped is appended, under the data directory's
      // lock, which the tool models hold until they are destroyed.
      alarms.reset();
      models.reset();
    }
  } shutdown{window, feed, runtime, models, remote, alarms, diagnostics};
  // What the services post to the page, as the alarm log sees it too.
  const auto post = [&window, &alarms](PageToken page, std::vector<std::string> messages) -> void {
    if (alarms)
      alarms->posted(page, messages, unix_now());
    window.post(page, std::move(messages));
  };
  if (config->remote_port != 0) {
    try {
      RemoteServer::Upload upload;
      if (config->remote_upload) {
        upload = [&](std::string_view name, std::string_view bytes, bool replace)
            -> std::expected<void, std::string> {
          auto* target = upload_runtime.load();
          if (target == nullptr)
            return std::unexpected("CoffeeCNC is starting.");
          ProgramWriter::Publish publish;
          if (replace) {
            publish = [target](const std::string& path, const ProgramWriter::Commit& commit)
                -> std::expected<void, std::string> {
              return target->publish_program(path, commit);
            };
          }
          if (auto written = upload_writer.receive(name, bytes, replace, publish); !written)
            return std::unexpected(written.error().text);
          return {};
        };
      }
      remote = std::make_unique<RemoteServer>(*ui, config->remote_port, config->remote_hosts,
                                              std::move(upload));
      say(std::format("LAN screen on port {}.", remote->port()));
    } catch (const std::exception& error) {
      say(std::format("The LAN screen could not start: {}", error.what()));
    }
  }
  int code = 0;
  try {
    // Before the tool models take the data directory's lock: the start-up check of a restore
    // takes it for itself, and releases it.
    {
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
      if (location.availability == ToolModelAvailability::available)
        paths.data_directory = location.directory;
      machine_data = std::make_unique<MachineData>(machine_data_files, std::move(paths), unix_now);
    }
    // Its writer delivers only for a command or a snapshot, after the alarm log below exists.
    models = std::make_unique<ToolModelStore>(model_files, location, post, config->limits);
    // In the machine's data directory, next to the offset history, under the lock the tool
    // models hold; without one, for this run only.
    alarms = std::make_unique<AlarmLog>(
        alarm_files,
        location.availability == ToolModelAvailability::available ? location.directory : "",
        location.availability == ToolModelAvailability::available ? "" : location.reason,
        [&window](PageToken page, std::vector<std::string> messages) -> void {
          window.post(page, std::move(messages));
        },
        [store = models.get()]() -> std::optional<std::string> {
          const auto held = store->held_location();
          if (held.availability == ToolModelAvailability::available)
            return std::nullopt;
          return held.reason;
        });
    progress = std::make_unique<ProgramProgress>(progress_files, location.directory);
    // Next to the program progress, in the machine's data directory; without one, for this
    // session only.
    history = std::make_unique<OffsetHistoryStore>(
        history_files,
        location.availability == ToolModelAvailability::available ? location.directory : "",
        location.availability == ToolModelAvailability::available
            ? ""
            : location.reason);
    program_writer.emplace(programs, edit_files);
    models->set_fixed_probe_tool(config->touch_probe_tool);
    diagnostics = std::make_unique<DiagnosticsFeed>(
        DiagnosticsFeed::Source{
            .begin = [&wiring]() -> void {
              // Unknown wiring (halcmd failed) leaves every linked pin `live`.
              wiring = pin_signals(halcmd_show("sig", "").value_or(""));
            },
            .read = [&]() -> Diagnostics {
              return {read_diagnostics(diagnostic_rows, hal_lookup, wiring), launcher.states()};
            },
            .launch = [&launcher](HalTool tool) -> std::optional<std::string> {
              return launcher.launch(tool);
            }},
        post);
    runtime = std::make_unique<Runtime>(
        **port, **reader, control, config->limits, programs,
        [&window, &remote, &alarms](const Outcome& outcome) -> void {
          if (alarms)
            alarms->posted(outcome.page, outcome.messages, unix_now());
          window.post(outcome.page, outcome.messages);
          if (remote)
            remote->notifications(outcome.messages);
        },
        models.get(), [&window](PageToken page) -> void { window.close_after_posts(page); },
        &preview, &restart_engine, &*program_writer, history.get(), diagnostics.get(), &copy_writer,
        machine_data.get());
    upload_runtime.store(runtime.get());
    PanelFeed::Mirror mirror;
    if (remote) {
      mirror = [&remote](std::span<const std::string> messages, std::span<const PanelRecord> records,
                         const Status& status, bool has_program) -> void {
        remote->publish(messages, records, status, has_program);
      };
    }
    // The `tool_models` snapshot follows the `hello` it belongs to.
    feed = std::make_unique<PanelFeed>(
        source, post, &preview,
        [&models, &alarms, &machine_data, &post](PageToken page) -> void {
          models->send_snapshot(page);
          alarms->send_snapshot(page);
          // What a restore before this run came to, once.
          if (auto notice = machine_data->take_notice())
            post(page, {encode_message(notice->first, notice->second, unix_now())});
        },
        progress.get(), history.get(), std::move(mirror));
    if (hooks.before_load)
      hooks.before_load(window, *config);
    window.run();
  } catch (const std::exception& error) {
    say(error.what());
    code = 1;
  }
  if (machine_data && machine_data->staged() && !location.directory.empty())
    start_restore_helper(location.directory);
  return code;
}

auto apply_restore(std::span<const std::string_view> arguments) -> int {
  if (arguments.size() != 4 || arguments[2] != "--after") {
    say("usage: coffeecnc --apply-restore <data directory> --after <pid>");
    return 1;
  }
  long parent{};
  try {
    parent = std::stol(std::string(arguments[3]));
  } catch (const std::exception&) {
    say("--after needs a process id.");
    return 1;
  }
  LocalToolModelFiles files;
  LocalRestoreHost host;
  const auto outcome = run_restore_helper(files, std::string(arguments[1]), parent, host);
  say(outcome.line);
  return outcome.code;
}

} // namespace coffeecnc
