#include "fake_tool_model_files.hpp"
#include <coffeecnc/app/operator_settings.hpp>
#include <coffeecnc/app/tool_model_store.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <unistd.h>

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
  return {"G0759", revision, {StartupUnits::inch, {9, 1}, 1800, 88.9, 6.35, ProgramLayout::text, PanelTheme::dark, {}, {}, {}}};
}
void documents_and_limits() {
  const OperatorSettingsFile value{"G0759", 1, command().document};
  const auto encoded = encode_operator_settings_file(value);
  require(encoded.has_value(), "File encodes");
  const auto decoded = decode_operator_settings_file(encoded.value_or(""));
  require(decoded && decoded->document == value.document, "Numeric preferences round trip exactly");
  for (const auto& [theme, name] :
       {std::pair{PanelTheme::sandstone, "sandstone"}, std::pair{PanelTheme::harbor, "harbor"}}) {
    auto themed = value;
    themed.document.theme = theme;
    const auto saved_theme = encode_operator_settings_file(themed);
    require(saved_theme.has_value(), "The selected palette can be stored");
    const auto loaded_theme = decode_operator_settings_file(*saved_theme);
    require(loaded_theme && loaded_theme->document == themed.document,
            "Restoring a palette retains the whole preferences document");
    const auto body = Json::parse(*saved_theme).at("document");
    const auto requested_theme = decode_operator_preferences(body.dump());
    require(body.at("theme") == name && requested_theme && requested_theme->theme == theme,
            "The native settings command accepts the palette name");
  }
  auto legacy = Json::parse(*encoded);
  legacy["document"].erase("tool_reference_height");
  legacy["document"].erase("program_layout");
  legacy["document"].erase("theme");
  const auto upgraded = decode_operator_settings_file(legacy.dump());
  auto expected = value.document;
  expected.tool_reference_height = 0;
  expected.program_layout = ProgramLayout::path;
  expected.theme = PanelTheme::light;
  require(upgraded && upgraded->document == expected,
          "Earlier v1 documents retain their preferences and default the new fields");

  auto invalid_presentation = Json::parse(*encoded);
  invalid_presentation["document"]["program_layout"] = false;
  invalid_presentation["document"]["theme"] = "sepia";
  const auto normalized = decode_operator_settings_file(invalid_presentation.dump());
  expected.tool_reference_height = value.document.tool_reference_height;
  require(normalized && normalized->document == expected,
          "Invalid view and theme fall back independently of other preferences");
  for (const auto& height : {Json(-1), Json("6.35"), Json(nullptr), Json(1e10)}) {
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
void saved_positions() {
  OperatorSettingsFile value{"G0759", 1, command().document};
  value.document.saved_positions = {{"Vise jaw", {-120.5, 33.25, -10}}, {"Position 2", {0, 0, 0}}};
  const auto encoded = encode_operator_settings_file(value);
  require(encoded.has_value(), "File with saved positions encodes");
  const auto decoded = decode_operator_settings_file(encoded.value_or(""));
  require(decoded && decoded->document == value.document, "Saved positions round trip exactly");
  require(Json::parse(*encoded)["document"]["saved_positions"][0] ==
              Json{{"name", "Vise jaw"}, {"position", {-120.5, 33.25, -10}}},
          "Saved positions are a list of name and machine XYZ");
  auto older = Json::parse(*encoded);
  older["document"].erase("saved_positions");
  const auto upgraded = decode_operator_settings_file(older.dump());
  require(upgraded && upgraded->document.saved_positions.empty() &&
              upgraded->document.default_spindle_rpm == 1800,
          "Documents without saved positions keep their other preferences");
  for (const auto& damaged :
       {Json("x"), Json::array({Json{{"name", "A"}, {"position", {1, 2}}}}),
        Json::array({Json{{"name", ""}, {"position", {1, 2, 3}}}}),
        Json::array({Json{{"name", "   "}, {"position", {1, 2, 3}}}}),
        Json::array({Json{{"name", "A\n"}, {"position", {1, 2, 3}}}}),
        Json::array({Json{{"name", std::string(65, 'a')}, {"position", {1, 2, 3}}}}),
        Json::array({Json{{"name", "A"}, {"position", {1, "2", 3}}}}),
        Json::array({Json{{"name", "A"}, {"position", {1, 2, 1e7}}}}),
        Json::array({Json{{"name", "A"}, {"position", {1, 2, 3}}, {"z", true}}})}) {
    auto invalid = Json::parse(*encoded);
    invalid["document"]["saved_positions"] = damaged;
    const auto dropped = decode_operator_settings_file(invalid.dump());
    require(dropped && dropped->document.saved_positions.empty() &&
                dropped->document.default_spindle_rpm == 1800,
            "A damaged position list is dropped without losing other settings");
  }
  auto seventeen = Json::parse(*encoded);
  seventeen["document"]["saved_positions"] = Json::array();
  for (int index = 0; index < 17; ++index)
    seventeen["document"]["saved_positions"].push_back({{"name", "P"}, {"position", {0, 0, 0}}});
  const auto many = decode_operator_settings_file(seventeen.dump());
  require(many && many->document.saved_positions.empty(), "At most sixteen saved positions");
  auto requested = command();
  requested.document.saved_positions = {{"A", {0, 0, std::nan("")}}};
  require(!operator_settings_command_error(requested, limits).empty(),
          "A non-finite saved position is refused");
  requested.document.saved_positions.assign(17, SavedPosition{"A", {0, 0, 0}});
  require(!operator_settings_command_error(requested, limits).empty(),
          "A seventeenth saved position is refused");
  requested.document.saved_positions = {{std::string(64, 'a'), {1, 2, 3}}};
  require(operator_settings_command_error(requested, limits).empty(),
          "A 64-byte name is accepted");
}
void go_to_safe_z() {
  OperatorSettingsFile value{"G0759", 1, command().document};
  require(!value.document.go_to_safe_z, "No safe Z for Go to by default");
  const auto unset = encode_operator_settings_file(value);
  require(unset && Json::parse(*unset)["document"]["go_to_safe_z"].is_null(),
          "An unset safe Z is written as null");
  value.document.go_to_safe_z = 40.125;
  const auto encoded = encode_operator_settings_file(value);
  const auto decoded = decode_operator_settings_file(encoded.value_or(""));
  require(decoded && decoded->document == value.document && decoded->document.go_to_safe_z == 40.125,
          "The safe Z for Go to round trips exactly");
  auto older = Json::parse(*encoded);
  older["document"].erase("go_to_safe_z");
  const auto upgraded = decode_operator_settings_file(older.dump());
  require(upgraded && !upgraded->document.go_to_safe_z &&
              upgraded->document.default_spindle_rpm == 1800,
          "Documents without a safe Z keep their other preferences and use the default");
  for (const auto& damaged : {Json("40"), Json(true), Json(1e10), Json::array({40})}) {
    auto invalid = Json::parse(*encoded);
    invalid["document"]["go_to_safe_z"] = damaged;
    const auto dropped = decode_operator_settings_file(invalid.dump());
    require(dropped && !dropped->document.go_to_safe_z &&
                dropped->document.default_spindle_rpm == 1800,
            "A damaged safe Z falls back to the default without losing other settings");
    require(!decode_operator_preferences(invalid["document"].dump()),
            "A save request with a damaged safe Z is refused");
  }
  auto requested = command();
  requested.document.go_to_safe_z = std::nan("");
  require(!operator_settings_command_error(requested, limits).empty(),
          "A non-finite safe Z is refused");
  requested.document.go_to_safe_z = -12.5;
  require(operator_settings_command_error(requested, limits).empty(),
          "A negative machine Z is a valid safe Z");
}
// A save request replaces the whole document: a damaged optional field refuses it, where a damaged
// file only falls back for that field.
void damaged_requests() {
  OperatorSettingsFile value{"G0759", 1, command().document};
  value.document.saved_positions = {{"Vise jaw", {-120.5, 33.25, -10}}};
  value.document.recent_programs = {"/home/turo/nc/a.ngc"};
  const auto document = Json::parse(encode_operator_settings_file(value).value_or("{}"))["document"];
  const auto valid = decode_operator_preferences(document.dump());
  require(valid && *valid == value.document, "A valid save request decodes exactly");
  auto older = document;
  older.erase("program_layout");
  older.erase("theme");
  older.erase("tool_reference_height");
  require(decode_operator_preferences(older.dump()).has_value(),
          "A save request may still omit the optional fields");
  auto seventeen = Json::array();
  for (int index = 0; index < 17; ++index)
    seventeen.push_back({{"name", "P"}, {"position", {0, 0, 0}}});
  for (const auto& [field, damaged] : std::initializer_list<std::pair<std::string, Json>>{
           {"saved_positions", Json("x")},
           {"saved_positions", seventeen},
           {"saved_positions", Json::array({Json{{"name", "A\n"}, {"position", {1, 2, 3}}}})},
           {"saved_positions", Json::array({Json{{"name", "A"}, {"position", {1, 2, 1e7}}}})},
           {"recent_programs", Json::array({"relative.ngc"})},
           {"recent_programs", Json("/a.ngc")},
           {"program_layout", Json(false)},
           {"program_layout", Json("sideways")},
           {"theme", Json("sepia")}}) {
    auto invalid = document;
    invalid[field] = damaged;
    const auto decoded = decode_operator_preferences(invalid.dump());
    require(!decoded, "A save request with a damaged " + field + " is refused");
  }
}
// The writer never writes a file the reader refuses: that would leave the settings unavailable.
void bounded_files() {
  auto requested = command();
  requested.document.recent_programs.assign(8, '/' + std::string(2100, 'p'));
  require(operator_settings_command_error(requested, limits).empty(),
          "Eight long recent programs are valid preferences");
  const auto encoded = encode_operator_settings_file({"G0759", 1, requested.document});
  require(!encoded, "A file over 16 KiB is not encoded");
  FakeDisk disk;
  FakeToolModelFiles files{disk};
  ToolModelArchive owner{files, location};
  owner.open();
  OperatorSettingsArchive archive{files, limits};
  archive.open(owner.shared_location());
  require(archive.apply(command()).outcome == CommandOutcome::accepted, "Initial file saved");
  const auto saved = disk.files.at(file);
  requested.base_revision = 1;
  require(archive.apply(requested).outcome == CommandOutcome::rejected &&
              disk.files.at(file) == saved && archive.settings().revision == 1,
          "A save over 16 KiB is refused and the saved file is unchanged");
  OperatorSettingsArchive restarted{files, limits};
  restarted.open(owner.shared_location());
  require(restarted.settings().availability == ToolModelAvailability::available &&
              restarted.settings().revision == 1,
          "Settings stay available after a refused large save");
}
void archive_and_restart() {
  FakeDisk disk;
  disk.files[location.directory + "/tool-models.json"] = "corrupt models";
  FakeToolModelFiles files{disk};
  ToolModelArchive owner{files, location};
  owner.open();
  OperatorSettingsArchive archive{files, limits};
  archive.open(owner.shared_location());
  const auto defaults = archive.settings();
  require(defaults.revision == 0 && defaults.document && defaults.document->default_jog_velocity == 600,
          "Missing file gives factory defaults without writing a file");
  require(!disk.files.contains(file), "No file until explicit save");
  require(archive.apply(command()).outcome == CommandOutcome::accepted,
          "Save accepted after durable replacement");
  const auto saved_settings = archive.settings();
  require(saved_settings.revision == 1 && saved_settings.document &&
              saved_settings.document->enabled_wcs == std::vector<int>{1, 9},
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
    files.failures.emplace(step, 1);
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
    const auto restored = settings.settings();
    require(restored.revision == 1 && restored.document &&
                restored.document->startup_units == StartupUnits::inch &&
                restored.document->default_jog_velocity == 88.9,
            "A fresh file handle and archive restore saved preferences");
    owner.close();
  }
}
} // namespace
auto main() -> int {
  try {
    documents_and_limits();
    saved_positions();
    go_to_safe_z();
    damaged_requests();
    bounded_files();
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
