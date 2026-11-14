#include <coffeecnc/linuxcnc/nml_panel_source.hpp>

#include <coffeecnc/linuxcnc/panel_mapping.hpp>
#include <coffeecnc/linuxcnc/tool_table.hpp>

#include <cstdint>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <utility>

namespace coffeecnc {

struct NmlPanelSource::Impl {
  MachineConfig config;
  const NmlStatusMailbox& mailbox;
  ToolReader tools;
  ProgramReader programs;
  const NmlLoads* loads;
  std::function<Clock::time_point()> clock;

  // The table accepted last, and what the panel is shown of it.
  std::optional<std::vector<LoadedTool>> loaded;
  // The pass of this call equals it: the prepared index may be resolved in it.
  bool loaded_current{};
  // A pass that waits to be read again, and when it was taken.
  struct Pending {
    std::vector<LoadedTool> tools;
    Clock::time_point taken_at;
  };
  std::optional<Pending> pending;
  std::uint64_t tool_loads_seen{};
  std::map<int, std::string> comments;
  ToolTable tool_table;
  bool tools_failed{};
  // The load the program text belongs to.
  std::string program_file;
  std::uint64_t program_loads_seen{};
  std::uint64_t program_revision{};
  std::shared_ptr<const ProgramText> program;
  // The line counters of the loaded revision; see the header.
  struct Lines {
    std::uint64_t starts_seen{}; // the port's count when the revision began
    bool started{};
    int current_before{}; // as read last before the start
    int motion_before{};
    bool current_shown{};
    bool motion_shown{};
  };
  Lines lines;

  Impl(MachineConfig machine, const NmlStatusMailbox& readings, ToolReader tool_reader,
       ProgramReader program_reader, const NmlLoads* counted,
       std::function<Clock::time_point()> time)
      : config(std::move(machine)), mailbox(readings), tools(std::move(tool_reader)),
        programs(std::move(program_reader)), loads(counted), clock(std::move(time)) {}

  void show_tools(const std::vector<LoadedTool>& accepted) {
    if (std::ifstream in{config.tool_table, std::ios::binary}; in && !config.tool_table.empty())
      comments = comments_of(
          std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}});
    std::vector<Tool> shown;
    shown.reserve(accepted.size());
    for (const auto& entry : accepted) {
      shown.push_back(entry.tool);
      if (const auto comment = comments.find(entry.tool.number); comment != comments.end())
        shown.back().comment = comment->second;
    }
    if (tool_table.revision == 0 || shown != tool_table.tools)
      tool_table = {tool_table.revision + 1, std::move(shown)};
  }

  void read_tools(std::vector<ControllerMessage>& messages) {
    loaded_current = false;
    if (!tools)
      return;
    // The status as it was before this pass: it can vouch for an earlier pass only.
    const auto status = mailbox.latest();
    auto pass = tools();
    if (!pass) {
      pending.reset();
      if (!std::exchange(tools_failed, true))
        messages.push_back({true, "LinuxCNC's tool table cannot be read."});
      return;
    }
    tools_failed = false;
    const auto counted = loads != nullptr ? loads->tool_tables.load() : 0;
    if (loaded && *pass == *loaded) {
      pending.reset();
      loaded_current = true;
      // A load the port reports with the same numbers: the comments may have changed.
      if (counted != tool_loads_seen) {
        tool_loads_seen = counted;
        show_tools(*pass);
      }
      return;
    }
    if (!pending || *pass != pending->tools) {
      pending = Pending{std::move(*pass), clock()};
      return;
    }
    // The same table a second time: accepted once a status read after the first pass says
    // that task has nothing in execution, so no table load either.
    if (!status || status->read_at <= pending->taken_at ||
        status->status.controller.link == Link::down || status->status.task_executing)
      return;
    show_tools(*pass);
    loaded = std::move(*pass);
    pending.reset();
    loaded_current = true;
    tool_loads_seen = counted;
  }

  // The port's counts, the starts as they were while `programs` had this value.
  struct Counts {
    std::uint64_t programs{};
    std::uint64_t starts{};
  };
  [[nodiscard]] auto counts() const -> Counts {
    if (loads == nullptr)
      return {};
    for (auto opened = loads->programs.load();;) {
      const auto starts = loads->program_starts.load();
      const auto again = loads->programs.load();
      if (again == opened)
        return {opened, starts};
      opened = again;
    }
  }

  // The counters of one status of the loaded revision, each 0 until that revision produced it.
  void track_lines(const NmlStatus& status, const Counts& now, ProgramStatus& shown) {
    const auto& controller = status.controller;
    if (!lines.started) {
      if (now.starts != lines.starts_seen ||
          (controller.mode == TaskMode::automatic && controller.interp != InterpState::idle)) {
        lines.started = true;
      } else {
        lines.current_before = status.current_line;
        lines.motion_before = status.motion_line;
      }
    }
    lines.current_shown =
        lines.current_shown || (lines.started && status.current_line != lines.current_before);
    lines.motion_shown =
        lines.motion_shown || (lines.started && status.motion_line != lines.motion_before);
    shown.current_line = lines.current_shown ? status.current_line : 0;
    shown.motion_line = lines.motion_shown ? status.motion_line : 0;
  }

  void read_program(const NmlStatus& status, const Counts& now,
                    std::vector<ControllerMessage>& messages) {
    if (status.controller.link == Link::down)
      return; // Nothing is known: what was read last stays.
    const auto counted = now.programs;
    if (status.program_file.empty()) {
      program_file.clear();
      program_loads_seen = counted;
      program.reset();
      return;
    }
    if (status.program_file == program_file && counted == program_loads_seen)
      return;
    program_file = status.program_file;
    program_loads_seen = counted;
    ++program_revision;
    lines = Lines{.starts_seen = now.starts};
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
                               ToolReader tools, ProgramReader programs, const NmlLoads* loads,
                               std::function<Clock::time_point()> clock)
    : impl_(std::make_unique<Impl>(config, mailbox, std::move(tools), std::move(programs), loads,
                                   std::move(clock))) {}
NmlPanelSource::~NmlPanelSource() = default;

auto NmlPanelSource::hello() const -> Hello { return to_hello(impl_->config); }

auto NmlPanelSource::read() -> PanelReading {
  PanelReading reading;
  // Everything that can take time first: the status is taken after it, and aged last.
  impl_->read_tools(reading.messages);
  const auto counts = impl_->counts();
  if (const auto before = impl_->mailbox.latest())
    impl_->read_program(before->status, counts, reading.messages);
  auto latest = impl_->mailbox.latest();
  // Task named another file meanwhile: its text is read now, and the status taken again.
  if (latest && latest->status.program_file != impl_->program_file) {
    impl_->read_program(latest->status, counts, reading.messages);
    latest = impl_->mailbox.latest();
  }
  auto status = latest ? latest->status : NmlStatus{};
  if (latest && status.controller.link == Link::live && impl_->clock() - latest->read_at > max_age)
    status.controller.link = Link::stale;
  const bool loaded = status.program_file == impl_->program_file && !status.program_file.empty();
  // The prepared index is resolved only in a table that is current.
  std::span<const LoadedTool> tools;
  if (const auto& accepted = impl_->loaded; accepted && impl_->loaded_current)
    tools = *accepted;
  reading.status =
      to_panel_status(status, impl_->config, tools, loaded ? impl_->program_revision : 0);
  // The text goes with the status that names its file; a file task named during this very
  // call is read in the next period.
  if (loaded && status.controller.link != Link::down)
    impl_->track_lines(status, counts, reading.status.program);
  if (!loaded)
    reading.status.program = ProgramStatus{.file = {},
                                           .revision = 0,
                                           .current_line = 0,
                                           .motion_line = 0,
                                           .optional_stop = reading.status.program.optional_stop,
                                           .block_delete = reading.status.program.block_delete};
  reading.tool_table = impl_->tool_table;
  reading.work_systems = to_work_systems(status);
  reading.program = loaded ? impl_->program : nullptr;
  return reading;
}

} // namespace coffeecnc
