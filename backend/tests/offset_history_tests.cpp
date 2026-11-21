#include <coffeecnc/app/messages.hpp>
#include <coffeecnc/app/offset_history.hpp>

#include "fake_tool_model_files.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace coffeecnc;
using namespace std::chrono_literals;

namespace {
using Json = nlohmann::json;
using Clock = OffsetHistoryStore::Clock;

int checks = 0;
void require(bool condition, const std::string& message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

constexpr auto directory = "/data/G0759";
constexpr auto file = "/data/G0759/offset-history.json";
constexpr auto epoch = 1700000000000ms;
const Clock::time_point start{};

// A live, idle machine in G54 with two tools.
auto reading(Vec3 g54 = {10, 20, -30}) -> PanelReading {
  PanelReading sample;
  sample.status.link = Link::live;
  sample.status.machine = MachineState::on;
  sample.status.offsets.wcs = 1;
  sample.status.offsets.g5x = g54;
  sample.tool_table = {1, {{3, 3, 48.2, 6, "End mill 6 mm"}, {4, 4, 61.75, 3.175, ""}}};
  return sample;
}

struct Fixture {
  FakeDisk disk;
  FakeToolModelFiles files{disk};
  std::unique_ptr<OffsetHistoryStore> store;

  Fixture() { open(); }
  void open() {
    store.reset();
    disk.directories.insert(directory);
    store = std::make_unique<OffsetHistoryStore>(files, directory);
  }
  // One reading at `elapsed`; what the store set on it.
  auto observe(PanelReading sample, std::chrono::milliseconds elapsed = 0ms) -> PanelReading {
    store->observe(sample, epoch + elapsed, start + elapsed);
    return sample;
  }
  [[nodiscard]] auto history() const -> OffsetHistory { return *store->snapshot(); }
  [[nodiscard]] auto newest() const -> HistoryEntry {
    const auto entries = history().entries;
    require(!entries.empty(), "The history has an entry");
    return entries.front();
  }
  [[nodiscard]] auto saved() const -> Json {
    const auto found = disk.files.find(file);
    return found == disk.files.end() ? Json() : Json::parse(found->second);
  }
};

void first_reading_is_no_entry() {
  Fixture f;
  const auto shown = f.observe(reading());
  require(shown.offset_history != nullptr && shown.offset_history->entries.empty() &&
              shown.offset_history->saved && shown.offset_history->reason.empty(),
          "The first values are learnt, not recorded");
  require(f.saved().at("version") == 1 && f.saved().at("entries").empty() &&
              f.saved().at("seen").at("systems").at(0).at(0) == 10 &&
              f.saved().at("seen").at("tools").size() == 2,
          "What was seen is written: " + f.saved().dump());
  const auto writes = f.files.count("rename");
  auto g55 = reading({1, 2, 3});
  g55.status.offsets.wcs = 2;
  f.observe(g55, 500ms);
  require(f.files.count("rename") == writes, "Learning more waits for the write period");
  f.observe(g55, 1500ms);
  require(f.files.count("rename") == writes + 1 &&
              f.saved().at("seen").at("systems").at(1).at(2) == 3,
          "and is written after it");
}

void outside_change_and_attribution() {
  Fixture f;
  f.observe(reading());
  f.observe(reading({12.5, 20, -30}), 100ms);
  auto entry = f.newest();
  require(entry.source == HistorySource::outside && entry.target == HistoryTarget::wcs &&
              entry.number == 1 && entry.changes.size() == 1 &&
              entry.changes[0].field == HistoryField::x && entry.changes[0].before == 10 &&
              entry.changes[0].after == 12.5 && entry.time == (epoch + 100ms).count() &&
              !entry.restores,
          "A change without a panel command is outside the panel");
  require(f.saved().at("entries").size() == 1, "An entry is written at once");

  // A panel command: while it runs and for the grace after it.
  const auto mark = f.store->begin(HistorySource::zero);
  f.observe(reading({0, 20, -30}), 200ms);
  require(f.newest().source == HistorySource::zero, "Seen while the command runs");
  f.store->end(mark, start + 300ms);
  f.observe(reading({0, 5, -30}), 2000ms);
  require(f.newest().source == HistorySource::zero, "Seen in the grace after it returned");
  f.observe(reading({0, 5, -31}), 2400ms);
  require(f.newest().source == HistorySource::outside, "After the grace it is outside again");

  // MDI still executing keeps its command responsible.
  const auto mdi = f.store->begin(HistorySource::mdi);
  f.store->end(mdi, start + 3000ms);
  auto busy = reading({0, 5, -31});
  busy.status.mode = TaskMode::mdi;
  busy.status.interp = InterpState::reading;
  f.observe(busy, 4900ms);
  busy.status.offsets.g5x = {1, 5, -31};
  f.observe(busy, 6000ms);
  require(f.newest().source == HistorySource::mdi, "An executing MDI block extends the grace");

  // A program run, and the grace after it.
  auto running = reading({1, 5, -31});
  running.status.mode = TaskMode::automatic;
  running.status.interp = InterpState::reading;
  f.observe(running, 20000ms);
  running.status.offsets.g92 = {0, 0, 2};
  f.observe(running, 20100ms);
  entry = f.newest();
  require(entry.source == HistorySource::program && entry.target == HistoryTarget::g92 &&
              entry.number == 0 && entry.changes.size() == 1 &&
              entry.changes[0].field == HistoryField::z && entry.changes[0].after == 2,
          "A G92 of a running program is the program's");
  auto ended = reading({1, 5, -31});
  ended.status.offsets.g92 = {0, 0, 0};
  f.observe(ended, 21000ms);
  require(f.newest().source == HistorySource::program, "Seen just after the program ended");

  // Rotation is a field of the work system.
  auto rotated = ended;
  rotated.status.offsets.rotation_xy = 15;
  f.observe(rotated, 30000ms);
  entry = f.newest();
  require(entry.changes.size() == 1 && entry.changes[0].field == HistoryField::rotation_xy &&
              entry.changes[0].before == 0 && entry.changes[0].after == 15,
          "XY rotation is recorded");

  // Not a change: noise below the tolerance, a link that is not live, another system.
  const auto count = f.history().entries.size();
  auto noise = rotated;
  noise.status.offsets.g5x[0] += 1e-9;
  f.observe(noise, 31000ms);
  auto down = noise;
  down.status.link = Link::down;
  down.status.offsets.g5x = {};
  down.tool_table.tools.clear();
  f.observe(down, 32000ms);
  require(f.history().entries.size() == count, "Noise and a link that is down are no entries");
}

void inactive_systems() {
  Fixture f;
  f.observe(reading());
  const auto mark = f.store->begin(HistorySource::work_offset_table);
  f.store->written(mark, 2, {25.4, -50.8, 0, std::nullopt}, epoch + 10ms);
  auto entry = f.newest();
  require(entry.source == HistorySource::work_offset_table && entry.number == 2 &&
              entry.changes.size() == 3 && !entry.changes[0].before &&
              entry.changes[0].after == 25.4,
          "A system never seen has no earlier values");
  f.store->written(mark, 2, {30, -50.8, 0, std::nullopt}, epoch + 20ms);
  entry = f.newest();
  require(entry.changes.size() == 1 && entry.changes[0].before == 25.4 &&
              entry.changes[0].after == 30,
          "What the panel wrote is known from then on");
  f.store->end(mark, start + 30ms);
  // Selected later: rotation learnt silently, XYZ as written.
  auto g55 = reading({30, -50.8, 0});
  g55.status.offsets.wcs = 2;
  f.observe(g55, 5000ms);
  require(f.history().entries.size() == 2, "Selecting it shows what the history knew");
  // The active system is left to the readings.
  const auto active = f.store->begin(HistorySource::work_offset_table);
  f.store->written(active, 2, {1, 2, 3, std::nullopt}, epoch + 6000ms);
  require(f.history().entries.size() == 2, "A write to the active system waits for a reading");
  g55.status.offsets.g5x = {1, 2, 3};
  f.observe(g55, 6100ms);
  require(f.history().entries.size() == 3 &&
              f.newest().source == HistorySource::work_offset_table,
          "The reading records it, with the command that wrote it");
  f.store->end(active, start + 6200ms);
}

void tools() {
  Fixture f;
  f.observe(reading());
  auto edited = reading();
  edited.tool_table.tools[0].length = 50;
  edited.tool_table.tools.push_back({7, 7, 25.4, 12.7, ""});
  edited.tool_table.tools.erase(edited.tool_table.tools.begin() + 1);
  const auto mark = f.store->begin(HistorySource::tool_table);
  f.observe(edited, 100ms);
  f.store->end(mark, start + 200ms);
  const auto entries = f.history().entries;
  require(entries.size() == 3, "One entry per tool that changed");
  // Newest first: the removal of T4 was found last.
  require(entries[0].number == 4 && entries[0].changes.size() == 2 &&
              entries[0].changes[0].before == 61.75 && !entries[0].changes[0].after,
          "A removed tool has no `after`");
  require(entries[1].number == 7 && !entries[1].changes[0].before &&
              entries[1].changes[0].after == 25.4 && entries[1].changes[1].after == 12.7,
          "An added tool has no `before`");
  require(entries[2].number == 3 && entries[2].changes.size() == 1 &&
              entries[2].changes[0].field == HistoryField::length &&
              entries[2].changes[0].before == 48.2 && entries[2].changes[0].after == 50 &&
              entries[2].source == HistorySource::tool_table,
          "A changed length");
  // An unsettled pass, a table not accepted yet and T0 are not looked at.
  auto unaccepted = edited;
  unaccepted.tool_table = {};
  f.observe(unaccepted, 4000ms);
  auto unsettled = edited;
  unsettled.tool_table.tools[0].diameter = 99;
  unsettled.tools_settled = false;
  f.observe(unsettled, 5000ms);
  auto spindle = edited;
  spindle.tool_table.tools.push_back({0, 0, 1, 1, ""});
  f.observe(spindle, 6000ms);
  require(f.history().entries.size() == 3,
          "Unsettled passes, a table not accepted yet and T0 make no entries");
  // A withheld table counts with the accepted rows.
  auto withheld = edited;
  withheld.accepted_tools = withheld.tool_table.tools;
  withheld.accepted_tools->at(0).diameter = 8;
  withheld.tool_table.tools.clear();
  f.observe(withheld, 7000ms);
  require(f.newest().number == 3 && f.newest().changes[0].field == HistoryField::diameter,
          "The accepted rows of a withheld table are compared");
}

void bounded_and_ordered() {
  Fixture f;
  f.observe(reading());
  for (int i = 1; i <= 250; ++i)
    f.observe(reading({10.0 + i, 20, -30}), std::chrono::milliseconds{i * 10});
  const auto entries = f.history().entries;
  require(entries.size() == OffsetHistoryStore::max_entries, "At most 200 entries");
  require(entries.front().id == 250 && entries.back().id == 51 &&
              entries.front().changes[0].after == 260,
          "Newest first; the oldest are dropped");
  for (std::size_t i = 1; i < entries.size(); ++i)
    require(entries[i - 1].id == entries[i].id + 1, "Ids go down by one");
  require(f.saved().at("entries").size() == 200 && f.saved().at("next_id") == 251,
          "The file holds the same bound");
}

void persistence() {
  Fixture f;
  f.observe(reading());
  f.observe(reading({11, 20, -30}), 100ms);
  f.observe(reading({12, 20, -30}), 200ms);
  const auto before = f.history().entries;
  f.open();
  require(f.history().entries == before && f.history().reason.empty(),
          "A new store reads the same entries");
  // A change made while CoffeeCNC did not run.
  f.observe(reading({13, 20, -30}), 0ms);
  const auto entry = f.newest();
  require(entry.id == 3 && entry.source == HistorySource::outside &&
              entry.changes[0].before == 12 && entry.changes[0].after == 13,
          "What was seen is kept: an offline change is an outside entry, ids go on");
  require(f.store->entry(2) == before[0] && !f.store->entry(99),
          "Entries are found by id");
  require(f.files.count("rename") >= 2 && f.files.count("sync_directory") >= 2 &&
              !f.disk.files.contains(std::string(file) + ".tmp"),
          "Every write replaces the file durably");
}

void damaged_files() {
  for (const auto& [content, what] : std::vector<std::pair<std::string, std::string>>{
           {"{not json", "not JSON"},
           {R"({"version":2,"entries":[],"next_id":1,"seen":{}})", "another version"},
           {R"({"version":1,"entries":[{"id":1}],"next_id":2,"seen":{}})", "an entry cut short"},
           {R"({"version":1,"entries":[],"next_id":1})", "no seen state"},
           {std::string(OffsetHistoryStore::max_file_bytes + 1, ' '), "too large"}}) {
    Fixture f;
    f.disk.files[file] = content;
    f.open();
    const auto history = f.history();
    require(history.entries.empty() && history.reason.find("damaged") != std::string::npos,
            "A damaged file starts empty and says so: " + what);
    require(f.disk.files.at("/data/G0759/offset-history.json.corrupt") == content &&
                !f.disk.files.contains(file),
            "The damaged file is kept aside: " + what);
    auto first = f.observe(reading());
    auto second = f.observe(reading(), 100ms);
    require(first.messages.size() == 1 && first.messages[0].error &&
                first.messages[0].text == history.reason && second.messages.empty(),
            "The operator is told once: " + what);
    f.observe(reading({1, 2, 3}), 200ms);
    require(f.saved().at("entries").size() == 1, "The history works on: " + what);
  }
  Fixture unreadable;
  unreadable.files.failures["read"] = 1;
  unreadable.open();
  require(unreadable.history().reason.find("cannot be read") != std::string::npos,
          "A file that cannot be read is said");
}

void failed_writes() {
  Fixture f;
  f.observe(reading());
  f.files.failures["rename"] = 1;
  auto failed = f.observe(reading({1, 2, 3}), 100ms);
  require(failed.messages.size() == 1 &&
              failed.messages[0].text.find("could not be saved") != std::string::npos &&
              f.history().reason == failed.messages[0].text && f.saved().at("entries").empty(),
          "A failed write is said once, in the message and the history");
  auto quiet = f.observe(reading({1, 2, 3}), 200ms);
  require(quiet.messages.empty(), "Not repeated");
  f.observe(reading({1, 2, 3}), 1200ms);
  require(f.saved().at("entries").size() == 1 && f.history().reason.empty(),
          "Written again after the period; the reason is cleared");
}

void session_only() {
  FakeDisk disk;
  FakeToolModelFiles files{disk};
  OffsetHistoryStore store{files, "", "No machine identity configured"};
  PanelReading sample = reading();
  store.observe(sample, epoch, start);
  sample = reading({5, 5, 5});
  store.observe(sample, epoch + 2000ms, start + 2000ms);
  const auto history = *store.snapshot();
  require(!history.saved && history.reason == "No machine identity configured" &&
              history.entries.size() == 1 && files.calls.empty(),
          "Without a directory the history is kept in memory only");
}

void encoding() {
  Fixture f;
  f.observe(reading());
  f.observe(reading({1, 20, -30}), 100ms);
  const auto encoded = encode_offset_history(f.history());
  require(encoded.has_value(), "The history encodes");
  const auto message = Json::parse(*encoded);
  require(message.at("type") == "offset_history" && message.at("storage") == "saved" &&
              message.at("entries").at(0).at("source") == "outside" &&
              message.at("entries").at(0).at("changes").at(0).at("field") == "x" &&
              message.at("entries").at(0).at("restores").is_null(),
          "Wire form: " + *encoded);
  require(f.saved().at("entries").at(0) == message.at("entries").at(0),
          "The file keeps entries in their wire form");
}

} // namespace

auto main() -> int {
  try {
    first_reading_is_no_entry();
    outside_change_and_attribution();
    inactive_systems();
    tools();
    bounded_and_ordered();
    persistence();
    damaged_files();
    failed_writes();
    session_only();
    encoding();
    std::cout << checks << " offset history checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Check " << checks << " failed: " << error.what() << '\n';
    return 1;
  }
}
