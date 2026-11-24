#include <coffeecnc/app/backup_archive.hpp>

#include <archive.h>
#include <archive_entry.h>

#include <array>
#include <format>
#include <memory>
#include <utility>

namespace coffeecnc {
namespace {

struct ReadDeleter {
  void operator()(archive* handle) const { archive_read_free(handle); }
};
struct WriteDeleter {
  void operator()(archive* handle) const { archive_write_free(handle); }
};
struct EntryDeleter {
  void operator()(archive_entry* entry) const { archive_entry_free(entry); }
};

auto error_of(archive* handle) -> std::string {
  const char* text = archive_error_string(handle);
  return text != nullptr ? std::string(text) : std::string("unknown error");
}

auto append(archive* /*handle*/, void* client, const void* buffer, std::size_t length)
    -> la_ssize_t {
  static_cast<std::string*>(client)->append(static_cast<const char*>(buffer), length);
  return static_cast<la_ssize_t>(length);
}

} // namespace

auto write_archive(const std::vector<ArchiveEntry>& entries)
    -> std::expected<std::string, LocalizedReason> {
  std::string out;
  const std::unique_ptr<archive, WriteDeleter> writer{archive_write_new()};
  if (!writer)
    return std::unexpected(LocalizedReason{"The archive cannot be made.",
                                           MessageKey{"backend.machineData.cannotMake"}});
  if (archive_write_set_format_pax_restricted(writer.get()) != ARCHIVE_OK ||
      archive_write_add_filter_gzip(writer.get()) != ARCHIVE_OK ||
      archive_write_set_bytes_in_last_block(writer.get(), 1) != ARCHIVE_OK ||
      archive_write_open(writer.get(), &out, nullptr, append, nullptr) != ARCHIVE_OK) {
    const auto detail = error_of(writer.get());
    return std::unexpected(
        LocalizedReason{std::format("The archive cannot be made: {}", detail),
                        MessageKey{"backend.machineData.cannotMakeDetail", {{"detail", detail}}}});
  }
  for (const auto& item : entries) {
    if (item.name.empty())
      return std::unexpected(LocalizedReason{"An archive entry has no name.",
                                             MessageKey{"backend.machineData.noName"}});
    if (item.bytes.size() > max_archive_entry_bytes)
      return std::unexpected(
          LocalizedReason{std::format("{} is larger than 16 MiB.", item.name),
                          MessageKey{"backend.machineData.tooLarge", {{"path", item.name}}}});
    const std::unique_ptr<archive_entry, EntryDeleter> entry{archive_entry_new()};
    archive_entry_set_pathname(entry.get(), item.name.c_str());
    archive_entry_set_filetype(entry.get(), AE_IFREG);
    archive_entry_set_perm(entry.get(), 0644);
    archive_entry_set_size(entry.get(), static_cast<la_int64_t>(item.bytes.size()));
    archive_entry_set_mtime(entry.get(), item.modified, 0);
    if (archive_write_header(writer.get(), entry.get()) != ARCHIVE_OK ||
        archive_write_data(writer.get(), item.bytes.data(), item.bytes.size()) !=
            static_cast<la_ssize_t>(item.bytes.size())) {
      const auto detail = error_of(writer.get());
      return std::unexpected(LocalizedReason{
          std::format("{} cannot be put in the archive: {}", item.name, detail),
          MessageKey{"backend.machineData.cannotPut", {{"name", item.name}, {"detail", detail}}}});
    }
  }
  if (archive_write_close(writer.get()) != ARCHIVE_OK) {
    const auto detail = error_of(writer.get());
    return std::unexpected(
        LocalizedReason{std::format("The archive cannot be made: {}", detail),
                        MessageKey{"backend.machineData.cannotMakeDetail", {{"detail", detail}}}});
  }
  return out;
}

auto read_archive(std::string_view bytes)
    -> std::expected<std::vector<ArchiveEntry>, LocalizedReason> {
  if (bytes.size() > max_archive_bytes)
    return std::unexpected(LocalizedReason{"The archive is larger than 64 MiB.",
                                           MessageKey{"backend.machineData.archiveTooLarge"}});
  const std::unique_ptr<archive, ReadDeleter> reader{archive_read_new()};
  if (!reader)
    return std::unexpected(LocalizedReason{"The archive cannot be read.",
                                           MessageKey{"backend.machineData.cannotOpenArchive"}});
  // Tar only, plain or gzip: nothing else is a CoffeeCNC backup.
  if (archive_read_support_format_tar(reader.get()) != ARCHIVE_OK ||
      archive_read_support_filter_gzip(reader.get()) != ARCHIVE_OK ||
      archive_read_open_memory(reader.get(), bytes.data(), bytes.size()) != ARCHIVE_OK) {
    const auto detail = error_of(reader.get());
    return std::unexpected(
        LocalizedReason{std::format("This is not a backup archive: {}", detail),
                        MessageKey{"backend.machineData.notArchive", {{"detail", detail}}}});
  }
  std::vector<ArchiveEntry> entries;
  std::size_t total = 0;
  for (;;) {
    archive_entry* entry = nullptr;
    const int status = archive_read_next_header(reader.get(), &entry);
    if (status == ARCHIVE_EOF)
      break;
    if (status != ARCHIVE_OK || entry == nullptr) {
      const auto detail = error_of(reader.get());
      return std::unexpected(
          LocalizedReason{std::format("The archive is damaged: {}", detail),
                          MessageKey{"backend.machineData.damaged", {{"detail", detail}}}});
    }
    const char* path = archive_entry_pathname(entry);
    const std::string name = path != nullptr ? path : "";
    const auto type = archive_entry_filetype(entry);
    if (type == AE_IFDIR)
      continue;
    if (type != AE_IFREG || archive_entry_hardlink(entry) != nullptr ||
        archive_entry_symlink(entry) != nullptr)
      return std::unexpected(
          LocalizedReason{std::format("{} is not a regular file: the archive is refused.", name),
                          MessageKey{"backend.machineData.notRegular", {{"name", name}}}});
    const auto size = archive_entry_size(entry);
    if (size < 0 || static_cast<std::uint64_t>(size) > max_archive_entry_bytes)
      return std::unexpected(
          LocalizedReason{std::format("{} is larger than 16 MiB.", name),
                          MessageKey{"backend.machineData.tooLarge", {{"path", name}}}});
    ArchiveEntry item{name, {}, archive_entry_mtime(entry)};
    std::array<char, 65536> buffer{};
    for (;;) {
      const auto got = archive_read_data(reader.get(), buffer.data(), buffer.size());
      if (got == 0)
        break;
      if (got < 0) {
        const auto detail = error_of(reader.get());
        return std::unexpected(
            LocalizedReason{std::format("The archive is damaged: {}", detail),
                            MessageKey{"backend.machineData.damaged", {{"detail", detail}}}});
      }
      total += static_cast<std::size_t>(got);
      if (item.bytes.size() + static_cast<std::size_t>(got) > max_archive_entry_bytes)
        return std::unexpected(
            LocalizedReason{std::format("{} is larger than 16 MiB.", name),
                            MessageKey{"backend.machineData.tooLarge", {{"path", name}}}});
      if (total > max_archive_bytes)
        return std::unexpected(LocalizedReason{"The archive is larger than 64 MiB.",
                                               MessageKey{"backend.machineData.archiveTooLarge"}});
      item.bytes.append(buffer.data(), static_cast<std::size_t>(got));
    }
    entries.push_back(std::move(item));
  }
  return entries;
}

} // namespace coffeecnc
