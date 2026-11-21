#include <coffeecnc/app/offset_history.hpp>

#include <coffeecnc/app/messages.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace coffeecnc {
namespace {
using Json = nlohmann::json;

constexpr int file_version = 1;
constexpr std::uint64_t max_safe_integer = 9007199254740991; // 2^53 - 1
constexpr std::array fields{HistoryField::x, HistoryField::y, HistoryField::z};

// A file that is not what this version writes.
class Damaged : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

auto differs(double a, double b) -> bool { return std::abs(a - b) > OffsetHistoryStore::tolerance; }

template <typename T, std::size_t N>
auto parse(const Json& value, const std::array<T, N>& choices) -> T {
  if (!value.is_string())
    throw Damaged("a name is not a string");
  for (const auto choice : choices)
    if (history_name(choice) == value.get<std::string>())
      return choice;
  throw Damaged("an unknown name");
}

constexpr std::array sources{
    HistorySource::touch_off,  HistorySource::zero,           HistorySource::half,
    HistorySource::probe,      HistorySource::tool_setter,    HistorySource::tool_touch_off,
    HistorySource::tool_table, HistorySource::work_offset_table, HistorySource::mdi,
    HistorySource::program,    HistorySource::restore,        HistorySource::outside};
constexpr std::array targets{HistoryTarget::wcs, HistoryTarget::g92, HistoryTarget::tool};
constexpr std::array all_fields{HistoryField::x,           HistoryField::y,
                                HistoryField::z,           HistoryField::rotation_xy,
                                HistoryField::length,      HistoryField::diameter};

auto finite(const Json& value) -> double {
  if (!value.is_number() || !std::isfinite(value.get<double>()))
    throw Damaged("a value is not a finite number");
  return value.get<double>();
}
auto optional_finite(const Json& value) -> std::optional<double> {
  return value.is_null() ? std::nullopt : std::optional<double>{finite(value)};
}
auto counter(const Json& value) -> std::uint64_t {
  if (!value.is_number_unsigned() || value.get<std::uint64_t>() > max_safe_integer)
    throw Damaged("a counter is out of range");
  return value.get<std::uint64_t>();
}
auto vec(const Json& value) -> Vec3 {
  if (!value.is_array() || value.size() != 3)
    throw Damaged("a position is not three numbers");
  return {finite(value[0]), finite(value[1]), finite(value[2])};
}
auto optional_json(const std::optional<double>& value) -> Json {
  return value ? Json(*value) : Json(nullptr);
}

auto entry_json(const HistoryEntry& entry) -> Json {
  auto changes = Json::array();
  for (const auto& change : entry.changes)
    changes.push_back({{"field", history_name(change.field)},
                       {"before", optional_json(change.before)},
                       {"after", optional_json(change.after)}});
  return {{"id", entry.id},
          {"time", entry.time},
          {"source", history_name(entry.source)},
          {"target", history_name(entry.target)},
          {"number", entry.number},
          {"changes", std::move(changes)},
          {"restores", entry.restores ? Json(*entry.restores) : Json(nullptr)}};
}

auto entry_from(const Json& value) -> HistoryEntry {
  HistoryEntry entry;
  entry.id = counter(value.at("id"));
  if (!value.at("time").is_number_integer())
    throw Damaged("a time is not an integer");
  entry.time = value.at("time").get<std::int64_t>();
  entry.source = parse(value.at("source"), sources);
  entry.target = parse(value.at("target"), targets);
  if (!value.at("number").is_number_integer())
    throw Damaged("a number is not an integer");
  entry.number = value.at("number").get<int>();
  const bool numbered = entry.target == HistoryTarget::wcs ? entry.number >= 1 && entry.number <= 9
                        : entry.target == HistoryTarget::g92 ? entry.number == 0
                                                             : entry.number > 0;
  if (entry.id == 0 || entry.time < 0 || !numbered || !value.at("changes").is_array() ||
      value.at("changes").empty())
    throw Damaged("an entry is not complete");
  for (const auto& change : value.at("changes"))
    entry.changes.push_back({parse(change.at("field"), all_fields),
                             optional_finite(change.at("before")),
                             optional_finite(change.at("after"))});
  if (!value.at("restores").is_null())
    entry.restores = counter(value.at("restores"));
  return entry;
}

} // namespace

OffsetHistoryStore::OffsetHistoryStore(ToolModelFiles& files, std::string directory,
                                       std::string unsaved_reason)
    : files_(files), directory_(std::move(directory)), saved_(!directory_.empty()) {
  if (saved_)
    load();
  else
    reason_ = std::move(unsaved_reason);
  publish();
}

void OffsetHistoryStore::load() {
  const auto path = directory_ + '/' + std::string(file_name);
  const auto content = files_.read(path, max_file_bytes);
  if (!content) {
    reason_ = "The saved offset history cannot be read (" + content.error() +
              "). The history starts empty.";
    load_notice_ = reason_;
    return;
  }
  if (!*content)
    return;
  try {
    if ((*content)->more)
      throw Damaged("the file is too large");
    const auto root = Json::parse((*content)->bytes, nullptr, false);
    if (!root.is_object() || !root.contains("version") || root["version"] != file_version)
      throw Damaged("not version 1");
    std::vector<HistoryEntry> entries;
    if (!root.at("entries").is_array())
      throw Damaged("the entries are not a list");
    for (const auto& value : root.at("entries"))
      entries.push_back(entry_from(value));
    if (entries.size() > max_entries ||
        !std::ranges::is_sorted(entries, std::ranges::greater{}, &HistoryEntry::id))
      throw Damaged("the entries are not in order");
    const auto next = counter(root.at("next_id"));
    if (!entries.empty() && next <= entries.front().id)
      throw Damaged("the next id is used");
    Seen seen;
    const auto& known = root.at("seen");
    const auto& systems = known.at("systems");
    if (!systems.is_array() || systems.size() != seen.systems.size())
      throw Damaged("not nine work systems");
    for (std::size_t i = 0; i < seen.systems.size(); ++i) {
      if (!systems[i].is_array() || systems[i].size() != seen.systems[i].size())
        throw Damaged("a work system is not four values");
      for (std::size_t field = 0; field < seen.systems[i].size(); ++field)
        seen.systems[i].at(field) = optional_finite(systems[i][field]);
    }
    if (!known.at("g92").is_null())
      seen.g92 = vec(known.at("g92"));
    if (!known.at("tools").is_null()) {
      if (!known.at("tools").is_array())
        throw Damaged("the tools are not a list");
      std::map<int, ToolValue> tools;
      for (const auto& row : known.at("tools")) {
        if (!row.at("number").is_number_integer() || row.at("number").get<int>() <= 0)
          throw Damaged("a tool number is not positive");
        tools[row.at("number").get<int>()] = {finite(row.at("length")),
                                              finite(row.at("diameter"))};
      }
      seen.tools = std::move(tools);
    }
    entries_ = std::move(entries);
    next_id_ = next;
    seen_ = std::move(seen);
  } catch (const std::exception&) {
    // Damaged, or a field missing or of another type (nlohmann's exceptions). Kept aside, not
    // written over: the operator may want what is in it.
    const auto aside = directory_ + '/' + std::string(corrupt_name);
    const auto moved = files_.rename(path, aside);
    reason_ = moved ? "The saved offset history was damaged and starts empty. The damaged file "
                      "is kept as " + std::string(corrupt_name) + "."
                    : "The saved offset history was damaged and starts empty.";
    load_notice_ = reason_;
  }
}

auto OffsetHistoryStore::begin(HistorySource source, std::optional<std::uint64_t> restores)
    -> std::uint64_t {
  const std::scoped_lock lock{mutex_};
  const auto id = next_mark_++;
  marks_.push_back({id, source, restores, true, {}});
  return id;
}

void OffsetHistoryStore::end(std::uint64_t mark, Clock::time_point now) {
  const std::scoped_lock lock{mutex_};
  for (auto& found : marks_)
    if (found.id == mark) {
      found.open = false;
      found.until = now + grace;
    }
}

auto OffsetHistoryStore::source(Clock::time_point now) const
    -> std::pair<HistorySource, std::optional<std::uint64_t>> {
  // The command begun last that is still running, else the one that returned last.
  const auto open = std::ranges::find_if(marks_.rbegin(), marks_.rend(),
                                         [](const Mark& mark) -> bool { return mark.open; });
  if (open != marks_.rend())
    return {open->source, open->restores};
  if (!marks_.empty())
    return {marks_.back().source, marks_.back().restores};
  if (now < program_until_)
    return {HistorySource::program, std::nullopt};
  return {HistorySource::outside, std::nullopt};
}

void OffsetHistoryStore::publish() {
  auto history = std::make_shared<OffsetHistory>();
  history->revision = revision_++;
  history->saved = saved_;
  history->reason = save_error_.empty() ? reason_ : save_error_;
  history->entries = entries_;
  snapshot_ = std::move(history);
}

void OffsetHistoryStore::add(HistoryEntry entry) {
  entry.id = next_id_++;
  entries_.insert(entries_.begin(), std::move(entry));
  if (entries_.size() > max_entries)
    entries_.resize(max_entries);
  entries_due_ = true;
  publish();
}

void OffsetHistoryStore::change_system(
    int wcs, const System& values, bool record_unknown, std::chrono::milliseconds unix_time,
    std::pair<HistorySource, std::optional<std::uint64_t>> by) {
  constexpr std::array names{HistoryField::x, HistoryField::y, HistoryField::z,
                             HistoryField::rotation_xy};
  auto& known = seen_.systems.at(static_cast<std::size_t>(wcs) - 1);
  HistoryEntry entry{0, unix_time.count(), by.first, HistoryTarget::wcs, wcs, {}, by.second};
  for (std::size_t i = 0; i < values.size(); ++i) {
    const auto& value = values.at(i);
    auto& before = known.at(i);
    if (!value || (before && !differs(*before, *value)))
      continue;
    if (before || record_unknown)
      entry.changes.push_back({names.at(i), before, value});
    else
      seen_due_ = true;
    before = value;
  }
  if (!entry.changes.empty())
    add(std::move(entry));
}

void OffsetHistoryStore::written(std::uint64_t mark, int wcs,
                                 const std::array<std::optional<double>, 4>& values,
                                 std::chrono::milliseconds unix_time) {
  if (wcs < 1 || wcs > static_cast<int>(seen_.systems.size()))
    return;
  const std::scoped_lock lock{mutex_};
  // The active system is the readings' to record: one taken before the write but compared
  // after it would otherwise be seen as a change back.
  if (active_wcs_ == wcs)
    return;
  const auto found = std::ranges::find(marks_, mark, &Mark::id);
  change_system(wcs, values, true, unix_time,
                found != marks_.end() ? std::pair{found->source, found->restores}
                                      : std::pair{HistorySource::outside,
                                                  std::optional<std::uint64_t>{}});
}

auto OffsetHistoryStore::entry(std::uint64_t id) const -> std::optional<HistoryEntry> {
  const std::scoped_lock lock{mutex_};
  const auto found = std::ranges::find(entries_, id, &HistoryEntry::id);
  return found == entries_.end() ? std::nullopt : std::optional<HistoryEntry>{*found};
}

auto OffsetHistoryStore::snapshot() const -> std::shared_ptr<const OffsetHistory> {
  const std::scoped_lock lock{mutex_};
  return snapshot_;
}

void OffsetHistoryStore::observe(PanelReading& reading, std::chrono::milliseconds unix_time,
                                 Clock::time_point now) {
  std::optional<std::string> text;
  {
    const std::scoped_lock lock{mutex_};
    const auto& status = reading.status;
    // MDI that is still executing keeps the command that sent it responsible.
    if (status.mode == TaskMode::mdi && status.interp != InterpState::idle)
      for (auto& mark : marks_)
        if (!mark.open)
          mark.until = std::max(mark.until, now + grace);
    std::erase_if(marks_, [&](const Mark& mark) -> bool { return !mark.open && mark.until < now; });
    if (status.mode == TaskMode::automatic && status.interp != InterpState::idle)
      program_until_ = now + grace;
    const auto by = source(now);
    if (status.link == Link::live) {
      const int wcs = status.offsets.wcs;
      if (wcs >= 1 && wcs <= static_cast<int>(seen_.systems.size())) {
        active_wcs_ = wcs;
        const auto& g5x = status.offsets.g5x;
        change_system(wcs, {g5x[0], g5x[1], g5x[2], status.offsets.rotation_xy}, false,
                      unix_time, by);
      }
      if (!seen_.g92) {
        seen_.g92 = status.offsets.g92;
        seen_due_ = true;
      } else {
        HistoryEntry entry{0, unix_time.count(), by.first, HistoryTarget::g92, 0, {}, by.second};
        for (std::size_t i = 0; i < fields.size(); ++i)
          if (differs(seen_.g92->at(i), status.offsets.g92.at(i)))
            entry.changes.push_back({fields.at(i), seen_.g92->at(i), status.offsets.g92.at(i)});
        if (!entry.changes.empty()) {
          seen_.g92 = status.offsets.g92;
          add(std::move(entry));
        }
      }
    }
    // Revision 0: no pass over the tool data has been accepted yet; its empty table is no value.
    if (status.link == Link::live && reading.tools_settled && reading.tool_table.revision != 0) {
      std::map<int, ToolValue> tools;
      for (const auto& tool : reading.accepted_tools.value_or(reading.tool_table.tools))
        if (tool.number > 0 && std::isfinite(tool.length) && std::isfinite(tool.diameter))
          tools[tool.number] = {tool.length, tool.diameter};
      if (!seen_.tools) {
        seen_.tools = tools;
        seen_due_ = true;
      } else {
        bool changed = false;
        const auto record = [&](int number, std::optional<ToolValue> before,
                                std::optional<ToolValue> after) -> void {
          HistoryEntry entry{0, unix_time.count(), by.first, HistoryTarget::tool, number, {},
                             by.second};
          const auto length = [](const std::optional<ToolValue>& v) -> std::optional<double> {
            return v ? std::optional<double>{v->length} : std::nullopt;
          };
          const auto diameter = [](const std::optional<ToolValue>& v) -> std::optional<double> {
            return v ? std::optional<double>{v->diameter} : std::nullopt;
          };
          if (!before || !after || differs(before->length, after->length))
            entry.changes.push_back({HistoryField::length, length(before), length(after)});
          if (!before || !after || differs(before->diameter, after->diameter))
            entry.changes.push_back({HistoryField::diameter, diameter(before), diameter(after)});
          if (!entry.changes.empty()) {
            changed = true;
            add(std::move(entry));
          }
        };
        for (const auto& [number, value] : tools) {
          const auto known = seen_.tools->find(number);
          record(number,
                 known == seen_.tools->end() ? std::nullopt : std::optional<ToolValue>{known->second},
                 value);
        }
        for (const auto& [number, value] : *seen_.tools)
          if (!tools.contains(number))
            record(number, value, std::nullopt);
        if (changed)
          seen_.tools = std::move(tools);
      }
    }
    if (!load_notice_.empty()) {
      reading.messages.push_back({true, std::exchange(load_notice_, {})});
    }
    if (saved_ && (entries_due_ || (seen_due_ && now >= write_after_))) {
      entries_due_ = false;
      seen_due_ = false;
      write_after_ = now + write_period;
      auto entries = Json::array();
      for (const auto& entry : entries_)
        entries.push_back(entry_json(entry));
      auto systems = Json::array();
      for (const auto& system : seen_.systems) {
        auto fields_json = Json::array();
        for (const auto& value : system)
          fields_json.push_back(optional_json(value));
        systems.push_back(std::move(fields_json));
      }
      Json tools = nullptr;
      if (seen_.tools) {
        tools = Json::array();
        for (const auto& [number, value] : *seen_.tools)
          tools.push_back({{"number", number},
                           {"length", value.length},
                           {"diameter", value.diameter}});
      }
      text = Json{{"version", file_version},
                  {"next_id", next_id_},
                  {"entries", std::move(entries)},
                  {"seen",
                   {{"systems", std::move(systems)},
                    {"g92", seen_.g92 ? Json(*seen_.g92) : Json(nullptr)},
                    {"tools", std::move(tools)}}}}
                 .dump() +
             '\n';
    }
  }
  // Outside the mutex: the worker never waits for the disk.
  if (text) {
    const auto file = directory_ + '/' + std::string(file_name);
    const auto temporary = file + ".tmp";
    auto step = files_.open_temporary(temporary);
    if (step)
      step = files_.write(*text);
    if (step)
      step = files_.sync();
    if (step)
      step = files_.rename(temporary, file);
    if (step)
      step = files_.sync_directory(directory_);
    if (!step)
      files_.discard(temporary);
    const std::scoped_lock lock{mutex_};
    const auto error = step ? std::string{} : "The offset history could not be saved: " + step.error();
    // Not written: tried again after write_period.
    if (!error.empty())
      seen_due_ = true;
    if (error != save_error_) {
      if (!error.empty())
        reading.messages.push_back({true, error});
      save_error_ = error;
      publish();
    }
  }
  const std::scoped_lock lock{mutex_};
  reading.offset_history = snapshot_;
}

} // namespace coffeecnc
