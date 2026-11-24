#include <coffeecnc/app/alarm_log.hpp>

#include "message_key_json.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <iostream>
#include <iterator>
#include <set>
#include <utility>

namespace coffeecnc {
namespace {
using Json = nlohmann::json;

constexpr int record_version = 1;
constexpr std::uint64_t max_safe_integer = 9007199254740991; // 2^53 - 1

auto level_name(MessageLevel level) -> std::string_view {
  return level == MessageLevel::info ? "info" : "error";
}

auto dump(const Json& value) -> std::string {
  // Controller text is not guaranteed to be UTF-8; dump() must not throw on it.
  return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

// At most `limit` bytes, cut before a UTF-8 continuation byte.
auto bounded(std::string text, std::size_t limit) -> std::string {
  if (text.size() <= limit)
    return text;
  auto end = limit;
  while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U)
    --end;
  text.resize(end);
  return text;
}

// Ignore invalid optional metadata so an older or edited record keeps its English text.
auto localization_of(const Json& value, std::string_view field) -> std::optional<MessageKey> {
  const auto found = value.find(field);
  if (found == value.end() || !found->is_object())
    return std::nullopt;
  const auto* metadata = &*found;
  Json without_lists;
  if (dump(*metadata).size() > AlarmLog::max_text_bytes) {
    without_lists = *metadata;
    if (without_lists.erase("params_lists") == 0 ||
        dump(without_lists).size() > AlarmLog::max_text_bytes)
      return std::nullopt;
    metadata = &without_lists;
  }
  auto parsed = message_parameter_from_json(*metadata);
  if (!parsed)
    return std::nullopt;
  MessageKey message{std::move(parsed->key), std::move(parsed->params)};
  const auto keys = metadata->find("params_localization");
  if (keys != metadata->end() && keys->is_object())
    for (const auto& [name, key] : keys->items())
      if (message.params.contains(name))
        if (auto parameter = message_parameter_from_json(key))
          message.params_localization.emplace(name, std::move(*parameter));
  const auto lists = metadata->find("params_lists");
  if (lists != metadata->end() && lists->is_object())
    for (const auto& [name, items] : lists->items())
      if (message.params.contains(name))
        if (auto list = message_list_from_json(items))
          message.params_lists.emplace(name, std::move(*list));
  return message;
}

auto record_json(const AlarmRecord& record) -> Json {
  Json value{{"session", record.session},
             {"time", record.time},
             {"level", level_name(record.level)},
             {"text", record.text},
             {"outcome", record.outcome.empty() ? Json(nullptr) : Json(record.outcome)},
             {"command", nullptr}};
  if (record.localization)
    value["localization"] = message_key_json(*record.localization);
  if (!record.command.empty()) {
    auto command = Json::parse(record.command, nullptr, false);
    if (command.is_object())
      value["command"] = std::move(command);
  }
  return value;
}

// A record of a line, or none when the line is not one.
auto record_from(std::string_view line) -> std::optional<AlarmRecord> {
  const auto value = Json::parse(line, nullptr, false);
  if (!value.is_object())
    return std::nullopt;
  const auto field = [&](std::string_view name) -> const Json* {
    const auto found = value.find(name);
    return found == value.end() ? nullptr : &*found;
  };
  const auto* version = field("v");
  const auto* session = field("session");
  const auto* time = field("time");
  const auto* level = field("level");
  const auto* text = field("text");
  const auto* outcome = field("outcome");
  const auto* command = field("command");
  const auto* number = field("n");
  if (number != nullptr &&
      (!number->is_number_unsigned() || number->get<std::uint64_t>() > max_safe_integer))
    return std::nullopt;
  if (version == nullptr || !version->is_number_integer() || *version != record_version ||
      session == nullptr || !session->is_number_unsigned() || *session == 0 ||
      session->get<std::uint64_t>() > max_safe_integer || time == nullptr ||
      !time->is_number_integer() || level == nullptr || !level->is_string() || text == nullptr ||
      !text->is_string() || (outcome != nullptr && !outcome->is_null() && !outcome->is_string()) ||
      (command != nullptr && !command->is_null() && !command->is_object()))
    return std::nullopt;
  const auto level_text = level->get<std::string_view>();
  if (level_text != "error" && level_text != "info")
    return std::nullopt;
  AlarmRecord record;
  record.session = session->get<std::uint64_t>();
  record.number = number != nullptr ? number->get<std::uint64_t>() : 0;
  record.time = time->get<std::int64_t>();
  record.level = level_text == "info" ? MessageLevel::info : MessageLevel::error;
  record.text = bounded(text->get<std::string>(), AlarmLog::max_text_bytes);
  record.localization = localization_of(value, "localization");
  if (outcome != nullptr && outcome->is_string())
    record.outcome = outcome->get<std::string>();
  if (command != nullptr && command->is_object()) {
    record.command = dump(*command);
    if (record.command.size() > AlarmLog::max_command_bytes)
      record.command.clear();
  }
  return record;
}

auto line_of(const AlarmRecord& record) -> std::string {
  auto value = record_json(record);
  value["v"] = record_version;
  value["n"] = record.number;
  return dump(value) + '\n';
}

// The id of a request and its command as the log keeps it: without the id, bounded.
auto command_of(std::string_view json) -> std::optional<std::pair<std::uint64_t, std::string>> {
  auto value = Json::parse(json, nullptr, false);
  if (!value.is_object())
    return std::nullopt;
  const auto id = value.find("id");
  if (id == value.end() || !id->is_number_unsigned())
    return std::nullopt;
  const auto number = id->get<std::uint64_t>();
  value.erase("id");
  auto text = dump(value);
  if (text.size() > AlarmLog::max_command_bytes) {
    const auto type = value.find("type");
    text = type != value.end() && type->is_string() ? dump(Json{{"type", *type}}) : "";
  }
  return std::pair{number, std::move(text)};
}

} // namespace

auto encode_alarm_log(const AlarmLogView& view) -> std::string {
  auto entries = Json::array();
  std::ranges::transform(view.entries, std::back_inserter(entries), record_json);
  Json message{{"type", "alarm_log"},     {"storage", view.saved ? "saved" : "session_only"},
               {"reason", view.reason},   {"session", view.session},
               {"skipped", view.skipped}, {"entries", std::move(entries)}};
  if (const auto& key = view.localization)
    message["localization"] = message_key_json(*key);
  return dump(message);
}

AlarmLog::AlarmLog(ToolModelFiles& files, std::string directory, LocalizedReason unsaved_reason,
                   Deliver deliver, Lock lock)
    : files_(files), directory_(std::move(directory)), deliver_(std::move(deliver)),
      lock_(std::move(lock)),
      reason_(directory_.empty() ? std::move(unsaved_reason) : LocalizedReason{}),
      thread_([this] { loop(); }) {}

AlarmLog::~AlarmLog() {
  {
    const std::scoped_lock lock{mutex_};
    stop_ = true;
  }
  wake_.notify_all();
  thread_.join();
}

auto AlarmLog::path(std::string_view name) const -> std::string {
  return directory_ + '/' + std::string(name);
}

auto AlarmLog::locked() -> bool {
  std::optional<std::string> problem;
  if (lock_) {
    problem = lock_();
  } else if (const auto taken = files_.lock(directory_); !taken) {
    if (taken.error().in_use)
      problem = "it is in use by another CoffeeCNC.";
    else
      problem = "its lock cannot be taken: " + taken.error().reason + ".";
  } else {
    own_lock_ = true;
  }
  if (!problem)
    return true;
  const std::scoped_lock lock{mutex_};
  reason_ = {
      std::format("The alarm log in {} is kept for this run only: {}", directory_, *problem),
      MessageKey{"backend.alarmLog.keptForRun", {{"directory", directory_}, {"detail", *problem}}}};
  directory_.clear();
  pending_.clear();
  return false;
}

void AlarmLog::load() {
  std::vector<AlarmRecord> read;
  std::uint64_t skipped = 0;
  LocalizedReason problem;
  bool open_line = false;
  std::size_t current_bytes = 0;
  std::set<std::pair<std::uint64_t, std::uint64_t>> written; // session, number
  for (const auto name : {oldest_name, older_name, file_name}) {
    const auto file = path(name);
    const auto content = files_.read(file, 2 * max_file_bytes);
    if (!content) {
      problem = {
          std::format("The alarm log {} cannot be read: {}", file, content.error()),
          MessageKey{"backend.alarmLog.cannotRead", {{"file", file}, {"detail", content.error()}}}};
      continue;
    }
    const auto& found = *content;
    if (!found)
      continue;
    const auto& bytes = found->bytes;
    const bool more = found->more;
    if (more)
      ++skipped; // what is beyond the bound is not read
    std::string_view rest{bytes};
    while (!rest.empty()) {
      const auto end = rest.find('\n');
      const auto line = rest.substr(0, end);
      rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
      if (line.empty())
        continue;
      if (auto record = record_from(line)) {
        if (record->number == 0 || written.emplace(record->session, record->number).second)
          read.push_back(std::move(*record));
      } else {
        ++skipped;
      }
    }
    if (name == file_name) {
      current_bytes = more ? 2 * max_file_bytes + 1 : bytes.size();
      open_line = !bytes.empty() && bytes.back() != '\n';
    }
  }
  const std::uint64_t highest =
      read.empty() ? 0 : std::ranges::max(read, {}, &AlarmRecord::session).session;
  const std::scoped_lock lock{mutex_};
  session_ = highest + 1;
  skipped_ = skipped;
  file_bytes_ = current_bytes;
  needs_line_break_ = open_line;
  if (!problem.empty())
    reason_ = std::move(problem);
  // Before what this run recorded while the files were read; the newest max_entries.
  const auto first = read.size() > max_entries ? read.size() - max_entries : 0;
  std::deque<Recorded> earlier;
  for (auto i = first; i < read.size(); ++i)
    earlier.push_back({0, std::move(read[i])});
  for (auto& kept : kept_)
    kept.record.session = session_;
  for (auto& waiting : pending_)
    waiting.session = session_;
  kept_.insert(kept_.begin(), std::make_move_iterator(earlier.begin()),
               std::make_move_iterator(earlier.end()));
  while (kept_.size() > max_entries)
    kept_.pop_front();
  loaded_ = true;
}

auto AlarmLog::append(const std::vector<AlarmRecord>& batch) -> bool {
  std::string text = needs_line_break_ ? "\n" : "";
  for (const auto& record : batch)
    text += line_of(record);
  const auto file = path(file_name);
  ToolModelFiles::Done done;
  // Only what appends that worked wrote counts: a failed append on a full disk can make the file
  // look larger, and rotating it would push the older files out while nothing is saved.
  if (file_bytes_ > 0 && file_bytes_ + text.size() > max_file_bytes) {
    // There is no older file before the second rotation.
    if (const auto older = files_.read(path(older_name), 0); !older || *older)
      done = files_.rename(path(older_name), path(oldest_name));
    if (done)
      done = files_.rename(file, path(older_name));
    if (done)
      done = files_.sync_directory(directory_);
    if (!done) {
      const std::scoped_lock lock{mutex_};
      reason_ = {std::format("The alarm log could not be rotated: {}", done.error()),
                 MessageKey{"backend.alarmLog.notRotated", {{"detail", done.error()}}}};
      return false;
    }
    file_bytes_ = 0;
    if (needs_line_break_)
      text.erase(0, 1);
    needs_line_break_ = false;
  }
  const bool created = file_bytes_ == 0;
  done = files_.append(file, text);
  if (done && created)
    done = files_.sync_directory(directory_);
  const std::scoped_lock lock{mutex_};
  if (!done) {
    // Part of it may be in the file: the next append ends that line first. It is not counted.
    needs_line_break_ = true;
    reason_ = {std::format("The alarm log could not be saved: {}", done.error()),
               MessageKey{"backend.alarmLog.notSaved", {{"detail", done.error()}}}};
    return false;
  }
  file_bytes_ += text.size();
  needs_line_break_ = false;
  reason_ = {};
  return true;
}

void AlarmLog::loop() {
  if (!directory_.empty() && locked())
    load();
  else {
    const std::scoped_lock lock{mutex_};
    session_ = 1;
    for (auto& kept : kept_)
      kept.record.session = session_;
    for (auto& waiting : pending_)
      waiting.session = session_;
    loaded_ = true;
  }
  std::unique_lock lock{mutex_};
  for (;;) {
    const bool can_write = !directory_.empty() && !pending_.empty() &&
                           (!retry_at_ || stop_ || Clock::now() >= *retry_at_);
    if (!can_write && !snapshot_due_) {
      if (stop_)
        break;
      busy_ = false;
      idle_.notify_all();
      if (retry_at_ && !directory_.empty() && !pending_.empty())
        wake_.wait_until(lock, *retry_at_);
      else
        wake_.wait(lock);
      continue;
    }
    busy_ = true;
    if (can_write) {
      std::vector<AlarmRecord> batch(pending_.begin(), pending_.end());
      lock.unlock();
      const bool written = append(batch);
      lock.lock();
      if (written) {
        // By number: add() may have dropped the oldest ones meanwhile, and what it added after
        // them was not written.
        const auto last = batch.back().number;
        while (!pending_.empty() && pending_.front().number <= last)
          pending_.pop_front();
        retry_at_.reset();
      } else {
        retry_at_ = Clock::now() + retry_period;
        if (stop_) {
          // The last try at the end of the run.
          pending_.clear();
        }
      }
      if (reason_.text != said_) {
        said_ = reason_.text;
        // Not under the mutex: a stderr that blocks must not block the threads that post.
        if (!said_.empty()) {
          const auto text = said_;
          lock.unlock();
          std::cerr << "coffeecnc: " << text << '\n' << std::flush;
          lock.lock();
        }
      }
    }
    if (snapshot_due_) {
      const auto [page, order] = *snapshot_due_;
      snapshot_due_.reset();
      auto text = encode_alarm_log(view_before(order));
      lock.unlock();
      deliver_(page, {std::move(text)});
      lock.lock();
    }
  }
  busy_ = false;
  idle_.notify_all();
  if (own_lock_) {
    lock.unlock();
    files_.unlock();
  }
}

void AlarmLog::add(AlarmRecord entry) {
  entry.session = session_;
  entry.number = next_order_;
  entry.text = bounded(std::move(entry.text), max_text_bytes);
  kept_.push_back({next_order_++, entry});
  while (kept_.size() > max_entries)
    kept_.pop_front();
  if (directory_.empty())
    return;
  pending_.push_back(std::move(entry));
  while (pending_.size() > max_pending) {
    pending_.pop_front();
    ++lost_;
  }
  // A new record is a reason to try again at once.
  retry_at_.reset();
}

void AlarmLog::request(PageToken page, std::string_view json) {
  auto command = command_of(json);
  if (!command)
    return;
  const std::scoped_lock lock{mutex_};
  commands_[{page.value, command->first}] = std::move(command->second);
  while (commands_.size() > max_requests)
    commands_.erase(commands_.begin());
}

void AlarmLog::posted(PageToken page, std::span<const std::string> messages,
                      std::chrono::milliseconds unix_time) {
  std::vector<std::pair<std::optional<std::uint64_t>, AlarmRecord>> found;
  for (const auto& message : messages) {
    const bool result = message.find(R"("type":"result")") != std::string::npos;
    if (!result && message.find(R"("type":"message")") == std::string::npos)
      continue;
    const auto value = Json::parse(message, nullptr, false);
    if (!value.is_object())
      continue;
    const auto type = value.value("type", std::string{});
    if (type == "result") {
      const auto id = value.value("id", std::uint64_t{});
      const auto outcome = value.value("outcome", std::string{});
      AlarmRecord record;
      record.time = unix_time.count();
      record.text = value.value("reason", std::string{});
      record.outcome = outcome;
      if (outcome == "rejected")
        record.localization = localization_of(value, "refusal");
      if (!record.localization && outcome != "accepted")
        record.localization = localization_of(value, "localization");
      found.emplace_back(id, outcome == "accepted" ? AlarmRecord{} : std::move(record));
    } else if (type == "message") {
      const auto time = value.find("time");
      AlarmRecord record;
      record.time = time != value.end() && time->is_number_integer() ? time->get<std::int64_t>()
                                                                     : unix_time.count();
      record.level =
          value.value("level", std::string{}) == "info" ? MessageLevel::info : MessageLevel::error;
      record.text = value.value("text", std::string{});
      record.localization = localization_of(value, "localization");
      found.emplace_back(std::nullopt, std::move(record));
    }
  }
  if (found.empty())
    return;
  {
    const std::scoped_lock lock{mutex_};
    for (auto& [id, record] : found) {
      if (id) {
        const auto command = commands_.find({page.value, *id});
        if (command != commands_.end()) {
          record.command = std::move(command->second);
          commands_.erase(command);
        }
        if (record.outcome.empty() || record.outcome == "accepted")
          continue;
      }
      add(std::move(record));
    }
  }
  wake_.notify_all();
}

void AlarmLog::send_snapshot(PageToken page) {
  {
    const std::scoped_lock lock{mutex_};
    snapshot_due_ = {page, next_order_};
  }
  wake_.notify_all();
}

auto AlarmLog::view_before(std::uint64_t order) const -> AlarmLogView {
  AlarmLogView shown;
  shown.saved = !directory_.empty();
  auto reason = lost_ > 0 && reason_.empty()
                    ? LocalizedReason{"Some alarm log entries could not be saved.",
                                      MessageKey{"backend.alarmLog.entriesLost"}}
                    : reason_;
  shown.reason = std::move(reason.text);
  shown.localization = std::move(reason.localization);
  shown.session = session_;
  shown.skipped = skipped_;
  for (const auto& kept : kept_)
    if (kept.order < order)
      shown.entries.push_back(kept.record);
  return shown;
}

auto AlarmLog::view() const -> AlarmLogView {
  const std::scoped_lock lock{mutex_};
  return view_before(next_order_);
}

void AlarmLog::settle() {
  std::unique_lock lock{mutex_};
  idle_.wait(lock, [this] {
    return loaded_ && !busy_ && !snapshot_due_ &&
           (directory_.empty() || pending_.empty() || retry_at_.has_value());
  });
}

} // namespace coffeecnc
