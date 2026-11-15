#include <coffeecnc/linuxcnc/preview_engine.hpp>

#include "preview_wire.hpp"
#include "program_watch.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <inifile.hh>

namespace coffeecnc {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// ---- the files of a computation ---------------------------------------------------------------

struct SnapshotFile {
  std::string original; // where it was read
  std::string name;     // what it is called in the private directory
  std::string bytes;    // as read
};

// For one called name: the file the search path gave, or none.
struct Call {
  std::string name;
  std::string found;
};

struct Manifest {
  std::vector<SnapshotFile> files; // the program first
  std::vector<Call> calls;
};

// The bytes of a file, when it can be read and is not larger than `limit`.
auto read_file(const std::string& path, std::size_t limit) -> std::optional<std::string> {
  std::error_code error;
  if (!fs::is_regular_file(path, error))
    return {};
  std::ifstream in{path, std::ios::binary};
  if (!in)
    return {};
  std::string bytes(limit + 1, '\0');
  in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (in.bad() || static_cast<std::size_t>(in.gcount()) > limit)
    return {};
  bytes.resize(static_cast<std::size_t>(in.gcount()));
  return bytes;
}

// As Interp::find_ngc_file looks (LinuxCNC 2.9.4, rs274ngc_pre.cc:2660): the first directory
// of the search path that has `name.ngc`.
auto find_subroutine(const std::vector<std::string>& search_path, const std::string& name)
    -> std::string {
  for (const auto& directory : search_path) {
    const auto file = fs::path(directory) / (name + ".ngc");
    std::error_code error;
    if (fs::is_regular_file(file, error) && std::ifstream{file}.good())
      return file.string();
  }
  return {};
}

// The names a text calls as subroutines: `o<name> call`, `o123 call`. A name is what a file
// could be called; a computed one is not a name (the helper refuses that block).
auto called_names(std::string_view text) -> std::vector<std::string> {
  std::vector<std::string> names;
  while (!text.empty()) {
    const auto end = text.find('\n');
    const auto block = bare_block(text.substr(0, end));
    text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
    std::string_view rest = block;
    if (!rest.empty() && rest.front() == '/')
      rest.remove_prefix(1);
    if (rest.size() > 1 && rest.front() == 'n' && std::isdigit(static_cast<unsigned char>(rest[1]))) {
      rest.remove_prefix(1);
      while (!rest.empty() &&
             (std::isdigit(static_cast<unsigned char>(rest.front())) != 0 || rest.front() == '.'))
        rest.remove_prefix(1);
    }
    if (rest.size() < 2 || rest.front() != 'o')
      continue;
    rest.remove_prefix(1);
    std::string name;
    if (rest.front() == '<') {
      const auto close = rest.find('>');
      if (close == std::string_view::npos)
        continue;
      name = std::string(rest.substr(1, close - 1));
      rest.remove_prefix(close + 1);
    } else {
      while (!rest.empty() && std::isdigit(static_cast<unsigned char>(rest.front())) != 0) {
        name += rest.front();
        rest.remove_prefix(1);
      }
    }
    if (name.empty() || !rest.starts_with("call") || name.find('/') != std::string::npos ||
        name.find("..") != std::string::npos)
      continue;
    if (std::ranges::find(names, name) == names.end())
      names.push_back(std::move(name));
  }
  return names;
}

auto megabytes(std::size_t bytes) -> std::string {
  return std::format("{} MiB", bytes / (1024 * 1024));
}

// The program and every subroutine file it reaches, read once each.
auto take_snapshot(const PreviewSettings& settings, const std::string& program)
    -> std::expected<Manifest, std::string> {
  Manifest manifest;
  auto bytes = read_file(program, settings.max_input_bytes);
  if (!bytes)
    return std::unexpected(std::format("The program cannot be read for a preview, or is larger "
                                       "than {}.",
                                       megabytes(settings.max_input_bytes)));
  std::size_t total = bytes->size();
  manifest.files.push_back({program, "coffeecnc-program.ngc", std::move(*bytes)});
  std::set<std::string> seen;
  for (std::size_t next = 0; next < manifest.files.size(); ++next) {
    for (auto& name : called_names(manifest.files[next].bytes)) {
      if (!seen.insert(name).second)
        continue;
      auto found = find_subroutine(settings.search_path, name);
      manifest.calls.push_back({name, found});
      if (found.empty())
        continue;
      if (manifest.files.size() >= settings.max_files)
        return std::unexpected(std::format("The program uses more than {} files: no preview.",
                                           settings.max_files));
      auto text = read_file(found, settings.max_input_bytes - total);
      if (!text)
        return std::unexpected(std::format("The program and its subroutine files cannot be "
                                           "read, or are larger than {} together: no preview.",
                                           megabytes(settings.max_input_bytes)));
      total += text->size();
      manifest.files.push_back({std::move(found), name + ".ngc", std::move(*text)});
    }
  }
  return manifest;
}

// Whether an original is no longer what the manifest holds: every call searched again, every
// file compared byte for byte.
auto files_differ(const Manifest& manifest, const std::vector<std::string>& search_path) -> bool {
  for (const auto& call : manifest.calls)
    if (find_subroutine(search_path, call.name) != call.found)
      return true;
  return std::ranges::any_of(manifest.files, [](const SnapshotFile& file) -> bool {
    const auto now = read_file(file.original, file.bytes.size());
    return !now || *now != file.bytes;
  });
}

// ---- the start state ---------------------------------------------------------------------------

// Task's modal state as blocks for the helper's interpreter, or why it cannot be replayed.
auto start_codes(const InterpreterStart& start) -> std::expected<std::vector<std::string>, std::string> {
  std::string units;
  std::string plane;
  std::string distance;
  std::string arc_distance;
  std::string feed_mode;
  std::string retract;
  std::string motion;
  for (const auto& code : start.active_codes) {
    if (code.size() < 2 || code.front() != 'G')
      continue;
    double number = 0;
    const auto [end, error] = std::from_chars(code.data() + 1, code.data() + code.size(), number);
    if (error != std::errc{} || end != code.data() + code.size())
      continue;
    const auto tenths = std::lround(number * 10);
    switch (tenths) {
    case 200:
    case 210:
      units = code;
      break;
    case 170:
    case 180:
    case 190:
      plane = code;
      break;
    case 900:
    case 910:
      distance = code;
      break;
    case 901:
    case 911:
      arc_distance = code;
      break;
    case 930:
    case 940:
    case 950:
      feed_mode = code;
      break;
    case 980:
    case 990:
      retract = code;
      break;
    case 0:
    case 10:
      motion = code;
      break;
    case 800:
      motion.clear();
      break;
    case 171:
    case 181:
    case 191:
      return std::unexpected(std::format("A UVW plane ({}) is active: no preview.", code));
    case 410:
    case 411:
    case 420:
    case 421:
      return std::unexpected(std::format("Cutter compensation ({}) is active: no preview.", code));
    case 20:
    case 30:
    case 50:
    case 51:
    case 52:
    case 53:
    case 330:
    case 331:
    case 382:
    case 383:
    case 384:
    case 385:
    case 730:
    case 760:
    case 810:
    case 820:
    case 830:
    case 840:
    case 850:
    case 860:
    case 870:
    case 880:
    case 890:
      return std::unexpected(std::format(
          "The motion mode {} is active from before: a preview is computed only when a program "
          "starts in G0, G1 or G80.",
          code));
    default:
      break;
    }
  }
  std::vector<std::string> codes;
  for (const auto* code : {&units, &plane, &distance, &arc_distance, &retract})
    if (!code->empty())
      codes.push_back(*code);
  if (motion == "G1") {
    // A bare G1 is refused while the feed is zero: set under a feed, then the feed in force.
    codes.insert(codes.end(), {"G94", "F1", "G1"});
    if (!feed_mode.empty())
      codes.push_back(feed_mode);
    if (feed_mode != "G93")
      codes.push_back(std::format(
          "F{:.6f}", start.feed / (start.program_units == LengthUnit::inch ? 25.4 : 1.0)));
  } else {
    if (!motion.empty())
      codes.push_back(motion);
    if (!feed_mode.empty())
      codes.push_back(feed_mode);
  }
  return codes;
}

// The machine's parameter file with what is live written over it: the active work system, its
// rotation and G92. Machine units, as the interpreter keeps the file.
auto parameter_text(const PreviewSettings& settings, const PreviewInputs& inputs) -> std::string {
  std::map<int, double> parameters;
  if (const auto saved = read_file(settings.parameter_file, std::size_t{1024} * 1024)) {
    std::istringstream lines{*saved};
    int number = 0;
    double value = 0;
    while (lines >> number >> value)
      parameters[number] = value;
  }
  const double unit = settings.units_per_mm;
  const int wcs = inputs.start.wcs;
  const auto& system = inputs.systems.at(static_cast<std::size_t>(wcs - 1));
  const int base = 5221 + 20 * (wcs - 1);
  parameters[5220] = wcs;
  for (int axis = 0; axis < 3; ++axis) {
    parameters[base + axis] = system.offset.at(static_cast<std::size_t>(axis)) * unit;
    parameters[5211 + axis] = inputs.g92.at(static_cast<std::size_t>(axis)) * unit;
  }
  parameters[base + 9] = system.rotation_xy;
  parameters[5210] = inputs.g92 != Vec3{} ? 1 : 0;
  std::string text;
  for (const auto& [number, value] : parameters)
    text += std::format("{}\t{:.17g}\n", number, value);
  return text;
}

auto ini_text(const PreviewSettings& settings, const std::string& directory) -> std::string {
  std::string text = "[RS274NGC]\nPARAMETER_FILE = preview.var\nINI_VARS = 0\nHAL_PIN_VARS = 0\n"
                     "DISABLE_FANUC_STYLE_SUB = 1\n";
  text += std::format("SUBROUTINE_PATH = {}\n", directory);
  for (const auto& option : settings.interpreter_options)
    text += option + "\n";
  text += std::format("[DISPLAY]\nPROGRAM_PREFIX = {}\n", directory);
  text += std::format("[EMCIO]\nTOOL_CHANGE_QUILL_UP = {}\n", settings.tool_change_quill_up ? 1 : 0);
  text += "[TRAJ]\nSPINDLES = 1\n";
  return text;
}

auto write_file(const fs::path& file, std::string_view text) -> bool {
  std::ofstream out{file, std::ios::binary | std::ios::trunc};
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  return out.good();
}

// A directory of one computation, removed with everything in it.
class PrivateDirectory {
public:
  explicit PrivateDirectory(const std::string& parent) {
    std::error_code error;
    fs::create_directories(parent, error);
    std::string pattern = (fs::path(parent) / "preview-XXXXXX").string();
    if (mkdtemp(pattern.data()) != nullptr)
      path_ = pattern;
  }
  PrivateDirectory(const PrivateDirectory&) = delete;
  auto operator=(const PrivateDirectory&) -> PrivateDirectory& = delete;
  ~PrivateDirectory() {
    std::error_code error;
    if (!path_.empty())
      fs::remove_all(path_, error);
  }
  [[nodiscard]] auto path() const -> const fs::path& { return path_; }

private:
  fs::path path_;
};

// ---- the helper process ------------------------------------------------------------------------

struct HelperRun {
  bool started{};
  bool cancelled{};
  bool timed_out{};
  bool too_much{};
  int status{}; // of waitpid
  std::string result;
  std::string diagnostics;
};

void limit(int resource, rlim_t value) {
  const rlimit bound{value, value};
  static_cast<void>(setrlimit(resource, &bound));
}

auto run_helper(const PreviewSettings& settings, const fs::path& directory,
                const std::string& request_file, const std::function<bool()>& cancelled)
    -> HelperRun {
  HelperRun run;
  std::array<int, 2> result{};
  std::array<int, 2> diagnostics{};
  if (pipe2(result.data(), O_CLOEXEC) != 0)
    return run;
  if (pipe2(diagnostics.data(), O_CLOEXEC) != 0) {
    close(result[0]);
    close(result[1]);
    return run;
  }
  const int null = open("/dev/null", O_RDONLY | O_CLOEXEC);
  // Everything the child needs is made before the fork: after it only system calls.
  const std::string home = "HOME=" + directory.string();
  const std::string ini = "INI_FILE_NAME=" + (directory / "preview.ini").string();
  std::array<char*, 4> arguments{const_cast<char*>("coffeecnc-preview"), // NOLINT: execve's type
                                 const_cast<char*>(request_file.c_str()), // NOLINT
                                 const_cast<char*>("3"), nullptr};        // NOLINT
  std::array<char*, 8> environment{
      const_cast<char*>(home.c_str()), // NOLINT
      const_cast<char*>(ini.c_str()),  // NOLINT
      const_cast<char*>("PATH=/usr/bin:/bin"),          // NOLINT
      const_cast<char*>("LC_ALL=C"),                    // NOLINT
      const_cast<char*>("PYTHONDONTWRITEBYTECODE=1"),   // NOLINT
      const_cast<char*>("PYTHONNOUSERSITE=1"),          // NOLINT
      const_cast<char*>("PYTHONSAFEPATH=1"),            // NOLINT
      nullptr};
  const auto cpu_seconds =
      static_cast<rlim_t>(std::chrono::duration_cast<std::chrono::seconds>(settings.timeout).count() + 2);

  const pid_t child = fork();
  if (child == 0) {
    // The result pipe on descriptor 3, diagnostics on 1 and 2, nothing else inherited.
    setpgid(0, 0);
    if (null < 0 || dup2(null, 0) < 0 || dup2(diagnostics[1], 1) < 0 ||
        dup2(diagnostics[1], 2) < 0)
      _exit(126);
    if (result[1] == 3) {
      if (fcntl(3, F_SETFD, 0) != 0)
        _exit(126);
    } else if (dup2(result[1], 3) < 0) {
      _exit(126);
    }
    if (close_range(4, ~0U, 0) != 0 || chdir(directory.c_str()) != 0)
      _exit(126);
    limit(RLIMIT_CPU, cpu_seconds);
    limit(RLIMIT_AS, rlim_t{2} * 1024 * 1024 * 1024);
    limit(RLIMIT_FSIZE, rlim_t{8} * 1024 * 1024);
    limit(RLIMIT_CORE, 0);
    static_cast<void>(nice(10));
    execve(settings.helper.c_str(), arguments.data(), environment.data());
    _exit(127);
  }
  if (null >= 0)
    close(null);
  close(result[1]);
  close(diagnostics[1]);
  if (child < 0) {
    close(result[0]);
    close(diagnostics[0]);
    return run;
  }
  run.started = true;
  setpgid(child, child);

  // One point is at most about 40 bytes of text.
  const std::size_t most = settings.max_points * 48 + 1024 * 1024;
  const auto deadline = Clock::now() + settings.timeout;
  bool killed = false;
  const auto end = [&]() -> void {
    if (std::exchange(killed, true))
      return;
    kill(-child, SIGKILL);
    kill(child, SIGKILL);
  };
  std::array<pollfd, 2> watched{pollfd{result[0], POLLIN, 0}, pollfd{diagnostics[0], POLLIN, 0}};
  std::array<char, 65536> buffer{};
  int open_ends = 2;
  while (open_ends > 0) {
    if (!killed) {
      if (cancelled && cancelled()) {
        run.cancelled = true;
        end();
      } else if (Clock::now() >= deadline) {
        run.timed_out = true;
        end();
      } else if (run.result.size() > most) {
        run.too_much = true;
        end();
      }
    }
    if (poll(watched.data(), watched.size(), 20) < 0 && errno != EINTR)
      break;
    for (auto& end_of : watched) {
      if (end_of.fd < 0 || (end_of.revents & (POLLIN | POLLHUP | POLLERR)) == 0)
        continue;
      const auto got = read(end_of.fd, buffer.data(), buffer.size());
      if (got > 0) {
        const std::string_view text{buffer.data(), static_cast<std::size_t>(got)};
        if (end_of.fd == result[0]) {
          if (!killed)
            run.result += text;
        } else if (run.diagnostics.size() < settings.max_diagnostics_bytes) {
          // What is over the bound is read and dropped, so the helper never waits on it.
          run.diagnostics += text.substr(
              0, settings.max_diagnostics_bytes - run.diagnostics.size());
        }
      } else if (got == 0 || errno != EINTR) {
        end_of.fd = -1;
        --open_ends;
      }
    }
  }
  end(); // nothing of its process group stays, however the pipes ended
  close(result[0]);
  close(diagnostics[0]);
  while (waitpid(child, &run.status, 0) < 0 && errno == EINTR) {
  }
  return run;
}

auto failed(int line, std::string text) -> PreviewOutcome {
  PreviewOutcome outcome;
  outcome.state = PreviewState::failed;
  outcome.error = PreviewError{line, std::move(text)};
  return outcome;
}

auto unavailable(std::string text) -> PreviewOutcome {
  PreviewOutcome outcome;
  outcome.state = PreviewState::unavailable;
  if (!text.empty())
    outcome.error = PreviewError{0, std::move(text)};
  return outcome;
}

// What a helper run is worth as a preview.
auto outcome_of(const PreviewSettings& settings, HelperRun& run) -> PreviewOutcome {
  if (!run.started)
    return unavailable("The preview program could not be started.");
  auto output = parse_output(run.result);
  PreviewOutcome outcome;
  outcome.segments = std::move(output.segments);
  outcome.bounds = feed_bounds(outcome.segments);
  outcome.state = PreviewState::failed;
  const auto seconds =
      std::chrono::duration_cast<std::chrono::seconds>(settings.timeout).count();
  if (run.timed_out) {
    outcome.error = PreviewError{
        output.last_line,
        std::format("The preview took longer than {} s and was ended; the program may loop "
                    "without end. The path is shown as far as it was computed.",
                    seconds)};
  } else if (run.too_much) {
    outcome.error = PreviewError{output.last_line,
                                 "The preview wrote more than its limit and was ended."};
  } else if (output.end == HelperEnd::ok && !output.problem) {
    outcome.state = PreviewState::ready;
  } else if (output.end == HelperEnd::error && output.problem) {
    outcome.error = std::move(output.problem);
  } else if ((output.end == HelperEnd::stop || output.end == HelperEnd::cut) && output.problem) {
    outcome.error = PreviewError{
        output.problem->line,
        output.end == HelperEnd::cut
            ? std::format("Line {}: {}.", output.problem->line, output.problem->text)
            : std::format("The preview ends at line {}: {}.", output.problem->line,
                          output.problem->text)};
  } else {
    const bool signalled = WIFSIGNALED(run.status);
    outcome.error = PreviewError{
        output.last_line,
        signalled && WTERMSIG(run.status) == SIGXCPU
            ? std::string("The preview used more processor time than its limit and was ended; "
                          "the program may loop without end.")
            : std::format("The preview program ended unexpectedly ({} {}).",
                          signalled ? "signal" : "exit code",
                          signalled ? WTERMSIG(run.status) : WEXITSTATUS(run.status))};
  }
  return outcome;
}

auto to_helper_tools(const std::vector<RawTool>& tools) -> std::vector<HelperTool> {
  std::vector<HelperTool> out;
  out.reserve(tools.size());
  for (const auto& tool : tools)
    out.push_back({tool.index, tool.number, tool.pocket, tool.offset, tool.diameter,
                   tool.front_angle, tool.back_angle, tool.orientation});
  return out;
}

} // namespace

auto load_preview_settings(const MachineConfig& config, std::string helper,
                           std::string work_directory) -> PreviewSettings {
  PreviewSettings settings;
  settings.helper = std::move(helper);
  settings.work_directory = std::move(work_directory);
  settings.parameter_file = config.parameter_file;
  settings.units_per_mm = 1.0 / config.millimetres_per_unit;
  settings.search_path.push_back(config.directory);
  if (!config.program_prefix.empty())
    settings.search_path.push_back(config.program_prefix);

  IniFile ini;
  if (!ini.Open(config.ini_file.c_str())) {
    settings.refusal = "The machine's INI file cannot be read for the preview.";
    return settings;
  }
  const auto text = [&](const char* section, const char* key) -> std::optional<std::string> {
    const char* value = ini.Find(key, section);
    return value != nullptr ? std::optional<std::string>{value} : std::nullopt;
  };
  const auto positive = [&](const char* section, const char* key) -> bool {
    const auto value = text(section, key);
    return value && std::atoi(value->c_str()) > 0;
  };
  const fs::path base{config.directory};
  if (const auto path = text("RS274NGC", "SUBROUTINE_PATH")) {
    std::string_view rest = *path;
    while (!rest.empty()) {
      const auto end = rest.find(':');
      const auto directory = rest.substr(0, end);
      rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
      if (!directory.empty())
        settings.search_path.push_back((base / directory).lexically_normal().string());
    }
  }
  settings.tool_change_at_g30 = positive("EMCIO", "TOOL_CHANGE_AT_G30");
  settings.tool_change_quill_up = positive("EMCIO", "TOOL_CHANGE_QUILL_UP");
  if (const auto position = text("EMCIO", "TOOL_CHANGE_POSITION")) {
    std::istringstream words{*position};
    Vec3 where{};
    if (words >> where[0] >> where[1] >> where[2]) {
      for (auto& axis : where)
        axis *= config.millimetres_per_unit;
      settings.tool_change_position = where;
    }
  }
  for (const char* key :
       {"RETAIN_G43", "OWORD_NARGS", "OWORD_WARNONLY", "DISABLE_G92_PERSISTENCE"})
    if (const auto value = text("RS274NGC", key))
      settings.interpreter_options.push_back(std::format("{} = {}", key, *value));

  std::error_code error;
  if (text("RS274NGC", "REMAP"))
    settings.refusal = "This machine remaps codes ([RS274NGC] REMAP): the preview cannot follow "
                       "a remapped code and is not computed.";
  else if (positive("EMCIO", "RANDOM_TOOLCHANGER"))
    settings.refusal = "This machine has a random tool changer: no preview is computed.";
  else if (positive("RS274NGC", "NO_DOWNCASE_OWORD"))
    settings.refusal = "This machine keeps the case of O word names ([RS274NGC] "
                       "NO_DOWNCASE_OWORD): no preview is computed.";
  else if (settings.helper.empty() || access(settings.helper.c_str(), X_OK) != 0)
    settings.refusal = "The preview program (coffeecnc-preview) is not installed.";
  return settings;
}

HelperPreviewEngine::HelperPreviewEngine(PreviewSettings settings, ToolReader tools,
                                         AfterSnapshot after_snapshot)
    : settings_(std::move(settings)), tools_(std::move(tools)),
      after_snapshot_(std::move(after_snapshot)) {}

auto HelperPreviewEngine::compute(const PreviewInputs& inputs,
                                  const std::function<bool()>& cancelled) -> PreviewComputation {
  if (!settings_.refusal.empty())
    return {unavailable(settings_.refusal), false, {}};
  if (!inputs.program_file)
    return {unavailable({}), false, {}};
  const int wcs = inputs.start.wcs;
  if (wcs < 1 || wcs > 9 || !inputs.systems.at(static_cast<std::size_t>(wcs - 1)).available)
    return {unavailable("The offsets of the active work system are not known: no preview."),
            false,
            {}};
  const auto codes = start_codes(inputs.start);
  if (!codes)
    return {unavailable(codes.error()), false, {}};
  const auto tools = tools_ ? tools_() : std::nullopt;
  if (!tools)
    return {unavailable("LinuxCNC's tool table cannot be read: no preview."), false, {}};

  HelperRequest request;
  request.units_per_mm = settings_.units_per_mm;
  request.position = inputs.start.position;
  request.tool_in_spindle = inputs.start.tool_in_spindle;
  request.tool_prepared = inputs.start.tool_prepared.value_or(-1);
  request.tool_offset = inputs.start.tool_offset;
  request.feed = inputs.start.feed;
  request.block_delete = inputs.block_delete;
  request.wcs = wcs;
  request.tool_change_at_g30 = settings_.tool_change_at_g30;
  request.tool_change_position = settings_.tool_change_position;
  request.max_points = settings_.max_points;
  request.chord_tolerance = settings_.chord_tolerance;
  request.codes = *codes;
  request.tools = to_helper_tools(*tools);

  std::shared_ptr<const Manifest> last;
  for (int attempt = 1; attempt <= settings_.attempts; ++attempt) {
    if (cancelled && cancelled())
      return {{}, true, {}};
    auto taken = take_snapshot(settings_, *inputs.program_file);
    if (!taken)
      return {failed(0, taken.error()), false, {}};
    const auto manifest = std::make_shared<const Manifest>(std::move(*taken));
    last = manifest;
    const PrivateDirectory directory{settings_.work_directory};
    if (directory.path().empty())
      return {unavailable("No directory for the preview's files could be made."), false, {}};
    bool written = true;
    for (const auto& file : manifest->files)
      written = written && write_file(directory.path() / file.name, file.bytes);
    request.program = (directory.path() / manifest->files.front().name).string();
    request.parameters = (directory.path() / "preview.var").string();
    const auto request_file = (directory.path() / "request").string();
    written = written && write_file(request.parameters, parameter_text(settings_, inputs)) &&
              write_file(directory.path() / "preview.ini",
                         ini_text(settings_, directory.path().string())) &&
              write_file(request_file, write_request(request));
    if (!written)
      return {unavailable("The preview's files could not be written."), false, {}};
    if (after_snapshot_)
      after_snapshot_(attempt);

    ++runs_;
    auto run = run_helper(settings_, directory.path(), request_file, cancelled);
    diagnostics_ = std::move(run.diagnostics);
    if (run.cancelled)
      return {{}, true, {}};
    auto outcome = outcome_of(settings_, run);
    // The result stands only for files that are still what was read.
    if (files_differ(*manifest, settings_.search_path))
      continue;
    return {std::move(outcome), false,
            [manifest, search_path = settings_.search_path]() -> bool {
              return files_differ(*manifest, search_path);
            }};
  }
  return {failed(0, std::format("The program or one of its subroutine files changed on disk "
                                "during each of {} computations: no preview.",
                                settings_.attempts)),
          false,
          [last, search_path = settings_.search_path]() -> bool {
            return files_differ(*last, search_path);
          }};
}

} // namespace coffeecnc
