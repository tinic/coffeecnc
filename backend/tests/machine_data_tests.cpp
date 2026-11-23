#include <coffeecnc/app/backup_archive.hpp>
#include <coffeecnc/app/machine_data.hpp>
#include <coffeecnc/core/command_policy.hpp>

#include "fake_tool_model_files.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

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

  Fixture() {
    disk.directories.insert({config, std::string(config) + "/hal", data, usb});
    disk.files[ini] = "[EMC]\nMACHINE = G0759\n";
    disk.files[hal] = "loadrt trivkins\n";
    disk.files[custom] = "net estop-loop iocontrol.0.user-enable-out\n";
    disk.files[postgui] = "net spindle coffeecnc.spindle-rpm\n";
    disk.files[tools] = "T1 P1 Z48.2 D6 ;End mill\n";
    disk.files[var] = "5221\t10.000000\n5222\t20.000000\n";
    disk.files[std::string(data) + "/tool-models.json"] = R"({"version":1})";
    disk.files[std::string(data) + "/offset-history.json"] = R"({"version":1,"entries":[]})";
    open();
  }
  void open(MachineDataPaths given = paths()) {
    machine = std::make_unique<MachineData>(files, std::move(given), [] { return now; });
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
    const auto found = std::ranges::find_if(listed, [&](const Json& file) { return file["name"] == name; });
    return found == listed.end() ? "absent" : (*found)["change"].get<std::string>();
  }
};

const auto allow = []() -> std::string { return {}; };

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
              written["not_included"][1]["path"] == "../elsewhere.hal",
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
  require(f.disk.files.at(path).size() == written["bytes"].get<std::size_t>(), "Its size is said");
  require(!f.disk.files.contains(path + ".coffeecnc-tmp"), "No temporary file is left");
  // A folder that cannot be written: failed, nothing left.
  f.files.failures["rename"] = 1;
  const auto failed = f.machine->backup(usb);
  require(failed.outcome == CommandOutcome::failed && failed.messages.empty(),
          "A backup that cannot be written fails");
}

void check_and_changes() {
  Fixture f;
  const std::string path = f.backup()["path"];
  auto summary = f.summary(path);
  require(summary["machine_id"] == "G0759" && summary["created"] == now.count() &&
              summary["files"].size() == 8 && summary["token"].get<std::string>().size() == 16,
          "The summary has the machine, the time, every file and a token");
  for (const auto& file : summary["files"])
    require(file["change"] == "same", "Right after the backup every file is the same");
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
              other.reason == "This backup is of machine G0759; this machine is BENCH.",
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

// A crafted archive: a manifest that lists `name` with `role`, and that entry.
auto crafted(const std::string& role, const std::string& name) -> std::string {
  const Json manifest{{"format", "coffeecnc-backup"},
                      {"version", 1},
                      {"machine_id", "G0759"},
                      {"created", 1},
                      {"files",
                       {{{"role", "ini"}, {"name", "config/g0759.ini"}},
                        {{"role", role}, {"name", name}}}}};
  return *write_archive({{"coffeecnc-backup.json", manifest.dump(), 0},
                         {"config/g0759.ini", "[EMC]\n", 0},
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
    const auto restored = f.machine->restore(path, "0000000000000000", allow);
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
  f.disk.files[path] = *write_archive({{"coffeecnc-backup.json", "{}", 0}, {"a", "1", 0}, {"a", "2", 0}});
  require(f.check(path).outcome == CommandOutcome::rejected, "A name twice is refused");
  f.disk.files.erase(path);
  require(f.disk.files == before, "Nothing was written by any of them");
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
  auto done = f.machine->restore(path, token, [] { return std::string("Machine is on"); });
  require(done.outcome == CommandOutcome::rejected && done.reason == "Machine is on" &&
              !f.machine->staged(),
          "The recheck just before staging refuses");
  f.disk.files[path] += "x"; // the file changed after the check
  done = f.machine->restore(path, token, allow);
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
  const std::string token = f.summary(path)["token"];
  const auto done = f.machine->restore(path, token, allow);
  require(done.outcome == CommandOutcome::accepted && done.quit && f.machine->staged(),
          "A restore is staged and quits: " + done.reason);
  require(f.disk.files[tools] == "T1 P1 Z99 D6\n", "Nothing is written while LinuxCNC runs");
  const auto plan = Json::parse(f.disk.files.at(std::string(data) + "/restore/plan.json"));
  require(plan["files"].size() == 3, "Only the three files that change are staged");
  // LinuxCNC writes its .var as it stops: the helper comes after that.
  f.disk.files[var] = "5221\t0\n5222\t0\n";
  const auto applied = apply_staged_restore(f.files, data, now + 60s);
  require(applied && *applied == 3, "The helper writes three files");
  require(f.disk.files[tools] == original_tools && f.disk.files[var] == original_var &&
              f.disk.files.contains(custom),
          "The tool table, the .var and the HAL file are the backup's");
  const auto copy = std::string(data) + "/pre-restore-20261118-110740.tar.gz";
  const auto saved = read_archive(f.disk.files.at(copy));
  require(saved && saved->size() == 4, "The files before the restore are kept as a backup, with the INI file");
  require(std::ranges::any_of(*saved, [](const ArchiveEntry& e) {
            return e.name == "parameters/g0759.var" && e.bytes == "5221\t0\n5222\t0\n";
          }),
          "The pre-restore copy has the .var as LinuxCNC left it");
  require(!f.disk.files.contains(std::string(data) + "/restore/plan.json") &&
              !f.disk.files.contains(std::string(data) + "/restore/0.staged"),
          "The plan and the staged copies are removed");
  require(apply_staged_restore(f.files, data, now).value_or(99) == 0, "A second run does nothing");
  f.open();
  const auto notice = f.machine->take_notice();
  require(notice && notice->first == MessageLevel::info &&
              notice->second.find("3 files were written") != std::string::npos &&
              notice->second.find(copy) != std::string::npos,
          "The next run says what was restored");
  require(!f.machine->take_notice(), "Once");
  // The pre-restore copy is a backup the panel can restore.
  const auto back = f.summary(copy);
  require(back["files"].size() == 3, "The copy checks (the HAL file was new, so it is not in it)");
  // Nothing changed: nothing staged, no quit.
  f.open();
  const std::string again = f.backup()["path"];
  const auto same = f.machine->restore(again, f.summary(again)["token"], allow);
  require(same.outcome == CommandOutcome::accepted && !same.quit && !f.machine->staged(),
          "A restore that changes nothing does not quit");
}

void partial_write_failure() {
  Fixture f;
  const std::string path = f.backup()["path"];
  const auto backup_tools = f.disk.files[tools];
  f.disk.files[tools] = "now tools\n";
  f.disk.files[var] = "now var\n";
  f.disk.files.erase(custom);
  const auto done = f.machine->restore(path, f.summary(path)["token"], allow);
  require(done.quit, "Staged");
  const auto plan = Json::parse(f.disk.files.at(std::string(data) + "/restore/plan.json"));
  // The second file of the plan cannot be put in place: the disk is full.
  const std::string second = plan["files"][1]["to"];
  f.files.failures["rename " + second + ".coffeecnc-tmp " + second] = 1;
  const auto applied = apply_staged_restore(f.files, data, now);
  require(!applied && applied.error().find(second) != std::string::npos,
          "The helper fails, naming the file");
  require(f.disk.files[tools] == "now tools\n" && f.disk.files[var] == "now var\n" &&
              !f.disk.files.contains(custom),
          "The file written before it is put back; a new one is removed");
  require(!f.disk.files.contains(second + ".coffeecnc-tmp"), "No temporary file is left");
  require(!f.disk.files.contains(std::string(data) + "/restore/plan.json"),
          "The plan is not tried again");
  f.open();
  const auto notice = f.machine->take_notice();
  require(notice && notice->first == MessageLevel::error &&
              notice->second.starts_with("The restore was not completed:"),
          "The next run says it failed");
  static_cast<void>(backup_tools);
  // A plan the helper never got to.
  const auto staged = f.machine->restore(path, f.summary(path)["token"], allow);
  require(staged.quit, "Staged again");
  f.open();
  const auto left = f.machine->take_notice();
  require(left && left->second.starts_with("A restore was prepared but not written"),
          "A plan left at the next start is reported and discarded");
  require(!f.disk.files.contains(std::string(data) + "/restore/plan.json"), "It is discarded");
}

} // namespace

auto main() -> int {
  try {
    archive_round_trip();
    backup_contents();
    check_and_changes();
    path_traversal();
    refusal_states();
    restore_and_apply();
    partial_write_failure();
    std::cout << checks << " machine data checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Check " << checks << " failed: " << error.what() << '\n';
    return 1;
  }
}
