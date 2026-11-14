#ifndef COFFEECNC_LINUXCNC_LOADED_TOOLS_HPP
#define COFFEECNC_LINUXCNC_LOADED_TOOLS_HPP

#include <coffeecnc/core/command_policy.hpp>

#include <expected>
#include <string>
#include <vector>

namespace coffeecnc {

// One entry of the tool table LinuxCNC has loaded. The comment is empty: LinuxCNC keeps
// comments in the file only.
struct LoadedTool {
  // Where the entry is in LinuxCNC's table: what EMC_STAT's io.tool.pocketPrepped names. On a
  // machine without a random tool changer entry n is the n-th tool row of the file as it was
  // when LinuxCNC loaded it; index 0 is the spindle and not a table row.
  int index{};
  Tool tool; // lengths in millimetres
  friend auto operator==(const LoadedTool&, const LoadedTool&) -> bool = default;
};

// The tool table as LinuxCNC has it loaded: its tool data shared memory, the same that
// LinuxCNC's own GUIs read (Python `stat.tool_table`). Not NML: EMC_STAT carries only the tool
// in the spindle on LinuxCNC 2.9. The NML gate is not involved.
//
// LinuxCNC's io process creates the memory as the file $HOME/.tool.mmap when it starts; this
// maps it through libtooldata. Every access takes a lock word in the memory for one entry;
// libtooldata waits up to 10 s for it and then goes on without, so a read belongs on a thread
// that may block: the publisher thread.
//
// What one pass is worth. The lock covers one entry, not the table, and the memory has no
// sequence number and no mark for a load in progress (src/emc/tooldata/tooldata_mmap.cc: the
// header holds the lock word, the last index and the tool changer kind). A reload replaces the
// entries one by one, so a pass can see half of it, and two equal passes can both see the same
// half while io stands still. A pass is therefore only "as read"; NmlPanelSource decides when
// one may be shown.
//
// libtooldata keeps the mapping in the process, not in an object: one LoadedToolTable per
// process, used by one thread. NOT thread-safe.
class LoadedToolTable {
public:
  // Maps the memory. Fails while no LinuxCNC runs for this user.
  [[nodiscard]] static auto attach(double millimetres_per_unit)
      -> std::expected<LoadedToolTable, std::string>;

  // One pass: every entry that holds a tool (index 1 and up), in index order, as read.
  [[nodiscard]] auto read() const -> std::vector<LoadedTool>;

private:
  explicit LoadedToolTable(double millimetres_per_unit) : scale_(millimetres_per_unit) {}
  double scale_;
};

} // namespace coffeecnc

#endif // COFFEECNC_LINUXCNC_LOADED_TOOLS_HPP
