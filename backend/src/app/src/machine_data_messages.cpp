#include <coffeecnc/app/machine_data.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iterator>
#include <utility>

namespace coffeecnc {
namespace {
using Json = nlohmann::json;

auto dump(const Json& value) -> std::string {
  return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}
} // namespace

auto encode_backup_written(const BackupWritten& written) -> std::string {
  auto left_out = Json::array();
  for (const auto& [path, reason] : written.not_included)
    left_out.push_back({{"path", path}, {"reason", reason}});
  return dump(Json{{"type", "backup_written"},
                   {"path", written.path},
                   {"files", written.files},
                   {"bytes", written.bytes},
                   {"not_included", std::move(left_out)}});
}

auto encode_restore_summary(const RestoreSummary& summary) -> std::string {
  auto files = Json::array();
  std::ranges::transform(summary.files, std::back_inserter(files), [](const auto& file) -> Json {
    return {{"role", file.role},
            {"name", file.name},
            {"target", file.target},
            {"change", file.change},
            {"bytes", file.bytes},
            {"symlink", file.symlink}};
  });
  return dump(Json{{"type", "restore_summary"},
                   {"path", summary.path},
                   {"token", summary.token},
                   {"machine_id", summary.machine_id ? Json(*summary.machine_id) : Json(nullptr)},
                   {"created", summary.created},
                   {"files", std::move(files)}});
}

} // namespace coffeecnc
