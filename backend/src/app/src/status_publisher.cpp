#include <coffeecnc/app/status_publisher.hpp>

#include <coffeecnc/app/messages.hpp>

#include <bit>
#include <string_view>
#include <utility>

namespace coffeecnc {
namespace {

// 64-bit FNV-1a. Every value is fed as eight bytes, least significant first, and every sequence
// with its length first, so the result does not depend on the host or on where one value ends.
class Hash {
public:
  void integer(std::uint64_t value) {
    for (unsigned byte = 0; byte < 8; ++byte) {
      state_ ^= (value >> (8 * byte)) & 0xFFU;
      state_ *= 1099511628211ULL;
    }
  }
  void signed_integer(int value) { integer(static_cast<std::uint64_t>(std::int64_t{value})); }
  void real(double value) {
    // -0.0 equals 0.0 and must hash like it.
    integer(std::bit_cast<std::uint64_t>(value == 0 ? 0.0 : value));
  }
  void vec(const Vec3& value) {
    for (const auto coordinate : value)
      real(coordinate);
  }
  void text(std::string_view value) {
    integer(value.size());
    for (const auto character : value)
      integer(static_cast<unsigned char>(character));
  }
  [[nodiscard]] auto folded() const -> std::uint64_t {
    constexpr unsigned bits = 53;
    return (state_ ^ (state_ >> bits)) & ((std::uint64_t{1} << bits) - 1);
  }

private:
  std::uint64_t state_{14695981039346656037ULL};
};

} // namespace

auto preview_context(const PreviewInputs& inputs) -> std::uint64_t {
  Hash hash;
  hash.integer(inputs.program_file ? 1 : 0);
  hash.text(inputs.program_file ? std::string_view{*inputs.program_file} : std::string_view{});
  hash.integer(inputs.program_revision);
  for (const auto& system : inputs.systems) {
    hash.vec(system.offset);
    hash.real(system.rotation_xy);
  }
  hash.vec(inputs.g92);
  hash.integer(inputs.tools.size());
  for (const auto& tool : inputs.tools) {
    hash.signed_integer(tool.number);
    hash.signed_integer(tool.pocket);
    hash.real(tool.length);
    hash.real(tool.diameter);
  }
  return hash.folded();
}

auto StatusPublisher::publish(const PanelState& state, bool snapshots)
    -> std::expected<std::vector<std::string>, std::string> {
  std::vector<std::string> messages;
  std::string error;
  const auto add = [&messages, &error](std::string_view type, Encoded encoded) -> bool {
    if (!encoded) {
      error = std::string(type) + ": " + encoded.error();
      return false;
    }
    messages.push_back(std::move(*encoded));
    return true;
  };
  const bool tool_table = snapshots || tool_table_ != state.tool_table;
  const bool work_offsets = snapshots || work_offsets_ != state.work_offsets;
  const bool program = state.program != nullptr && (snapshots || program_ != *state.program);
  const bool preview = snapshots || preview_ != state.preview;

  if ((snapshots && !add("hello", encode_hello(state.hello))) ||
      (tool_table && !add("tool_table", encode_tool_table(state.tool_table))) ||
      (work_offsets && !add("work_offsets", encode_work_offsets(state.work_offsets))) ||
      (program && !add("program", encode_program(*state.program))) ||
      (preview && !add("preview", encode_preview(state.preview))) ||
      !add("status", encode_status(state.status, seq_ + 1)))
    return std::unexpected(std::move(error));

  ++seq_;
  if (tool_table)
    tool_table_ = state.tool_table;
  if (work_offsets)
    work_offsets_ = state.work_offsets;
  if (program)
    program_ = *state.program;
  else if (state.program == nullptr)
    program_.reset(); // The next program is new even when it equals the one sent last.
  if (preview)
    preview_ = state.preview;
  return messages;
}

} // namespace coffeecnc
