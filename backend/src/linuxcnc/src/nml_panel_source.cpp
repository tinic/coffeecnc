#include <coffeecnc/linuxcnc/nml_panel_source.hpp>
#include <coffeecnc/core/text_file.hpp>

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
  RawToolReader raw_reader;
  // The table with every field: accepted by the same rule as the table shown, on every field.
  std::vector<PreviewTool> preview_tools;
  struct PendingRaw {
    std::vector<PreviewTool> tools;
    Clock::time_point taken_at;
  };
  std::optional<PendingRaw> pending_raw;
  ToolTable tool_table;
  // The accepted rows while the table is not shown because its file cannot be read.
  std::optional<std::vector<Tool>> withheld;
  bool tools_failed{};
  // The tool table file could not be read for the accepted table; said already.
  bool comments_failed{};
  // The load the program text belongs to.
  std::string program_file;
  std::uint64_t program_loads_seen{};
  std::uint64_t program_revision{};
  std::shared_ptr<const ProgramText> program;
  // What is attributed to the loaded revision of task's line counters; see the header.
  struct Lines {
    // Statuses read up to here are not evidence: the revision or an MDI block came after.
    Clock::time_point not_before;
    std::optional<Clock::time_point> judged; // when the status judged last was read
    bool executing{};                         // that status showed the program running
    // The counters as read last while it did not run, current then motion.
    std::optional<std::pair<int, int>> before;
    bool current_moved{}; // in this run
    bool motion_moved{};
    int current{}; // attributed: what is reported
    int motion{};
  };
  Lines lines;
  std::uint64_t mdi_blocks_seen{};

  Impl(MachineConfig machine, const NmlStatusMailbox& readings, ToolReader tool_reader,
       ProgramReader program_reader, const NmlLoads* counted,
       std::function<Clock::time_point()> time)
      : config(std::move(machine)), mailbox(readings), tools(std::move(tool_reader)),
        programs(std::move(program_reader)), loads(counted), clock(std::move(time)) {}

  // A table is shown only with the comments read from the file for it: a tool's comment is
  // part of what a saved tool model is bound to, and neither the text of an earlier table nor
  // an empty text (a real value) may stand in for one that could not be read. While the file
  // cannot be read (not there, a directory, a read error, above max_tool_table_bytes) no row
  // is shown, which is said once; the accepted rows still count for the preview context
  // (`withheld`), and the prepared tool and the preview use other tables.
  void show_tools(const std::vector<LoadedTool>& accepted,
                  std::vector<ControllerMessage>& messages) {
    bool read = config.tool_table.empty();
    std::map<int, std::string> comments;
    if (!read) {
      if (const auto text = read_text_file(config.tool_table, max_tool_table_bytes)) {
        comments = comments_of(*text);
        read = true;
      }
    }
    std::vector<Tool> rows;
    rows.reserve(accepted.size());
    for (const auto& entry : accepted) {
      // T0 is a no-tool placeholder, never an editable tool/model row. Keep the
      // loaded table intact for prepared-tool and unload-handshake resolution.
      if (entry.tool.number <= 0)
        continue;
      rows.push_back(entry.tool);
      if (const auto comment = comments.find(entry.tool.number); comment != comments.end())
        rows.back().comment = comment->second;
    }
    std::vector<Tool> shown;
    if (read) {
      shown = std::move(rows);
      withheld.reset();
      comments_failed = false;
    } else {
      withheld = std::move(rows);
      if (!std::exchange(comments_failed, true))
        messages.push_back({true, "The tool table file " + config.tool_table +
                                      " cannot be read: the tool table is not shown."});
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
    if (pass && raw_reader)
      accept_raw(raw_reader(), status);
    show_pass(std::move(pass), status, messages);
  }

  // The table with every field, by the rule of the table shown: a pass that differs from the
  // accepted one is accepted when the next pass equals it in every field of every entry and
  // a status read after the first of the two says that task has nothing in execution. A
  // table caught half loaded is so never an input of a preview.
  void accept_raw(std::vector<PreviewTool> raw, const std::optional<TimedStatus>& status) {
    if (raw == preview_tools) {
      pending_raw.reset();
    } else if (!pending_raw || raw != pending_raw->tools) {
      pending_raw = PendingRaw{std::move(raw), clock()};
    } else if (status && status->read_at > pending_raw->taken_at &&
               status->status.controller.link != Link::down && !status->status.task_executing) {
      preview_tools = std::move(raw);
      pending_raw.reset();
    }
  }

  void show_pass(std::optional<std::vector<LoadedTool>> pass,
                 const std::optional<TimedStatus>& status,
                 std::vector<ControllerMessage>& messages) {
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
      // A load the port reports with the same numbers: the comments may have changed. And a
      // table withheld for its file is tried again every period.
      if (counted != tool_loads_seen || comments_failed) {
        tool_loads_seen = counted;
        show_tools(*pass, messages);
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
    show_tools(*pass, messages);
    loaded = std::move(*pass);
    pending.reset();
    loaded_current = true;
    tool_loads_seen = counted;
  }

  void track_lines(const NmlStatus& status, Clock::time_point read_at) {
    if (const auto counted = loads != nullptr ? loads->mdi_blocks.load() : 0;
        counted != mdi_blocks_seen) {
      mdi_blocks_seen = counted;
      lines = Lines{};
      lines.not_before = clock();
    }
    const auto& controller = status.controller;
    if (controller.link != Link::live || read_at <= lines.not_before ||
        (lines.judged && read_at <= *lines.judged))
      return;
    lines.judged = read_at;
    const std::pair now{status.current_line, status.motion_line};
    if (controller.mode != TaskMode::automatic) {
      const auto not_before = lines.not_before;
      lines = Lines{};
      lines.not_before = not_before;
      lines.judged = read_at;
      lines.before = now;
      return;
    }
    if (controller.interp == InterpState::idle) {
      // The run has ended: the lines it stopped at.
      if (std::exchange(lines.executing, false)) {
        if (lines.current_moved)
          lines.current = now.first;
        if (lines.motion_moved)
          lines.motion = now.second;
      }
      lines.before = now;
      return;
    }
    if (!std::exchange(lines.executing, true)) {
      lines.current_moved = lines.motion_moved = false;
      if (!lines.before)
        lines.before = now;
    }
    const bool moving = !controller.in_position;
    lines.current_moved = lines.current_moved || now.first != lines.before->first || moving ||
                          status.dwell_seconds_left > 0 || controller.requested_tool.has_value();
    lines.motion_moved = lines.motion_moved || now.second != lines.before->second || moving;
    lines.current = lines.current_moved ? now.first : 0;
    lines.motion = lines.motion_moved ? now.second : 0;
  }

  void read_program(const NmlStatus& status, std::vector<ControllerMessage>& messages) {
    if (status.controller.link == Link::down)
      return; // Nothing is known: what was read last stays.
    const auto counted = loads != nullptr ? loads->programs.load() : 0;
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
    lines = Lines{};
    lines.not_before = clock();
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

void NmlPanelSource::raw_tools(RawToolReader reader) { impl_->raw_reader = std::move(reader); }

auto NmlPanelSource::hello() const -> Hello { return to_hello(impl_->config); }

auto NmlPanelSource::read() -> PanelReading {
  PanelReading reading;
  // Everything that can take time first: the status is taken after it, and aged last.
  impl_->read_tools(reading.messages);
  if (const auto before = impl_->mailbox.latest())
    impl_->read_program(before->status, reading.messages);
  auto latest = impl_->mailbox.latest();
  // Task named another file meanwhile: its text is read now, and the status taken again.
  if (latest && latest->status.program_file != impl_->program_file) {
    impl_->read_program(latest->status, reading.messages);
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
  if (loaded && latest) {
    impl_->track_lines(status, latest->read_at);
    reading.status.program.current_line = impl_->lines.current;
    reading.status.program.motion_line = impl_->lines.motion;
  }
  if (!loaded)
    reading.status.program = ProgramStatus{.file = {},
                                           .revision = 0,
                                           .current_line = 0,
                                           .motion_line = 0,
                                           .optional_stop = reading.status.program.optional_stop,
                                           .block_delete = reading.status.program.block_delete};
  reading.tool_table = impl_->tool_table;
  reading.accepted_tools = impl_->withheld;
  reading.preview_tools = impl_->preview_tools;
  reading.tools_settled = !impl_->pending_raw.has_value();
  reading.task_executing = status.task_executing;
  reading.commanded_position = status.commanded_position;
  reading.program_aborts = impl_->loads ? impl_->loads->aborts.load() : 0;
  reading.program_failed = status.task_failed;
  reading.work_systems = to_work_systems(status);
  reading.program = loaded ? impl_->program : nullptr;
  return reading;
}

} // namespace coffeecnc
