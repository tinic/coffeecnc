#include <coffeecnc/linuxcnc/loaded_tools.hpp>

#include <emctool.h>

// libtooldata's interface. linuxcnc-uspace-dev 2.9.4 installs the library and emctool.h but
// not tooldata.hh (src/emc/tooldata/tooldata.hh), so the three functions used are declared
// here as that header declares them.
enum toolidx_t { IDX_OK = 0, IDX_NEW, IDX_FAIL };
auto tool_mmap_user() -> int;
auto tooldata_last_index_get() -> int;
auto tooldata_get(CANON_TOOL_TABLE* pdata, int idx) -> toolidx_t;

namespace coffeecnc {

auto LoadedToolTable::attach(double millimetres_per_unit)
    -> std::expected<LoadedToolTable, std::string> {
  // tool_mmap_user ends the process when the file is there and cannot be mapped, and
  // tooldata_get does when nothing is mapped: neither is called unless the map exists.
  if (tool_mmap_user() != 0)
    return std::unexpected(
        std::string("LinuxCNC's tool table memory is not there ($HOME/.tool.mmap)."));
  return LoadedToolTable{millimetres_per_unit};
}

auto LoadedToolTable::read() const -> std::vector<LoadedTool> {
  std::vector<LoadedTool> tools;
  const int last = tooldata_last_index_get();
  for (int index = 1; index <= last && index < CANON_POCKETS_MAX; ++index) {
    CANON_TOOL_TABLE entry{};
    // An empty entry has tool number -1.
    if (tooldata_get(&entry, index) != IDX_OK || entry.toolno < 0)
      continue;
    tools.push_back({index,
                     {entry.toolno, entry.pocketno, entry.offset.tran.z * scale_,
                      entry.diameter * scale_, {}}});
  }
  return tools;
}

} // namespace coffeecnc
