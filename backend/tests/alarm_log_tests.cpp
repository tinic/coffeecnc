#include <coffeecnc/app/alarm_log.hpp>
#include <coffeecnc/app/response.hpp>

#include "fake_tool_model_files.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <utility>
#include <vector>

using namespace coffeecnc;
using namespace std::chrono_literals;

namespace {

const LocalizedReason no_machine{"No machine identity is configured ([COFFEECNC] MACHINE_ID): tool "
                                 "models last until the panel closes.",
                                 MessageKey{"backend.toolModelStore.noMachineId"}};
using Json = nlohmann::json;

int checks = 0;
void require(bool condition, const std::string& message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

constexpr auto directory = "/data/G0759";
constexpr auto file = "/data/G0759/alarm-log.jsonl";
constexpr auto older = "/data/G0759/alarm-log.1.jsonl";
constexpr auto oldest = "/data/G0759/alarm-log.2.jsonl";
constexpr auto epoch = 1700000000000ms;
constexpr PageToken page{1};

struct Fixture {
  FakeDisk disk;
  FakeToolModelFiles files{disk};
  std::mutex delivered_mutex;
  std::vector<std::pair<PageToken, std::string>> delivered;
  std::unique_ptr<AlarmLog> log;

  Fixture() {
    disk.directories.insert(directory);
    open();
  }
  void open(std::string at = directory) {
    log.reset();
    log = std::make_unique<AlarmLog>(
        files, std::move(at), no_machine,
        [this](PageToken token, std::vector<std::string> messages) {
          const std::scoped_lock lock{delivered_mutex};
          std::ranges::transform(
              messages, std::back_inserter(delivered),
              [token](std::string& message) { return std::pair{token, std::move(message)}; });
        });
    log->settle();
  }
  // An operator message posted to the page.
  void message(const std::string& text, std::chrono::milliseconds at = epoch,
               MessageLevel level = MessageLevel::error) {
    const std::vector messages{encode_message(level, text, at)};
    log->posted(page, messages, at);
    log->settle();
  }
  // A request and its result.
  void command(RequestId id, const std::string& request, CommandOutcome outcome,
               const std::string& reason) {
    log->request(page, request);
    const std::vector messages{encode_result(id, outcome, reason)};
    log->posted(page, messages, epoch);
    log->settle();
  }
  [[nodiscard]] auto texts() const -> std::vector<std::string> {
    std::vector<std::string> out;
    std::ranges::transform(log->view().entries, std::back_inserter(out), &AlarmRecord::text);
    return out;
  }
  [[nodiscard]] auto lines(const std::string& path) const -> std::vector<Json> {
    std::vector<Json> out;
    const auto found = disk.files.find(path);
    if (found == disk.files.end())
      return out;
    std::istringstream stream{found->second};
    for (std::string line; std::getline(stream, line);)
      if (!line.empty())
        out.push_back(Json::parse(line));
    return out;
  }
};

void records_messages_and_refusals() {
  Fixture f;
  f.message("Joint 0 following error");
  f.message("Program loaded", epoch, MessageLevel::info);
  f.command(4, R"({"type":"home","axis":"x","id":4})", CommandOutcome::rejected,
            "The machine is off.");
  f.command(5, R"({"type":"pause","id":5})", CommandOutcome::accepted, "");
  f.command(6, R"({"type":"mdi","text":"G1 X10","id":6})", CommandOutcome::failed, "No feed rate.");
  const auto saved = f.lines(file);
  require(saved.size() == 4, std::format("Four records are on the disk: {}", saved.size()));
  require(saved[0]["text"] == "Joint 0 following error" && saved[0]["level"] == "error" &&
              saved[0]["outcome"].is_null() && saved[0]["command"].is_null() &&
              saved[0]["session"] == 1 && saved[0]["v"] == 1 && saved[0]["time"] == epoch.count(),
          "An operator message is recorded with its time");
  require(saved[1]["level"] == "info", "An info message is recorded");
  require(saved[2]["outcome"] == "rejected" && saved[2]["text"] == "The machine is off." &&
              saved[2]["command"] == Json({{"type", "home"}, {"axis", "x"}}),
          "A refusal is recorded with its command, without the id");
  require(saved[3]["outcome"] == "failed" && saved[3]["command"]["text"] == "G1 X10",
          "A failure is recorded; an accepted result is not");
  // A long command keeps its type only.
  const auto text = std::string(2000, 'G');
  f.command(7, std::format(R"({{"type":"save_program","path":"/a","text":"{}","id":7}})", text),
            CommandOutcome::rejected, "Not now.");
  require(f.lines(file).back()["command"] == Json({{"type", "save_program"}}),
          "A long command is kept as its type");
  // Status and other messages are not recorded.
  const std::vector others{std::string(R"({"type":"status","seq":1})"),
                           std::string(R"({"type":"directory","text":"\"type\":\"message\""})")};
  f.log->posted(page, others, epoch);
  f.log->settle();
  require(f.lines(file).size() == 5, "Other messages are not recorded");
  // Not UTF-8: replaced, never refused.
  f.message(std::string("bad \xff byte"));
  require(f.lines(file).size() == 6, "Text that is not UTF-8 is recorded");
}

void localization_survives_reload() {
  Fixture f;
  f.message("Joint 0 following error");
  const MessageKey contact{"backend.jogStop.probeContact"};
  const RefusalMessage refusal{"messages.policy.jogVelocityAxisLimit",
                               {{"axis", "Z"}, {"limit", "750"}}};
  const MessageKey ignored{"messages.session.requestIgnored",
                           {{"detail", "Missing required field: id."}},
                           {{"detail", {"messages.request.missingField", {{"field", "id"}}}}}};
  const std::vector messages{
      encode_message(MessageLevel::info, "Jog stopped: probe contact", epoch, contact),
      encode_result(1, CommandOutcome::rejected,
                    "Jog velocity is above the Z axis limit of 750 mm/min.", {}, {}, refusal),
      encode_message(MessageLevel::error, "Panel request ignored: Missing required field: id.",
                     epoch, ignored),
      std::string{
          R"({"type":"message","level":"error","text":"Keep this text","localization":{"key":"invalid","params":{"x":7}}})"},
      std::string{
          R"({"type":"message","level":"error","text":"Panel request ignored: Bad packet.","localization":{"key":"messages.session.requestIgnored","params":{"detail":"Bad packet."},"params_localization":{"detail":{"key":"messages.request.missingField","params":{"field":7}}}}})"}};
  f.log->posted(page, messages, epoch);
  f.log->settle();
  f.open();
  const auto view = f.log->view();
  require(view.entries.size() == 6 && view.skipped == 0, "Both older and localized records reload");
  require(!view.entries[0].localization && view.entries[1].localization == contact &&
              view.entries[2].localization == refusal && view.entries[3].localization == ignored &&
              !view.entries[4].localization && view.entries[4].text == "Keep this text" &&
              view.entries[5].localization ==
                  MessageKey{"messages.session.requestIgnored", {{"detail", "Bad packet."}}},
          "Known metadata survives reload and invalid metadata does not discard English text");
  const auto wire = Json::parse(encode_alarm_log(view)).at("entries");
  require(wire[1].at("localization").at("key") == contact.key &&
              wire[2].at("localization").at("params").at("axis") == "Z" &&
              wire[3].at("localization").at("params_localization").at("detail") ==
                  Json{{"key", "messages.request.missingField"}, {"params", {{"field", "id"}}}},
          "The alarm log provides keys and arguments to the next page");
}

void snapshot_has_what_came_before() {
  Fixture f;
  f.message("before the page");
  f.log->send_snapshot(page);
  f.log->settle();
  f.message("after the page");
  require(f.delivered.size() == 1, "One view is delivered");
  const auto view = Json::parse(f.delivered.front().second);
  require(f.delivered.front().first == page && view["type"] == "alarm_log" &&
              view["storage"] == "saved" && view["session"] == 1 && view["skipped"] == 0 &&
              view["entries"].size() == 1 && view["entries"][0]["text"] == "before the page",
          "The view has what was told before the page, not what is told live after it");
}

void ordering_across_restarts() {
  Fixture f;
  f.message("first run", epoch + 10s);
  f.open();
  // The clock went back between the runs: the order is the file's.
  f.message("second run", epoch);
  f.open();
  f.message("third run", epoch + 5s);
  const auto view = f.log->view();
  require(view.session == 3, "The third run is session 3");
  require(f.texts() == std::vector<std::string>{"first run", "second run", "third run"},
          "The view is in the order of the runs, whatever the clock said");
  require(view.entries[0].session == 1 && view.entries[1].session == 2 &&
              view.entries[2].session == 3,
          "Each entry keeps its run");
}

void corruption_tolerance() {
  Fixture f;
  f.message("kept one");
  f.log.reset();
  // A torn write, garbage, a record of a later version and a valid one by hand.
  f.disk.files[file] += R"({"v":1,"session":1,"time":5,"level":"error","text":"torn)";
  f.disk.files[file] += "\nnot json at all\n";
  f.disk.files[file] += R"({"v":2,"session":1,"time":5,"level":"error","text":"later"})"
                        "\n";
  f.disk.files[file] += R"({"v":1,"session":"x","time":5,"level":"error","text":"wrong type"})"
                        "\n";
  f.disk.files[file] += R"({"v":1,"session":1,"time":6,"level":"info","text":"by hand"})";
  f.open();
  auto view = f.log->view();
  require(view.skipped == 4, std::format("Four lines are skipped: {}", view.skipped));
  require(f.texts() == std::vector<std::string>{"kept one", "by hand"},
          "Readable lines are kept, in their order");
  require(view.session == 2, "The session follows the highest one read");
  f.message("after a torn end");
  const auto& bytes = f.disk.files[file];
  require(bytes.find("\"by hand\"}\n{") != std::string::npos,
          "A file without a final line break gets one before the next record");
  f.open();
  require(f.log->view().skipped == 4 && f.texts().back() == "after a torn end",
          "The record after the torn end reads back");
  // An unreadable file: the log goes on, and says so.
  f.files.failures["read " + std::string(file)] = 1;
  f.open();
  view = f.log->view();
  require(view.reason.find("cannot be read") != std::string::npos &&
              view.localization ==
                  MessageKey{"backend.alarmLog.cannotRead",
                             {{"file", file}, {"detail", "it cannot be read: Input/output error"}}},
          "An unreadable file reports its name and the read error");
}

void rotation() {
  Fixture f;
  const std::string text(1000, 'r');
  std::size_t written = 0;
  // Enough for more than three files.
  while (!f.disk.files.contains(oldest) || written < 4 * AlarmLog::max_file_bytes / 1000) {
    f.message(std::format("{} {}", written, text));
    ++written;
  }
  for (const auto* name : {file, older, oldest})
    require(f.disk.files[name].size() <= AlarmLog::max_file_bytes,
            std::format("{} stays within the bound", name));
  require(f.disk.files.size() <= 4, "At most three log files and the lock"); // no lock here
  // The newest records are in the newest file, the oldest ones are gone.
  const auto newest = f.lines(file).back()["text"].get<std::string>();
  require(newest.starts_with(std::format("{} ", written - 1)), "The newest record is last");
  const auto first_kept = f.lines(oldest).front()["text"].get<std::string>();
  require(!first_kept.starts_with("0 "), "The oldest records were rotated away");
  f.open();
  const auto view = f.log->view();
  require(view.entries.size() == AlarmLog::max_entries && view.skipped == 0,
          "After a restart the view holds the newest max_entries");
  require(view.entries.back().text.starts_with(std::format("{} ", written - 1)),
          "The view ends with the newest record");
  // A rotation that fails keeps the records and is said.
  f.files.failures["rename"] = 1;
  while (f.disk.files[file].size() + 1200 <= AlarmLog::max_file_bytes)
    f.message("filler " + text);
  f.message("rotation fails " + std::string(1200, 'f'));
  require(f.log->view().reason.find("rotated") != std::string::npos, "A failed rotation is said");
  f.log->posted(page, std::vector{encode_message(MessageLevel::error, "next", epoch)}, epoch);
  f.log.reset(); // the last append of the run
  const auto rotated = f.lines(file);
  require(rotated.size() == 2 &&
              rotated[0]["text"].get<std::string>().starts_with("rotation fails") &&
              rotated[1]["text"] == "next" &&
              f.lines(older).back()["text"].get<std::string>().starts_with("filler"),
          "The waiting records are written once the rotation works");
}

void full_disk() {
  Fixture f;
  f.message("before");
  f.files.failures["append"] = 3;
  f.message("while full 1");
  auto view = f.log->view();
  require(view.reason.find("No space left on device") != std::string::npos &&
              view.localization ==
                  MessageKey{"backend.alarmLog.notSaved",
                             {{"detail", "the file cannot be written: No space left on device"}}},
          "A full disk reports its storage error");
  require(f.texts().back() == "while full 1", "The view still has what was not saved");
  f.message("while full 2"); // a new record tries again at once: fails
  // Space again: a retry or the next record writes what waited, in order.
  f.files.failures.clear();
  f.message("after");
  view = f.log->view();
  require(view.reason.empty() && !view.localization,
          "The reason and key clear once the log is saved again");
  f.open();
  require(f.texts() == std::vector<std::string>{"before", "while full 1", "while full 2", "after"},
          "Nothing is lost or repeated, and the torn halves are skipped");
  require(f.log->view().skipped >= 1, "The half-written lines are skipped as damaged");
  // Bounded while the disk stays full.
  f.files.failures["append"] = 1000000;
  for (std::size_t i = 0; i < AlarmLog::max_pending + 5; ++i)
    f.log->posted(page, std::vector{encode_message(MessageLevel::error, std::to_string(i), epoch)},
                  epoch);
  f.log->settle();
  require(f.log->view().entries.size() == AlarmLog::max_entries, "The view stays bounded");
  f.files.failures.clear();
  f.log.reset();
  f.open();
  require(f.texts().back() == std::to_string(AlarmLog::max_pending + 4),
          "The newest waiting records are written at the end of the run");
}

void session_only() {
  Fixture f;
  f.open("");
  f.message("kept in memory");
  const auto view = f.log->view();
  require(!view.saved && view.reason == no_machine.text &&
              view.localization == no_machine.localization && view.session == 1 &&
              f.texts() == std::vector<std::string>{"kept in memory"},
          "Without a directory the log is kept for this run");
  require(std::ranges::none_of(f.files.calls,
                               [](const std::string& call) { return call.starts_with("append"); }),
          "Nothing is written");
  f.log->send_snapshot(page);
  f.log->settle();
  require(Json::parse(f.delivered.back().second)["storage"] == "session_only",
          "The view says session_only");
}

// A record line as a run writes it.
auto record_line(const std::string& text, std::uint64_t number) -> std::string {
  return Json{{"v", 1},           {"n", number},  {"session", 1},       {"time", epoch.count()},
              {"level", "error"}, {"text", text}, {"outcome", nullptr}, {"command", nullptr}}
             .dump() +
         '\n';
}

// The current file holds a record with this text.
auto saved_text(const FakeDisk& disk, const std::string& text) -> bool {
  const auto found = disk.files.find(file);
  return found != disk.files.end() &&
         found->second.find(std::format(R"("text":"{}")", text)) != std::string::npos;
}

// Review #270, 1: the bytes of failed appends were counted, so retries on a full disk rotated
// the files again and again, and the renames, which work on a full disk, pushed the older
// files out.
void full_disk_keeps_older_files() {
  Fixture f;
  f.log.reset();
  f.disk.files[oldest] = record_line("oldest record", 1);
  f.disk.files[older] = record_line("older record", 2);
  f.disk.files[file] = record_line("current record", 3);
  const auto kept_oldest = f.disk.files[oldest];
  const auto kept_older = f.disk.files[older];
  f.open();
  f.files.failures["append"] = 1000000;
  const std::string text(1000, 'd');
  // Enough for the counted bytes of the failed appends to pass max_file_bytes; few enough that
  // the halves the fake writes stay within what a start reads.
  for (int i = 0; i < 40; ++i)
    f.message(std::format("{} {}", i, text)); // each is a retry of all that waits: ENOSPC
  require(f.log->view().reason.find("No space left on device") != std::string::npos,
          "The full disk is said");
  require(f.files.count("rename") == 0,
          std::format("Nothing is rotated while the disk is full: {} renames",
                      f.files.count("rename")));
  require(f.disk.files[oldest] == kept_oldest && f.disk.files[older] == kept_older,
          "The older files are as they were");
  f.files.failures.clear();
  f.message("space again");
  f.open();
  const auto texts = f.texts();
  require(texts.size() == 44 && texts[0] == "oldest record" && texts[1] == "older record" &&
              texts[2] == "current record" && texts[3].starts_with("0 ") &&
              texts[42].starts_with("39 ") && texts[43] == "space again",
          std::format("The whole history is kept: {} entries", texts.size()));
}

// Review #270, 2: add() dropped the oldest waiting records while a batch was written, and the
// writer then erased as many records from the front as the batch had: new ones it never wrote.
void batch_erase_by_number() {
  Fixture f;
  f.files.failures["append"] = 1000000;
  std::vector<std::string> many;
  many.reserve(AlarmLog::max_pending);
  for (std::size_t i = 0; i < AlarmLog::max_pending; ++i)
    many.push_back(encode_message(MessageLevel::error, std::format("w{}", i), epoch));
  f.log->posted(page, many, epoch);
  f.log->settle();
  f.files.failures.clear();
  bool posted = false;
  f.files.during = [&](std::string_view step) {
    if (step != "append" || std::exchange(posted, true))
      return;
    // The queue is full: each of these drops the oldest one, which is in the batch.
    std::vector<std::string> late;
    late.reserve(3);
    for (int i = 0; i < 3; ++i)
      late.push_back(encode_message(MessageLevel::error, std::format("late {}", i), epoch));
    f.log->posted(page, late, epoch);
  };
  f.message("trigger");
  f.files.during = nullptr;
  // The failed append left a torn line: read line by line, skipping it.
  std::istringstream stream{f.disk.files[file]};
  std::vector<std::string> texts;
  for (std::string line; std::getline(stream, line);)
    if (const auto value = Json::parse(line, nullptr, false); value.is_object())
      texts.push_back(value["text"].get<std::string>());
  std::vector<std::string> late;
  std::ranges::copy_if(texts, std::back_inserter(late),
                       [](const std::string& text) { return text.starts_with("late "); });
  require(late == std::vector<std::string>{"late 0", "late 1", "late 2"},
          std::format("What was added during the write is written after it: {}", late.size()));
  require(!texts.empty() && texts.back() == "late 2", "In the order it was posted");
}

// A stream buffer that blocks every write until it is opened: a stderr nobody reads.
class BlockedStream final : public std::streambuf {
public:
  void open() {
    {
      const std::scoped_lock lock{mutex_};
      open_ = true;
    }
    changed_.notify_all();
  }
  auto wait_for_writer(std::chrono::milliseconds limit) -> bool {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, limit, [this] { return entered_; });
  }

protected:
  auto overflow(int_type c) -> int_type override {
    block();
    return traits_type::not_eof(c);
  }
  auto xsputn(const char_type* /*text*/, std::streamsize count) -> std::streamsize override {
    block();
    return count;
  }

private:
  void block() {
    std::unique_lock lock{mutex_};
    entered_ = true;
    changed_.notify_all();
    changed_.wait(lock, [this] { return open_; });
  }
  std::mutex mutex_;
  std::condition_variable changed_;
  bool entered_{};
  bool open_{};
};

// Review #270, 3: the reason was written to stderr under the mutex, so a blocked stderr
// blocked every thread that posts.
void blocked_stderr_does_not_block_posting() {
  Fixture f;
  BlockedStream blocked;
  auto* const saved = std::cerr.rdbuf(&blocked);
  f.files.failures["append"] = 1;
  f.log->posted(page, std::vector{encode_message(MessageLevel::error, "to a full disk", epoch)},
                epoch);
  const bool writing = blocked.wait_for_writer(10s);
  auto poster = std::async(std::launch::async, [&f] {
    f.log->posted(page, std::vector{encode_message(MessageLevel::error, "meanwhile", epoch)},
                  epoch);
    return f.log->view().entries.size();
  });
  const bool returned = poster.wait_for(5s) == std::future_status::ready;
  blocked.open();
  poster.wait();
  f.log->settle();
  std::cerr.rdbuf(saved);
  require(writing, "The writer says the reason on stderr");
  require(returned, "Posting does not wait for a stderr that blocks");
}

// Review #270, 4: the log wrote and rotated without the data directory's lock.
void writes_only_under_the_lock() {
  const auto noop = [](PageToken, const std::vector<std::string>&) {};
  const auto writes = [](const FakeToolModelFiles& files) {
    return files.count("append") + files.count("rename");
  };
  FakeDisk disk;
  disk.directories.insert(directory);
  FakeToolModelFiles other{disk};
  require(other.lock(directory).has_value(), "Another CoffeeCNC holds the lock");
  FakeToolModelFiles files{disk};
  {
    AlarmLog log{files, directory, {}, noop};
    log.posted(page, std::vector{encode_message(MessageLevel::error, "not saved", epoch)}, epoch);
    log.settle();
    const auto view = log.view();
    require(!view.saved && view.reason.find("in use by another CoffeeCNC") != std::string::npos &&
                view.localization ==
                    MessageKey{"backend.alarmLog.keptForRun",
                               {{"directory", directory},
                                {"detail", "it is in use by another CoffeeCNC."}}} &&
                view.entries.size() == 1,
            "Without the lock the log is kept for this run only, and says why: " + view.reason);
  }
  require(writes(files) == 0 && !disk.files.contains(file),
          "Nothing is written in a directory another CoffeeCNC holds");
  other.unlock();
  {
    AlarmLog log{files, directory, {}, noop, [] {
                   return std::optional<std::string>{"the tool models could not lock it"};
                 }};
    log.posted(page, std::vector{encode_message(MessageLevel::error, "not saved", epoch)}, epoch);
    log.settle();
    require(!log.view().saved && writes(files) == 0 && files.count("lock") == 1,
            "A lock that the caller does not hold is not taken over, and nothing is written");
  }
  // The application's case: the tool models hold the lock for the alarm log.
  FakeToolModelFiles store_files{disk};
  bool appended_unlocked = false;
  files.during = [&](std::string_view step) {
    if (step == "append" && !disk.locked.contains(directory))
      appended_unlocked = true;
  };
  {
    ToolModelStore store{
        store_files, ToolModelLocation{"G0759", ToolModelAvailability::available, "", directory},
        [](PageToken, const std::vector<std::string>&) {}};
    AlarmLog log{files, directory, {}, noop, [&store]() -> std::optional<std::string> {
                   const auto held = store.held_location();
                   if (held.availability == ToolModelAvailability::available)
                     return std::nullopt;
                   return held.reason;
                 }};
    log.posted(page, std::vector{encode_message(MessageLevel::error, "saved", epoch)}, epoch);
    log.settle();
    require(log.view().saved && saved_text(disk, "saved"), "Under the lock it is saved");
    store.stop();
    require(disk.locked.contains(directory), "A stopped store still holds the lock");
    log.posted(page, std::vector{encode_message(MessageLevel::error, "at the end", epoch)}, epoch);
  } // the log goes first: its last append
  files.during = nullptr;
  require(saved_text(disk, "at the end") && !appended_unlocked,
          "The last append of the run is still under the lock");
  require(!disk.locked.contains(directory), "The lock goes with the store");
}

// Linux: what append says on a device that is full.
void real_enospc() {
#ifdef __linux__
  LocalToolModelFiles files;
  const auto done = files.append("/dev/full", "a line\n");
  require(!done && done.error().find("No space left on device") != std::string::npos,
          "An append on a full device fails with ENOSPC");
#endif
}

void real_files(const std::filesystem::path& root) {
  const auto at = root / "alarm_log_tests";
  std::filesystem::remove_all(at);
  std::filesystem::create_directories(at);
  LocalToolModelFiles files;
  {
    AlarmLog log{files, at.string(), {}, [](PageToken, const std::vector<std::string>&) {}};
    log.posted(page, std::vector{encode_message(MessageLevel::error, "on a real disk", epoch)},
               epoch);
    log.settle();
  }
  std::ifstream in{at / "alarm-log.jsonl"};
  std::string line;
  std::getline(in, line);
  require(Json::parse(line)["text"] == "on a real disk", "A real file is appended to");
  const auto mode = std::filesystem::status(at / "alarm-log.jsonl").permissions();
  require((mode & std::filesystem::perms::all) ==
              (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write),
          "The log file has mode 0600");
  {
    AlarmLog log{files, at.string(), {}, [](PageToken, const std::vector<std::string>&) {}};
    log.settle();
    require(log.view().session == 2 && log.view().entries.size() == 1, "It reads back");
  }
  std::filesystem::remove_all(at);
}

} // namespace

auto main(int argc, char** argv) -> int {
  try {
    records_messages_and_refusals();
    localization_survives_reload();
    snapshot_has_what_came_before();
    ordering_across_restarts();
    corruption_tolerance();
    rotation();
    full_disk();
    session_only();
    full_disk_keeps_older_files();
    batch_erase_by_number();
    blocked_stderr_does_not_block_posting();
    writes_only_under_the_lock();
    real_enospc();
    real_files(argc > 1 ? std::filesystem::path{argv[1]} : std::filesystem::temp_directory_path());
    std::cout << checks << " alarm log checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Check " << checks << " failed: " << error.what() << '\n';
    return 1;
  }
}
