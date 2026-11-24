#include <coffeecnc/app/machine_data.hpp>

#include <coffeecnc/app/backup_archive.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>
#include <set>
#include <thread>
#include <utility>

#include <time.h>

namespace coffeecnc {
namespace {
using Json = nlohmann::json;

constexpr std::string_view format_name = "coffeecnc-backup";
constexpr int manifest_version = 1;
constexpr int plan_version = 4;
constexpr std::size_t max_relative_bytes = 255;
constexpr std::size_t max_record_bytes = std::size_t{1024} * 1024;
// A file the restore makes: the configuration's are readable by all, CoffeeCNC's own are not.
constexpr unsigned new_config_mode = 0644;
constexpr unsigned new_data_mode = 0600;

auto dump(const Json& value) -> std::string {
  return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

// One component of a path: not empty, not `.` or `..`, no `/`, no control character.
auto plain_component(std::string_view part) -> bool {
  return !part.empty() && part != "." && part != ".." && std::ranges::none_of(part, [](char c) {
    return static_cast<unsigned char>(c) < 0x20 || c == 0x7f || c == '/';
  });
}

// A relative path of plain components.
auto plain_relative(std::string_view path) -> bool {
  if (path.empty() || path.size() > max_relative_bytes || path.front() == '/')
    return false;
  while (!path.empty()) {
    const auto slash = path.find('/');
    if (!plain_component(path.substr(0, slash)))
      return false;
    path.remove_prefix(slash == std::string_view::npos ? path.size() : slash + 1);
    if (slash != std::string_view::npos && path.empty())
      return false; // a trailing slash
  }
  return true;
}

auto base_name(std::string_view path) -> std::string {
  const auto slash = path.rfind('/');
  return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

auto parent(std::string_view path) -> std::string {
  const auto slash = path.rfind('/');
  return slash == 0 || slash == std::string_view::npos ? std::string("/")
                                                       : std::string(path.substr(0, slash));
}

auto fnv(std::string_view bytes, std::uint64_t hash = 14695981039346656037ULL) -> std::uint64_t {
  for (const char c : bytes) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 1099511628211ULL;
  }
  return hash;
}

// What a target holds, as the plan records it: a hash of its bytes, or null when it is missing.
auto held_json(const std::optional<std::string>& bytes) -> Json {
  return bytes ? Json(std::format("{:016x}", fnv(*bytes))) : Json(nullptr);
}

auto stamp(std::chrono::milliseconds now) -> std::string {
  const std::chrono::sys_seconds seconds{std::chrono::floor<std::chrono::seconds>(now)};
  return std::format("{:%Y%m%d-%H%M%S}", seconds);
}

auto trimmed(std::string_view text) -> std::string_view {
  const auto first = text.find_first_not_of(" \t\r");
  if (first == std::string_view::npos)
    return {};
  const auto last = text.find_last_not_of(" \t\r");
  return text.substr(first, last - first + 1);
}

// The HAL files an INI file names: [HAL] HALFILE (each), POSTGUI_HALFILE and SHUTDOWN, as
// written, without the arguments a Tcl file may have.
auto ini_hal_files(std::string_view text) -> std::vector<std::string> {
  std::vector<std::string> found;
  bool hal = false;
  while (!text.empty()) {
    const auto end = text.find('\n');
    const auto line = trimmed(text.substr(0, end));
    text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
    if (line.empty() || line.front() == '#' || line.front() == ';')
      continue;
    if (line.front() == '[') {
      hal = line == "[HAL]";
      continue;
    }
    const auto equals = line.find('=');
    if (!hal || equals == std::string_view::npos)
      continue;
    const auto key = trimmed(line.substr(0, equals));
    if (key != "HALFILE" && key != "POSTGUI_HALFILE" && key != "SHUTDOWN")
      continue;
    const auto value = trimmed(line.substr(equals + 1));
    if (!value.empty())
      found.emplace_back(value);
  }
  return found;
}

// A HAL file as the INI names it, as a path below the configuration folder; none when it is
// not one (LinuxCNC's library, a path elsewhere).
auto hal_relative(std::string_view written, const std::string& config_directory)
    -> std::optional<std::string> {
  written = written.substr(0, written.find_first_of(" \t"));
  const auto prefix = config_directory + '/';
  std::string relative;
  if (written.starts_with("LIB:"))
    return std::nullopt;
  if (written.starts_with(prefix))
    relative = written.substr(prefix.size());
  else if (!written.starts_with('/'))
    relative = written;
  if (relative.empty() || !plain_relative(relative))
    return std::nullopt;
  return relative;
}

// A durable replacement of `target` by `bytes`, through a temporary file beside it.
auto replace_file(ToolModelFiles& files, const std::string& target, std::string_view bytes)
    -> ToolModelFiles::Done {
  const auto temporary = target + ".coffeecnc-tmp";
  auto step = files.open_temporary(temporary);
  if (step)
    step = files.write(bytes);
  if (step)
    step = files.sync();
  if (step)
    step = files.rename(temporary, target);
  if (!step) {
    files.discard(temporary);
    return std::unexpected(std::format("{}: {}", target, step.error()));
  }
  if (auto synced = files.sync_directory(parent(target)); !synced)
    return std::unexpected(std::format("{}: {}", target, synced.error()));
  return {};
}

// The content of a file; none when there is no such file.
auto read_file(ToolModelFiles& files, const std::string& path)
    -> std::expected<std::optional<std::string>, std::string> {
  const auto content = files.read(path, max_archive_entry_bytes);
  if (!content)
    return std::unexpected(std::format("{} cannot be read: {}", path, content.error()));
  const auto& found = *content;
  if (!found)
    return std::optional<std::string>{};
  if (found->more)
    return std::unexpected(std::format("{} is larger than 16 MiB.", path));
  return std::optional<std::string>{found->bytes};
}

// A configuration file `relative` beneath `root`, as LinuxCNC reads it; none when missing.
auto read_config(ToolModelFiles& files, const std::string& root, const std::string& relative)
    -> std::expected<std::optional<ToolModelFiles::Found>, std::string> {
  auto found = files.read_beneath(root, relative, max_archive_entry_bytes);
  if (!found)
    return std::unexpected(found.error());
  if (const auto& file = *found; file && file->content.more)
    return std::unexpected(std::format("{}/{} is larger than 16 MiB.", root, relative));
  return found;
}

struct Source {
  std::string role;
  std::string name;     // in the archive
  std::string root;     // the folder it is read beneath
  std::string relative; // below it
  bool symlink{};
  [[nodiscard]] auto path() const -> std::string { return root + '/' + relative; }
};

auto manifest_json(const std::optional<std::string>& machine_id, std::int64_t created,
                   const std::vector<Source>& sources, const std::vector<BackupOmission>& left_out)
    -> Json {
  auto files = Json::array();
  std::ranges::transform(sources, std::back_inserter(files), [](const Source& source) -> Json {
    return {{"role", source.role},
            {"name", source.name},
            {"path", source.path()},
            {"symlink", source.symlink}};
  });
  auto not_included = Json::array();
  for (const auto& item : left_out)
    not_included.push_back({{"path", item.path}, {"reason", item.reason}});
  return {{"format", format_name},
          {"version", manifest_version},
          {"machine_id", machine_id ? Json(*machine_id) : Json(nullptr)},
          {"created", created},
          {"files", std::move(files)},
          {"not_included", std::move(not_included)}};
}

// A list of the strings of a JSON array; the others are skipped.
auto strings(const Json& value) -> std::vector<std::string> {
  std::vector<std::string> out;
  if (value.is_array())
    for (const auto& item : value)
      if (item.is_string())
        out.push_back(item.get<std::string>());
  return out;
}

auto joined(const std::vector<std::string>& parts) -> std::string {
  std::string out;
  for (const auto& part : parts)
    out += (out.empty() ? "" : "; ") + part;
  return out;
}

// LinuxCNC writes its .var file as it stops, CoffeeCNC its data files: what they hold when the
// restore is prepared is not what the helper replaces, and it decides them.
auto decided_at_exit(std::string_view role) -> bool {
  return role == "parameters" || role == "data";
}

} // namespace

auto restored_data_file(std::string_view name) -> bool {
  return std::ranges::find(data_file_names, name) != data_file_names.end() &&
         !name.starts_with("alarm-log");
}

auto boot_time() -> BootTime {
  BootTime now;
  std::ifstream id{"/proc/sys/kernel/random/boot_id"};
  std::getline(id, now.boot_id);
#ifdef CLOCK_BOOTTIME
  constexpr clockid_t clock = CLOCK_BOOTTIME;
#else
  constexpr clockid_t clock = CLOCK_MONOTONIC;
#endif
  timespec time{};
  if (::clock_gettime(clock, &time) != 0)
    return {};
  now.since_boot = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::seconds{time.tv_sec} + std::chrono::nanoseconds{time.tv_nsec});
  return now;
}

MachineData::MachineData(ToolModelFiles& files, MachineDataPaths paths, Clock clock, Pause pause,
                         BootClock boot)
    : files_(files), paths_(std::move(paths)), clock_(std::move(clock)),
      boot_(boot ? std::move(boot) : BootClock{[] { return boot_time(); }}) {
  if (!pause)
    pause = [](std::chrono::milliseconds period) { std::this_thread::sleep_for(period); };
  report_last_restore(pause);
}

void MachineData::report_last_restore(const Pause& pause) {
  if (paths_.data_directory.empty())
    return;
  const auto staging = paths_.data_directory + '/' + std::string(restore_directory_name);
  const auto plan_file = staging + '/' + std::string(restore_plan_name);
  const auto applying_file = staging + '/' + std::string(restore_applying_name);
  const auto result_file = staging + '/' + std::string(restore_result_name);
  const auto exists = [&](const std::string& path) -> bool {
    const auto found = files_.read(path, 0);
    return !found || *found;
  };
  // A helper may still write: what it leaves is read under the lock it holds while it writes.
  bool locked = false;
  for (auto waited = std::chrono::milliseconds{};; waited += lock_poll) {
    const auto taken = files_.lock(paths_.data_directory);
    if (taken) {
      locked = true;
      break;
    }
    if (!taken.error().in_use || waited >= lock_wait)
      break;
    pause(lock_poll);
  }
  if (!locked) {
    if (exists(plan_file) || exists(applying_file) || exists(result_file))
      notice_ = {true,
                 "The data folder is in use: what came of the last restore is shown at the next "
                 "start.",
                 MessageKey{"backend.machineData.busy"}};
    return;
  }
  const auto record = [&](const std::string& path) -> Json {
    const auto content = files_.read(path, max_record_bytes);
    if (!content || !*content)
      return Json(nullptr);
    const auto value = Json::parse((*content)->bytes, nullptr, false);
    return value.is_object() ? value : Json::object();
  };
  const auto result = record(result_file);
  const auto applying = record(applying_file);
  const auto plan = record(plan_file);
  if (result.is_object()) {
    const auto written = result.value("written", std::uint64_t{});
    const auto copy = result.value("pre_restore", std::string{});
    const auto reason = result.value("reason", std::string{});
    const auto kept = strings(result.value("not_put_back", Json::array()));
    const auto groups = strings(result.value("group_not_kept", Json::array()));
    if (result.value("ok", false) && written == 0)
      notice_ = {false, "The backup was restored: every file already held what the backup holds.",
                 MessageKey{"backend.machineData.alreadyRestored"}};
    else if (result.value("ok", false) && !groups.empty())
      notice_ = {
          false,
          std::format("The backup was restored: {} files were written. The files from "
                      "before the restore are in {}. These files did not keep their "
                      "group, which this user is not in: {}.",
                      written, copy, joined(groups)),
          MessageKey{
              "backend.machineData.restoredGroups",
              {{"count", std::to_string(written)}, {"path", copy}, {"files", joined(groups)}}}};
    else if (result.value("ok", false))
      notice_ = {false,
                 std::format("The backup was restored: {} files were written. The files from "
                             "before the restore are in {}.",
                             written, copy),
                 MessageKey{"backend.machineData.restored",
                            {{"count", std::to_string(written)}, {"path", copy}}}};
    else if (kept.empty())
      notice_ = {
          true,
          std::format("The restore was not completed: {} The files are as they were.", reason),
          MessageKey{"backend.machineData.notCompleted", {{"detail", reason}}}};
    else
      notice_ = {true,
                 std::format("The restore was not completed: {} These files could not be put "
                             "back: {}. The files from before the restore are in {}.",
                             reason, joined(kept), copy),
                 MessageKey{"backend.machineData.notPutBack",
                            {{"detail", reason}, {"files", joined(kept)}, {"path", copy}}}};
  } else if (applying.is_object()) {
    const auto pre_restore = applying.value("pre_restore", std::string{});
    notice_ = {true,
               std::format("A restore was interrupted while it wrote the files: some files may "
                           "be the backup's and others not. The files from before the restore "
                           "are in {}: restore that backup to go back.",
                           pre_restore),
               MessageKey{"backend.machineData.interrupted", {{"path", pre_restore}}}};
  } else if (!plan.is_null()) {
    notice_ = {true,
               "A restore was prepared but not written: LinuxCNC did not stop in time, or the "
               "restore could not start. Nothing was changed; restore the backup again.",
               MessageKey{"backend.machineData.leftPlan"}};
  }
  if (plan.is_object() && plan.contains("files") && plan["files"].is_array())
    for (const auto& file : plan["files"])
      if (file.is_object() && file.value("from", std::string{}).starts_with(staging + '/'))
        files_.discard(file.value("from", std::string{}));
  for (const auto& path : {result_file, applying_file, plan_file})
    if (exists(path))
      files_.discard(path);
  // The removals are not needed for the report: what is left is reported again next start.
  const auto synced = files_.sync_directory(staging);
  static_cast<void>(synced);
  files_.unlock();
}

auto MachineData::take_notice() -> std::optional<ControllerMessage> {
  const std::scoped_lock lock{mutex_};
  return std::exchange(notice_, std::nullopt);
}

auto MachineData::backup(const std::string& directory) -> MachineDataAnswer {
  if (!directory.starts_with('/'))
    return {CommandOutcome::rejected,
            "Choose a folder for the backup.",
            {},
            false,
            MessageKey{"messages.policy.backupFolder"}};
  const auto failed = [](const std::string& detail) -> MachineDataAnswer {
    return {CommandOutcome::failed,
            std::format("The backup was not written: {}", detail),
            {},
            false,
            MessageKey{"backend.machineData.notWritten", {{"detail", detail}}}};
  };
  std::vector<Source> sources;
  std::vector<BackupOmission> left_out;
  const auto beside = [](std::string role, std::string name, const std::string& path) -> Source {
    return {std::move(role), std::move(name), parent(path), base_name(path), false};
  };
  sources.push_back(beside("ini", "config/" + base_name(paths_.ini_file), paths_.ini_file));
  std::set<std::string> taken{paths_.ini_file};
  for (const auto& written : paths_.hal_files) {
    if (written.starts_with("LIB:")) {
      left_out.emplace_back(written, "a HAL file of LinuxCNC's own library",
                            MessageKey{"backend.machineData.libHal"});
      continue;
    }
    const auto relative = hal_relative(written, paths_.config_directory);
    if (!relative) {
      left_out.emplace_back(written, "not a file in the configuration folder",
                            MessageKey{"backend.machineData.outsideConfig"});
      continue;
    }
    const auto path = paths_.config_directory + '/' + *relative;
    // Read as LinuxCNC reads it, through links on the way too.
    if (taken.insert(path).second)
      sources.push_back(beside("hal", "config/" + *relative, path));
  }
  if (!paths_.tool_table.empty())
    sources.push_back(
        beside("tool_table", "tool-table/" + base_name(paths_.tool_table), paths_.tool_table));
  if (!paths_.parameter_file.empty())
    sources.push_back(beside("parameters", "parameters/" + base_name(paths_.parameter_file),
                             paths_.parameter_file));
  if (!paths_.data_directory.empty())
    std::ranges::transform(data_file_names, std::back_inserter(sources),
                           [&](std::string_view name) -> Source {
                             return {"data", "data/" + std::string(name), paths_.data_directory,
                                     std::string(name), false};
                           });

  const auto now = clock_();
  std::vector<ArchiveEntry> entries(1);
  std::vector<Source> included;
  for (auto source : sources) {
    const auto content = read_config(files_, source.root, source.relative);
    if (!content)
      return failed(content.error());
    const auto& found = *content;
    if (!found) {
      // A data file that was never written, or a HAL file LinuxCNC finds elsewhere.
      if (source.role == "hal" || source.role == "ini")
        left_out.emplace_back(source.path(), "not found in the configuration folder",
                              MessageKey{"backend.machineData.notFound"});
      continue;
    }
    entries.push_back(
        {source.name, found->content.bytes, std::chrono::floor<std::chrono::seconds>(now).count()});
    source.symlink = found->symlink;
    included.push_back(std::move(source));
  }
  const auto machine = paths_.machine_id.empty() ? std::optional<std::string>{}
                                                 : std::optional<std::string>{paths_.machine_id};
  entries.front() = {std::string(backup_manifest_name),
                     dump(manifest_json(machine, now.count(), included, left_out)) + '\n',
                     std::chrono::floor<std::chrono::seconds>(now).count()};
  const auto archive = write_archive(entries);
  if (!archive)
    return failed(archive.error().text);
  const auto name =
      std::format("coffeecnc-{}-{}.tar.gz",
                  paths_.machine_id.empty() ? "machine" : paths_.machine_id, stamp(now));
  const auto target = (directory == "/" ? std::string{} : directory) + '/' + name;
  if (auto written = replace_file(files_, target, *archive); !written)
    return failed(written.error());
  return {CommandOutcome::accepted,
          "",
          {encode_backup_written({target, included.size(), archive->size(), left_out})},
          false};
}

auto MachineData::check(const std::string& path) -> std::expected<Checked, LocalizedReason> {
  if (!path.starts_with('/'))
    return std::unexpected(
        LocalizedReason{"Choose a backup file.", MessageKey{"messages.policy.backupFile"}});
  if (paths_.data_directory.empty())
    return std::unexpected(LocalizedReason{
        "A restore needs the machine's data folder: set [COFFEECNC] MACHINE_ID in the INI file.",
        MessageKey{"backend.machineData.needsData"}});
  const auto content = files_.read(path, max_archive_bytes);
  if (!content)
    return std::unexpected(
        LocalizedReason{std::format("{} cannot be read: {}", path, content.error()),
                        MessageKey{"backend.machineData.cannotRead",
                                   {{"path", path}, {"detail", content.error()}}}});
  const auto& found = *content;
  if (!found)
    return std::unexpected(
        LocalizedReason{std::format("{} does not exist.", path),
                        MessageKey{"backend.machineData.missing", {{"path", path}}}});
  if (found->more)
    return std::unexpected(LocalizedReason{"The archive is larger than 64 MiB.",
                                           MessageKey{"backend.machineData.archiveTooLarge"}});
  const auto& bytes = found->bytes;
  const auto entries = read_archive(bytes);
  if (!entries)
    return std::unexpected(entries.error());
  const auto manifest_entry =
      std::ranges::find(*entries, backup_manifest_name, &ArchiveEntry::name);
  if (manifest_entry == entries->end())
    return std::unexpected(LocalizedReason{"This is not a CoffeeCNC backup: it has no manifest.",
                                           MessageKey{"backend.machineData.noManifest"}});
  const auto manifest = Json::parse(manifest_entry->bytes, nullptr, false);
  if (!manifest.is_object() || manifest.value("format", std::string{}) != format_name ||
      !manifest.contains("version") || manifest["version"] != manifest_version ||
      !manifest.contains("files") || !manifest["files"].is_array())
    return std::unexpected(
        LocalizedReason{"This backup was written by another version of CoffeeCNC, or is damaged.",
                        MessageKey{"backend.machineData.otherVersion"}});
  Checked checked;
  if (const auto id = manifest.find("machine_id"); id != manifest.end() && id->is_string())
    checked.machine_id = id->get<std::string>();
  if (const auto created = manifest.find("created");
      created != manifest.end() && created->is_number_integer())
    checked.created = created->get<std::int64_t>();
  // A restore overwrites this machine's INI and HAL files: a backup that does not say whose it
  // is could be another machine's.
  if (!paths_.machine_id.empty() && !checked.machine_id)
    return std::unexpected(LocalizedReason{
        std::format(
            "This backup does not name its machine; this machine is {}. It is not restored.",
            paths_.machine_id),
        MessageKey{"backend.machineData.noMachine", {{"machine", paths_.machine_id}}}});
  if (checked.machine_id && !paths_.machine_id.empty() && *checked.machine_id != paths_.machine_id)
    return std::unexpected(LocalizedReason{
        std::format("This backup is of machine {}; this machine is {}.", *checked.machine_id,
                    paths_.machine_id),
        MessageKey{"backend.machineData.otherMachine",
                   {{"backup", *checked.machine_id}, {"machine", paths_.machine_id}}}});
  // The role of each name, from the manifest.
  std::vector<std::pair<std::string, std::string>> listed;
  for (const auto& file : manifest["files"]) {
    if (!file.is_object() || !file.contains("role") || !file["role"].is_string() ||
        !file.contains("name") || !file["name"].is_string())
      return std::unexpected(
          LocalizedReason{"This backup was written by another version of CoffeeCNC, or is damaged.",
                          MessageKey{"backend.machineData.otherVersion"}});
    listed.emplace_back(file["role"].get<std::string>(), file["name"].get<std::string>());
  }
  // The HAL files a restore may write: those this machine's INI file names, and those the
  // archive's names (it replaces this one).
  std::set<std::string> hal_names;
  const auto name_hal_files = [&](const std::vector<std::string>& written) {
    for (const auto& file : written)
      if (const auto relative = hal_relative(file, paths_.config_directory))
        hal_names.insert(*relative);
  };
  name_hal_files(paths_.hal_files);
  for (const auto& entry : *entries)
    if (std::ranges::contains(listed, std::pair<std::string, std::string>{"ini", entry.name}))
      name_hal_files(ini_hal_files(entry.bytes));
  const std::set<std::string> own_files{paths_.ini_file, paths_.tool_table, paths_.parameter_file};
  std::set<std::string> names;
  std::set<std::string> targets;
  bool ini = false;
  for (const auto& entry : *entries) {
    if (!names.insert(entry.name).second)
      return std::unexpected(
          LocalizedReason{std::format("{} is in the archive twice.", entry.name),
                          MessageKey{"backend.machineData.twice", {{"name", entry.name}}}});
    if (entry.name == backup_manifest_name)
      continue;
    const auto role =
        std::ranges::find(listed, entry.name, &std::pair<std::string, std::string>::second);
    if (role == listed.end())
      return std::unexpected(
          LocalizedReason{std::format("{} is not in the backup's manifest.", entry.name),
                          MessageKey{"backend.machineData.notListed", {{"name", entry.name}}}});
    const auto refused = [&]() -> std::unexpected<LocalizedReason> {
      return std::unexpected(LocalizedReason{
          std::format("{} is not a name this backup may restore: the archive is refused.",
                      entry.name),
          MessageKey{"backend.machineData.badName", {{"name", entry.name}}}});
    };
    Planned planned;
    const auto& kind = role->first;
    const auto after = [&](std::string_view head) -> std::string {
      return entry.name.substr(head.size());
    };
    const auto beside = [&](const std::string& target) {
      planned.root = parent(target);
      planned.relative = base_name(target);
    };
    if (kind == "ini" || kind == "hal") {
      if (!entry.name.starts_with("config/") || !plain_relative(after("config/")))
        return refused();
      if (kind == "ini") {
        if (ini || after("config/").find('/') != std::string::npos)
          return refused();
        ini = true;
        beside(paths_.ini_file);
      } else {
        const auto relative = after("config/");
        const bool script = relative.ends_with(".hal") || relative.ends_with(".tcl");
        if (!hal_names.contains(relative) || !script ||
            own_files.contains(paths_.config_directory + '/' + relative))
          return std::unexpected(LocalizedReason{
              std::format("{} is not a HAL file the INI file names: the archive is refused.",
                          entry.name),
              MessageKey{"backend.machineData.halNotNamed", {{"name", entry.name}}}});
        planned.root = paths_.config_directory;
        planned.relative = relative;
      }
    } else if (kind == "tool_table" || kind == "parameters") {
      const std::string_view head = kind == "tool_table" ? "tool-table/" : "parameters/";
      if (!entry.name.starts_with(head) || !plain_component(after(head)) ||
          after(head).size() > max_relative_bytes)
        return refused();
      const auto& target = kind == "tool_table" ? paths_.tool_table : paths_.parameter_file;
      if (target.empty()) {
        const auto* setting =
            kind == "tool_table" ? "[EMCIO] TOOL_TABLE" : "[RS274NGC] PARAMETER_FILE";
        return std::unexpected(
            LocalizedReason{std::format("This configuration has no {}: {} cannot be restored.",
                                        setting, entry.name),
                            MessageKey{"backend.machineData.notConfigured",
                                       {{"key", setting}, {"name", entry.name}}}});
      }
      beside(target);
    } else if (kind == "data") {
      const auto name = entry.name.starts_with("data/") ? after("data/") : std::string{};
      if (std::ranges::find(data_file_names, name) == data_file_names.end())
        return refused();
      if (!restored_data_file(name))
        continue; // the alarm log: kept in the backup, not restored
      planned.root = paths_.data_directory;
      planned.relative = name;
    } else {
      return refused();
    }
    if (!targets.insert(planned.target()).second)
      return std::unexpected(LocalizedReason{
          std::format("Two files of the backup go to {}.", planned.target()),
          MessageKey{"backend.machineData.sameTarget", {{"path", planned.target()}}}});
    const auto now = read_config(files_, planned.root, planned.relative);
    if (!now)
      return std::unexpected(
          LocalizedReason{std::format("{} cannot be restored: {}. The archive is refused.",
                                      entry.name, now.error()),
                          MessageKey{"backend.machineData.unreachable",
                                     {{"name", entry.name}, {"detail", now.error()}}}});
    const auto& current = *now;
    // A restore makes no folder (but the data folder): a target in a folder that is not there
    // cannot be written once LinuxCNC has stopped.
    if (!current && kind != "data") {
      const auto folder = files_.folder_exists_beneath(planned.root, planned.relative);
      if (!folder)
        return std::unexpected(
            LocalizedReason{std::format("{} cannot be restored: {}. The archive is refused.",
                                        entry.name, folder.error()),
                            MessageKey{"backend.machineData.unreachable",
                                       {{"name", entry.name}, {"detail", folder.error()}}}});
      if (!*folder)
        return std::unexpected(LocalizedReason{
            std::format("{} cannot be restored: the folder of {} does not exist. The archive is "
                        "refused.",
                        entry.name, planned.target()),
            MessageKey{"backend.machineData.noFolder",
                       {{"name", entry.name}, {"path", planned.target()}}}});
    }
    planned.role = kind;
    planned.name = entry.name;
    planned.bytes = entry.bytes;
    planned.symlink = current && current->symlink;
    if (current)
      planned.held = current->content.bytes;
    planned.change = decided_at_exit(kind)                   ? "at_exit"
                     : !current                              ? "new"
                     : current->content.bytes == entry.bytes ? "same"
                                                             : "changed";
    checked.files.push_back(std::move(planned));
  }
  if (!ini)
    return std::unexpected(
        LocalizedReason{"This backup has no INI file.", MessageKey{"backend.machineData.noIni"}});
  checked.token = std::format("{:016x}", fnv(bytes, fnv(path + '\0')));
  return checked;
}

auto MachineData::check_restore(const std::string& path) -> MachineDataAnswer {
  const auto checked = check(path);
  if (!checked)
    return {
        CommandOutcome::rejected, checked.error().text, {}, false, checked.error().localization};
  RestoreSummary summary{path, checked->token, checked->machine_id, checked->created, {}};
  for (const auto& file : checked->files)
    summary.files.push_back(
        {file.role, file.name, file.target(), file.change, file.bytes.size(), file.symlink});
  return {CommandOutcome::accepted, "", {encode_restore_summary(summary)}, false};
}

auto MachineData::restore(const std::string& path, const std::string& token, std::uint64_t page,
                          const std::function<LocalizedReason()>& recheck) -> MachineDataAnswer {
  const auto refuse = [](const LocalizedReason& reason) -> MachineDataAnswer {
    return {CommandOutcome::rejected, reason.text, {}, false, reason.localization};
  };
  const auto checked = check(path);
  if (!checked)
    return refuse(checked.error());
  if (checked->token != token)
    return refuse({"The backup file changed after it was checked. Check it again.",
                   MessageKey{"backend.machineData.changedSince"}});
  if (const auto reason = recheck(); !reason.empty())
    return refuse(reason);
  // After the recheck: a page that begins meanwhile abandons under this lock, once this staging
  // is done (Session::begin_page holds its own lock then, and this code never takes it).
  const std::scoped_lock lock{stage_mutex_};
  std::vector<const Planned*> changes;
  for (const auto& file : checked->files)
    if (file.change != "same")
      changes.push_back(&file);
  if (changes.empty())
    return {CommandOutcome::accepted,
            "Nothing to restore: every file is the same.",
            {},
            false,
            MessageKey{"backend.machineData.nothing"}};
  const auto staging = paths_.data_directory + '/' + std::string(restore_directory_name);
  const auto failed = [](const std::string& reason) -> MachineDataAnswer {
    return {CommandOutcome::failed,
            std::format("The restore was not prepared: {}", reason),
            {},
            false,
            MessageKey{"backend.machineData.notPrepared", {{"detail", reason}}}};
  };
  // The data folder too, where nothing was saved in it yet: each missing folder 0700.
  if (const auto made = prepare_data_directory(files_, paths_.data_directory); !made)
    return failed(made.error());
  if (const auto made = files_.make_directory(staging); !made)
    return failed(made.error());
  const auto plan_file = staging + '/' + std::string(restore_plan_name);
  abandon_locked();
  files_.discard(plan_file);
  auto plan = Json::array();
  for (std::size_t i = 0; i < changes.size(); ++i) {
    const auto& file = *changes[i];
    const auto copy = std::format("{}/{}.staged", staging, i);
    staged_files_.push_back(copy);
    if (auto written = replace_file(files_, copy, file.bytes); !written)
      return failed(written.error());
    plan.push_back({{"role", file.role},
                    {"name", file.name},
                    {"from", copy},
                    {"root", file.root},
                    {"relative", file.relative},
                    {"held", held_json(file.held)}});
  }
  // How old the plan is is told on the boot clock, which NTP does not step.
  const auto booted = boot_();
  const Json document{{"version", plan_version},
                      {"machine_id", paths_.machine_id},
                      {"staged", clock_().count()},
                      {"boot_id", booted.boot_id},
                      {"booted", booted.since_boot.count()},
                      {"page", page},
                      {"source", path},
                      {"ini", paths_.ini_file},
                      {"files", std::move(plan)}};
  if (auto written = replace_file(files_, plan_file, dump(document) + '\n'); !written)
    return failed(written.error());
  staged_ = true;
  return {CommandOutcome::accepted, "", {}, true};
}

void MachineData::abandon_restore() {
  const std::scoped_lock lock{stage_mutex_};
  abandon_locked();
}

void MachineData::abandon_locked() {
  if (paths_.data_directory.empty())
    return;
  const auto staging = paths_.data_directory + '/' + std::string(restore_directory_name);
  if (staged_.exchange(false)) {
    // The plan first: a staged copy without a plan is never written.
    files_.discard(staging + '/' + std::string(restore_plan_name));
    const auto synced = files_.sync_directory(staging);
    static_cast<void>(synced);
  }
  for (const auto& copy : std::exchange(staged_files_, {}))
    files_.discard(copy);
}

auto apply_staged_restore(ToolModelFiles& files, const std::string& data_directory,
                          std::chrono::milliseconds now, const RestoreHooks& hooks)
    -> std::expected<std::size_t, std::string> {
  const auto staging = data_directory + '/' + std::string(restore_directory_name);
  const auto plan_file = staging + '/' + std::string(restore_plan_name);
  const auto applying_file = staging + '/' + std::string(restore_applying_name);
  const auto result_file = staging + '/' + std::string(restore_result_name);
  const auto plan_content = files.read(plan_file, max_record_bytes);
  if (!plan_content)
    return std::unexpected(plan_content.error());
  struct Replacement {
    std::string role;
    std::string name;
    std::string from;
    std::string root;
    std::string relative;
    std::string bytes;
    std::optional<ToolModelFiles::Found> before;
    [[nodiscard]] auto target() const -> std::string { return root + '/' + relative; }
  };
  std::vector<Replacement> steps;
  std::string reason;
  std::string copy_path;
  std::size_t written = 0;
  std::vector<std::string> not_put_back;
  std::vector<std::string> group_not_kept;
  const auto& plan_found = *plan_content;
  if (!plan_found)
    return 0;
  const auto plan = Json::parse(plan_found->bytes, nullptr, false);
  const auto damaged = "The prepared restore is damaged.";
  // Only a plan of this machine's data folder: the folder is named by the machine ID.
  if (!plan.is_object() || plan.value("version", 0) != plan_version ||
      !plan.contains("machine_id") || !plan["machine_id"].is_string() ||
      plan["machine_id"].get<std::string>() != base_name(data_directory) ||
      !plan.contains("staged") || !plan["staged"].is_number_integer() ||
      !plan.contains("boot_id") || !plan["boot_id"].is_string() || !plan.contains("booted") ||
      !plan["booted"].is_number_integer() || !plan.contains("files") ||
      !plan["files"].is_array())
    reason = damaged;
  // A plan whose quit was not delivered, written by a later exit: what it was checked against is
  // old, and the .var and data files are not compared.
  // Measured on the boot clock: a system clock that NTP stepped meanwhile does not count.
  const auto ended = reason.empty() ? hooks.ended.value_or(boot_time()) : BootTime{};
  if (reason.empty() &&
      (ended.boot_id.empty() || plan["boot_id"].get<std::string>() != ended.boot_id))
    reason = "The restore was prepared before the computer was started again: nothing was "
             "written.";
  if (reason.empty()) {
    const auto age =
        ended.since_boot - std::chrono::milliseconds{plan["booted"].get<std::int64_t>()};
    constexpr auto limit =
        std::chrono::duration_cast<std::chrono::milliseconds>(restore_plan_limit);
    if (age > limit || age < -limit)
      reason = std::format("The restore was prepared {} s before CoffeeCNC ended, more than {} s: "
                           "nothing was written.",
                           std::chrono::floor<std::chrono::seconds>(age).count(),
                           std::chrono::duration_cast<std::chrono::seconds>(limit).count());
  }
  const auto machine_id = reason.empty() ? plan["machine_id"].get<std::string>() : std::string{};
  for (const auto& file : reason.empty() ? plan["files"] : Json::array()) {
    if (!file.is_object() || !file.contains("held")) {
      reason = damaged;
      break;
    }
    Replacement step{file.value("role", std::string{}),
                     file.value("name", std::string{}),
                     file.value("from", std::string{}),
                     file.value("root", std::string{}),
                     file.value("relative", std::string{}),
                     {},
                     {}};
    // Only copies this program staged, and only to plain paths below absolute folders.
    if (!step.from.starts_with(staging + '/') || !step.root.starts_with('/') ||
        !plain_relative(step.relative)) {
      reason = damaged;
      break;
    }
    const auto staged = read_file(files, step.from);
    if (!staged) {
      reason = staged.error();
      break;
    }
    const auto& staged_bytes = *staged;
    if (!staged_bytes) {
      reason = step.from + " is missing.";
      break;
    }
    auto before = read_config(files, step.root, step.relative);
    if (!before) {
      reason = before.error();
      break;
    }
    // What was there when the restore was prepared, unless LinuxCNC or CoffeeCNC wrote it as
    // it stopped.
    const auto& existing = *before;
    const auto now_held = existing ? std::optional<std::string>{existing->content.bytes}
                                   : std::optional<std::string>{};
    if (!decided_at_exit(step.role) && held_json(now_held) != file["held"]) {
      reason = std::format("{} changed after the restore was prepared: nothing was written.",
                           step.target());
      break;
    }
    step.bytes = *staged_bytes;
    step.before = std::move(*before);
    // Already the backup's (the .var LinuxCNC left may be): not written.
    if (!step.before || step.before->content.bytes != step.bytes)
      steps.push_back(std::move(step));
  }
  if (reason.empty() && !steps.empty()) {
    // The files as they are now, as a backup of this machine that the panel can restore.
    std::vector<ArchiveEntry> entries(1);
    auto listed = Json::array();
    const auto seconds = std::chrono::floor<std::chrono::seconds>(now).count();
    // The INI file always: a backup without one is not restored.
    const auto ini = plan.value("ini", std::string{});
    if (!ini.empty() &&
        std::ranges::none_of(steps, [&](const auto& step) { return step.target() == ini; })) {
      const auto current = read_config(files, parent(ini), base_name(ini));
      if (current) {
        const auto& found = *current;
        if (found) {
          entries.push_back({"config/" + base_name(ini), found->content.bytes, seconds});
          listed.push_back({{"role", "ini"},
                            {"name", "config/" + base_name(ini)},
                            {"path", ini},
                            {"symlink", found->symlink}});
        }
      }
    }
    for (const auto& step : steps)
      if (step.before) {
        entries.push_back({step.name, step.before->content.bytes, seconds});
        listed.push_back({{"role", step.role},
                          {"name", step.name},
                          {"path", step.target()},
                          {"symlink", step.before->symlink}});
      }
    entries.front() = {std::string(backup_manifest_name),
                       dump(Json{{"format", format_name},
                                 {"version", manifest_version},
                                 {"machine_id", machine_id},
                                 {"created", now.count()},
                                 {"files", std::move(listed)},
                                 {"not_included", Json::array()}}) +
                           '\n',
                       seconds};
    copy_path = std::format("{}/pre-restore-{}.tar.gz", data_directory, stamp(now));
    const auto archive = write_archive(entries);
    const auto saved = archive ? replace_file(files, copy_path, *archive)
                               : ToolModelFiles::Done{std::unexpected(archive.error().text)};
    if (!saved)
      reason = "the files before the restore could not be saved: " + saved.error();
  }
  if (reason.empty() && !steps.empty() && hooks.may_write)
    if (auto refused = hooks.may_write())
      reason = std::move(*refused);
  if (reason.empty() && !steps.empty()) {
    // Before the first rename: a crash from here on is reported as a partial write, with the
    // copy to go back to.
    auto targets = Json::array();
    std::ranges::transform(steps, std::back_inserter(targets),
                           [](const auto& step) -> Json { return step.target(); });
    const Json applying{
        {"pre_restore", copy_path}, {"files", std::move(targets)}, {"time", now.count()}};
    if (auto marked = replace_file(files, applying_file, dump(applying) + '\n'); !marked)
      reason = "the restore could not be marked as started: " + marked.error();
  }
  if (reason.empty()) {
    for (; written < steps.size(); ++written) {
      const auto& step = steps[written];
      if (hooks.still_may_write)
        if (auto refused = hooks.still_may_write()) {
          reason = std::move(*refused);
          break;
        }
      const auto mode = step.before           ? step.before->mode
                        : step.role == "data" ? new_data_mode
                                              : new_config_mode;
      const auto group =
          step.before ? std::optional<unsigned>{step.before->group} : std::optional<unsigned>{};
      const auto replaced =
          files.replace_beneath(step.root, step.relative, step.bytes, mode, group);
      if (!replaced) {
        // Renamed, and only its folder's fsync failed: it is the backup's now, and is put back.
        if (replaced.error().renamed)
          ++written;
        reason = replaced.error().reason;
        break;
      }
      if (!replaced->group_kept)
        group_not_kept.push_back(step.target());
    }
    if (!reason.empty()) {
      // Back to what was there, newest first.
      for (auto i = written; i-- > 0;) {
        const auto& step = steps[i];
        if (!step.before) {
          if (auto removed = files.remove_beneath(step.root, step.relative); !removed)
            not_put_back.push_back(removed.error());
        } else if (step.before->symlink && !step.before->link.empty()) {
          // The restore replaced a link by the file: the same link again, not its content.
          if (auto relinked = files.link_beneath(step.root, step.relative, step.before->link);
              !relinked)
            not_put_back.push_back(relinked.error());
        } else if (auto back =
                       files.replace_beneath(step.root, step.relative, step.before->content.bytes,
                                             step.before->mode, step.before->group);
                   !back && !back.error().renamed) {
          not_put_back.push_back(back.error().reason);
        }
      }
      written = 0;
      group_not_kept.clear();
    }
  }
  const bool ok = reason.empty();
  const Json kept = not_put_back;
  const Json result{{"ok", ok},
                    {"written", written},
                    {"reason", reason},
                    {"pre_restore", copy_path},
                    {"not_put_back", kept},
                    {"group_not_kept", group_not_kept},
                    {"time", now.count()}};
  const auto said = replace_file(files, result_file, dump(result) + '\n');
  // The marker goes only once the result is on the disk: without one the next start says the
  // write was interrupted.
  if (said)
    files.discard(applying_file);
  for (const auto& step : plan.is_object() && plan.contains("files") && plan["files"].is_array()
                              ? plan["files"]
                              : Json::array())
    if (step.is_object() && step.value("from", std::string{}).starts_with(staging + '/'))
      files.discard(step.value("from", std::string{}));
  files.discard(plan_file);
  // The removals are not needed for the result: the plan is gone or is reported next run.
  const auto synced = files.sync_directory(staging);
  static_cast<void>(synced);
  if (!ok)
    return std::unexpected(reason);
  if (!said)
    return std::unexpected(said.error());
  return written;
}

} // namespace coffeecnc
