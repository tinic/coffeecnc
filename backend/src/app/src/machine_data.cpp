#include <coffeecnc/app/machine_data.hpp>

#include <coffeecnc/app/backup_archive.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <set>
#include <utility>

namespace coffeecnc {
namespace {
using Json = nlohmann::json;

constexpr std::string_view format_name = "coffeecnc-backup";
constexpr int manifest_version = 1;
constexpr std::string_view plan_name = "plan.json";
constexpr std::string_view result_name = "result.json";
constexpr std::size_t max_relative_bytes = 255;

auto dump(const Json& value) -> std::string {
  return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

// One component of a path: not empty, not `.` or `..`, no `/`, no control character.
auto plain_component(std::string_view part) -> bool {
  return !part.empty() && part != "." && part != ".." &&
         std::ranges::none_of(part, [](char c) {
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

auto stamp(std::chrono::milliseconds now) -> std::string {
  const std::chrono::sys_seconds seconds{std::chrono::floor<std::chrono::seconds>(now)};
  return std::format("{:%Y%m%d-%H%M%S}", seconds);
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

struct Source {
  std::string role;
  std::string name; // in the archive
  std::string path;
};

auto manifest_json(const std::optional<std::string>& machine_id, std::int64_t created,
                   const std::vector<Source>& sources,
                   const std::vector<std::pair<std::string, std::string>>& left_out) -> Json {
  auto files = Json::array();
  for (const auto& source : sources)
    files.push_back({{"role", source.role}, {"name", source.name}, {"path", source.path}});
  auto not_included = Json::array();
  for (const auto& [path, reason] : left_out)
    not_included.push_back({{"path", path}, {"reason", reason}});
  return {{"format", format_name},
          {"version", manifest_version},
          {"machine_id", machine_id ? Json(*machine_id) : Json(nullptr)},
          {"created", created},
          {"files", std::move(files)},
          {"not_included", std::move(not_included)}};
}

} // namespace

MachineData::MachineData(ToolModelFiles& files, MachineDataPaths paths, Clock clock)
    : files_(files), paths_(std::move(paths)), clock_(std::move(clock)) {
  if (paths_.data_directory.empty())
    return;
  const auto staging = paths_.data_directory + '/' + std::string(restore_directory_name);
  const auto plan = staging + '/' + std::string(plan_name);
  const auto result = staging + '/' + std::string(result_name);
  if (const auto left = files_.read(plan, 0); left && *left) {
    notice_ = {MessageLevel::error,
               "A restore was prepared but not written: LinuxCNC did not stop in time, or the "
               "restore could not start. Nothing was changed; restore the backup again."};
    files_.discard(plan);
  }
  const auto content = files_.read(result, std::size_t{64} * 1024);
  if (!content)
    return;
  const auto& found = *content;
  if (!found)
    return;
  const auto value = Json::parse(found->bytes, nullptr, false);
  files_.discard(result);
  if (!value.is_object())
    return;
  const auto written = value.value("written", std::uint64_t{});
  const auto copy = value.value("pre_restore", std::string{});
  if (value.value("ok", false))
    notice_ = {MessageLevel::info,
               std::format("The backup was restored: {} files were written. The files from "
                           "before the restore are in {}.",
                           written, copy)};
  else
    notice_ = {MessageLevel::error,
               std::format("The restore was not completed: {} The files are as they were.",
                           value.value("reason", std::string{}))};
}

auto MachineData::take_notice() -> std::optional<std::pair<MessageLevel, std::string>> {
  const std::scoped_lock lock{mutex_};
  return std::exchange(notice_, std::nullopt);
}

auto MachineData::backup(const std::string& directory) -> MachineDataAnswer {
  const auto refuse = [](std::string reason) -> MachineDataAnswer {
    return {CommandOutcome::rejected, std::move(reason), {}, false};
  };
  if (!directory.starts_with('/'))
    return refuse("Choose a folder for the backup.");
  std::vector<Source> sources;
  std::vector<std::pair<std::string, std::string>> left_out;
  sources.push_back({"ini", "config/" + base_name(paths_.ini_file), paths_.ini_file});
  std::set<std::string> taken{paths_.ini_file};
  const auto prefix = paths_.config_directory + '/';
  for (const auto& written : paths_.hal_files) {
    std::string relative;
    if (written.starts_with("LIB:")) {
      left_out.emplace_back(written, "a HAL file of LinuxCNC's own library");
      continue;
    }
    if (written.starts_with(prefix))
      relative = written.substr(prefix.size());
    else if (!written.starts_with('/'))
      relative = written;
    if (relative.empty() || !plain_relative(relative)) {
      left_out.emplace_back(written, "not a file in the configuration folder");
      continue;
    }
    const auto path = prefix + relative;
    if (taken.insert(path).second)
      sources.push_back({"hal", "config/" + relative, path});
  }
  if (!paths_.tool_table.empty())
    sources.push_back({"tool_table", "tool-table/" + base_name(paths_.tool_table), paths_.tool_table});
  if (!paths_.parameter_file.empty())
    sources.push_back(
        {"parameters", "parameters/" + base_name(paths_.parameter_file), paths_.parameter_file});
  if (!paths_.data_directory.empty())
    for (const auto name : data_file_names)
      sources.push_back({"data", "data/" + std::string(name),
                         paths_.data_directory + '/' + std::string(name)});

  const auto now = clock_();
  std::vector<ArchiveEntry> entries(1);
  std::vector<Source> included;
  for (const auto& source : sources) {
    const auto content = read_file(files_, source.path);
    if (!content)
      return {CommandOutcome::failed, "The backup was not written: " + content.error(), {}, false};
    const auto& bytes = *content;
    if (!bytes) {
      // A data file that was never written, or a HAL file LinuxCNC finds elsewhere.
      if (source.role == "hal" || source.role == "ini")
        left_out.emplace_back(source.path, "not found in the configuration folder");
      continue;
    }
    entries.push_back({source.name, *bytes,
                       std::chrono::floor<std::chrono::seconds>(now).count()});
    included.push_back(source);
  }
  const auto machine = paths_.machine_id.empty() ? std::optional<std::string>{}
                                                 : std::optional<std::string>{paths_.machine_id};
  entries.front() = {std::string(backup_manifest_name),
                     dump(manifest_json(machine, now.count(), included, left_out)) + '\n',
                     std::chrono::floor<std::chrono::seconds>(now).count()};
  const auto archive = write_archive(entries);
  if (!archive)
    return {CommandOutcome::failed, "The backup was not written: " + archive.error(), {}, false};
  const auto name = std::format("coffeecnc-{}-{}.tar.gz",
                                paths_.machine_id.empty() ? "machine" : paths_.machine_id, stamp(now));
  const auto target = (directory == "/" ? std::string{} : directory) + '/' + name;
  if (auto written = replace_file(files_, target, *archive); !written)
    return {CommandOutcome::failed, "The backup was not written: " + written.error(), {}, false};
  return {CommandOutcome::accepted,
          "",
          {encode_backup_written({target, included.size(), archive->size(), left_out})},
          false};
}

auto MachineData::check(const std::string& path) -> std::expected<Checked, std::string> {
  if (!path.starts_with('/'))
    return std::unexpected(std::string("Choose a backup file."));
  if (paths_.data_directory.empty())
    return std::unexpected(std::string(
        "A restore needs the machine's data folder: set [COFFEECNC] MACHINE_ID in the INI file."));
  const auto content = files_.read(path, max_archive_bytes);
  if (!content)
    return std::unexpected(std::format("{} cannot be read: {}", path, content.error()));
  const auto& found = *content;
  if (!found)
    return std::unexpected(std::format("{} does not exist.", path));
  if (found->more)
    return std::unexpected(std::string("The archive is larger than 64 MiB."));
  const auto& bytes = found->bytes;
  const auto entries = read_archive(bytes);
  if (!entries)
    return std::unexpected(entries.error());
  const auto manifest_entry =
      std::ranges::find(*entries, backup_manifest_name, &ArchiveEntry::name);
  if (manifest_entry == entries->end())
    return std::unexpected(std::string("This is not a CoffeeCNC backup: it has no manifest."));
  const auto manifest = Json::parse(manifest_entry->bytes, nullptr, false);
  if (!manifest.is_object() || manifest.value("format", std::string{}) != format_name ||
      !manifest.contains("version") || manifest["version"] != manifest_version ||
      !manifest.contains("files") || !manifest["files"].is_array())
    return std::unexpected(
        std::string("This backup was written by another version of CoffeeCNC, or is damaged."));
  Checked checked;
  if (const auto id = manifest.find("machine_id"); id != manifest.end() && id->is_string())
    checked.machine_id = id->get<std::string>();
  if (const auto created = manifest.find("created");
      created != manifest.end() && created->is_number_integer())
    checked.created = created->get<std::int64_t>();
  if (checked.machine_id && !paths_.machine_id.empty() && *checked.machine_id != paths_.machine_id)
    return std::unexpected(std::format("This backup is of machine {}; this machine is {}.",
                                       *checked.machine_id, paths_.machine_id));
  // The role of each name, from the manifest.
  std::vector<std::pair<std::string, std::string>> listed;
  for (const auto& file : manifest["files"]) {
    if (!file.is_object() || !file.contains("role") || !file["role"].is_string() ||
        !file.contains("name") || !file["name"].is_string())
      return std::unexpected(
          std::string("This backup was written by another version of CoffeeCNC, or is damaged."));
    listed.emplace_back(file["role"].get<std::string>(), file["name"].get<std::string>());
  }
  std::set<std::string> names;
  std::set<std::string> targets;
  bool ini = false;
  for (const auto& entry : *entries) {
    if (!names.insert(entry.name).second)
      return std::unexpected(std::format("{} is in the archive twice.", entry.name));
    if (entry.name == backup_manifest_name)
      continue;
    const auto role = std::ranges::find(listed, entry.name,
                                        &std::pair<std::string, std::string>::second);
    if (role == listed.end())
      return std::unexpected(std::format("{} is not in the backup's manifest.", entry.name));
    const auto refused = [&]() -> std::unexpected<std::string> {
      return std::unexpected(
          std::format("{} is not a name this backup may restore: the archive is refused.",
                      entry.name));
    };
    std::string target;
    const auto& kind = role->first;
    const auto after = [&](std::string_view head) -> std::string_view {
      return std::string_view(entry.name).substr(head.size());
    };
    if (kind == "ini" || kind == "hal") {
      if (!entry.name.starts_with("config/") || !plain_relative(after("config/")))
        return refused();
      if (kind == "ini") {
        if (ini || after("config/").find('/') != std::string_view::npos)
          return refused();
        ini = true;
        target = paths_.ini_file;
      } else {
        target = paths_.config_directory + '/' + std::string(after("config/"));
      }
    } else if (kind == "tool_table" || kind == "parameters") {
      const std::string_view head = kind == "tool_table" ? "tool-table/" : "parameters/";
      if (!entry.name.starts_with(head) || !plain_component(after(head)) ||
          after(head).size() > max_relative_bytes)
        return refused();
      target = kind == "tool_table" ? paths_.tool_table : paths_.parameter_file;
      if (target.empty())
        return std::unexpected(std::format(
            "This configuration has no {}: {} cannot be restored.",
            kind == "tool_table" ? "[EMCIO] TOOL_TABLE" : "[RS274NGC] PARAMETER_FILE",
            entry.name));
    } else if (kind == "data") {
      const auto name = entry.name.starts_with("data/") ? after("data/") : std::string_view{};
      if (std::ranges::find(data_file_names, name) == data_file_names.end())
        return refused();
      target = paths_.data_directory + '/' + std::string(name);
    } else {
      return refused();
    }
    if (!targets.insert(target).second)
      return std::unexpected(std::format("Two files of the backup go to {}.", target));
    const auto now = read_file(files_, target);
    if (!now)
      return std::unexpected(now.error());
    const auto& current = *now;
    checked.files.push_back({kind, entry.name, target, entry.bytes,
                             !current                    ? "new"
                             : *current == entry.bytes ? "same"
                                                       : "changed"});
  }
  if (!ini)
    return std::unexpected(std::string("This backup has no INI file."));
  checked.token = std::format("{:016x}", fnv(bytes, fnv(path + '\0')));
  return checked;
}

auto MachineData::check_restore(const std::string& path) -> MachineDataAnswer {
  const auto checked = check(path);
  if (!checked)
    return {CommandOutcome::rejected, checked.error(), {}, false};
  RestoreSummary summary{path, checked->token, checked->machine_id, checked->created, {}};
  for (const auto& file : checked->files)
    summary.files.push_back({file.role, file.name, file.target, file.change, file.bytes.size()});
  return {CommandOutcome::accepted, "", {encode_restore_summary(summary)}, false};
}

auto MachineData::restore(const std::string& path, const std::string& token,
                          const std::function<std::string()>& recheck) -> MachineDataAnswer {
  const auto refuse = [](std::string reason) -> MachineDataAnswer {
    return {CommandOutcome::rejected, std::move(reason), {}, false};
  };
  const auto checked = check(path);
  if (!checked)
    return refuse(checked.error());
  if (checked->token != token)
    return refuse("The backup file changed after it was checked. Check it again.");
  if (const auto reason = recheck(); !reason.empty())
    return refuse(reason);
  std::vector<const Planned*> changes;
  for (const auto& file : checked->files)
    if (file.change != "same")
      changes.push_back(&file);
  if (changes.empty())
    return {CommandOutcome::accepted, "Nothing to restore: every file is the same.", {}, false};
  const auto staging = paths_.data_directory + '/' + std::string(restore_directory_name);
  const auto failed = [](const std::string& reason) -> MachineDataAnswer {
    return {CommandOutcome::failed, "The restore was not prepared: " + reason, {}, false};
  };
  if (const auto made = files_.make_directory(staging); !made)
    return failed(made.error());
  const auto plan_file = staging + '/' + std::string(plan_name);
  files_.discard(plan_file);
  auto plan = Json::array();
  for (std::size_t i = 0; i < changes.size(); ++i) {
    const auto& file = *changes[i];
    const auto copy = std::format("{}/{}.staged", staging, i);
    if (auto written = replace_file(files_, copy, file.bytes); !written)
      return failed(written.error());
    plan.push_back({{"role", file.role}, {"name", file.name}, {"from", copy}, {"to", file.target}});
  }
  const Json document{{"version", 1},
                      {"source", path},
                      {"ini", paths_.ini_file},
                      {"files", std::move(plan)}};
  if (auto written = replace_file(files_, plan_file, dump(document) + '\n'); !written)
    return failed(written.error());
  staged_ = true;
  return {CommandOutcome::accepted, "", {}, true};
}

auto apply_staged_restore(ToolModelFiles& files, const std::string& data_directory,
                          std::chrono::milliseconds now) -> std::expected<std::size_t, std::string> {
  const auto staging = data_directory + '/' + std::string(restore_directory_name);
  const auto plan_file = staging + '/' + std::string(plan_name);
  const auto result_file = staging + '/' + std::string(result_name);
  const auto plan_content = files.read(plan_file, std::size_t{1024} * 1024);
  if (!plan_content)
    return std::unexpected(plan_content.error());
  if (!*plan_content)
    return 0;
  struct Replacement {
    std::string role;
    std::string name;
    std::string from;
    std::string to;
    std::string bytes;
    std::optional<std::string> before;
  };
  std::vector<Replacement> steps;
  std::string reason;
  std::string copy_path;
  std::size_t written = 0;
  const auto& plan_found = *plan_content;
  if (!plan_found)
    return 0;
  const auto plan = Json::parse(plan_found->bytes, nullptr, false);
  if (!plan.is_object() || !plan.contains("files") || !plan["files"].is_array())
    reason = "The prepared restore is damaged.";
  for (const auto& file : reason.empty() ? plan["files"] : Json::array()) {
    Replacement step{file.value("role", std::string{}), file.value("name", std::string{}),
              file.value("from", std::string{}), file.value("to", std::string{}), {}, {}};
    // Only copies this program staged, and only to absolute targets.
    if (!step.from.starts_with(staging + '/') || !step.to.starts_with('/')) {
      reason = "The prepared restore is damaged.";
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
    const auto before = read_file(files, step.to);
    if (!before) {
      reason = before.error();
      break;
    }
    step.bytes = *staged_bytes;
    step.before = *before;
    steps.push_back(std::move(step));
  }
  if (reason.empty()) {
    // The files as they are now, as a backup the panel can restore.
    std::vector<ArchiveEntry> entries(1);
    auto listed = Json::array();
    const auto seconds = std::chrono::floor<std::chrono::seconds>(now).count();
    // The INI file always: a backup without one is not restored.
    const auto ini = plan.value("ini", std::string{});
    if (!ini.empty() && std::ranges::none_of(steps, [&](const auto& step) { return step.to == ini; })) {
      const auto current = read_file(files, ini);
      if (const auto* bytes = current ? &*current : nullptr; bytes != nullptr && *bytes) {
        entries.push_back({"config/" + base_name(ini), **bytes, seconds});
        listed.push_back({{"role", "ini"}, {"name", "config/" + base_name(ini)}, {"path", ini}});
      }
    }
    for (const auto& step : steps)
      if (step.before) {
        entries.push_back({step.name, *step.before, seconds});
        listed.push_back({{"role", step.role}, {"name", step.name}, {"path", step.to}});
      }
    entries.front() = {std::string(backup_manifest_name),
                       dump(Json{{"format", format_name},
                                 {"version", manifest_version},
                                 {"machine_id", nullptr},
                                 {"created", now.count()},
                                 {"files", std::move(listed)},
                                 {"not_included", Json::array()}}) +
                           '\n',
                       seconds};
    copy_path = std::format("{}/pre-restore-{}.tar.gz", data_directory, stamp(now));
    const auto archive = write_archive(entries);
    const auto saved =
        archive ? replace_file(files, copy_path, *archive)
                : ToolModelFiles::Done{std::unexpected(archive.error())};
    if (!saved)
      reason = "the files before the restore could not be saved: " + saved.error();
  }
  if (reason.empty()) {
    for (; written < steps.size(); ++written) {
      const auto& step = steps[written];
      if (auto replaced = replace_file(files, step.to, step.bytes); !replaced) {
        reason = replaced.error();
        break;
      }
    }
    if (!reason.empty()) {
      // Back to what was there, newest first.
      for (auto i = written; i-- > 0;) {
        const auto& step = steps[i];
        if (!step.before)
          files.discard(step.to);
        else if (const auto back = replace_file(files, step.to, *step.before); !back)
          reason += " It could not be put back: " + back.error();
      }
      written = 0;
    }
  }
  const bool ok = reason.empty();
  const Json result{{"ok", ok},
                    {"written", written},
                    {"reason", reason},
                    {"pre_restore", copy_path},
                    {"time", now.count()}};
  const auto said = replace_file(files, result_file, dump(result) + '\n');
  for (const auto& step : steps)
    files.discard(step.from);
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
