#include "fake_tool_model_files.hpp"
#include <coffeecnc/app/operator_settings.hpp>
#include <coffeecnc/app/tool_model_store.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>

using namespace coffeecnc;
using namespace std::chrono_literals;
namespace {
using Json = nlohmann::json;
int checks = 0;
void require(bool condition, std::string_view why) {
  ++checks;
  if (!condition)
    throw std::runtime_error(std::string(why));
}
const Limits limits{1000, 1.2, .5, 1.2, 3000};
const ToolModelLocation location{"G0759", ToolModelAvailability::available, "", "/settings/G0759"};
const std::string file = location.directory + "/operator-settings.json";
auto command(std::uint64_t revision = 0) -> SetOperatorSettings {
  return {"G0759", revision, {StartupUnits::inch, {9, 1}, 1800, 88.9, 6.35}};
}
void documents_and_limits() {
  const OperatorSettingsFile value{"G0759", 1, command().document};
  const auto encoded = encode_operator_settings_file(value);
  require(encoded.has_value(), "File encodes");
  const auto decoded = decode_operator_settings_file(encoded.value_or(""));
  require(decoded && decoded->document == value.document, "Numeric preferences round trip exactly");
  auto legacy = Json::parse(*encoded);
  legacy["document"].erase("tool_reference_height");
  const auto upgraded = decode_operator_settings_file(legacy.dump());
  auto expected = value.document;
  expected.tool_reference_height = 0;
  require(upgraded && upgraded->document == expected,
          "Existing v1 settings retain all preferences with zero reference height");
  for (const auto height : {Json(-1), Json("6.35"), Json(nullptr), Json(1e10)}) {
    auto invalid = Json::parse(*encoded);
    invalid["document"]["tool_reference_height"] = height;
    require(!decode_operator_settings_file(invalid.dump()), "Invalid reference height rejected");
  }
  auto broken = Json::parse(*encoded);
  for (const auto& systems : {Json::array(), Json::array({1, 1}), Json::array({1, 10}),
                              Json::array({1.5}), Json::array({true})}) {
    broken["document"]["enabled_wcs"] = systems;
    require(!decode_operator_settings_file(broken.dump()),
            "Malformed work system shortcuts rejected");
  }
  for (const auto text : {"{}", "[]", "{\"a\":1,\"a\":2}", "null"})
    require(!decode_operator_settings_file(text), "Malformed file rejected");
  require(!decode_operator_settings_file(std::string(max_operator_settings_bytes + 1, ' ')),
          "Bounded reads");
  broken = Json::parse(*encoded);
  broken["document"]["version"] = 2;
  require(!decode_operator_settings_file(broken.dump()), "Future version is not overwritten");
  broken = Json::parse(*encoded);
  broken["document"]["extra"] = true;
  require(!decode_operator_settings_file(broken.dump()), "Unknown fields rejected");
  broken = Json::parse(*encoded);
  broken["revision"] = 0;
  require(!decode_operator_settings_file(broken.dump()), "On-disk revision starts at one");
  auto requested = command();
  requested.document.default_jog_velocity = 1001;
  require(!operator_settings_command_error(requested, limits).empty(), "Jog INI ceiling enforced");
  requested = command();
  requested.document.default_spindle_rpm = 3001;
  require(!operator_settings_command_error(requested, limits).empty(),
          "Spindle INI ceiling enforced");
  ControllerState down;
  require(CommandPolicy(limits).evaluate(command(), down).allowed,
          "Preferences do not require machine power or motion");
}
void archive_and_restart() {
  FakeDisk disk;
  disk.files[location.directory + "/tool-models.json"] = "corrupt models";
  FakeToolModelFiles files{disk};
  ToolModelArchive owner{files, location};
  owner.open();
  OperatorSettingsArchive archive{files, limits};
  archive.open(owner.shared_location());
  require(archive.settings().revision == 0 &&
              archive.settings().document->default_jog_velocity == 600,
          "Missing file gives factory defaults without writing a file");
  require(!disk.files.contains(file), "No file until explicit save");
  require(archive.apply(command()).outcome == CommandOutcome::accepted,
          "Save accepted after durable replacement");
  require(archive.settings().revision == 1 &&
              archive.settings().document->enabled_wcs == std::vector<int>{1, 9},
          "New revision and sorted shortcuts");
  require(disk.files.at(location.directory + "/tool-models.json") == "corrupt models",
          "Operator save leaves a corrupt tool-model file alone");
  const auto saved = disk.files.at(file);
  require(archive.apply(command()).outcome == CommandOutcome::rejected &&
              disk.files.at(file) == saved,
          "Stale editor cannot overwrite");
  auto other = command(1);
  other.machine_id = "OtherMill";
  require(archive.apply(other).outcome == CommandOutcome::rejected, "Wrong machine cannot save");
  OperatorSettingsArchive restarted{files, Limits{50, 1.2, .5, 1.2, 1000}};
  restarted.open(owner.shared_location());
  require(restarted.settings() == archive.settings(),
          "Restart preserves saved defaults even when machine limits shrink");
  disk.files[file] = "corrupt";
  require(archive.apply(command(1)).outcome == CommandOutcome::rejected &&
              disk.files[file] == "corrupt",
          "Externally edited file is not overwritten");
  require(archive.settings().availability == ToolModelAvailability::unavailable,
          "Storage becomes unavailable");
  OperatorSettingsArchive corrupt{files, limits};
  corrupt.open(owner.shared_location());
  require(corrupt.settings().availability == ToolModelAvailability::unavailable,
          "Corrupt startup file is unavailable");
  // The common directory is usable even if the independent tool-model file is corrupt.
  require(owner.shared_location().availability == ToolModelAvailability::available,
          "One shared directory lock");
}
void failures() {
  for (const std::string step : {"open_temporary", "write", "sync", "rename", "sync_directory"}) {
    FakeDisk disk;
    FakeToolModelFiles files{disk};
    ToolModelArchive owner{files, location};
    owner.open();
    OperatorSettingsArchive archive{files, limits};
    archive.open(owner.shared_location());
    require(archive.apply(command()).outcome == CommandOutcome::accepted, "Initial file saved");
    const auto saved = disk.files.at(file);
    files.failures[step] = 1;
    auto changed = command(1);
    changed.document.default_spindle_rpm = 2000;
    const auto reply = archive.apply(changed);
    require(reply.outcome == CommandOutcome::failed, "File-system failure never reports success");
    if (step == "sync_directory") {
      require(reply.snapshot && archive.settings().revision == 2 &&
                  reply.reason.find("uncertain") != std::string::npos,
              "After rename, report uncertainty and read-back values");
    } else
      require(disk.files.at(file) == saved && archive.settings().revision == 1,
              "Before rename, original survives");
  }
  FakeDisk disk;
  FakeToolModelFiles first{disk}, second{disk};
  ToolModelArchive owner{first, location};
  owner.open();
  ToolModelArchive blocked{second, location};
  blocked.open();
  OperatorSettingsArchive archive{second, limits};
  archive.open(blocked.shared_location());
  require(archive.settings().availability == ToolModelAvailability::unavailable,
          "A second process cannot borrow a failed lock");
  require(archive.apply(command()).outcome == CommandOutcome::rejected &&
              !disk.files.contains(file),
          "Locked directory is never written");
  FakeDisk session_disk;
  FakeToolModelFiles session_files{session_disk};
  ToolModelArchive session{session_files, {}};
  session.open();
  OperatorSettingsArchive ephemeral{session_files, limits};
  ephemeral.open(session.shared_location());
  require(ephemeral.settings().availability == ToolModelAvailability::session_only &&
              session_files.calls.empty(),
          "Missing identity does no file I/O");
}
void writer_delivery() {
  FakeDisk disk;
  FakeToolModelFiles files{disk};
  std::mutex mutex;
  std::condition_variable wake;
  std::vector<std::vector<std::string>> deliveries;
  ToolModelStore store{files, location,
                       [&](PageToken, std::vector<std::string> messages) -> void {
                         const std::scoped_lock lock{mutex};
                         deliveries.push_back(std::move(messages));
                         wake.notify_all();
                       },
                       limits};
  const auto wait = [&](std::size_t count) -> void {
    std::unique_lock lock{mutex};
    require(wake.wait_for(lock, 5s, [&]() -> bool { return deliveries.size() >= count; }),
            "Writer answers promptly");
  };
  store.send_snapshot(PageToken{1});
  wait(1);
  {
    const std::scoped_lock lock{mutex};
    require(deliveries[0].size() == 2 &&
                Json::parse(deliveries[0][1]).at("type") == "operator_settings",
            "Both documents follow hello");
  }
  require(!store.submit_operator_settings(PageToken{1}, 28, command()),
          "Save queued on the common writer");
  wait(2);
  {
    const std::scoped_lock lock{mutex};
    require(deliveries[1].size() == 2 && Json::parse(deliveries[1][0]).at("revision") == 1 &&
                Json::parse(deliveries[1][1]).at("outcome") == "accepted",
            "Updated snapshot precedes accepted result");
  }
  store.page_ended(PageToken{1});
  require(store.submit_operator_settings(PageToken{1}, 29, command(1)).has_value(),
          "Ended page cannot queue a write");
  store.send_snapshot(PageToken{2});
  wait(3);
  {
    const std::scoped_lock lock{mutex};
    require(Json::parse(deliveries[2][1]).at("revision") == 1,
            "A fresh page receives saved preferences");
  }
}

void host_file_restart() {
  std::array<char, 48> pattern{};
  const std::string prefix = "/tmp/coffeecnc-operator-settings-XXXXXX";
  std::ranges::copy(prefix, pattern.begin());
  const auto* made = ::mkdtemp(pattern.data());
  require(made != nullptr, "Private test directory created");
  const std::filesystem::path root{made};
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }
  } cleanup{root};
  const ToolModelLocation local{"G0759", ToolModelAvailability::available, "",
                                (root / "G0759").string()};
  {
    LocalToolModelFiles files;
    ToolModelArchive owner{files, local};
    owner.open();
    OperatorSettingsArchive settings{files, limits};
    settings.open(owner.shared_location());
    require(settings.apply(command()).outcome == CommandOutcome::accepted,
            "POSIX write, fsync, rename and directory fsync succeed");
    const auto permissions =
        std::filesystem::status(root / "G0759/operator-settings.json").permissions();
    require((permissions & (std::filesystem::perms::group_all |
                            std::filesystem::perms::others_all)) == std::filesystem::perms::none,
            "Operator file is private");
    owner.close();
  }
  {
    LocalToolModelFiles files;
    ToolModelArchive owner{files, local};
    owner.open();
    OperatorSettingsArchive settings{files, limits};
    settings.open(owner.shared_location());
    require(settings.settings().revision == 1 &&
                settings.settings().document->startup_units == StartupUnits::inch &&
                settings.settings().document->default_jog_velocity == 88.9,
            "A fresh file handle and archive restore saved preferences");
    owner.close();
  }
}
} // namespace
auto main() -> int {
  try {
    documents_and_limits();
    archive_and_restart();
    failures();
    writer_delivery();
    host_file_restart();
    std::cout << checks << " operator settings checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
