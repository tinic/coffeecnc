#include <coffeecnc/app/backup_archive.hpp>
#include <coffeecnc/app/machine_data.hpp>
#include <coffeecnc/app/restore_helper.hpp>
#include <coffeecnc/core/command_policy.hpp>

#include "fake_tool_model_files.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace coffeecnc;
using namespace std::chrono_literals;

namespace {
using Json = nlohmann::json;

int checks = 0;
void require(bool condition, const std::string& message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

constexpr auto config = "/home/cnc/linuxcnc/configs/g0759";
constexpr auto ini = "/home/cnc/linuxcnc/configs/g0759/g0759.ini";
constexpr auto hal = "/home/cnc/linuxcnc/configs/g0759/g0759.hal";
constexpr auto custom = "/home/cnc/linuxcnc/configs/g0759/hal/custom.hal";
constexpr auto postgui = "/home/cnc/linuxcnc/configs/g0759/postgui.hal";
constexpr auto tools = "/home/cnc/linuxcnc/configs/g0759/tool.tbl";
constexpr auto var = "/home/cnc/linuxcnc/configs/g0759/g0759.var";
constexpr auto data = "/home/cnc/.local/share/coffeecnc/G0759";
constexpr auto usb = "/media/cnc/STICK";
constexpr auto now = 1795000000000ms; // 2026-11-18 11:06:40 UTC

auto in_data(std::string_view name) -> std::string {
  return std::string(data) + '/' + std::string(name);
}
auto in_staging(std::string_view name) -> std::string {
  return in_data("restore/") + std::string(name);
}

auto paths(std::string machine = "G0759") -> MachineDataPaths {
  return {std::move(machine),
          ini,
          config,
          {"g0759.hal", "hal/custom.hal", "LIB:basic_sim.tcl", "../elsewhere.hal", postgui},
          tools,
          var,
          data};
}

struct Fixture {
  FakeDisk disk;
  FakeToolModelFiles files{disk};
  std::unique_ptr<MachineData> machine;
  int pauses = 0;
  // The boot clock that staging reads; none: the system's (boot_time()).
  std::optional<BootTime> boot;

  Fixture() {
    disk.directories.insert({config, std::string(config) + "/hal", data, usb});
    disk.files[ini] = "[EMC]\nMACHINE = G0759\n[HAL]\nHALFILE = g0759.hal\n";
    disk.files[hal] = "loadrt trivkins\n";
    disk.files[custom] = "net estop-loop iocontrol.0.user-enable-out\n";
    disk.files[postgui] = "net spindle coffeecnc.spindle-rpm\n";
    disk.files[tools] = "T1 P1 Z48.2 D6 ;End mill\n";
    disk.files[var] = "5221\t10.000000\n5222\t20.000000\n";
    disk.files[in_data("tool-models.json")] = R"({"version":1})";
    disk.files[in_data("offset-history.json")] = R"({"version":1,"entries":[]})";
    open();
  }
  void open(MachineDataPaths given = paths()) {
    machine = std::make_unique<MachineData>(
        files, std::move(given), [] { return now; },
        [this](std::chrono::milliseconds) { ++pauses; },
        [this] { return boot.value_or(boot_time()); });
  }
  // The `backup_written` message of a backup to the stick.
  auto backup() -> Json {
    const auto done = machine->backup(usb);
    require(done.outcome == CommandOutcome::accepted && done.messages.size() == 1 && !done.quit,
            "The backup is written: " + done.reason);
    return Json::parse(done.messages.front());
  }
  auto check(const std::string& path) -> MachineDataAnswer { return machine->check_restore(path); }
  auto summary(const std::string& path) -> Json {
    const auto done = check(path);
    require(done.outcome == CommandOutcome::accepted && done.messages.size() == 1,
            "The archive checks: " + done.reason);
    return Json::parse(done.messages.front());
  }
  [[nodiscard]] auto change_of(const Json& summary, const std::string& name) const -> std::string {
    const auto& listed = summary["files"];
    const auto found =
        std::ranges::find_if(listed, [&](const Json& file) { return file["name"] == name; });
    return found == listed.end() ? "absent" : (*found)["change"].get<std::string>();
  }
  // Stages a restore of the backup at `path`; it must quit.
  void stage(const std::string& path) {
    const auto done =
        machine->restore(path, summary(path)["token"], 7, [] { return LocalizedReason{}; });
    require(done.outcome == CommandOutcome::accepted && done.quit && machine->staged(),
            "A restore is staged: " + done.reason);
  }
  [[nodiscard]] auto plan() const -> Json {
    return Json::parse(disk.files.at(in_staging("plan.json")));
  }
};

const auto allow = []() -> LocalizedReason { return {}; };

void archive_round_trip() {
  const std::vector<ArchiveEntry> entries{
      {"coffeecnc-backup.json", "{}\n", 1795000000},
      {"config/" + std::string(150, 'h') + "/deep.hal", "net a b\n", 1795000000},
      {"data/binary", std::string("\0\xff\x01 bytes", 9), 1795000001},
      {"empty", "", 0}};
  const auto archive = write_archive(entries);
  require(archive.has_value(), "An archive is written");
  require(archive->starts_with("\x1f\x8b"), "It is gzip-compressed");
  const auto read = read_archive(*archive);
  require(read.has_value() && *read == entries, "It reads back exactly, long names included");
  require(!read_archive("not an archive at all").has_value(), "Garbage is refused");
  require(!read_archive(archive->substr(0, archive->size() / 2)).has_value(),
          "A cut archive is refused");
  require(!write_archive({{"big", std::string(max_archive_entry_bytes + 1, 'x'), 0}}).has_value(),
          "An entry over 16 MiB is not written");
}

// An archive with a symbolic link, made with libarchive itself.
auto with_link(const std::string& name, const std::string& target) -> std::string {
  std::string out;
  archive* writer = archive_write_new();
  archive_write_set_format_pax_restricted(writer);
  archive_write_open(
      writer, &out, nullptr,
      [](archive*, void* client, const void* buffer, std::size_t length) -> la_ssize_t {
        static_cast<std::string*>(client)->append(static_cast<const char*>(buffer), length);
        return static_cast<la_ssize_t>(length);
      },
      nullptr);
  archive_entry* entry = archive_entry_new();
  archive_entry_set_pathname(entry, name.c_str());
  archive_entry_set_filetype(entry, AE_IFLNK);
  archive_entry_set_symlink(entry, target.c_str());
  archive_entry_set_perm(entry, 0777);
  archive_write_header(writer, entry);
  archive_entry_free(entry);
  archive_write_close(writer);
  archive_write_free(writer);
  return out;
}

void backup_contents() {
  Fixture f;
  const auto written = f.backup();
  const std::string path = written["path"];
  require(path == std::string(usb) + "/coffeecnc-G0759-20261118-110640.tar.gz",
          "The archive is named by machine and UTC time: " + path);
  require(written["files"] == 8, std::format("Eight files are in it: {}", written["files"].dump()));
  require(written["not_included"].size() == 2 &&
              written["not_included"][0]["path"] == "LIB:basic_sim.tcl" &&
              written["not_included"][1]["path"] == "../elsewhere.hal" &&
              written["not_included"][0]["localization"]["key"] == "backend.machineData.libHal" &&
              written["not_included"][1]["localization"]["key"] ==
                  "backend.machineData.outsideConfig",
          "HAL files outside the configuration folder are listed as not included");
  const auto entries = read_archive(f.disk.files.at(path));
  require(entries.has_value(), "The archive reads");
  std::map<std::string, std::string> by_name;
  for (const auto& entry : *entries)
    by_name[entry.name] = entry.bytes;
  require(by_name.at("config/g0759.ini") == f.disk.files[ini] &&
              by_name.at("config/hal/custom.hal") == f.disk.files[custom] &&
              by_name.at("config/postgui.hal") == f.disk.files[postgui] &&
              by_name.at("tool-table/tool.tbl") == f.disk.files[tools] &&
              by_name.at("parameters/g0759.var") == f.disk.files[var] &&
              by_name.contains("data/tool-models.json") &&
              by_name.contains("data/offset-history.json") &&
              !by_name.contains("data/alarm-log.jsonl"),
          "INI, HAL, tool table, .var and the data files that exist are in it");
  const auto manifest = Json::parse(by_name.at("coffeecnc-backup.json"));
  require(manifest["format"] == "coffeecnc-backup" && manifest["version"] == 1 &&
              manifest["machine_id"] == "G0759" && manifest["files"].size() == 8,
          "The manifest lists every file");
  require(!manifest["not_included"][0].contains("localization") &&
              manifest["not_included"][0]["reason"] == written["not_included"][0]["reason"],
          "The archive retains the plain migration-compatible omission reason");
  require(f.disk.files.at(path).size() == written["bytes"].get<std::size_t>(), "Its size is said");
  require(!f.disk.files.contains(path + ".coffeecnc-tmp"), "No temporary file is left");
  // A folder that cannot be written: failed, nothing left.
  f.files.failures["rename"] = 1;
  const auto failed = f.machine->backup(usb);
  require(failed.outcome == CommandOutcome::failed && failed.messages.empty() &&
              failed.localization && failed.localization->key == "backend.machineData.notWritten",

          "A backup that cannot be written fails");
}

// Review #274, 10: a symbolic link of a configuration file made the backup fail. It is backed
// up by its content, the manifest says it was a link, and the summary says the restore
// replaces the link by the file.
void symlinked_configuration() {
  Fixture f;
  const std::string shared = "/home/cnc/shared/g0759.ini";
  f.disk.files[shared] = f.disk.files[ini];
  f.disk.files.erase(ini);
  f.disk.links[ini] = shared;
  f.disk.files["/home/cnc/shared/tool.tbl"] = f.disk.files[tools];
  f.disk.files.erase(tools);
  f.disk.links[tools] = "/home/cnc/shared/tool.tbl";
  const auto written = f.backup();
  const std::string path = written["path"];
  const auto entries = read_archive(f.disk.files.at(path));
  require(entries && std::ranges::any_of(*entries,
                                         [&](const ArchiveEntry& entry) {
                                           return entry.name == "config/g0759.ini" &&
                                                  entry.bytes == f.disk.files[shared];
                                         }),
          "A linked INI file is backed up by its content");
  const auto manifest = Json::parse(entries->front().bytes);
  const auto listed = std::ranges::find_if(
      manifest["files"], [](const Json& file) { return file["name"] == "tool-table/tool.tbl"; });
  require(listed != manifest["files"].end() && (*listed)["symlink"] == true,
          "The manifest says the tool table was a link");
  const auto summary = f.summary(path);
  for (const auto& file : summary["files"])
    require(file["symlink"] == (file["target"] == ini || file["target"] == tools),
            "The summary says which targets are links: " + file.dump());
  // The restore replaces the link; where it pointed is not written.
  f.disk.files["/home/cnc/shared/tool.tbl"] = "T1 P1 Z99\n";
  f.stage(path);
  require(apply_staged_restore(f.files, data, now + 60s).has_value(), "The helper writes");
  require(!f.disk.links.contains(tools) && f.disk.files[tools] == "T1 P1 Z48.2 D6 ;End mill\n" &&
              f.disk.files["/home/cnc/shared/tool.tbl"] == "T1 P1 Z99\n",
          "The link is replaced by the file; its target is left as it is");
}

void check_and_changes() {
  Fixture f;
  const std::string path = f.backup()["path"];
  auto summary = f.summary(path);
  require(summary["machine_id"] == "G0759" && summary["created"] == now.count() &&
              summary["files"].size() == 8 && summary["token"].get<std::string>().size() == 16,
          "The summary has the machine, the time, every file and a token");
  for (const auto& file : summary["files"])
    require(file["change"] ==
                (file["role"] == "parameters" || file["role"] == "data" ? "at_exit" : "same"),
            "Right after the backup every file is the same; the .var and the data files are "
            "compared at exit");
  f.disk.files[tools] = "T1 P1 Z50 D6 ;End mill\n";
  f.disk.files.erase(custom);
  summary = f.summary(path);
  require(f.change_of(summary, "tool-table/tool.tbl") == "changed" &&
              f.change_of(summary, "config/hal/custom.hal") == "new" &&
              f.change_of(summary, "config/g0759.ini") == "same",
          "A changed and a missing file are told apart");
  for (const auto& file : summary["files"])
    if (file["name"] == "tool-table/tool.tbl")
      require(file["target"] == tools, "The target is this machine's tool table");
  // Another machine's backup.
  f.open(paths("BENCH"));
  const auto other = f.check(path);
  require(other.outcome == CommandOutcome::rejected &&
              other.reason == "This backup is of machine G0759; this machine is BENCH." &&
              other.localization == MessageKey{"backend.machineData.otherMachine",
                                               {{"backup", "G0759"}, {"machine", "BENCH"}}},
          "Another machine's backup is refused: " + other.reason);
  // No data folder: nothing to stage in.
  auto bare = paths();
  bare.data_directory.clear();
  f.open(bare);
  require(f.check(path).outcome == CommandOutcome::rejected, "A restore needs the data folder");
  // No tool table configured here.
  auto without = paths();
  without.tool_table.clear();
  f.open(without);
  require(f.check(path).reason.find("TOOL_TABLE") != std::string::npos,
          "A tool table with nowhere to go is refused");
}

// A crafted archive: a manifest of `machine` that lists `name` with `role`, that entry, and an
// INI file with `ini_text`.
auto crafted(const std::string& role, const std::string& name,
             const std::string& ini_text = "[EMC]\n", const Json& machine = Json("G0759"))
    -> std::string {
  const Json manifest{
      {"format", "coffeecnc-backup"},
      {"version", 1},
      {"machine_id", machine},
      {"created", 1},
      {"files",
       {{{"role", "ini"}, {"name", "config/g0759.ini"}}, {{"role", role}, {"name", name}}}}};
  return *write_archive({{"coffeecnc-backup.json", manifest.dump(), 0},
                         {"config/g0759.ini", ini_text, 0},
                         {name, "owned\n", 0}});
}

void path_traversal() {
  Fixture f;
  const auto before = f.disk.files;
  const std::string path = std::string(usb) + "/evil.tar.gz";
  for (const auto& [role, name] : std::vector<std::pair<std::string, std::string>>{
           {"hal", "config/../../../../etc/passwd"},
           {"hal", "config/hal/../../escape.hal"},
           {"hal", "/etc/passwd"},
           {"hal", "config//double.hal"},
           {"hal", "config/./dot.hal"},
           {"hal", "config/"},
           {"data", "data/../../../.bashrc"},
           {"data", "data/evil.json"},
           {"tool_table", "tool-table/../../../etc/shadow"},
           {"parameters", "parameters/sub/g0759.var"},
           {"ini", "config/other.ini"},
           {"anything", "config/x.hal"}}) {
    f.disk.files[path] = crafted(role, name);
    const auto done = f.check(path);
    require(done.outcome == CommandOutcome::rejected && done.messages.empty(),
            std::format("Refused: {} {}", role, name));
    const auto restored = f.machine->restore(path, "0000000000000000", 1, allow);
    require(restored.outcome == CommandOutcome::rejected && !restored.quit,
            std::format("Not restored: {} {}", role, name));
  }
  // An entry that is not in the manifest.
  f.disk.files[path] = *write_archive(
      {{"coffeecnc-backup.json",
        R"({"format":"coffeecnc-backup","version":1,"machine_id":"G0759","files":[{"role":"ini","name":"config/g0759.ini"}]})",
        0},
       {"config/g0759.ini", "x", 0},
       {"config/unlisted.hal", "x", 0}});
  require(f.check(path).outcome == CommandOutcome::rejected, "An unlisted entry is refused");
  // A symbolic link, and a name twice.
  f.disk.files[path] = with_link("config/g0759.hal", "/etc/passwd");
  require(f.check(path).reason.find("not a regular file") != std::string::npos,
          "A symbolic link is refused: " + f.check(path).reason);
  f.disk.files[path] =
      *write_archive({{"coffeecnc-backup.json", "{}", 0}, {"a", "1", 0}, {"a", "2", 0}});
  require(f.check(path).outcome == CommandOutcome::rejected, "A name twice is refused");
  f.disk.files.erase(path);
  require(f.disk.files == before, "Nothing was written by any of them");
}

// Review #274, 4: the hal role took any plain path below the configuration folder (scripts, the
// .var, the tool table), and a symbolically linked subfolder led out of it.
void hal_role_is_bounded() {
  Fixture f;
  const std::string path = std::string(usb) + "/evil.tar.gz";
  const auto refused = [&](const std::string& archive, const std::string& what) {
    f.disk.files[path] = archive;
    const auto done = f.check(path);
    require(done.outcome == CommandOutcome::rejected, "Refused: " + what);
    return done.reason;
  };
  // Named by neither INI file.
  auto reason = refused(crafted("hal", "config/scripts/run.hal"), "a HAL file no INI names");
  require(reason == "config/scripts/run.hal is not a HAL file the INI file names: the archive is "
                    "refused.",
          "It says why: " + reason);
  // Named by the archive's INI, but the .var file, the tool table, a script.
  for (const auto* name : {"g0759.var", "tool.tbl", "start.sh"})
    refused(
        crafted("hal", std::format("config/{}", name), std::format("[HAL]\nHALFILE = {}\n", name)),
        std::format("the archive's INI names {} as a HAL file", name));
  // Named by the archive's INI, a HAL file: allowed, with its arguments cut off.
  f.disk.files[path] =
      crafted("hal", "config/extra.tcl", "[HAL]\n# a comment\nHALFILE = extra.tcl -arg 1\n");
  require(f.check(path).outcome == CommandOutcome::accepted,
          "A HAL file the archive's INI names is restored: " + f.check(path).reason);
  // This machine's own HAL file, but its subfolder is a symbolic link out of the folder.
  const std::string backup = f.backup()["path"];
  f.disk.links[std::string(config) + "/hal"] = "/etc/coffeecnc-outside";
  f.disk.files["/etc/coffeecnc-outside/custom.hal"] = "outside\n";
  f.disk.files[path] = f.disk.files[backup];
  reason = refused(f.disk.files[path], "a HAL file reached through a linked subfolder");
  require(reason.find("symbolic link") != std::string::npos &&
              reason.find("custom.hal") != std::string::npos,
          "It says the link: " + reason);
  // Linked after the restore was prepared: the helper does not write through it either.
  f.disk.links.erase(std::string(config) + "/hal");
  f.disk.files.erase(custom);
  f.stage(backup);
  f.disk.links[std::string(config) + "/hal"] = "/etc/coffeecnc-outside";
  require(!apply_staged_restore(f.files, data, now + 60s).has_value(), "The helper refuses");
  require(f.disk.files["/etc/coffeecnc-outside/custom.hal"] == "outside\n" &&
              !f.disk.files.contains("/etc/coffeecnc-outside/custom.hal.coffeecnc-tmp"),
          "Nothing is written outside the configuration folder");
}

// Review #274, 5: a backup without a machine ID restored onto any machine, and the pre-restore
// copy was written without one.
void machine_id_required() {
  Fixture f;
  const std::string path = std::string(usb) + "/nobody.tar.gz";
  f.disk.files[path] = crafted("data", "data/tool-models.json", "[EMC]\n", Json(nullptr));
  const auto done = f.check(path);
  require(done.outcome == CommandOutcome::rejected &&
              done.reason == "This backup does not name its machine; this machine is G0759. It "
                             "is not restored.",
          "A backup without a machine ID is refused on a machine with one: " + done.reason);
  // The pre-restore copy names this machine, so it restores here and nowhere else.
  const std::string backup = f.backup()["path"];
  f.disk.files[tools] = "changed\n";
  f.stage(backup);
  require(f.plan()["machine_id"] == "G0759", "The plan names the machine");
  const auto applied = apply_staged_restore(f.files, data, now + 60s);
  require(applied.has_value(), "Applied");
  const auto copy = in_data("pre-restore-20261118-110740.tar.gz");
  const auto saved = read_archive(f.disk.files.at(copy));
  require(saved && Json::parse(saved->front().bytes)["machine_id"] == "G0759",
          "The pre-restore copy names this machine");
  f.open(paths("BENCH"));
  require(f.check(copy).outcome == CommandOutcome::rejected, "and is refused on another one");
  // A plan of another machine's data folder is refused by the helper.
  f.open();
  f.disk.files[tools] = "changed again\n";
  f.stage(backup);
  auto plan = f.plan();
  plan["machine_id"] = "BENCH";
  f.disk.files[in_staging("plan.json")] = plan.dump();
  require(!apply_staged_restore(f.files, data, now + 120s).has_value() &&
              f.disk.files[tools] == "changed again\n",
          "A plan of another machine is not written");
}

void refusal_states() {
  CommandPolicy policy{Limits{1500, {1500, 1500, 750}, 2.0, 0.5, 1.0, 1500, false}};
  ControllerState s;
  s.link = Link::live;
  s.machine = MachineState::estop;
  const RestoreBackup restore{"/media/cnc/STICK/b.tar.gz", "0123456789abcdef"};
  const CheckRestore check{"/media/cnc/STICK/b.tar.gz"};
  require(policy.evaluate(restore, s).allowed && policy.evaluate(check, s).allowed,
          "In E-stop, idle: allowed");
  s.machine = MachineState::off;
  s.program_loaded = true;
  require(policy.evaluate(restore, s).allowed, "Off with an idle program loaded: allowed");
  s.machine = MachineState::on;
  auto decision = policy.evaluate(restore, s);
  require(!decision.allowed &&
              decision.reason == "Switch the machine off or press E-stop before a restore.",
          "Machine on: refused");
  require(!policy.evaluate(check, s).allowed, "Machine on: the check is refused too");
  s.machine = MachineState::off;
  for (const auto interp : {InterpState::reading, InterpState::paused, InterpState::waiting}) {
    s.interp = interp;
    decision = policy.evaluate(restore, s);
    require(!decision.allowed && decision.reason == "Stop the program before a restore.",
            "A running or paused program: refused");
  }
  s.interp = InterpState::idle;
  s.link = Link::stale;
  require(!policy.evaluate(restore, s).allowed, "Without live status: refused");
  require(policy.evaluate(Backup{"/media/cnc/STICK"}, s).allowed,
          "A backup needs no machine state");
  require(!policy.evaluate(RestoreBackup{"relative.tar.gz", ""}, ControllerState{}).allowed,
          "A relative path is refused");
  // The machine data's own recheck and token.
  Fixture f;
  const std::string path = f.backup()["path"];
  f.disk.files[tools] = "changed\n";
  const std::string token = f.summary(path)["token"];
  auto done = f.machine->restore(path, token, 1, [] {
    return LocalizedReason{"Switch the machine off or press E-stop before a restore.",
                           MessageKey{"messages.policy.restoreMachineOn"}};
  });
  require(done.outcome == CommandOutcome::rejected &&
              done.reason == "Switch the machine off or press E-stop before a restore." &&
              done.localization == MessageKey{"messages.policy.restoreMachineOn"} &&
              !f.machine->staged(),
          "The recheck just before staging refuses");
  f.disk.files[path] += "x"; // the file changed after the check
  done = f.machine->restore(path, token, 1, allow);
  require(done.outcome == CommandOutcome::rejected && !f.machine->staged(),
          "A file that changed after its check is refused");
}

void restore_and_apply() {
  Fixture f;
  const std::string path = f.backup()["path"];
  const auto original_tools = f.disk.files[tools];
  const auto original_var = f.disk.files[var];
  f.disk.files[tools] = "T1 P1 Z99 D6\n";
  f.disk.files[var] = "5221\t0\n";
  f.disk.files.erase(custom);
  f.disk.file_modes[tools] = 0640;
  f.disk.groups[tools] = 27;
  f.stage(path);
  require(f.disk.files[tools] == "T1 P1 Z99 D6\n", "Nothing is written while LinuxCNC runs");
  require(f.plan()["files"].size() == 5,
          "The three files that change are staged, and the two data files, decided at exit");
  // LinuxCNC writes its .var as it stops: the helper comes after that.
  f.disk.files[var] = "5221\t0\n5222\t0\n";
  const auto applied = apply_staged_restore(f.files, data, now + 60s);
  require(applied && *applied == 3, "The helper writes three files");
  require(f.disk.files[tools] == original_tools && f.disk.files[var] == original_var &&
              f.disk.files.contains(custom),
          "The tool table, the .var and the HAL file are the backup's");
  require(f.disk.file_modes[tools] == 0640 && f.disk.groups[tools] == 27 &&
              f.disk.file_modes[custom] == 0644,
          "Review #274, 10: a replaced file keeps its mode and group; a new one is 0644");
  const auto copy = in_data("pre-restore-20261118-110740.tar.gz");
  const auto saved = read_archive(f.disk.files.at(copy));
  require(saved && saved->size() == 4,
          "The files before the restore are kept as a backup, with the INI file");
  require(std::ranges::any_of(*saved,
                              [](const ArchiveEntry& e) {
                                return e.name == "parameters/g0759.var" &&
                                       e.bytes == "5221\t0\n5222\t0\n";
                              }),
          "The pre-restore copy has the .var as LinuxCNC left it");
  require(!f.disk.files.contains(in_staging("plan.json")) &&
              !f.disk.files.contains(in_staging("0.staged")) &&
              !f.disk.files.contains(in_staging("applying.json")),
          "The plan, the marker and the staged copies are removed");
  require(apply_staged_restore(f.files, data, now).value_or(99) == 0, "A second run does nothing");
  f.open();
  const auto notice = f.machine->take_notice();
  require(notice && !notice->error &&
              notice->localization ==
                  MessageKey{"backend.machineData.restored", {{"count", "3"}, {"path", copy}}} &&
              notice->text.find("3 files were written") != std::string::npos &&
              notice->text.find(copy) != std::string::npos,
          "The next run says what was restored");
  require(!f.machine->take_notice(), "Once");
  // The pre-restore copy is a backup the panel can restore.
  const auto back = f.summary(copy);
  require(back["files"].size() == 3, "The copy checks (the HAL file was new, so it is not in it)");
  // Without a .var file and data files, nothing that changes: nothing staged, no quit.
  auto bare = paths();
  bare.parameter_file.clear();
  f.disk.files.erase(in_data("tool-models.json"));
  f.disk.files.erase(in_data("offset-history.json"));
  f.open(bare);
  const std::string again = f.backup()["path"];
  const auto same = f.machine->restore(again, f.summary(again)["token"], 1, allow);
  require(same.outcome == CommandOutcome::accepted && !same.quit && !f.machine->staged() &&
              same.localization == MessageKey{"backend.machineData.nothing"},
          "A restore that changes nothing does not quit");
}

// Review #274, 2: a .var file that was the same when the restore was prepared was not staged,
// and LinuxCNC writes its offsets from memory as it stops: the backup's offsets were lost.
void var_decided_after_exit() {
  Fixture f;
  const std::string path = f.backup()["path"];
  const auto backed_up = f.disk.files[var];
  f.stage(path); // every file is the same now
  const auto plan = f.plan();
  require(plan["files"].size() == 3 && plan["files"][0]["role"] == "parameters" &&
              plan["files"][1]["role"] == "data" && plan["files"][2]["role"] == "data",
          "The .var file and the data files are staged although they are the same now");
  f.disk.files[var] = "5221\t99\n"; // what LinuxCNC held in memory
  const auto applied = apply_staged_restore(f.files, data, now + 60s);
  require(applied && *applied == 1 && f.disk.files[var] == backed_up,
          "The helper writes the backup's .var after LinuxCNC has written its own");
  // And not when LinuxCNC left it as the backup has it.
  f.open();
  f.stage(path);
  const auto untouched = apply_staged_restore(f.files, data, now + 120s);
  require(untouched && *untouched == 0, "A .var that is the backup's at exit is not written");
  f.open();
  const auto notice = f.machine->take_notice();
  require(notice && notice->text == "The backup was restored: every file already held what "
                                    "the backup holds.",
          "and the next start says so: " + (notice ? notice->text : std::string{}));
}

// Review #274, 3: a restore whose quit was not delivered stayed staged, and the next normal exit
// wrote it from old data. The plan goes; a target changed after the plan is not written.
void abandoned_and_changed_targets() {
  Fixture f;
  const std::string path = f.backup()["path"];
  f.disk.files[tools] = "now tools\n";
  f.stage(path);
  f.machine->abandon_restore();
  require(!f.machine->staged() && !f.disk.files.contains(in_staging("plan.json")) &&
              !f.disk.files.contains(in_staging("0.staged")),
          "An abandoned restore leaves no plan and no copy");
  require(apply_staged_restore(f.files, data, now + 60s).value_or(99) == 0 &&
              f.disk.files[tools] == "now tools\n",
          "A later exit writes nothing");
  // A target the operator changed after the plan: refused, nothing written.
  f.stage(path);
  f.disk.files[tools] = "edited after the check\n";
  f.disk.files[var] = "LinuxCNC's own\n"; // the .var may change
  const auto applied = apply_staged_restore(f.files, data, now + 60s);
  require(!applied && applied.error() == std::string(tools) +
                                             " changed after the restore was prepared: nothing "
                                             "was written.",
          "The helper refuses a changed target: " + (applied ? std::string{} : applied.error()));
  require(f.disk.files[tools] == "edited after the check\n" &&
              f.disk.files[var] == "LinuxCNC's own\n",
          "Nothing is written");
}

void partial_write_failure() {
  Fixture f;
  const std::string path = f.backup()["path"];
  f.disk.files[tools] = "now tools\n";
  f.disk.files[var] = "now var\n";
  f.disk.files.erase(custom);
  f.stage(path);
  const auto plan = f.plan();
  // The second file of the plan cannot be put in place: the disk is full.
  const auto second = plan["files"][1]["root"].get<std::string>() + '/' +
                      plan["files"][1]["relative"].get<std::string>();
  f.files.failures["replace " + second] = 1;
  const auto applied = apply_staged_restore(f.files, data, now);
  require(!applied && applied.error().find(second) != std::string::npos,
          "The helper fails, naming the file");
  require(f.disk.files[tools] == "now tools\n" && f.disk.files[var] == "now var\n" &&
              !f.disk.files.contains(custom),
          "The file written before it is put back; a new one is removed");
  require(!f.disk.files.contains(in_staging("plan.json")), "The plan is not tried again");
  f.open();
  const auto notice = f.machine->take_notice();
  require(notice && notice->error && notice->text.starts_with("The restore was not completed:") &&
              notice->text.ends_with("The files are as they were."),
          "The next run says it failed");
  // A plan the helper never got to.
  f.stage(path);
  f.open();
  const auto left = f.machine->take_notice();
  require(left && left->text.starts_with("A restore was prepared but not written"),
          "A plan left at the next start is reported and discarded");
  require(!f.disk.files.contains(in_staging("plan.json")) &&
              !f.disk.files.contains(in_staging("0.staged")),
          "It is discarded, with its copies");
}

// Review #274, 8: a rename that worked, followed by a failed directory fsync, was not counted,
// so the rollback skipped that file; and the notice said "as they were" when a file could not
// be put back.
void rollback_counts_renamed_files() {
  Fixture f;
  const std::string path = f.backup()["path"];
  f.disk.files[tools] = "now tools\n";
  f.disk.files[var] = "now var\n";
  f.stage(path);
  const auto plan = f.plan();
  const auto target = [&](std::size_t i) {
    return plan["files"][i]["root"].get<std::string>() + '/' +
           plan["files"][i]["relative"].get<std::string>();
  };
  // The first is replaced, but its folder cannot be synchronised.
  f.files.failures["replace_sync " + target(0)] = 1;
  require(!apply_staged_restore(f.files, data, now).has_value(), "The helper fails");
  require(f.disk.files[tools] == "now tools\n" && f.disk.files[var] == "now var\n",
          "The renamed file is put back too");
  f.open();
  auto notice = f.machine->take_notice();
  require(notice && notice->text.ends_with("The files are as they were."),
          "Every file was put back: the notice says so");
  // The second fails, and the first cannot be put back.
  f.stage(path);
  bool second_failed = false;
  f.files.during = [&](std::string_view step) {
    if (step == "replace" && !second_failed && f.files.calls.back() == "replace " + target(1)) {
      second_failed = true;
      f.files.failures["replace " + target(1)] = 1;
      f.files.failures["replace " + target(0)] = 1; // the put-back
    }
  };
  require(!apply_staged_restore(f.files, data, now + 60s).has_value(), "The helper fails again");
  f.files.during = nullptr;
  f.open();
  notice = f.machine->take_notice();
  const auto copy = in_data("pre-restore-20261118-110740.tar.gz");
  require(notice && notice->error &&
              notice->text.find("These files could not be put back: " + target(0)) !=
                  std::string::npos &&
              notice->text.find(copy) != std::string::npos &&
              notice->text.find("as they were") == std::string::npos,
          "The notice names what was not put back and the copy: " +
              (notice ? notice->text : std::string{}));
}

struct Crash {};

// Review #274, 1: a crash or a power loss in the apply loop left mixed files, and the next
// start said nothing was changed. Every file system call of the helper is a crash point.
void crash_points() {
  int tested = 0;
  for (int crash_at = 1;; ++crash_at) {
    Fixture f;
    const std::string path = f.backup()["path"];
    f.disk.files[tools] = "now tools\n";
    f.disk.files[var] = "now var\n";
    f.disk.files.erase(custom);
    f.stage(path);
    const std::map<std::string, std::optional<std::string>> before{
        {tools, f.disk.files[tools]}, {var, f.disk.files[var]}, {custom, std::nullopt}};
    int calls = 0;
    bool crashed = false;
    f.files.during = [&](std::string_view) {
      if (++calls == crash_at) {
        crashed = true;
        throw Crash{};
      }
    };
    try {
      const auto applied = apply_staged_restore(f.files, data, now + 60s);
      require(applied.has_value(), "The helper writes when nothing stops it");
    } catch (const Crash&) {
      require(crashed, "The crash is this test's");
    }
    f.files.during = nullptr;
    if (!crashed)
      break;
    ++tested;
    const bool changed = std::ranges::any_of(before, [&](const auto& file) {
      const auto found = f.disk.files.find(file.first);
      const auto now_held = found == f.disk.files.end() ? std::optional<std::string>{}
                                                        : std::optional<std::string>{found->second};
      return now_held != file.second;
    });
    f.open(); // the next start
    const auto notice = f.machine->take_notice();
    const auto at = std::format("crash at call {}", crash_at);
    require(notice.has_value(), at + ": the next start says what came of it");
    require(!changed || notice->text.find("Nothing was changed") == std::string::npos,
            at + ": files changed, and the notice does not say otherwise: " + notice->text);
    if (notice->text.starts_with("A restore was interrupted")) {
      const auto copy = in_data("pre-restore-20261118-110740.tar.gz");
      require(notice->text.find(copy) != std::string::npos && f.disk.files.contains(copy),
              at + ": the notice names the copy from before the restore");
      require(f.summary(copy)["files"].size() >= 1, at + ": and it can be restored");
    }
    require(!f.disk.files.contains(in_staging("plan.json")) &&
                !f.disk.files.contains(in_staging("applying.json")),
            at + ": nothing is left for a later start");
  }
  require(tested > 10, std::format("Every call is a crash point: {}", tested));
}

// Review #274, 9: the start-up check read and removed the helper's files without the data
// folder's lock.
void start_check_under_lock() {
  Fixture f;
  const std::string path = f.backup()["path"];
  f.disk.files[tools] = "now\n";
  f.stage(path);
  FakeToolModelFiles helper{f.disk};
  require(helper.lock(data).has_value(), "A helper holds the lock");
  f.pauses = 0;
  f.open();
  const auto notice = f.machine->take_notice();
  require(f.pauses > 0 && notice &&
              notice->text == "The data folder is in use: what came of the last restore is "
                              "shown at the next start.",
          "The start waits for the lock, then says it is in use");
  require(f.disk.files.contains(in_staging("plan.json")), "and leaves the helper's files alone");
  helper.unlock();
  f.open();
  require(f.machine->take_notice().has_value() && !f.disk.files.contains(in_staging("plan.json")),
          "With the lock it reads and removes them");
  require(!f.disk.locked.contains(data), "and releases the lock");
}

class FakeHost final : public RestoreHost {
public:
  std::function<bool(long)> parent_alive = [](long) { return false; };
  std::function<std::optional<std::string>()> running = [] { return std::optional<std::string>{}; };
  std::chrono::milliseconds clock{};
  bool lock_file = false; // /tmp/linuxcnc.lock exists
  bool lock_ours = false; // and this helper made it
  int locks_taken = 0;
  auto alive(long pid) -> bool override { return parent_alive(pid); }
  auto linuxcnc_running() -> std::optional<std::string> override { return running(); }
  auto take_linuxcnc_lock() -> std::optional<std::string> override {
    if (lock_file)
      return std::string("/tmp/linuxcnc.lock cannot be made: File exists");
    lock_file = lock_ours = true;
    ++locks_taken;
    return std::nullopt;
  }
  auto holds_linuxcnc_lock() -> bool override { return lock_file && lock_ours; }
  void release_linuxcnc_lock() override {
    if (holds_linuxcnc_lock())
      lock_file = false;
    lock_ours = false;
  }
  void pause(std::chrono::milliseconds period) override { clock += period; }
  auto elapsed() -> std::chrono::milliseconds override { return clock; }
  auto now() -> std::chrono::milliseconds override { return 1795000000000ms + clock; }
  BootTime boot = coffeecnc::boot_time(); // when the helper started
  auto boot_time() -> BootTime override { return {boot.boot_id, boot.since_boot + clock}; }
};

// Review #274, 6: the helper waited only for its parent, not at all when that was PID 1, and
// wrote while a LinuxCNC that started meanwhile read the files.
void helper_waits_for_linuxcnc() {
  Fixture f;
  const std::string path = f.backup()["path"];
  f.disk.files[tools] = "now\n";
  f.stage(path);
  FakeHost host;
  auto outcome = run_restore_helper(f.files, data, 1, host);
  require(outcome.code == 1 && outcome.line.starts_with("restore=not_written reason=the parent "
                                                        "process is 1"),
          "A parent of PID 1 is refused: " + outcome.line);
  require(f.disk.files[tools] == "now\n" && f.disk.files.contains(in_staging("plan.json")),
          "Nothing is written; the plan is left for the next start to report");
  // The parent is gone, but LinuxCNC still runs: it waits, then gives up.
  host.running = [] { return std::optional<std::string>{"milltask (pid 4242) runs"}; };
  outcome = run_restore_helper(f.files, data, 4000, host);
  require(outcome.code == 1 && host.clock > restore_helper_wait &&
              outcome.line == "restore=not_written reason=LinuxCNC did not stop (milltask (pid "
                              "4242) runs)",
          "While LinuxCNC runs nothing is written: " + outcome.line);
  require(f.disk.files[tools] == "now\n", "Nothing is written");
  // LinuxCNC stops a little later.
  host.clock = {};
  host.running = [&host] {
    return host.clock < 1s ? std::optional<std::string>{"/tmp/linuxcnc.lock exists"}
                           : std::optional<std::string>{};
  };
  bool asked_parent = false;
  host.parent_alive = [&](long pid) {
    asked_parent = pid == 4000;
    return host.clock < 400ms;
  };
  // and starts again while the copy from before the restore is written.
  bool restarted = false;
  f.files.during = [&](std::string_view step) {
    if (step == "open_temporary" && f.files.calls.back().find("pre-restore") != std::string::npos)
      restarted = true;
  };
  const auto running_after = host.running;
  host.running = [&] {
    return restarted ? std::optional<std::string>{"rtapi_app (pid 77) runs"} : running_after();
  };
  outcome = run_restore_helper(f.files, data, 4000, host);
  f.files.during = nullptr;
  require(asked_parent && outcome.code == 1 &&
              outcome.line.find("LinuxCNC runs again") != std::string::npos &&
              f.disk.files[tools] == "now\n",
          "A LinuxCNC that starts meanwhile stops the write: " + outcome.line);
  require(!f.disk.locked.contains(data), "The helper releases the lock");
  // And with LinuxCNC stopped for good it writes.
  f.open();
  f.stage(path);
  host.running = [] { return std::optional<std::string>{}; };
  outcome = run_restore_helper(f.files, data, 4000, host);
  require(outcome.code == 0 && outcome.line == "restore=written files=1",
          "It writes once LinuxCNC has stopped: " + outcome.line);
}

// Re-review #274, 1: the window drops a close when the page changed after the quit was decided,
// and a later normal exit wrote the old plan; the .var is not compared, so the backup's offsets
// replaced newer ones. The plan says when it was staged and for which page; the helper refuses
// one staged long before the run ended.
void stale_plan_refused() {
  Fixture f;
  f.boot = BootTime{"boot-1", 1h};
  f.open();
  const std::string path = f.backup()["path"];
  f.disk.files[tools] = "now tools\n";
  f.stage(path);
  require(f.plan()["staged"] == now.count() && f.plan()["page"] == 7 &&
              f.plan()["boot_id"] == "boot-1" && f.plan()["booted"] == 3600000,
          "The plan has the time it was staged, on both clocks, and the page that asked");
  f.disk.files[var] = "5221\t42\n"; // newer offsets, written at the late exit
  RestoreHooks hooks;
  hooks.ended = BootTime{"boot-1", 1h + 3min + 1s};
  const auto late = apply_staged_restore(f.files, data, now + 3min + 1s, hooks);
  require(!late && late.error() == "The restore was prepared 181 s before CoffeeCNC ended, more "
                                   "than 120 s: nothing was written.",
          "A plan staged 3 minutes before the exit is refused: " +
              (late ? std::string{} : late.error()));
  require(f.disk.files[tools] == "now tools\n" && f.disk.files[var] == "5221\t42\n",
          "Nothing is written");
  require(!f.disk.files.contains(in_staging("plan.json")), "and the plan is not tried again");
  f.open();
  const auto notice = f.machine->take_notice();
  require(notice && notice->error && notice->text.find("more than 120 s") != std::string::npos &&
              notice->text.ends_with("The files are as they were."),
          "The next start says so: " + (notice ? notice->text : std::string{}));
  // The run ended at once, and LinuxCNC took long to stop: the end of the run counts.
  f.stage(path);
  hooks.ended = BootTime{"boot-1", 1h + 5s};
  const auto slow = apply_staged_restore(f.files, data, now + 4min, hooks);
  require(slow && *slow >= 1 && f.disk.files[tools] == "T1 P1 Z48.2 D6 ;End mill\n",
          "A plan of the run that just ended is written although LinuxCNC took 4 minutes");
  // A plan staged after the end of the run is as old.
  f.open();
  f.stage(path);
  hooks.ended = BootTime{"boot-1", 1h - 10min};
  require(!apply_staged_restore(f.files, data, now, hooks).has_value(),
          "A plan staged after the end of the run is refused");
  // Review of a2865e8, 2: NTP steps the system clock between staging and the helper. Only the
  // boot clock counts: 30 s on it is written, whichever way the system clock went.
  for (const auto stepped : {now + 3h, now - 1h}) {
    f.open();
    f.disk.files[tools] = "now tools\n";
    f.stage(path);
    hooks.ended = BootTime{"boot-1", 1h + 30s};
    const auto applied = apply_staged_restore(f.files, data, stepped, hooks);
    require(applied && f.disk.files[tools] == "T1 P1 Z48.2 D6 ;End mill\n",
            "A system clock stepped by NTP does not refuse the restore: " +
                (applied ? std::string{} : applied.error()));
  }
  // Another boot: the plan is of a run before it, however its clock reads.
  f.open();
  f.disk.files[tools] = "now tools\n";
  f.stage(path);
  hooks.ended = BootTime{"boot-2", 1h + 5s};
  const auto rebooted = apply_staged_restore(f.files, data, now + 5s, hooks);
  require(!rebooted && rebooted.error() == "The restore was prepared before the computer was "
                                           "started again: nothing was written.",
          "A plan of another boot is refused: " +
              (rebooted ? std::string{} : rebooted.error()));
  require(f.disk.files[tools] == "now tools\n", "Nothing is written");
  // The helper takes its own start as the end of the run.
  f.open();
  f.stage(path);
  FakeHost host;
  host.boot = BootTime{"boot-1", 1h};
  host.clock = 3min; // started 3 minutes after the plan
  const auto outcome = run_restore_helper(f.files, data, 4000, host);
  require(outcome.code == 1 && outcome.line.find("more than 120 s") != std::string::npos,
          "The helper refuses a plan of 3 minutes before it started: " + outcome.line);
}

// Re-review #274, 2: data files that CoffeeCNC writes until it ends (the alarm log, the offset
// history) made the helper refuse the whole restore. They are decided after the exit, as the
// .var is; the alarm log is kept in the backup and never restored.
void data_decided_after_exit() {
  Fixture f;
  f.disk.files[in_data("alarm-log.jsonl")] = "{\"old\":1}\n";
  const std::string path = f.backup()["path"];
  const auto entries = read_archive(f.disk.files.at(path));
  require(entries && std::ranges::any_of(*entries,
                                         [](const ArchiveEntry& entry) {
                                           return entry.name == "data/alarm-log.jsonl";
                                         }),
          "The alarm log is in the backup");
  const auto summary = f.summary(path);
  require(f.change_of(summary, "data/alarm-log.jsonl") == "absent" &&
              f.change_of(summary, "data/offset-history.json") == "at_exit",
          "The alarm log is not restored; the offset history is decided at exit");
  const auto backed_up = f.disk.files[in_data("offset-history.json")];
  f.disk.files[tools] = "now tools\n";
  f.stage(path);
  require(std::ranges::none_of(f.plan()["files"],
                               [](const Json& file) {
                                 return file["name"].get<std::string>().starts_with(
                                     "data/alarm-log");
                               }),
          "The alarm log is not staged");
  // What CoffeeCNC wrote after the restore was prepared, up to its end.
  f.disk.files[in_data("alarm-log.jsonl")] = "{\"old\":1}\n{\"new\":2}\n";
  f.disk.files[in_data("offset-history.json")] = R"({"version":1,"entries":[1]})";
  const auto applied = apply_staged_restore(f.files, data, now + 30s);
  require(applied.has_value(), "Data files written after staging do not refuse the restore: " +
                                   (applied ? std::string{} : applied.error()));
  require(f.disk.files[in_data("offset-history.json")] == backed_up &&
              f.disk.files[in_data("alarm-log.jsonl")] == "{\"old\":1}\n{\"new\":2}\n" &&
              f.disk.files[tools] == "T1 P1 Z48.2 D6 ;End mill\n",
          "The offset history is the backup's; the alarm log keeps what was told since");
}

// Re-review #274, 3: a target in a folder that does not exist passed the check, and could not be
// written once LinuxCNC had stopped.
void missing_folder_refused() {
  Fixture f;
  const std::string path = f.backup()["path"];
  f.disk.files.erase(custom);
  f.disk.directories.erase(std::string(config) + "/hal");
  const auto checked = f.check(path);
  require(checked.outcome == CommandOutcome::rejected &&
              checked.reason == "config/hal/custom.hal cannot be restored: the folder of " +
                                    std::string(custom) +
                                    " does not exist. The archive is refused.",
          "A target in a missing folder is refused at the check: " + checked.reason);
  // A missing data file is made: the restore makes the data folder.
  f.disk.directories.insert(std::string(config) + "/hal");
  f.disk.files.erase(in_data("tool-models.json"));
  f.disk.directories.erase(data);
  const auto data_missing = f.summary(path);
  require(f.change_of(data_missing, "config/hal/custom.hal") == "new",
          "A missing file in a folder that exists is new");
}

// Re-review #274, 4: the rollback wrote a link's content as a file and the notice said "as they
// were"; a group the user is not in made fchown fail and rolled back the whole restore.
void links_and_groups() {
  Fixture f;
  const std::string shared = "/home/cnc/shared/tool.tbl";
  f.disk.files[shared] = f.disk.files[tools];
  f.disk.files.erase(tools);
  f.disk.links[tools] = shared;
  const std::string path = f.backup()["path"];
  f.disk.files[shared] = "shared now\n";
  f.disk.files[var] = "now var\n";
  f.stage(path);
  const auto plan = f.plan();
  const auto target = [&](std::size_t i) {
    return plan["files"][i]["root"].get<std::string>() + '/' +
           plan["files"][i]["relative"].get<std::string>();
  };
  require(target(0) == tools && target(1) == var, "The tool table, then the .var");
  f.files.failures["replace " + target(1)] = 1;
  require(!apply_staged_restore(f.files, data, now).has_value(), "The second file fails");
  require(f.disk.links.contains(tools) && f.disk.links[tools] == shared &&
              !f.disk.files.contains(tools) && f.disk.files[shared] == "shared now\n",
          "The tool table is the same link again");
  f.open();
  auto notice = f.machine->take_notice();
  require(notice && notice->text.ends_with("The files are as they were."),
          "and the notice can say so: " + (notice ? notice->text : std::string{}));
  // The link cannot be made again: the notice says so.
  f.stage(path);
  f.files.failures["replace " + target(1)] = 1;
  f.files.failures["relink " + std::string(tools)] = 1;
  require(!apply_staged_restore(f.files, data, now).has_value(), "It fails again");
  f.open();
  notice = f.machine->take_notice();
  require(notice &&
              notice->text.find("These files could not be put back: " + std::string(tools) +
                                " cannot be made a link again") != std::string::npos &&
              notice->text.find("as they were") == std::string::npos,
          "The notice names the link that is not back: " + (notice ? notice->text : std::string{}));
  // A group the user is not in: written with the user's group, and said.
  Fixture g;
  const std::string again = g.backup()["path"];
  g.disk.files[tools] = "now tools\n";
  g.disk.groups[tools] = 27;
  g.disk.foreign_groups.insert(27);
  g.stage(again);
  const auto applied = apply_staged_restore(g.files, data, now);
  require(applied.has_value() && g.disk.files[tools] == "T1 P1 Z48.2 D6 ;End mill\n" &&
              !g.disk.groups.contains(tools),
          "A group that cannot be kept does not stop the restore: " +
              (applied ? std::string{} : applied.error()));
  g.open();
  notice = g.machine->take_notice();
  require(notice && !notice->error &&
              notice->text.ends_with("These files did not keep their group, which this user is "
                                     "not in: " +
                                     std::string(tools) + "."),
          "The next start says which files did not keep their group: " +
              (notice ? notice->text : std::string{}));
}

// Re-review #274, 5: a LinuxCNC started between the last check and the last write read mixed
// files. The helper holds /tmp/linuxcnc.lock while it writes; a lock that is gone stops it.
void helper_holds_linuxcnc_lock() {
  Fixture f;
  const std::string path = f.backup()["path"];
  f.disk.files[tools] = "now tools\n";
  f.disk.files[var] = "now var\n";
  f.stage(path);
  FakeHost host;
  bool held_while_writing = false;
  f.files.during = [&](std::string_view step) {
    if (step == "replace")
      held_while_writing = host.holds_linuxcnc_lock();
  };
  auto outcome = run_restore_helper(f.files, data, 4000, host);
  f.files.during = nullptr;
  require(outcome.code == 0 && held_while_writing && host.locks_taken == 1 && !host.lock_file,
          "The lock is held while the files are written, and removed after: " + outcome.line);
  // A `linuxcnc` that cleans up "the other session" removes the lock while the helper writes.
  f.open();
  f.disk.files[tools] = "now tools\n";
  f.disk.files[var] = "now var\n";
  f.stage(path);
  f.files.during = [&](std::string_view step) {
    if (step == "replace" && host.lock_ours) {
      host.lock_file = true; // LinuxCNC's own lock now
      host.lock_ours = false;
    }
  };
  outcome = run_restore_helper(f.files, data, 4000, host);
  f.files.during = nullptr;
  require(outcome.code == 1 && outcome.line.find("LinuxCNC was started while the files were "
                                                 "written") != std::string::npos,
          "A lock that is gone stops the restore: " + outcome.line);
  require(f.disk.files[tools] == "now tools\n" && f.disk.files[var] == "now var\n",
          "and what was written is put back");
  require(host.lock_file, "LinuxCNC's own lock is not removed");
  // A lock that is there when the helper is to write: nothing.
  f.open();
  f.stage(path);
  host.running = [] { return std::optional<std::string>{}; };
  outcome = run_restore_helper(f.files, data, 4000, host);
  require(outcome.code == 1 &&
              outcome.line.find("LinuxCNC runs again (/tmp/linuxcnc.lock cannot "
                                "be made: File exists)") != std::string::npos &&
              f.disk.files[tools] == "now tools\n",
          "The lock is made O_EXCL: " + outcome.line);
}

// A file that read_beneath found; the check fails when it did not.
auto found(const std::expected<std::optional<ToolModelFiles::Found>, std::string>& read)
    -> ToolModelFiles::Found {
  require(read.has_value(), "The file is read: " + (read ? std::string{} : read.error()));
  const auto& file = *read;
  require(file.has_value(), "The file exists");
  return file.value_or(ToolModelFiles::Found{});
}

// Real files below `root`: a directory made for the test.
auto write_real(const std::filesystem::path& path, const std::string& bytes) -> void {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream{path, std::ios::binary} << bytes;
}
auto read_real(const std::filesystem::path& path) -> std::string {
  std::ifstream in{path, std::ios::binary | std::ios::ate};
  const auto size = static_cast<std::streamoff>(in.tellg());
  std::string bytes(size > 0 ? static_cast<std::size_t>(size) : 0, '\0');
  in.seekg(0);
  in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return bytes;
}

// The local file system: a linked subfolder is refused (review #274, 4), modes are kept
// (review #274, 10), a link is replaced and not followed.
void real_files(const std::filesystem::path& base) {
  const auto root = base / "machine_data_tests";
  std::filesystem::remove_all(root);
  const auto configs = root / "configs";
  const auto outside = root / "outside";
  write_real(configs / "machine.hal", "net a b\n");
  write_real(outside / "custom.hal", "outside\n");
  write_real(outside / "shared.tbl", "T1\n");
  std::filesystem::create_directory_symlink(outside, configs / "hal");
  std::filesystem::create_symlink(outside / "shared.tbl", configs / "tool.tbl");
  require(::chmod((configs / "machine.hal").c_str(), 0640) == 0, "The test's file has mode 0640");
  LocalToolModelFiles files;
  const auto dir = configs.string();
  auto read = files.read_beneath(dir, "hal/custom.hal", 100);
  require(!read && read.error().find("symbolic link") != std::string::npos,
          "A file through a linked subfolder is not read: " +
              (read ? std::string{} : read.error()));
  auto replaced = files.replace_beneath(dir, "hal/custom.hal", "written\n", 0644, std::nullopt);
  require(!replaced && !replaced.error().renamed &&
              read_real(outside / "custom.hal") == "outside\n" &&
              !std::filesystem::exists(outside / "custom.hal.coffeecnc-tmp"),
          "Nothing is written through a linked subfolder");
  require(!files.remove_beneath(dir, "hal/custom.hal") &&
              std::filesystem::exists(outside / "custom.hal"),
          "Nothing is removed through it");
  require(!files.read_beneath(dir, "../outside/custom.hal", 100), "`..` is refused");
  // The mode and the group are kept.
  const auto held = found(files.read_beneath(dir, "machine.hal", 100));
  require(held.mode == 0640 && !held.symlink && held.group == ::getegid(),
          "The mode and group are read");
  require(files.replace_beneath(dir, "machine.hal", "net c d\n", held.mode, held.group)
              .has_value(),
          "A file is replaced");
  struct stat info{};
  require(::stat((configs / "machine.hal").c_str(), &info) == 0 && (info.st_mode & 07777) == 0640 &&
              read_real(configs / "machine.hal") == "net c d\n",
          "with its mode");
  // A linked tool table is read through the link, and replaced, not followed.
  const auto linked = found(files.read_beneath(dir, "tool.tbl", 100));
  require(linked.symlink && linked.content.bytes == "T1\n", "A link is read by its content");
  require(
      files.replace_beneath(dir, "tool.tbl", "T2\n", linked.mode, std::nullopt).has_value() &&
          !std::filesystem::is_symlink(configs / "tool.tbl") &&
          read_real(configs / "tool.tbl") == "T2\n" && read_real(outside / "shared.tbl") == "T1\n",
      "The link is replaced by the file; its target is unchanged");
  require(files.remove_beneath(dir, "tool.tbl").has_value() &&
              !std::filesystem::exists(configs / "tool.tbl"),
          "A file is removed");
  // Re-review #274, 4: a link is made again, as it was written.
  require(files.link_beneath(dir, "tool.tbl", "../outside/shared.tbl").has_value() &&
              std::filesystem::read_symlink(configs / "tool.tbl") == "../outside/shared.tbl" &&
              found(files.read_beneath(dir, "tool.tbl", 100)).link == "../outside/shared.tbl",
          "A link is made again with its target as written");
  // Re-review #274, 4: a group this user may not give is not kept, and is not a failure.
  if (::geteuid() != 0) {
    const auto foreign =
        files.replace_beneath(dir, "machine.hal", "net e f\n", 0640, ::getegid() == 0 ? 1U : 0U);
    require(foreign.has_value() && !foreign->group_kept &&
                read_real(configs / "machine.hal") == "net e f\n",
            "A file is written with this user's group: " +
                (foreign ? std::string{} : foreign.error().reason));
    const auto own = files.replace_beneath(dir, "machine.hal", "net g h\n", 0640, ::getegid());
    require(own.has_value() && own->group_kept, "Its own group is kept");
  }
  // Re-review #274, 3: a folder that is not there.
  require(files.folder_exists_beneath(dir, "missing/x.hal") ==
                  std::expected<bool, std::string>{false} &&
              files.folder_exists_beneath(dir, "x.hal") == std::expected<bool, std::string>{true},
          "A missing folder is told from one that exists");
  require(!files.folder_exists_beneath(dir, "hal/x.hal").has_value(),
          "A folder reached through a link is an error");
  std::filesystem::remove_all(root);
}

// The helper with the local lock file at `path`; LinuxCNC does not run, the parent is gone.
class LocalLockHost final : public RestoreHost {
public:
  explicit LocalLockHost(const std::string& path) : local(path) {}
  LocalRestoreHost local;
  auto alive(long) -> bool override { return false; }
  auto linuxcnc_running() -> std::optional<std::string> override { return std::nullopt; }
  auto take_linuxcnc_lock() -> std::optional<std::string> override {
    return local.take_linuxcnc_lock();
  }
  auto holds_linuxcnc_lock() -> bool override { return local.holds_linuxcnc_lock(); }
  void release_linuxcnc_lock() override { local.release_linuxcnc_lock(); }
  void pause(std::chrono::milliseconds) override {}
  auto elapsed() -> std::chrono::milliseconds override { return {}; }
  auto now() -> std::chrono::milliseconds override { return 1795000000000ms; }
  auto boot_time() -> BootTime override { return local.boot_time(); }
};

// Review of a2865e8, 1: the helper took a lock as its own by device and inode only, so a lock
// that LinuxCNC made again on the freed inode passed, and the helper then removed LinuxCNC's
// live lock. The lock holds a token of the helper, and the content is compared too.
void real_linuxcnc_lock(const std::filesystem::path& base) {
  const auto lock = base / "machine_data_linuxcnc.lock";
  std::filesystem::remove(lock);
  {
    LocalRestoreHost host{lock.string()};
    require(!host.take_linuxcnc_lock().has_value(), "The lock is made");
    const auto token = read_real(lock);
    const auto pid = std::format("{} ", ::getpid());
    require(token.starts_with(pid) && token.size() == pid.size() + 33 && token.ends_with('\n'),
            "It holds this process ID and 16 random bytes: " + token);
    require(host.holds_linuxcnc_lock(), "and is this helper's");
    LocalRestoreHost other{lock.string()};
    require(other.take_linuxcnc_lock().has_value() && !other.holds_linuxcnc_lock(),
            "A lock that exists is refused, and is not the other helper's");
    host.release_linuxcnc_lock();
    require(!std::filesystem::exists(lock), "Released: removed");
  }
  Fixture f;
  const std::string path = f.backup()["path"];
  f.disk.files[tools] = "now tools\n";
  f.disk.files[var] = "now var\n";
  f.stage(path);
  LocalLockHost host{lock.string()};
  bool same_inode = false;
  f.files.during = [&](std::string_view step) {
    struct stat before{};
    if (step != "replace" || same_inode || ::stat(lock.c_str(), &before) != 0)
      return;
    // LinuxCNC's lock on the same inode: the file is emptied and written, not made anew.
    std::ofstream{lock, std::ios::binary | std::ios::trunc} << "4242\n";
    struct stat after{};
    same_inode = ::stat(lock.c_str(), &after) == 0 && after.st_dev == before.st_dev &&
                 after.st_ino == before.st_ino;
  };
  auto outcome = run_restore_helper(f.files, data, 4000, host);
  f.files.during = nullptr;
  require(same_inode, "The lock is LinuxCNC's now, on the same inode");
  require(outcome.code == 1 && outcome.line.find("LinuxCNC was started while the files were "
                                                 "written") != std::string::npos,
          "A lock with LinuxCNC's content stops the restore: " + outcome.line);
  require(f.disk.files[tools] == "now tools\n" && f.disk.files[var] == "now var\n",
          "and what was written is put back");
  require(std::filesystem::exists(lock) && read_real(lock) == "4242\n",
          "LinuxCNC's lock is not removed");
  std::filesystem::remove(lock);
  // Left alone, the helper writes and removes its own lock.
  f.open();
  f.stage(path);
  outcome = run_restore_helper(f.files, data, 4000, host);
  require(outcome.code == 0 && !std::filesystem::exists(lock),
          "The helper writes and removes its lock: " + outcome.line);
}

// Review #274, 7: the helper inherited the descriptors of CoffeeCNC that are not close-on-exec
// (asio's sockets: the LAN screen's port stayed taken).
void spawn_closes_descriptors(const std::filesystem::path& base) {
  const auto out = base / "machine_data_spawn.txt";
  std::filesystem::remove(out);
  const int inherited = ::open("/dev/null", O_RDONLY); // not close-on-exec
  require(inherited >= 0 && ::dup2(inherited, 57) == 57, "A descriptor that is not close-on-exec");
  const auto child = spawn_detached(
      {"/bin/sh", "-c",
       std::format("if [ -e /proc/self/fd/57 ]; then echo open; else echo closed; fi > '{}'; "
                   "cut -d ' ' -f 6 /proc/$$/stat >> '{}'",
                   out.string(), out.string())});
  require(child.has_value(), "The child starts");
  int status = 0;
  require(::waitpid(static_cast<pid_t>(*child), &status, 0) == *child, "and ends");
  static_cast<void>(::close(57));
  static_cast<void>(::close(inherited));
  std::istringstream said{read_real(out)};
  std::string fd;
  std::string session;
  std::getline(said, fd);
  std::getline(said, session);
  require(fd == "closed", "The child has no descriptor of this process but 0, 1, 2: " + fd);
  require(session == std::to_string(*child), "The child leads a session of its own: " + session);
  std::filesystem::remove(out);
}

} // namespace

auto main(int argc, char** argv) -> int {
  try {
    const auto base =
        argc > 1 ? std::filesystem::path{argv[1]} : std::filesystem::temp_directory_path();
    archive_round_trip();
    backup_contents();
    symlinked_configuration();
    check_and_changes();
    path_traversal();
    hal_role_is_bounded();
    machine_id_required();
    refusal_states();
    restore_and_apply();
    var_decided_after_exit();
    abandoned_and_changed_targets();
    partial_write_failure();
    rollback_counts_renamed_files();
    crash_points();
    start_check_under_lock();
    helper_waits_for_linuxcnc();
    stale_plan_refused();
    data_decided_after_exit();
    missing_folder_refused();
    links_and_groups();
    helper_holds_linuxcnc_lock();
    real_files(base);
    real_linuxcnc_lock(base);
    spawn_closes_descriptors(base);
    std::cout << checks << " machine data checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Check " << checks << " failed: " << error.what() << '\n';
    return 1;
  }
}
