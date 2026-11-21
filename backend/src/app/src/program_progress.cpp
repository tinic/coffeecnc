#include <coffeecnc/app/program_progress.hpp>
#include <coffeecnc/app/program_store.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <utility>

namespace coffeecnc {
namespace {
using Json = nlohmann::json;
constexpr std::size_t max_records = 128;
constexpr std::size_t max_bytes = 256 * 1024;
constexpr auto write_period = std::chrono::seconds{1};
constexpr auto file_name = "program-progress.json";
} // namespace

ProgramProgress::ProgramProgress(ToolModelFiles& files, std::string directory)
    : files_(files), directory_(std::move(directory)) {
  load();
}

auto ProgramProgress::stamp(const std::string& path) -> std::optional<Stamp> {
  std::error_code error;
  const auto time = std::filesystem::last_write_time(path, error);
  if (error)
    return {};
  const auto bytes = std::filesystem::file_size(path, error);
  if (error)
    return {};
  // Keep the native file-clock epoch and tick size used by existing saved records.
  using FileTicks = std::chrono::duration<std::int64_t, std::filesystem::file_time_type::period>;
  const auto ticks = time.time_since_epoch();
  if (ticks < FileTicks::min() || ticks > FileTicks::max())
    return {};
  return Stamp{std::chrono::duration_cast<FileTicks>(ticks).count(), bytes};
}

void ProgramProgress::load() {
  if (directory_.empty())
    return;
  const auto content = files_.read(directory_ + '/' + file_name, max_bytes);
  if (!content || !*content || (*content)->more)
    return;
  const auto root = Json::parse((*content)->bytes, nullptr, false);
  if (!root.is_object() || !root.contains("version") || root["version"] != 1 ||
      !root.contains("programs") || !root["programs"].is_array())
    return;
  for (const auto& r : root["programs"]) {
    if (records_.size() == max_records)
      break;
    try {
      Record entry{r.at("path").get<std::string>(),
                   {r.at("modified").get<std::int64_t>(), r.at("bytes").get<std::uintmax_t>()},
                   {r.at("line").get<int>(), r.at("tool").get<int>()},
                   r.at("completed_line").get<int>()};
      if (entry.path.starts_with('/') &&
          entry.path.size() <= ProgramStore::max_program_path_bytes &&
          entry.path.find('\0') == std::string::npos && entry.next.line > 0 &&
          entry.next.tool >= 0 && entry.next.tool <= 99999 && entry.completed_line >= 0 &&
          entry.completed_line < entry.next.line)
        records_.push_back(std::move(entry));
    } catch (const Json::exception&) {
      // A damaged record does not hide the others.
    }
  }
}

void ProgramProgress::save(PanelReading& reading) {
  if (directory_.empty()) {
    dirty_ = false;
    return;
  }
  auto programs = Json::array();
  for (const auto& entry : records_)
    programs.push_back({{"path", entry.path},
                        {"modified", entry.stamp.modified},
                        {"bytes", entry.stamp.bytes},
                        {"line", entry.next.line},
                        {"tool", entry.next.tool},
                        {"completed_line", entry.completed_line}});
  const auto file = directory_ + '/' + file_name;
  const auto temporary = file + ".tmp";
  auto step = files_.open_temporary(temporary);
  if (step)
    step = files_.write(Json{{"version", 1}, {"programs", std::move(programs)}}.dump() + '\n');
  if (step)
    step = files_.sync();
  if (step)
    step = files_.rename(temporary, file);
  if (step)
    step = files_.sync_directory(directory_);
  if (!step) {
    files_.discard(temporary);
    if (save_error_ != step.error()) {
      save_error_ = step.error();
      reading.messages.push_back({true, "Program progress could not be saved. " + save_error_});
    }
    return;
  }
  dirty_ = false;
  save_error_.clear();
}

void ProgramProgress::record(const Record& entry) {
  std::erase_if(records_, [&](const Record& prior) { return prior.path == entry.path; });
  records_.insert(records_.begin(), entry);
  if (records_.size() > max_records)
    records_.pop_back();
  dirty_ = true;
}

void ProgramProgress::observe(PanelReading& reading, Clock::time_point now) {
  auto& status = reading.status;
  auto& program = status.program;
  program.restart_hint.reset();
  program.cycle = ProgramCycle::idle;
  if (completed_revision_ && *completed_revision_ != program.revision)
    completed_revision_.reset();
  const auto& source = reading.program;
  const bool loaded = source && source->file == program.file && source->revision == program.revision;
  const bool complete_text = loaded && !source->truncated;
  const auto identity = loaded ? stamp(source->file) : std::nullopt;
  const bool active = status.mode == TaskMode::automatic &&
                      (status.interp != InterpState::idle ||
                       (running_ && (reading.task_executing || !status.motion.in_position)));
  const bool healthy =
      status.link == Link::live && status.machine == MachineState::on && !reading.program_failed;
  bool flush = false;
  if (running_) {
    const auto& run = *running_;
    const bool same = loaded && run.progress.path == source->file &&
                      run.revision == program.revision && run.progress.stamp == identity;
    if (!same || !healthy || run.aborts != reading.program_aborts || !active) {
      if (same && healthy && run.aborts == reading.program_aborts &&
          status.mode == TaskMode::automatic && !active) {
        completed_revision_ = program.revision;
        dirty_ = std::erase_if(
                     records_,
                     [&](const Record& entry) { return entry.path == run.progress.path; }) > 0 ||
                 dirty_;
      }
      running_.reset();
      interrupted_ = active;
      flush = true;
    }
  }
  if (!active)
    interrupted_ = false;
  if (active && healthy && identity && !interrupted_) {
    completed_revision_.reset();
    program.cycle = ProgramCycle::running;
    if (!running_)
      running_ = Run{{source->file, *identity, {}, 0}, program.revision, reading.program_aborts};
    auto& progress = running_->progress;
    // Motion line 0 means no observed program motion yet, for example while M6 waits.
    if (complete_text && program.motion_line > progress.next.line &&
        static_cast<std::size_t>(program.motion_line) <= source->lines.size()) {
      progress.completed_line = progress.next.line;
      progress.next = {program.motion_line, status.tool.in_spindle};
      record(progress);
    }
  }
  if (!active && healthy && completed_revision_)
    program.cycle = ProgramCycle::completed;
  if (dirty_ && (flush || now >= write_after_)) {
    save(reading);
    write_after_ = now + write_period;
  }
  if (!active && complete_text && identity && status.link == Link::live &&
      status.motion.in_position) {
    const auto found = std::ranges::find_if(records_, [&](const Record& entry) {
      return entry.path == source->file && entry.stamp == *identity &&
             static_cast<std::size_t>(entry.next.line) <= source->lines.size();
    });
    if (found != records_.end())
      program.restart_hint = found->next;
  }
}

} // namespace coffeecnc
