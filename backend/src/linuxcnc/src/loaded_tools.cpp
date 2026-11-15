#include <coffeecnc/linuxcnc/loaded_tools.hpp>

// LinuxCNC's own declarations: the installed header where the development files have one,
// src/linuxcnc/upstream/tooldata.hh where they do not (see CMakeLists.txt).
#include <tooldata.hh>

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

auto loaded_tools(const std::vector<RawTool>& raw, double millimetres_per_unit)
    -> std::vector<LoadedTool> {
  std::vector<LoadedTool> tools;
  for (const auto& entry : raw)
    if (entry.index >= 1)
      tools.push_back({entry.index,
                       {entry.number, entry.pocket, entry.offset[2] * millimetres_per_unit,
                        entry.diameter * millimetres_per_unit, {}}});
  return tools;
}

auto LoadedToolTable::read() const -> std::vector<LoadedTool> {
  return loaded_tools(read_raw(), scale_);
}

auto LoadedToolTable::read_raw() const -> std::vector<RawTool> {
  std::vector<RawTool> tools;
  const int last = tooldata_last_index_get();
  for (int index = 0; index <= last && index < CANON_POCKETS_MAX; ++index) {
    CANON_TOOL_TABLE entry{};
    if (tooldata_get(&entry, index) != IDX_OK || entry.toolno < 0)
      continue;
    tools.push_back({index,
                     entry.toolno,
                     entry.pocketno,
                     {entry.offset.tran.x, entry.offset.tran.y, entry.offset.tran.z},
                     entry.diameter,
                     entry.frontangle,
                     entry.backangle,
                     entry.orientation});
  }
  return tools;
}

} // namespace coffeecnc
