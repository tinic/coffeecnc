#include <coffeecnc/linuxcnc/preview_engine.hpp>
#include <coffeecnc/linuxcnc/tool_table.hpp>

#include "preview_wire.hpp"
#include "program_watch.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
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
#include <expected>
#include <system_error>
#include <thread>
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

using Cancelled = std::function<bool()>;
using Lookups = std::shared_ptr<std::atomic<std::size_t>>;

struct SnapshotFile {
  std::string original; // where it was read
  std::string name;     // what it is called in its private directory
  std::string bytes;    // as read
};

// For one called name: the file the search path gave, or none.
struct Call {
  std::string name;
  std::string found;
};

struct Manifest {
  SnapshotFile program;
  std::vector<SnapshotFile> subroutines;
  std::vector<Call> calls; // every distinct name called, found or not
  // The machine's parameter file as read with the program; none when it cannot be read.
  std::optional<std::string> parameters;
  int wcs{1}; // the active work system, whose values the live state writes over
};

// The machine's parameter file is read up to this size.
constexpr std::size_t max_parameter_bytes = std::size_t{1024} * 1024;

// The first parameter of the active work system's block (5221, 5241, ...).
auto system_base(int wcs) -> int { return 5221 + 20 * (wcs - 1); }

// The values of a parameter file that the preview takes from it: without G92 (5210-5219), the
// active system (5220) and its offsets, which the live state writes over.
auto file_values(const std::optional<std::string>& text, int wcs)
    -> std::optional<std::map<int, double>> {
  if (!text)
    return {};
  auto values = stored_parameters(*text);
  std::erase_if(values, [base = system_base(wcs)](const auto& entry) {
    return (entry.first >= 5210 && entry.first <= 5220) ||
           (entry.first >= base && entry.first <= base + 9);
  });
  return values;
}

// What the preview knows from a parameter file: the work systems and saved positions it holds.
struct Saved {
  unsigned systems{};
  bool g28{};
  bool g30{};
};
auto saved_in(const std::map<int, double>& parameters) -> Saved {
  const auto has = [&](int first) {
    return parameters.contains(first) && parameters.contains(first + 1) &&
           parameters.contains(first + 2);
  };
  Saved saved{0, has(5161), has(5181)};
  for (int wcs = 1; wcs <= 9; ++wcs)
    if (has(system_base(wcs)))
      saved.systems |= 1U << static_cast<unsigned>(wcs - 1);
  return saved;
}

auto is_cancelled(const Cancelled& cancelled) -> bool { return cancelled && cancelled(); }

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

// As Interp::find_ngc_file looks (LinuxCNC 2.9.4, rs274ngc_pre.cc:2660) for a name that is
// plainly a name: the first directory of the search path that has `name.ngc`.
auto find_subroutine(const std::vector<std::string>& search_path, const std::string& name,
                     const Lookups& lookups) -> std::string {
  for (const auto& directory : search_path) {
    const auto file = fs::path(directory) / (name + ".ngc");
    std::error_code error;
    if (lookups)
      ++*lookups;
    if (fs::is_regular_file(file, error) && std::ifstream{file}.good())
      return file.string();
  }
  return {};
}

auto megabytes(std::size_t bytes) -> std::string {
  return std::format("{} MiB", bytes / (std::size_t{1024} * 1024));
}

struct SnapshotFailure {
  bool cancelled{};
  PreviewError error;
  bool blocks_run{};
};

auto unconfigured_tapping(const PreviewSettings& settings, std::string_view bytes,
                          std::string_view subroutine, const Cancelled& cancelled)
    -> std::optional<SnapshotFailure> {
  if (settings.rigid_tap)
    return {};
  const auto line = first_rigid_tap_line(bytes, cancelled);
  if (is_cancelled(cancelled))
    return SnapshotFailure{true, {}};
  if (!line)
    return {};
  const std::string reason = "G33.1 rigid tapping is not configured on this machine.";
  return SnapshotFailure{
      false, subroutine.empty() ? PreviewError{*line, std::format("Line {}: {}", *line, reason)}
                                : PreviewError{0, std::format("{}.ngc line {}: {}",
                                                              subroutine, *line, reason)},
      true};
}

// The program and every subroutine file it reaches, read once each. Bounded before anything
// is accumulated: bytes, files, and distinct called names, found or not. `cancelled` is asked
// before every file and every name.
auto take_snapshot(const PreviewSettings& settings, const std::string& program,
                   const Cancelled& cancelled, const Lookups& lookups)
    -> std::expected<Manifest, SnapshotFailure> {
  const auto refused = [](int line, std::string text) -> std::unexpected<SnapshotFailure> {
    return std::unexpected(SnapshotFailure{false, PreviewError{line, std::move(text)}});
  };
  Manifest manifest;
  auto bytes = read_file(program, settings.max_input_bytes);
  if (!bytes)
    return refused(0, std::format("The program cannot be read for a preview, or is larger than "
                                  "{}.",
                                  megabytes(settings.max_input_bytes)));
  if (auto tap = unconfigured_tapping(settings, *bytes, {}, cancelled))
    return std::unexpected(std::move(*tap));
  // Nothing with a comment that runs Python or writes a file, or with an O word that is not
  // plainly a name, is given to the interpreter: it acts on some of them while it reads,
  // also in lines it never executes.
  if (const auto found = first_forbidden_line(*bytes))
    return refused(found->line,
                   std::format("The preview ends at line {}: {}.", found->line, found->what));
  std::size_t total = bytes->size();
  manifest.program = {program, "program.ngc", std::move(*bytes)};
  std::set<std::string> seen;
  // The texts still to be looked through for calls: the program, then each file as it is read.
  for (std::size_t next = 0; next <= manifest.subroutines.size(); ++next) {
    // Collected within the bound: the scan stops at the first name too many.
    const auto called =
        called_names(next == 0 ? manifest.program.bytes : manifest.subroutines[next - 1].bytes,
                     seen, settings.max_calls - seen.size(), cancelled);
    if (called.cancelled)
      return std::unexpected(SnapshotFailure{true, {}});
    if (called.too_many)
      return refused(0, std::format("The program calls more than {} different subroutines: no "
                                    "preview.",
                                    settings.max_calls));
    const auto& names = called.names;
    for (const auto& name : names) {
      if (!seen.insert(name).second)
        continue;
      if (is_cancelled(cancelled))
        return std::unexpected(SnapshotFailure{true, {}});
      auto found = find_subroutine(settings.search_path, name, lookups);
      manifest.calls.push_back({name, found});
      if (found.empty())
        continue;
      if (manifest.subroutines.size() + 2 > settings.max_files)
        return refused(0, std::format("The program uses more than {} files: no preview.",
                                      settings.max_files));
      auto text = read_file(found, settings.max_input_bytes - total);
      if (!text)
        return refused(0, std::format("The program and its subroutine files cannot be read, or "
                                      "are larger than {} together: no preview.",
                                      megabytes(settings.max_input_bytes)));
      if (auto tap = unconfigured_tapping(settings, *text, name, cancelled))
        return std::unexpected(std::move(*tap));
      if (const auto forbidden = first_forbidden_line(*text))
        return refused(0, std::format("No preview: {}.ngc line {}: {}.", name, forbidden->line,
                                      forbidden->what));
      total += text->size();
      manifest.subroutines.push_back({std::move(found), name + ".ngc", std::move(*text)});
    }
  }
  if (!settings.parameter_file.empty())
    manifest.parameters = read_file(settings.parameter_file, max_parameter_bytes);
  return manifest;
}

// Whether an original is no longer what the manifest holds: every call searched again, every
// file compared byte for byte. `cancelled` is asked before every name and every file; given
// up, the answer is "no change".
auto files_differ(const Manifest& manifest, const std::vector<std::string>& search_path,
                  const std::string& parameter_file, const Cancelled& cancelled,
                  const Lookups& lookups) -> bool {
  // The parameter file by its values: task writes it again and again with the same ones.
  if (!parameter_file.empty()) {
    if (is_cancelled(cancelled))
      return false;
    if (file_values(read_file(parameter_file, max_parameter_bytes), manifest.wcs) !=
        file_values(manifest.parameters, manifest.wcs))
      return true;
  }
  for (const auto& call : manifest.calls) {
    if (is_cancelled(cancelled))
      return false;
    if (find_subroutine(search_path, call.name, lookups) != call.found)
      return true;
  }
  const auto differs = [&](const SnapshotFile& file) -> bool {
    if (is_cancelled(cancelled))
      return false;
    const auto now = read_file(file.original, file.bytes.size());
    return !now || *now != file.bytes;
  };
  return differs(manifest.program) || std::ranges::any_of(manifest.subroutines, differs);
}

// A path the interpreter can be given in an INI file: it expands what it reads there as a
// shell would (wordexp).
auto plain_path(std::string_view path) -> bool {
  return !path.empty() && std::ranges::all_of(path, [](char c) -> bool {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '/' || c == '_' || c == '-' ||
           c == '.' || c == '+';
  });
}

// ---- the start state ---------------------------------------------------------------------------

// Task's modal state as blocks for the helper's interpreter, or why it cannot be replayed.
struct StartState {
  std::vector<std::string> codes;
  std::string inherited_motion;
  std::string tool_length; // G43, G43.1, G43.2 or G49, as task has it
};
auto start_state(const InterpreterStart& start) -> std::expected<StartState, std::string> {
  StartState state;
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
    case 430:
    case 431:
    case 432:
    case 490:
      state.tool_length = code;
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
      // Not a mode a block can put in place: the helper is told, and ends the preview at a
      // block that would move by it.
      motion.clear();
      state.inherited_motion = code;
      break;
    default:
      break;
    }
  }
  auto& codes = state.codes;
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
  return state;
}

// The machine's parameter file with what is live written over it: the active work system, its
// rotation and G92. Machine units, as the interpreter keeps the file.
auto parameter_text(const PreviewSettings& settings, const PreviewInputs& inputs,
                    const std::optional<std::string>& saved) -> std::string {
  auto parameters = saved ? stored_parameters(*saved) : std::map<int, double>{};
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

// The INI file of the helper's interpreter: subroutines are looked for in `directory` alone.
// With the G30 position known, its interpreter moves there at a tool change as task's does.
auto ini_text(const PreviewSettings& settings, const std::string& directory, bool saved_g30)
    -> std::string {
  std::string text = "[RS274NGC]\nPARAMETER_FILE = preview.var\nINI_VARS = 0\nHAL_PIN_VARS = 0\n"
                     "DISABLE_FANUC_STYLE_SUB = 1\n";
  text += std::format("SUBROUTINE_PATH = {}\n", directory);
  for (const auto& option : settings.interpreter_options)
    text += option + "\n";
  text += std::format("[DISPLAY]\nPROGRAM_PREFIX = {}\n", directory);
  text += std::format("[EMCIO]\nTOOL_CHANGE_QUILL_UP = {}\n", settings.tool_change_quill_up ? 1 : 0);
  if (settings.tool_change_at_g30 && saved_g30)
    text += "TOOL_CHANGE_AT_G30 = 1\n";
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

// How a helper process ended: by itself, or by which of our hands.
enum class HelperExit : std::uint8_t {
  not_started,
  by_itself,   // `status` is its own: an exit code or a signal
  cancelled,   // killed: nobody wants the result
  timed_out,   // killed: it ran longer than the timeout
  too_much,    // killed: it wrote more than a result can be
  never_ended, // killed: it had closed its result and did not exit within exit_grace
};

struct HelperRun {
  HelperExit how{HelperExit::not_started};
  int status{}; // of waitpid on the helper itself
  std::string result;
  std::string diagnostics;
};

void limit(int resource, rlim_t value) {
  const rlimit bound{value, value};
  static_cast<void>(setrlimit(resource, &bound));
}

// `directory` holds the request and the INI file; the helper runs in `working`, an empty
// directory: the interpreter looks for a subroutine file in its working directory first.
auto run_helper(const PreviewSettings& settings, const fs::path& directory,
                const fs::path& working, const std::string& request_file,
                const std::function<bool()>& cancelled) -> HelperRun {
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
  // If the panel was launched with standard descriptors closed, redirecting the child's
  // diagnostics could overwrite the result pipe before it is moved to descriptor 3.
  if (result[1] <= STDERR_FILENO) {
    const int moved = fcntl(result[1], F_DUPFD_CLOEXEC, 4);
    close(result[1]);
    result[1] = moved;
  }
  const int null = open("/dev/null", O_RDONLY | O_CLOEXEC);
  if (result[1] < 0 || null < 0) {
    for (const int fd : {result[0], result[1], diagnostics[0], diagnostics[1], null})
      if (fd >= 0)
        close(fd);
    return run;
  }
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
    if (dup2(null, 0) < 0 || dup2(diagnostics[1], 1) < 0 ||
        dup2(diagnostics[1], 2) < 0)
      _exit(126);
    if (result[1] == 3) {
      if (fcntl(3, F_SETFD, 0) != 0)
        _exit(126);
    } else if (dup2(result[1], 3) < 0) {
      _exit(126);
    }
    if (close_range(4, ~0U, 0) != 0 || chdir(working.c_str()) != 0)
      _exit(126);
    limit(RLIMIT_CPU, cpu_seconds);
#if !defined(COFFEECNC_ADDRESS_SANITIZER) && !defined(COFFEECNC_THREAD_SANITIZER)
    // Not in a sanitizer build (the definitions are the build system's, cmake/
    // ProjectOptions.cmake): the sanitizers reserve address space far beyond any bound.
    limit(RLIMIT_AS, rlim_t{2} * 1024 * 1024 * 1024);
#endif
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
  run.how = HelperExit::by_itself;
  setpgid(child, child);

  // One point is at most about 40 bytes of text.
  const std::size_t most = settings.max_points * 48 + std::size_t{1024} * 1024;
  const auto deadline = Clock::now() + settings.timeout;
  bool killed = false;
  const auto end = [&](HelperExit why) -> void {
    if (std::exchange(killed, true))
      return;
    run.how = why;
    kill(-child, SIGKILL);
    kill(child, SIGKILL);
  };
  std::array<pollfd, 2> watched{pollfd{result[0], POLLIN, 0}, pollfd{diagnostics[0], POLLIN, 0}};
  std::array<char, 65536> buffer{};
  int open_ends = 2;
  while (open_ends > 0) {
    if (!killed) {
      if (cancelled && cancelled())
        end(HelperExit::cancelled);
      else if (Clock::now() >= deadline)
        end(HelperExit::timed_out);
      else if (run.result.size() > most)
        end(HelperExit::too_much);
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
  close(result[0]);
  close(diagnostics[0]);
  // Its pipes are closed: the helper is given exit_grace to end by itself, so that how it
  // ended is its own doing and can be told from ours.
  bool reaped = false;
  for (const auto until = Clock::now() + settings.exit_grace; !killed && !reaped;) {
    const auto got = waitpid(child, &run.status, WNOHANG);
    reaped = got == child;
    if (reaped || (got < 0 && errno != EINTR))
      break;
    if (cancelled && cancelled())
      end(HelperExit::cancelled);
    else if (Clock::now() >= until)
      end(HelperExit::never_ended);
    else
      std::this_thread::sleep_for(std::chrono::milliseconds{2});
  }
  // Nothing of its process group stays. After a normal exit that is cleaning up, not an
  // outcome: it is said in the diagnostics only.
  if (reaped && kill(-child, SIGKILL) == 0)
    run.diagnostics += "\n[coffeecnc: the helper left processes behind; they were ended]\n";
  if (!reaped) {
    end(run.how == HelperExit::by_itself ? HelperExit::never_ended : run.how);
    while (waitpid(child, &run.status, 0) < 0 && errno == EINTR) {
    }
  }
  return run;
}

auto failed(int line, std::string text) -> PreviewOutcome {
  PreviewOutcome outcome;
  outcome.state = PreviewState::failed;
  outcome.error = PreviewError{line, std::move(text)};
  return outcome;
}

auto snapshot_rejection(const SnapshotFailure& failure) -> PreviewComputation {
  if (failure.cancelled)
    return {{}, true, {}};
  auto outcome = failed(failure.error.line, failure.error.text);
  if (failure.blocks_run)
    outcome.check.emplace().errors.push_back(failure.error.text);
  return {std::move(outcome), false, {}};
}

auto unavailable(std::string text) -> PreviewOutcome {
  PreviewOutcome outcome;
  outcome.state = PreviewState::unavailable;
  if (!text.empty())
    outcome.error = PreviewError{0, std::move(text)};
  return outcome;
}

// What a helper run is worth as a preview. Its result is read as text of unknown origin:
// only whole, valid records count, within the limits the helper was given. `ready` needs
// both a whole result that ends `ok` and the helper's own exit with status 0: a result that
// says it is finished is not the process having ended well.
struct CheckToolFiles {
  std::optional<std::string> table;
  std::optional<std::string> touched;
  friend auto operator==(const CheckToolFiles&, const CheckToolFiles&) -> bool = default;
};

auto check_tool_files(const std::string& table, const std::string& sidecar) -> CheckToolFiles {
  if (table.empty())
    return {};
  return {read_file(table, max_tool_table_bytes),
          sidecar.empty() ? std::nullopt : read_file(sidecar, max_tool_table_bytes)};
}

auto program_check(const PreviewSettings& settings, const PreviewInputs& inputs,
                   const HelperOutput& output, const PreviewOutcome& outcome,
                   const CheckToolFiles& files) -> ProgramCheck {
  ProgramCheck check;
  const auto& facts = output.check;
  const bool complete = outcome.state == PreviewState::ready && facts.has_value();
  if (!complete) {
    if (output.end == HelperEnd::error && output.problem && output.problem->line > 0 &&
        outcome.error) {
      check.errors.push_back(std::format("Line {}: {}", outcome.error->line, outcome.error->text));
    } else {
      check.warnings.emplace_back("CoffeeCNC could only check part of this program.");
    }
  }
  if (!facts)
    return check;

  std::map<int, double> touched;
  if (settings.reference_tool && files.table && files.touched)
    touched = restored_touched_off(*files.table, *settings.reference_tool,
                                   parse_touched_off(*files.touched));
  for (const int tool : facts->tools) {
    const auto raw = std::ranges::find_if(inputs.raw_tools, [=](const PreviewTool& row) {
      return row.index != 0 && row.number == tool;
    });
    ToolLengthCheck length = ToolLengthCheck::missing;
    if (raw != inputs.raw_tools.end() && std::isfinite(raw->offset[2])) {
      const auto mark = touched.find(tool);
      if (mark != touched.end() && std::abs(mark->second - raw->offset[2]) <= 0.001)
        length = ToolLengthCheck::touched_off;
      else if (raw->offset[2] != 0)
        length = ToolLengthCheck::table;
    }
    check.tools.push_back({tool, length});
    if (length == ToolLengthCheck::missing)
      check.errors.push_back(std::format("T{} has no verified length in the tool table.", tool));
  }
  check.frames = facts->frames;
  for (const auto& frame : check.frames)
    if (!frame.available) {
      static constexpr std::array names{"G54", "G55",   "G56",   "G57",  "G58",
                                        "G59", "G59.1", "G59.2", "G59.3"};
      check.errors.push_back(std::format("The {} work origin could not be verified.",
                                         names.at(static_cast<std::size_t>(frame.wcs - 1))));
    }
  check.feed = facts->feed;
  check.spindle = facts->spindle;
  if (facts->mm)
    check.units.push_back(LengthUnit::mm);
  if (facts->inch)
    check.units.push_back(LengthUnit::inch);
  if (facts->feed_per_revolution)
    check.warnings.emplace_back("Feed per revolution is excluded from the feed range.");
  if (facts->variable_spindle)
    check.warnings.emplace_back("Constant surface speed prevents checking a fixed spindle range.");
  if (facts->spindle && settings.max_spindle_rpm > 0 &&
      facts->spindle->max > settings.max_spindle_rpm)
    check.errors.push_back(std::format("The program requests {} rpm. The spindle limit is {} rpm.",
                                       facts->spindle->max, settings.max_spindle_rpm));

  const auto fits = [&](const std::optional<Bounds>& travel) {
    if (!facts->machine || !travel)
      return true;
    for (std::size_t axis = 0; axis < 3; ++axis)
      if (facts->machine->min[axis] < travel->min[axis] - 1e-6 ||
          facts->machine->max[axis] > travel->max[axis] + 1e-6)
        return false;
    return true;
  };
  if (!fits(settings.soft_travel) || !fits(settings.physical_travel)) {
    check.travel = TravelCheck::outside;
    check.errors.emplace_back("The program exceeds configured machine travel.");
  } else if (complete && facts->machine && settings.physical_travel) {
    check.travel = TravelCheck::fits;
  }
  return check;
}

auto outcome_of(const PreviewSettings& settings, const PreviewInputs& inputs, HelperRun& run,
                int program_lines, const CheckToolFiles& files) -> PreviewOutcome {
  if (run.how == HelperExit::not_started)
    return unavailable("The preview program could not be started.");
  auto output = parse_output(run.result, {settings.max_points + 2, program_lines});
  PreviewOutcome outcome;
  outcome.segments = std::move(output.segments);
  outcome.bounds = feed_bounds(outcome.segments);
  outcome.dwells = std::move(output.dwells);
  outcome.state = PreviewState::failed;
  const bool exited = run.how == HelperExit::by_itself && WIFEXITED(run.status);
  const bool well = exited && WEXITSTATUS(run.status) == 0;
  // How the helper ended, when that is what there is to say.
  const auto ending = [&]() -> std::string {
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(settings.timeout).count();
    switch (run.how) {
    case HelperExit::timed_out:
      return std::format("The preview took longer than {} s and was ended; the program may "
                         "loop without end. The path is shown as far as it was computed.",
                         seconds);
    case HelperExit::too_much:
      return "The preview wrote more than its limit and was ended.";
    case HelperExit::never_ended:
      return "The preview program closed its result and did not end; it was ended.";
    case HelperExit::by_itself:
      if (WIFSIGNALED(run.status))
        return WTERMSIG(run.status) == SIGXCPU
                   ? std::string("The preview used more processor time than its limit and was "
                                 "ended; the program may loop without end.")
                   : std::format("The preview program was ended by signal {}.",
                                 WTERMSIG(run.status));
      if (!well)
        return std::format("The preview program ended with exit code {}.",
                           WEXITSTATUS(run.status));
      return "The preview program ended without finishing its result; the path is shown as "
             "far as it was received.";
    default:
      return "The preview program did not run to its end.";
    }
  };
  if (!output.defect.empty())
    outcome.error = PreviewError{
        0, std::format("The preview program's result is not valid ({}); the path is shown as "
                       "far as it was.",
                       output.defect)};
  else if (output.end == HelperEnd::ok && well) {
    outcome.state = PreviewState::ready;
    outcome.restart = output.restart;
    outcome.restart_blocked = std::move(output.restart_blocked);
  }
  else if (output.end == HelperEnd::error && output.problem)
    // The interpreter's own error, with its line: that is the reason, however the helper
    // ended after it.
    outcome.error = std::move(output.problem);
  else if ((output.end == HelperEnd::stop || output.end == HelperEnd::cut) && output.problem)
    outcome.error = PreviewError{
        output.problem->line,
        output.end == HelperEnd::cut
            ? std::format("Line {}: {}.", output.problem->line, output.problem->text)
            : std::format("The preview ends at line {}: {}.", output.problem->line,
                          output.problem->text)};
  else
    outcome.error = PreviewError{0, ending()};
  if (!inputs.restart_line)
    outcome.check = program_check(settings, inputs, output, outcome, files);
  return outcome;
}

auto to_helper_tools(const std::vector<PreviewTool>& tools) -> std::vector<HelperTool> {
  std::vector<HelperTool> out;
  out.reserve(tools.size());
  std::ranges::transform(tools, std::back_inserter(out), [](const PreviewTool& tool) -> HelperTool {
    return {tool.index, tool.number, tool.pocket, tool.offset, tool.diameter,
            tool.front_angle, tool.back_angle, tool.orientation};
  });
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
  settings.max_spindle_rpm = config.limits.max_spindle_rpm;
  settings.rigid_tap = config.rigid_tap;
  settings.tool_table = config.tool_table;
  if (config.fixed_setter) {
    settings.reference_tool = config.fixed_setter->reference_tool;
    settings.touched_off_file = config.touched_off_file();
  }
  if (std::ranges::any_of(config.rotary_joints, [](const auto& joints) { return !joints.empty(); })) {
    settings.refusal = "Program preview and line restart are unavailable on rotary configurations.";
    return settings;
  }
  auto& soft = settings.soft_travel.emplace();
  for (std::size_t i = 0; i < config.travel.size(); ++i) {
    soft.min[i] = config.travel[i].min;
    soft.max[i] = config.travel[i].max;
  }
  if (config.limits.homing_required)
    settings.physical_travel = soft;
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
    return linuxcnc_switch(text(section, key).value_or(""));
  };
  const auto physical_min = text("COFFEECNC", "TRAVEL_MIN_MM");
  const auto physical_max = text("COFFEECNC", "TRAVEL_MAX_MM");
  if (physical_min || physical_max) {
    Bounds limits;
    std::istringstream minima{physical_min.value_or("")};
    std::istringstream maxima{physical_max.value_or("")};
    std::string extra;
    const bool valid = (minima >> limits.min[0] >> limits.min[1] >> limits.min[2]) &&
                       (maxima >> limits.max[0] >> limits.max[1] >> limits.max[2]) &&
                       !(minima >> extra) && !(maxima >> extra) &&
                       std::ranges::equal(limits.min, limits.max, [](double lo, double hi) {
                         return std::isfinite(lo) && std::isfinite(hi) && lo < hi;
                       });
    settings.physical_travel = valid ? std::optional{limits} : std::nullopt;
  }
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
      std::ranges::transform(where, where.begin(), [&](double axis) -> double {
        return axis * config.millimetres_per_unit;
      });
      settings.tool_change_position = where;
    }
  }
  for (const char* key :
       {"RETAIN_G43", "OWORD_NARGS", "OWORD_WARNONLY"})
    if (const auto value = text("RS274NGC", key))
      settings.interpreter_options.push_back(std::format("{} = {}", key, *value));

  // Every REMAP line names its code first: `REMAP = M6 modalgroup=6 ngc=change`.
  for (int number = 1; number <= 256; ++number) {
    const char* value = ini.Find("REMAP", "RS274NGC", number);
    if (value == nullptr)
      break;
    std::istringstream words{value};
    std::string code;
    if (words >> code) {
      std::ranges::transform(code, code.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      });
      settings.remapped.push_back(code);
    }
  }
  if (positive("EMCIO", "RANDOM_TOOLCHANGER"))
    settings.refusal = "This machine has a random tool changer: no preview is computed.";
  else if (positive("RS274NGC", "NO_DOWNCASE_OWORD"))
    settings.refusal = "This machine keeps the case of O word names ([RS274NGC] "
                       "NO_DOWNCASE_OWORD): no preview is computed.";
  else if (settings.helper.empty() || access(settings.helper.c_str(), X_OK) != 0)
    settings.refusal = "The preview program (coffeecnc-preview) is not installed.";
  return settings;
}

HelperPreviewEngine::HelperPreviewEngine(PreviewSettings settings, AfterSnapshot after_snapshot)
    : settings_(std::move(settings)), after_snapshot_(std::move(after_snapshot)),
      lookups_(std::make_shared<std::atomic<std::size_t>>(0)) {}

auto HelperPreviewEngine::file_lookups() const -> std::size_t { return *lookups_; }

auto HelperPreviewEngine::compute(const PreviewInputs& inputs,
                                  const std::function<bool()>& cancelled) -> PreviewComputation {
  if (!inputs.program_file)
    return {unavailable(settings_.refusal), false, {}};
  if (is_cancelled(cancelled))
    return {{}, true, {}};
  const auto without_preview = [&](std::string reason) -> PreviewComputation {
    if (!settings_.rigid_tap) {
      const auto source = take_snapshot(settings_, *inputs.program_file, cancelled, lookups_);
      if (!source && (source.error().cancelled || source.error().blocks_run))
        return snapshot_rejection(source.error());
    }
    return {unavailable(std::move(reason)), false, {}};
  };
  if (!settings_.refusal.empty())
    return without_preview(settings_.refusal);
  const int wcs = inputs.start.wcs;
  if (wcs < 1 || wcs > 9 || !inputs.systems.at(static_cast<std::size_t>(wcs - 1)).available)
    return without_preview("The offsets of the active work system are not known: no preview.");
  const auto start = start_state(inputs.start);
  if (!start)
    return without_preview(start.error());
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
  request.rigid_tap = settings_.rigid_tap;
  request.codes = start->codes;
  // The tool length offset in effect, in the interpreter as much as in the canon: the
  // interpreter takes it off where it works in machine coordinates (G53, G28, G30).
  if (start->tool_length != "G49" && !start->tool_length.empty() &&
      inputs.start.tool_offset != Vec3{}) {
    const auto& offset = inputs.start.tool_offset;
    const bool by_table = start->tool_length == "G43" &&
        std::ranges::any_of(inputs.raw_tools, [&](const PreviewTool& tool) {
          return tool.index != 0 && tool.number == inputs.start.tool_in_spindle &&
                 std::ranges::equal(tool.offset, offset, [&](double table, double live) {
                   return std::abs(table / settings_.units_per_mm - live) <= 1e-6;
                 });
        });
    const double unit = inputs.start.program_units == LengthUnit::inch ? 25.4 : 1.0;
    request.codes.push_back(by_table ? std::string("G43")
                                     : std::format("G43.1 X{:.8f} Y{:.8f} Z{:.8f}",
                                                   offset[0] / unit, offset[1] / unit,
                                                   offset[2] / unit));
  }
  const bool css = std::ranges::contains(inputs.start.active_codes, "G96");
  for (const auto& code : inputs.start.active_codes)
    if (code == "G61" || code == "G61.1")
      request.codes.push_back(code);
  // LinuxCNC requires the S word in the G96 block, even if S was already set.
  if (css || inputs.start.spindle_speed > 0)
    request.codes.push_back(std::format("{}S{:.8f}", css ? "G96 " : "", inputs.start.spindle_speed));
  if (inputs.start.spindle_direction != SpindleDirection::stopped)
    request.codes.push_back(inputs.start.spindle_direction == SpindleDirection::forward ? "M3"
                                                                                        : "M4");
  if (inputs.restart_line) {
    if (inputs.start.flood)
      request.codes.emplace_back("M8");
    if (inputs.start.mist)
      request.codes.emplace_back("M7");
  }
  request.inherited_motion = start->inherited_motion;
  request.tools = to_helper_tools(inputs.raw_tools);
  request.restart_line = inputs.restart_line;
  request.remapped = settings_.remapped;

  const std::string check_table = inputs.restart_line ? std::string{} : settings_.tool_table;
  const std::string touched = !settings_.reference_tool || check_table.empty() ? std::string{}
      : settings_.touched_off_file.empty()
          ? (fs::path(check_table).parent_path() / "coffeecnc-touched-off.txt").string()
          : settings_.touched_off_file;
  const auto watch =
      [lookups = lookups_, search_path = settings_.search_path,
       parameter_file = settings_.parameter_file, check_table,
       touched](std::shared_ptr<const Manifest> manifest,
                     std::shared_ptr<const CheckToolFiles> tools) -> PreviewComputation::Watch {
    return [manifest = std::move(manifest), lookups, search_path, parameter_file, check_table,
            touched, tools = std::move(tools)](const std::function<bool()>& given_up) -> bool {
      return files_differ(*manifest, search_path, parameter_file, given_up, lookups) ||
             (tools && !is_cancelled(given_up) &&
              *tools != check_tool_files(check_table, touched));
    };
  };
  std::shared_ptr<const Manifest> last;
  std::shared_ptr<const CheckToolFiles> last_tools;
  for (int attempt = 1; attempt <= settings_.attempts; ++attempt) {
    auto taken = take_snapshot(settings_, *inputs.program_file, cancelled, lookups_);
    if (!taken)
      return snapshot_rejection(taken.error());
    taken->wcs = wcs;
    const auto manifest = std::make_shared<const Manifest>(std::move(*taken));
    const auto& parameter_bytes = manifest->parameters;
    const auto saved = saved_in(parameter_bytes.has_value()
                                    ? stored_parameters(parameter_bytes.value())
                                    : std::map<int, double>{});
    request.known_systems = saved.systems;
    request.saved_g28 = saved.g28;
    request.saved_g30 = saved.g30;
    last = manifest;
    last_tools =
        std::make_shared<const CheckToolFiles>(check_tool_files(check_table, touched));
    // The program in a directory of its own, the subroutine files in another, which is the
    // only place the helper's interpreter looks for one, and an empty one to run in.
    const PrivateDirectory directory{settings_.work_directory};
    const auto programs = directory.path() / "program";
    const auto subroutines = directory.path() / "subroutines";
    const auto working = directory.path() / "empty";
    std::error_code error;
    if (directory.path().empty() || !plain_path(directory.path().string()) ||
        !fs::create_directory(programs, error) || !fs::create_directory(subroutines, error) ||
        !fs::create_directory(working, error))
      return {unavailable("No directory for the preview's files could be made, or its path "
                          "has characters the interpreter would expand."),
              false,
              {}};
    bool written = write_file(programs / manifest->program.name, manifest->program.bytes);
    for (const auto& file : manifest->subroutines)
      // Names are distinct: one file per called name.
      written = written && !fs::exists(subroutines / file.name, error) &&
                write_file(subroutines / file.name, file.bytes);
    request.program = (programs / manifest->program.name).string();
    request.subroutines = subroutines.string();
    request.parameters = (directory.path() / "preview.var").string();
    const auto request_file = (directory.path() / "request").string();
    written = written &&
              write_file(request.parameters,
                         parameter_text(settings_, inputs, manifest->parameters)) &&
              write_file(directory.path() / "preview.ini",
                         ini_text(settings_, subroutines.string(), saved.g30)) &&
              write_file(request_file, write_request(request));
    if (!written)
      return {unavailable("The preview's files could not be written."), false, {}};
    if (after_snapshot_)
      after_snapshot_(attempt);

    ++runs_;
    auto run = run_helper(settings_, directory.path(), working, request_file, cancelled);
    diagnostics_ = std::move(run.diagnostics);
    if (run.how == HelperExit::cancelled)
      return {{}, true, {}};
    const auto& text = manifest->program.bytes;
    const int program_lines =
        static_cast<int>(std::min<std::ptrdiff_t>(std::ranges::count(text, '\n') + 1, 2000000000));
    auto outcome = outcome_of(settings_, inputs, run, program_lines, *last_tools);
    if (outcome.restart &&
        outcome.restart->tool != inputs.restart_loaded_tool.value_or(inputs.start.tool_in_spindle) &&
        (settings_.tool_change_at_g30 || settings_.tool_change_position ||
         settings_.tool_change_quill_up)) {
      outcome.restart.reset();
      outcome.restart_blocked = "The configured tool change moves the machine independently.";
    }
    // The result stands only for files that are still what was read.
    auto changed = watch(manifest, last_tools);
    if (changed(cancelled))
      continue;
    if (is_cancelled(cancelled))
      return {{}, true, {}};
    return {std::move(outcome), false, std::move(changed), {manifest, &manifest->program.bytes}};
  }
  return {failed(0, std::format("The program or one of its subroutine files changed on disk "
                                "during each of {} computations: no preview.",
                                settings_.attempts)),
          false, watch(last, last_tools)};
}

} // namespace coffeecnc
