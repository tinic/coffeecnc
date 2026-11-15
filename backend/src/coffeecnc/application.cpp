#include "application.hpp"

#include <coffeecnc/app/file_system.hpp>
#include <coffeecnc/app/panel_feed.hpp>
#include <coffeecnc/app/preview.hpp>
#include <coffeecnc/app/program_store.hpp>
#include <coffeecnc/app/runtime.hpp>
#include <coffeecnc/linuxcnc/hal_component.hpp>
#include <coffeecnc/linuxcnc/hal_control.hpp>
#include <coffeecnc/linuxcnc/loaded_tools.hpp>
#include <coffeecnc/linuxcnc/nml_panel_source.hpp>
#include <coffeecnc/linuxcnc/nml_port.hpp>
#include <coffeecnc/linuxcnc/nml_status_reader.hpp>
#include <coffeecnc/linuxcnc/preview_engine.hpp>

#include <array>
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
    "\n"
    "The CoffeeCNC operator panel, a DISPLAY program of LinuxCNC: set\n"
    "[DISPLAY] DISPLAY = coffeecnc in the machine's INI file and start LinuxCNC.\n"
    "\n"
    "  -ini <file>       the machine's INI file, as LinuxCNC passes it\n"
    "  --windowed        a 1280 x 800 window instead of the full screen\n"
    "  --ui <directory>  the built panel (the directory with index.html)\n"
    "  --inspector       allow WebKit's developer tools\n"
    "  --help            this text\n";

void say(std::string_view reason) { std::cerr << "coffeecnc: " << reason << '\n' << std::flush; }

// Runs `halcmd -i <ini> -f <file>` in `directory` and waits for it, without a shell.
auto run_halcmd(std::string ini, std::string file, const std::string& directory)
    -> std::expected<void, std::string> {
  std::string program{"halcmd"};
  std::string ini_option{"-i"};
  std::string file_option{"-f"};
  // posix_spawnp takes char*, which only a string that is not const gives.
  std::array<char*, 6> argv{program.data(), ini_option.data(), ini.data(),
                            file_option.data(), file.data(), nullptr};
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
      auto file = value();
      if (!file)
        return std::unexpected(std::move(file.error()));
      options.ini_file = std::move(*file);
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
  if (!options.help && options.ini_file.empty())
    return std::unexpected(std::string("No INI file: -ini <file> is required."));
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

auto run_application(const Options& options, const ApplicationHooks& hooks) -> int {
  const auto config = load_machine_config(options.ini_file);
  if (!config) {
    say(config.error());
    return 1;
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
  HalShared shared;
  NmlLoads loads;
  auto port = NmlPort::connect(*config, &shared, &loads);
  if (!port) {
    say(port.error());
    return 1;
  }
  NmlStatusMailbox mailbox;
  auto reader = NmlStatusReader::connect(*config, &shared, &mailbox);
  if (!reader) {
    say(reader.error());
    return 1;
  }
  HalControl control{*hal, shared};
  const LocalFileSystem files;
  const ProgramStore programs{files, config->program_prefix};
  // LinuxCNC's io process made the tool table memory when it started.
  const auto tools = LoadedToolTable::attach(config->millimetres_per_unit);
  if (!tools) {
    say(tools.error());
    return 1;
  }
  // One pass over LinuxCNC's tool data per period, by the publisher thread: shown, and kept
  // with every field for the preview.
  std::vector<RawTool> pass;
  NmlPanelSource source{
      *config, mailbox,
      [&]() -> std::optional<std::vector<LoadedTool>> {
        pass = tools->read_raw();
        return loaded_tools(pass, config->millimetres_per_unit);
      },
      [&programs](std::string_view path,
                  std::uint64_t revision) -> std::expected<ProgramText, std::string> {
        return programs.read_program(path, revision);
      },
      &loads};
  source.raw_tools([&pass]() -> std::vector<PreviewTool> { return pass; });

  // Declared in the order they are made; destroyed in reverse: the publisher first, then the
  // runtime with its safe shutdown, the window last. By then the window is closed and drops
  // what the worker still posts.
  // The preview: an engine that starts the helper, and the thread that computes with it. Both
  // outlive the publisher, which hands contexts over and takes results.
  const WorkDirectory work;
  HelperPreviewEngine helper_engine{load_preview_settings(*config, helper, work.path())};
  ReportingEngine engine{helper_engine};
  PreviewWorker preview{engine};
  std::unique_ptr<Runtime> runtime;
  std::unique_ptr<PanelFeed> feed;
  PanelWindow window{
      {.ui_directory = *ui,
       .windowed = options.windowed,
       .developer_tools = options.developer_tools},
      {.begin_page = [&]() -> PageToken { return runtime->begin_page(); },
       .end_page = [&](PageToken page) -> void {
         feed->page_ended(page);
         runtime->end_page(page);
       },
       .message = [&](PageToken page, std::string_view json) -> void {
         auto outcome = runtime->submit(page, json);
         const bool ready = outcome.send_snapshots;
         window.post(outcome.page, std::move(outcome.messages));
         if (ready)
           feed->page_ready(page);
       }}};
  int code = 0;
  try {
    runtime = std::make_unique<Runtime>(
        **port, **reader, control, config->limits, programs,
        [&window](const Outcome& outcome) -> void { window.post(outcome.page, outcome.messages); });
    feed = std::make_unique<PanelFeed>(
        source, [&window](PageToken page, std::vector<std::string> messages) -> void {
          window.post(page, std::move(messages));
        },
        &preview);
    if (hooks.before_load)
      hooks.before_load(window, *config);
    window.run();
  } catch (const std::exception& error) {
    say(error.what());
    code = 1;
  }
  window.close();
  feed.reset();
  runtime.reset();
  return code;
}

} // namespace coffeecnc
