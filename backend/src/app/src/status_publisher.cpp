#include <coffeecnc/app/status_publisher.hpp>

#include <coffeecnc/app/messages.hpp>

#include <algorithm>
#include <string_view>
#include <utility>

namespace coffeecnc {
namespace {

auto same_tool(const Tool& a, const Tool& b) -> bool {
  return a.number == b.number && a.pocket == b.pocket && a.length == b.length &&
         a.diameter == b.diameter;
}

// Equal for the path: tool comments do not count.
auto same_inputs(const PreviewInputs& a, const PreviewInputs& b) -> bool {
  return a.program_file == b.program_file && a.program_revision == b.program_revision &&
         a.files_generation == b.files_generation && a.raw_tools == b.raw_tools &&
         a.systems == b.systems && a.g92 == b.g92 && a.block_delete == b.block_delete &&
         a.start == b.start &&
         std::ranges::equal(a.tools, b.tools, same_tool);
}

} // namespace

auto PreviewContext::update(const PreviewInputs& inputs, bool running) -> std::uint64_t {
  PreviewInputs now = inputs;
  if (running && last_) {
    // What the machine is while the program runs is the program's doing so far: a preview
    // computed from it would replay the program from its first line with offsets and tools
    // that exist only because part of it has run.
    now.start = last_->start;
    now.systems = last_->systems;
    now.g92 = last_->g92;
    now.tools = last_->tools;
    now.raw_tools = last_->raw_tools;
  }
  if (!last_ || !same_inputs(now, *last_)) {
    ++context_;
    last_ = std::move(now);
  }
  return context_;
}

auto StatusPublisher::publish(const PanelState& state, bool snapshots,
                              std::vector<PanelRecord>* records)
    -> std::expected<std::vector<std::string>, LocalizedReason> {
  std::vector<std::string> messages;
  LocalizedReason error;
  const auto add = [&](PanelRecord record, std::string_view type, EncodedMessage encoded) -> bool {
    if (!encoded) {
      error = std::move(encoded.error());
      const auto prefix = std::string(type) + ": ";
      error.text.insert(0, prefix);
      if (error.localization && error.localization->params.contains("field"))
        error.localization->params.at("field").insert(0, prefix);
      return false;
    }
    messages.push_back(std::move(*encoded));
    if (records != nullptr)
      records->push_back(record);
    return true;
  };
  const bool tool_table = snapshots || tool_table_ != state.tool_table;
  const bool work_offsets = snapshots || work_offsets_ != state.work_offsets;
  const bool history =
      state.offset_history != nullptr &&
      (snapshots || history_revision_ != state.offset_history->revision);
  const bool program = state.program != nullptr && (snapshots || program_ != *state.program);
  const bool preview = snapshots || preview_ != state.preview;

  if ((snapshots && !add(PanelRecord::hello, "hello", encode_hello(state.hello))) ||
      (tool_table && !add(PanelRecord::tool_table, "tool_table", encode_tool_table(state.tool_table))) ||
      (work_offsets && !add(PanelRecord::work_offsets, "work_offsets", encode_work_offsets(state.work_offsets))) ||
      (history && !add(PanelRecord::offset_history, "offset_history", encode_offset_history(*state.offset_history))) ||
      (program && !add(PanelRecord::program, "program", encode_program(*state.program))) ||
      (preview && !add(PanelRecord::preview, "preview", encode_preview(state.preview))) ||
      !add(PanelRecord::status, "status", encode_status(state.status, seq_ + 1)))
    return std::unexpected(std::move(error));

  ++seq_;
  if (tool_table)
    tool_table_ = state.tool_table;
  if (work_offsets)
    work_offsets_ = state.work_offsets;
  if (history)
    history_revision_ = state.offset_history->revision;
  if (program)
    program_ = *state.program;
  else if (state.program == nullptr)
    program_.reset(); // The next program is new even when it equals the one sent last.
  if (preview)
    preview_ = state.preview;
  return messages;
}

} // namespace coffeecnc
