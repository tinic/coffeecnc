#include <coffeecnc/linuxcnc/nml_panel_source.hpp>

#include <coffeecnc/linuxcnc/panel_mapping.hpp>
#include <coffeecnc/linuxcnc/tool_table.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

namespace coffeecnc {
namespace {

// What tells one state of a file from the next without reading it.
struct Stamp {
  std::uintmax_t bytes{};
  std::filesystem::file_time_type modified;
  friend auto operator==(const Stamp&, const Stamp&) -> bool = default;
};

auto stamp_of(const std::string& file) -> std::optional<Stamp> {
  std::error_code error;
  const auto bytes = std::filesystem::file_size(file, error);
  if (error)
    return {};
  const auto modified = std::filesystem::last_write_time(file, error);
  if (error)
    return {};
  return Stamp{bytes, modified};
}

// A text file that is read again when its stamp changes.
class Watched {
public:
  explicit Watched(std::string file) : file_(std::move(file)) {}

  // The text when it was read again in this call; nothing while the file is as it was, is not
  // configured or cannot be read.
  [[nodiscard]] auto changed() -> std::optional<std::string> {
    if (file_.empty())
      return {};
    const auto stamp = stamp_of(file_);
    if (stamp && stamp_ == stamp)
      return {};
    std::ifstream in{file_, std::ios::binary};
    if (!stamp || !in) {
      stamp_.reset();
      failed_ = true;
      return {};
    }
    std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    // Written while it was read: the next call reads it again.
    stamp_ = stamp_of(file_) == stamp ? stamp : std::nullopt;
    failed_ = false;
    return text;
  }
  // The file cannot be read at present; true once per failure.
  [[nodiscard]] auto newly_failed() -> bool {
    const bool report = failed_ && !reported_;
    reported_ = failed_;
    return report;
  }
  [[nodiscard]] auto file() const -> const std::string& { return file_; }

private:
  std::string file_;
  std::optional<Stamp> stamp_;
  bool failed_{};
  bool reported_{};
};

} // namespace

struct NmlPanelSource::Impl {
  MachineConfig config;
  const NmlStatusMailbox& mailbox;
  ProgramReader programs;
  std::function<Clock::time_point()> clock;
  Watched tool_file;
  ToolTable tool_table;
  // The program task has open, as it was read last.
  std::string program_file;
  std::optional<Stamp> program_stamp;
  std::uint64_t program_revision{};
  std::shared_ptr<const ProgramText> program;

  Impl(MachineConfig machine, const NmlStatusMailbox& readings, ProgramReader reader_of_programs,
       std::function<Clock::time_point()> time)
      : config(std::move(machine)), mailbox(readings), programs(std::move(reader_of_programs)),
        clock(std::move(time)),
        tool_file(config.tool_table) {}

  void read_tools(std::vector<ControllerMessage>& messages) {
    if (const auto text = tool_file.changed()) {
      auto tools = tools_of(*text, config.millimetres_per_unit);
      if (tool_table.revision == 0 || tools != tool_table.tools) {
        tool_table.tools = std::move(tools);
        ++tool_table.revision;
      }
    }
    if (tool_file.newly_failed())
      messages.push_back({true, std::format("Cannot read tool table {}.", tool_file.file())});
  }

  void read_program(const NmlStatus& status, std::vector<ControllerMessage>& messages) {
    if (status.controller.link == Link::down)
      return; // Nothing is known: what was read last stays.
    if (status.program_file.empty()) {
      program_file.clear();
      program_stamp.reset();
      program.reset();
      return;
    }
    const auto stamp = stamp_of(status.program_file);
    if (status.program_file == program_file && stamp == program_stamp)
      return;
    program_file = status.program_file;
    program_stamp = stamp;
    ++program_revision;
    program.reset();
    auto text = programs ? programs(program_file, program_revision)
                         : std::unexpected(std::string("No program directory is configured."));
    if (text)
      program = std::make_shared<const ProgramText>(std::move(*text));
    else
      messages.push_back(
          {true, std::format("The text of {} cannot be shown: {}", program_file, text.error())});
  }
};

NmlPanelSource::NmlPanelSource(const MachineConfig& config, const NmlStatusMailbox& mailbox,
                               ProgramReader programs, std::function<Clock::time_point()> clock)
    : impl_(std::make_unique<Impl>(config, mailbox, std::move(programs), std::move(clock))) {}
NmlPanelSource::~NmlPanelSource() = default;

auto NmlPanelSource::hello() const -> Hello { return to_hello(impl_->config); }

auto NmlPanelSource::read() -> PanelReading {
  const auto latest = impl_->mailbox.latest();
  auto status = latest ? latest->status : NmlStatus{};
  if (latest && status.controller.link == Link::live && impl_->clock() - latest->read_at > max_age)
    status.controller.link = Link::stale;
  PanelReading reading;
  impl_->read_tools(reading.messages);
  impl_->read_program(status, reading.messages);
  reading.status = to_panel_status(status, impl_->config, impl_->tool_table.tools,
                                   status.program_file.empty() ? 0 : impl_->program_revision);
  if (status.controller.link == Link::down && !impl_->program_file.empty())
    reading.status.program = {};
  reading.tool_table = impl_->tool_table;
  reading.work_systems = to_work_systems(status);
  reading.program = impl_->program;
  return reading;
}

} // namespace coffeecnc
