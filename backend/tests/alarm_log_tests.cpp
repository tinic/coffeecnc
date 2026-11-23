#include <coffeecnc/app/alarm_log.hpp>
#include <coffeecnc/app/response.hpp>

#include "fake_tool_model_files.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <sstream>
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
    log = std::make_unique<AlarmLog>(files, std::move(at), "No machine identity configured",
                                     [this](PageToken token, std::vector<std::string> messages) {
                                       const std::scoped_lock lock{delivered_mutex};
                                       std::ranges::transform(
                                           messages, std::back_inserter(delivered),
                                           [token](std::string& message) {
                                             return std::pair{token, std::move(message)};
                                           });
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
  f.command(6, R"({"type":"mdi","text":"G1 X10","id":6})", CommandOutcome::failed,
            "No feed rate.");
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
  require(view.reason.find("cannot be read") != std::string::npos,
          "A file that cannot be read is said");
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
  require(f.log->view().reason.find("rotated") != std::string::npos,
          "A failed rotation is said");
  f.log->posted(page, std::vector{encode_message(MessageLevel::error, "next", epoch)}, epoch);
  f.log.reset(); // the last append of the run
  const auto rotated = f.lines(file);
  require(rotated.size() == 2 && rotated[0]["text"].get<std::string>().starts_with("rotation fails") &&
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
  require(view.reason.find("No space left on device") != std::string::npos,
          "A full disk is said");
  require(f.texts().back() == "while full 1", "The view still has what was not saved");
  f.message("while full 2"); // a new record tries again at once: fails
  // Space again: a retry or the next record writes what waited, in order.
  f.files.failures.clear();
  f.message("after");
  view = f.log->view();
  require(view.reason.empty(), "The reason goes once the log is saved again");
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
  require(!view.saved && view.reason == "No machine identity configured" && view.session == 1 &&
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

void real_files(const std::filesystem::path& root) {
  const auto at = root / "alarm_log_tests";
  std::filesystem::remove_all(at);
  std::filesystem::create_directories(at);
  LocalToolModelFiles files;
  {
    AlarmLog log{files, at.string(), "", [](PageToken, const std::vector<std::string>&) {}};
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
    AlarmLog log{files, at.string(), "", [](PageToken, const std::vector<std::string>&) {}};
    log.settle();
    require(log.view().session == 2 && log.view().entries.size() == 1, "It reads back");
  }
  std::filesystem::remove_all(at);
}

} // namespace

auto main(int argc, char** argv) -> int {
  try {
    records_messages_and_refusals();
    snapshot_has_what_came_before();
    ordering_across_restarts();
    corruption_tolerance();
    rotation();
    full_disk();
    session_only();
    real_files(argc > 1 ? std::filesystem::path{argv[1]} : std::filesystem::temp_directory_path());
    std::cout << checks << " alarm log checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Check " << checks << " failed: " << error.what() << '\n';
    return 1;
  }
}
