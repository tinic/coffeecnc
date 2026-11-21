#include <coffeecnc/app/tool_model_store.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <format>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace coffeecnc {
namespace {

constexpr auto not_saved = "Tool models are not saved.";
constexpr auto left_alone = "It is not changed: move or remove it and restart CoffeeCNC.";
constexpr mode_t directory_mode = 0700;
constexpr mode_t file_mode = 0600;

// The path when it is given and absolute, else null.
auto absolute(const std::optional<std::string>& path) -> const std::string* {
  return path && path->starts_with('/') ? &*path : nullptr;
}

auto failure(std::string_view what) -> std::unexpected<std::string> {
  return std::unexpected(std::format("{}: {}", what, std::generic_category().message(errno)));
}

auto close_descriptor(int& descriptor) -> bool {
  if (descriptor < 0)
    return true;
  const bool closed = ::close(descriptor) == 0;
  descriptor = -1;
  return closed;
}

// The document a command leaves behind, or why there is none.
auto changed(ToolModelDocument document, const ToolModelCommand& command)
    -> std::expected<ToolModelDocument, std::string> {
  if (const auto* defaults = std::get_if<SetToolModelDefaults>(&command)) {
    document.defaults = defaults->defaults;
    return document;
  }
  const auto& asked = std::get<SetToolModelAssignment>(command);
  auto& assignments = document.assignments;
  // In rising tool order.
  const auto place =
      std::ranges::lower_bound(assignments, asked.tool.number, {},
                               [](const ToolModelAssignment& a) -> int { return a.tool.number; });
  const bool found = place != assignments.end() && place->tool.number == asked.tool.number;
  if (!asked.family) {
    if (found)
      assignments.erase(place);
    return document;
  }
  ToolModelAssignment assignment{asked.tool, *asked.family, asked.overrides, asked.holder};
  if (found) {
    *place = std::move(assignment);
  } else {
    if (assignments.size() >= max_tool_model_assignments)
      return std::unexpected(std::string("At most 1001 tools can have a model. Nothing was "
                                         "saved."));
    assignments.insert(place, std::move(assignment));
  }
  return document;
}

} // namespace

auto resolve_tool_model_location(const ToolModelSettings& settings) -> ToolModelLocation {
  ToolModelLocation location;
  if (!settings.machine_id) {
    location.reason = "No machine identity is configured ([COFFEECNC] MACHINE_ID): tool models "
                      "last until the panel closes.";
    return location;
  }
  location.availability = ToolModelAvailability::unavailable;
  if (!valid_machine_id(*settings.machine_id)) {
    location.reason =
        std::format("[COFFEECNC] MACHINE_ID is not valid: use 1 to 64 of the characters A-Z a-z "
                    "0-9 _ and -, the first a letter or a digit. {}",
                    not_saved);
    return location;
  }
  location.machine_id = settings.machine_id;
  std::string root;
  if (settings.data_directory) {
    const auto* configured = absolute(settings.data_directory);
    if (configured == nullptr) {
      location.reason =
          std::format("[COFFEECNC] DATA_DIRECTORY is not an absolute path. {}", not_saved);
      return location;
    }
    root = *configured;
  } else if (const auto* xdg = absolute(settings.xdg_data_home)) {
    root = *xdg + "/coffeecnc";
  } else if (const auto* home = absolute(settings.home)) {
    root = *home + "/.local/share/coffeecnc";
  } else {
    location.reason = std::format("Neither XDG_DATA_HOME nor HOME is an absolute path: set "
                                  "[COFFEECNC] DATA_DIRECTORY. {}",
                                  not_saved);
    return location;
  }
  location.availability = ToolModelAvailability::available;
  location.directory = root + '/' + *settings.machine_id;
  return location;
}

LocalToolModelFiles::~LocalToolModelFiles() {
  static_cast<void>(close_descriptor(temporary_));
  static_cast<void>(close_descriptor(lock_));
}

auto LocalToolModelFiles::make_directory(const std::string& path)
    -> std::expected<Directory, std::string> {
  constexpr unsigned permission_bits = 07777;
  bool created = ::mkdir(path.c_str(), directory_mode) == 0;
  if (!created && errno != EEXIST)
    return failure(std::format("the directory {} cannot be made", path));
  // The umask may have taken bits away; it cannot have added any.
  if (created && ::chmod(path.c_str(), directory_mode) != 0)
    return failure(std::format("the mode of the directory {} cannot be set", path));
  struct stat info{};
  if (::stat(path.c_str(), &info) != 0)
    return failure(std::format("the directory {} cannot be examined", path));
  if (!S_ISDIR(info.st_mode))
    return std::unexpected(std::format("{} is not a directory", path));
  return Directory{created, static_cast<unsigned>(info.st_mode) & permission_bits};
}

auto LocalToolModelFiles::lock(const std::string& directory) -> std::expected<void, LockError> {
  const auto error = [](std::string_view what) -> std::unexpected<LockError> {
    return std::unexpected(
        LockError{false, std::format("{}: {}", what, std::generic_category().message(errno))});
  };
  unlock();
  const auto file = directory + '/' + std::string(tool_model_lock_name);
  // Made by this call: 0600 exactly, whatever the umask. One that exists is left as it is.
  lock_ = ::open(file.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, file_mode);
  if (lock_ >= 0 && ::fchmod(lock_, file_mode) != 0) {
    auto refused = error("the mode of the lock file cannot be set");
    static_cast<void>(close_descriptor(lock_));
    return refused;
  }
  if (lock_ < 0 && errno == EEXIST)
    lock_ = ::open(file.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
  if (lock_ < 0)
    return error("the lock file cannot be opened");
  if (::flock(lock_, LOCK_EX | LOCK_NB) != 0) {
    const bool held = errno == EWOULDBLOCK;
    auto refused = error("the lock cannot be taken");
    refused.error().in_use = held;
    static_cast<void>(close_descriptor(lock_));
    return refused;
  }
  return {};
}

void LocalToolModelFiles::unlock() { static_cast<void>(close_descriptor(lock_)); }

auto LocalToolModelFiles::read(const std::string& path, std::size_t limit)
    -> std::expected<std::optional<Content>, std::string> {
  int file = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (file < 0) {
    if (errno == ENOENT)
      return std::optional<Content>{};
    return failure("it cannot be opened");
  }
  struct stat info{};
  if (::fstat(file, &info) != 0 || !S_ISREG(info.st_mode)) {
    static_cast<void>(close_descriptor(file));
    return std::unexpected(std::string("it is not a regular file"));
  }
  Content content;
  // One byte beyond the limit tells a longer file from one of exactly that size.
  content.bytes.resize(limit + 1);
  std::size_t size = 0;
  while (size < content.bytes.size()) {
    const auto got = ::read(file, content.bytes.data() + size, content.bytes.size() - size);
    if (got < 0 && errno == EINTR)
      continue;
    if (got < 0) {
      auto failed = failure("it cannot be read");
      static_cast<void>(close_descriptor(file));
      return failed;
    }
    if (got == 0)
      break;
    size += static_cast<std::size_t>(got);
  }
  static_cast<void>(close_descriptor(file));
  content.more = size > limit;
  content.bytes.resize(std::min(size, limit));
  return content;
}

auto LocalToolModelFiles::open_temporary(const std::string& path) -> Done {
  static_cast<void>(close_descriptor(temporary_));
  temporary_ =
      ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, file_mode);
  if (temporary_ < 0)
    return failure("the temporary file cannot be made");
  // Also for one a crashed write left there, and whatever the umask.
  if (::fchmod(temporary_, file_mode) != 0)
    return failure("the mode of the temporary file cannot be set");
  return {};
}

auto LocalToolModelFiles::write(std::string_view bytes) -> Done {
  while (!bytes.empty()) {
    const auto written = ::write(temporary_, bytes.data(), bytes.size());
    if (written < 0 && errno == EINTR)
      continue;
    if (written < 0)
      return failure("the temporary file cannot be written");
    bytes.remove_prefix(static_cast<std::size_t>(written));
  }
  return {};
}

auto LocalToolModelFiles::sync() -> Done {
  if (::fsync(temporary_) != 0)
    return failure("the temporary file cannot be synchronised");
  if (!close_descriptor(temporary_))
    return failure("the temporary file cannot be closed");
  return {};
}

auto LocalToolModelFiles::rename(const std::string& from, const std::string& to) -> Done {
  if (::rename(from.c_str(), to.c_str()) != 0)
    return failure("the file cannot be replaced");
  return {};
}

auto LocalToolModelFiles::link(const std::string& from, const std::string& to) -> Done {
  if (::link(from.c_str(), to.c_str()) != 0)
    return failure("the file cannot be made");
  return {};
}

auto LocalToolModelFiles::sync_directory(const std::string& directory) -> Done {
  int opened = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (opened < 0)
    return failure("the directory cannot be opened");
  if (::fsync(opened) != 0) {
    auto failed = failure("the directory cannot be synchronised");
    static_cast<void>(close_descriptor(opened));
    return failed;
  }
  if (!close_descriptor(opened))
    return failure("the directory cannot be closed");
  return {};
}

void LocalToolModelFiles::discard(const std::string& path) {
  static_cast<void>(close_descriptor(temporary_));
  static_cast<void>(::unlink(path.c_str()));
}

ToolModelArchive::ToolModelArchive(ToolModelFiles& files, ToolModelLocation location)
    : files_(files), location_(std::move(location)) {
  show({location_.machine_id,
        0,
        location_.availability == ToolModelAvailability::session_only
            ? ToolModelAvailability::session_only
            : ToolModelAvailability::unavailable,
        location_.availability == ToolModelAvailability::available
            ? std::string("Tool model settings have not been opened yet.")
            : location_.reason,
        {}});
}

auto ToolModelArchive::path(std::string_view name) const -> std::string {
  return location_.directory + '/' + std::string(name);
}

auto ToolModelArchive::unavailable(std::string reason) const -> ToolModels {
  return {location_.machine_id, 0, ToolModelAvailability::unavailable, std::move(reason), {}};
}

void ToolModelArchive::show(ToolModels models) {
  auto encoded = encode_tool_models(models);
  if (!encoded) {
    models = unavailable(
        std::format("Tool model settings cannot be shown: {} {}", encoded.error(), not_saved));
    encoded = encode_tool_models(models);
  }
  models_ = std::move(models);
  snapshot_ = encoded.value_or(std::string());
}

auto prepare_data_directory(ToolModelFiles& files, const std::string& directory)
    -> std::expected<ToolModelFiles::Directory, std::string> {
  // Outermost first. What exists is left as it is; what is made is made durable in its
  // parent before anything is made inside it.
  for (auto end = directory.find('/', 1);; end = directory.find('/', end + 1)) {
    const auto part = directory.substr(0, end);
    const auto made = files.make_directory(part);
    if (!made)
      return std::unexpected(made.error());
    if (made->created) {
      const auto slash = part.rfind('/');
      const auto parent =
          slash == 0 || slash == std::string::npos ? std::string("/") : part.substr(0, slash);
      if (const auto durable = files.sync_directory(parent); !durable)
        return std::unexpected(
            std::format("the new directory {} cannot be made durable: {}", part, durable.error()));
    }
    if (end == std::string::npos)
      return *made;
  }
}

auto ToolModelArchive::prepare() -> std::expected<void, std::string> {
  const auto made = prepare_data_directory(files_, location_.directory);
  if (!made)
    return std::unexpected(made.error());
  if ((made->mode & 077) != 0)
    note_ = std::format("The directory {} can be entered by other users (mode {:o}).",
                        location_.directory, made->mode);
  return {};
}

void ToolModelArchive::open() {
  if (location_.availability != ToolModelAvailability::available)
    return;
  if (const auto prepared = prepare(); !prepared) {
    show(unavailable(std::format("Tool model settings in {} cannot be opened: {}. {}",
                                 location_.directory, prepared.error(), not_saved)));
    return;
  }
  if (const auto locked = files_.lock(location_.directory); !locked) {
    show(unavailable(locked.error().in_use
                         ? std::format("Tool model settings in {} are already in use by another "
                                       "CoffeeCNC. {}",
                                       location_.directory, not_saved)
                         : std::format("Tool model settings in {} cannot be opened: {}. {}",
                                       location_.directory, locked.error().reason, not_saved)));
    return;
  }
  locked_ = true;
  show(read());
}

auto ToolModelArchive::shared_location() const -> ToolModelLocation {
  if (locked_)
    return {location_.machine_id, ToolModelAvailability::available, note_, location_.directory};
  return {location_.machine_id, models_.availability, models_.reason, {}};
}

void ToolModelArchive::close() {
  locked_ = false;
  files_.unlock();
}

auto ToolModelArchive::read() const -> ToolModels {
  const auto file = path(tool_model_file_name);
  auto read = files_.read(file, max_tool_model_bytes);
  if (!read)
    return unavailable(std::format("{} cannot be read: {}. {}", file, read.error(), not_saved));
  const std::optional<ToolModelFiles::Content> content = std::move(*read);
  if (!content)
    return {location_.machine_id, 0, ToolModelAvailability::available, note_,
            ToolModelDocument{factory_tool_defaults(), {}}};
  if (content->more)
    return unavailable(std::format("{} is larger than 1 MiB. {}", file, left_alone));
  auto saved = decode_tool_model_file(content->bytes);
  if (!saved)
    return unavailable(std::format("{} is not usable: {} {}", file, saved.error(), left_alone));
  if (saved->machine_id != location_.machine_id)
    return unavailable(std::format("{} belongs to machine {}, not to {}. {}", file,
                                   saved->machine_id, location_.machine_id.value_or(""),
                                   left_alone));
  ToolModels held{location_.machine_id, saved->revision, ToolModelAvailability::available, note_,
                  std::move(saved->document)};
  if (const auto encoded = encode_tool_models(held); !encoded)
    return unavailable(std::format("{} is not usable: {} {}", file, encoded.error(), left_alone));
  return held;
}

auto ToolModelArchive::apply(const ToolModelCommand& command) -> ToolModelReply {
  if (models_.availability != ToolModelAvailability::available)
    return {CommandOutcome::rejected, models_.reason, false};
  // Compare and swap against the file itself. The lock keeps other CoffeeCNC processes out;
  // a file that is nevertheless not what this process read or wrote last was changed by
  // something else. Nothing is written over it, and nothing of it is taken over: storage is
  // unavailable until the next start.
  if (auto now = read(); now != models_) {
    const auto file = path(tool_model_file_name);
    show(now.availability == ToolModelAvailability::available
             ? unavailable(std::format("{} was changed from outside while CoffeeCNC runs. {} "
                                       "Restart CoffeeCNC to use what it holds.",
                                       file, not_saved))
             : unavailable(std::format("{} was changed from outside while CoffeeCNC runs: {}", file,
                                       now.reason)));
    return {CommandOutcome::rejected, models_.reason, true};
  }
  if (!models_.document)
    return {CommandOutcome::rejected, models_.reason, true};
  const auto base =
      std::visit([](const auto& c) -> std::uint64_t { return c.base_revision; }, command);
  if (base != models_.revision)
    return {CommandOutcome::rejected,
            std::format("The tool models were changed since revision {} was shown: they are at "
                        "revision {}. Nothing was saved.",
                        base, models_.revision),
            true};
  if (models_.revision >= max_tool_model_revision)
    return {CommandOutcome::rejected,
            "The tool models have reached their last revision. Nothing was saved.", false};
  auto document = changed(*models_.document, command);
  if (!document)
    return {CommandOutcome::rejected, document.error(), false};
  ToolModels next{location_.machine_id, models_.revision + 1, ToolModelAvailability::available,
                  note_, std::move(*document)};
  const auto text =
      encode_tool_model_file({location_.machine_id.value_or(""), next.revision, *next.document});
  if (const auto message = encode_tool_models(next); !text || !message)
    return {CommandOutcome::rejected,
            std::format("{} Nothing was saved.", text ? message.error() : text.error()), false};

  const auto file = path(tool_model_file_name);
  const auto temporary = path(tool_model_temporary_name);
  auto step = files_.open_temporary(temporary);
  if (step)
    step = files_.write(*text);
  if (step)
    step = files_.sync();
  if (step)
    step = files_.rename(temporary, file);
  if (!step) {
    files_.discard(temporary);
    return {CommandOutcome::failed,
            std::format("The tool models were not saved: {}. The saved file is unchanged.",
                        step.error()),
            false};
  }
  if (const auto durable = files_.sync_directory(location_.directory); !durable) {
    // The file was replaced, and whether that outlasts a power failure is not known. What it
    // holds now is what is shown.
    show(read());
    return {CommandOutcome::failed,
            std::format("The tool models may not have been saved: {}. The outcome is uncertain; "
                        "what is shown was read back from the file.",
                        durable.error()),
            true};
  }
  show(std::move(next));
  return {CommandOutcome::accepted, {}, true};
}

ToolModelStore::ToolModelStore(ToolModelFiles& files, ToolModelLocation location, Deliver deliver,
                               std::optional<Limits> operator_limits)
    : location_(location), deliver_(std::move(deliver)), archive_(files, std::move(location)),
      operator_settings_(operator_limits
                             ? std::make_unique<OperatorSettingsArchive>(files, *operator_limits)
                             : nullptr),
      thread_([this]() -> void { loop(); }) {}

ToolModelStore::~ToolModelStore() {
  {
    const std::scoped_lock lock{mutex_};
    stop_ = true;
  }
  wake_.notify_all();
  thread_.join();
}

auto ToolModelStore::submit(PageToken page, RequestId id, ToolModelCommand command)
    -> std::optional<std::string> {
  if (location_.availability != ToolModelAvailability::available)
    return location_.reason;
  const auto& machine =
      std::visit([](const auto& c) -> const std::string& { return c.machine_id; }, command);
  if (machine != location_.machine_id)
    return std::format("This change is for another machine than this one ({}). Nothing was "
                       "saved.",
                       location_.machine_id.value_or(""));
  if (auto error = tool_model_command_error(command); !error.empty())
    return error;
  {
    const std::scoped_lock lock{mutex_};
    if (page.value <= ended_through_)
      return "This page is no longer active.";
    if (pending_.size() >= max_pending)
      return std::string(busy_reason);
    pending_.push_back({page, id, std::move(command)});
  }
  wake_.notify_all();
  return {};
}

auto ToolModelStore::submit_operator_settings(PageToken page, RequestId id,
                                              SetOperatorSettings command)
    -> std::optional<std::string> {
  if (!operator_settings_)
    return "Operator settings storage is not part of this session.";
  if (location_.availability != ToolModelAvailability::available)
    return location_.reason;
  if (command.machine_id != location_.machine_id)
    return "This settings change belongs to another machine.";
  if (auto error = operator_settings_command_error(command, operator_settings_->limits());
      !error.empty())
    return error;
  {
    const std::scoped_lock lock{mutex_};
    if (pending_.size() >= max_pending)
      return "Too many settings changes are waiting.";
    if (page.value <= ended_through_)
      return "This settings page has ended.";
    pending_.push_back({page, id, std::move(command)});
  }
  wake_.notify_all();
  return {};
}

void ToolModelStore::page_ended(PageToken page) {
  const std::scoped_lock lock{mutex_};
  ended_through_ = std::max(ended_through_, page.value);
  std::erase_if(pending_, [&](const Pending& waiting) -> bool {
    return waiting.page.value <= ended_through_;
  });
  if (snapshot_due_ && snapshot_due_->value <= ended_through_)
    snapshot_due_.reset();
}

void ToolModelStore::send_snapshot(PageToken page) {
  {
    const std::scoped_lock lock{mutex_};
    if (page.value <= ended_through_)
      return;
    snapshot_due_ = page;
  }
  wake_.notify_all();
}

void ToolModelStore::set_fixed_probe_tool(std::optional<int> tool_number) {
  const std::scoped_lock lock{mutex_};
  fixed_probe_tool_ = tool_number;
}

auto ToolModelStore::touch_probe_loaded(int tool_number) -> std::optional<bool> {
  if (tool_number <= 0)
    return false;
  const std::scoped_lock lock{mutex_};
  if (fixed_probe_tool_ == tool_number ||
      std::ranges::find(probe_tools_, tool_number) != probe_tools_.end() ||
      changing_probe_ == tool_number)
    return true;
  if (!probe_catalog_ready_)
    return std::nullopt;
  for (const auto& request : pending_) {
    const auto* command = std::get_if<ToolModelCommand>(&request.command);
    const auto* assignment = command ? std::get_if<SetToolModelAssignment>(command) : nullptr;
    if (assignment && assignment->family == ToolFamily::touch_probe &&
        assignment->tool.number == tool_number)
      return true;
  }
  return false;
}

void ToolModelStore::refresh_probe_tools() {
  const auto& models = archive_.models();
  const std::scoped_lock lock{mutex_};
  probe_catalog_ready_ = models.availability != ToolModelAvailability::unavailable;
  changing_probe_.reset();
  if (!models.document)
    return;
  probe_tools_.clear();
  for (const auto& assignment : models.document->assignments)
    if (assignment.family == ToolFamily::touch_probe)
      probe_tools_.push_back(assignment.tool.number);
}

void ToolModelStore::loop() {
  archive_.open();
  refresh_probe_tools();
  if (operator_settings_)
    operator_settings_->open(archive_.shared_location());
  const auto send = [this](PageToken page, std::vector<std::string> messages) -> void {
    {
      const std::scoped_lock lock{mutex_};
      if (page.value <= ended_through_)
        return;
    }
    if (deliver_)
      deliver_(page, std::move(messages));
  };
  for (;;) {
    std::optional<PageToken> snapshot;
    std::optional<Pending> next;
    {
      std::unique_lock lock{mutex_};
      wake_.wait(lock, [&]() -> bool { return stop_ || snapshot_due_ || !pending_.empty(); });
      if (stop_)
        break;
      if (snapshot_due_) {
        snapshot = std::exchange(snapshot_due_, std::nullopt);
      } else {
        next = std::move(pending_.front());
        pending_.pop_front();
        if (const auto* command = std::get_if<ToolModelCommand>(&next->command)) {
          const auto* assignment = std::get_if<SetToolModelAssignment>(command);
          if (assignment && assignment->family == ToolFamily::touch_probe)
            changing_probe_ = assignment->tool.number;
        }
      }
    }
    if (snapshot) {
      std::vector<std::string> messages{archive_.snapshot()};
      if (operator_settings_)
        messages.push_back(operator_settings_->snapshot());
      send(*snapshot, std::move(messages));
      continue;
    }
    if (!next)
      continue;
    ToolModelReply reply;
    std::string changed_snapshot;
    if (const auto* settings = std::get_if<SetOperatorSettings>(&next->command)) {
      reply = operator_settings_->apply(*settings);
      if (reply.snapshot)
        changed_snapshot = operator_settings_->snapshot();
    } else {
      reply = archive_.apply(std::get<ToolModelCommand>(next->command));
      refresh_probe_tools();
      if (reply.snapshot)
        changed_snapshot = archive_.snapshot();
    }
    std::vector<std::string> messages;
    if (reply.snapshot)
      messages.push_back(std::move(changed_snapshot));
    messages.push_back(encode_result(next->id, reply.outcome, reply.reason));
    send(next->page, std::move(messages));
  }
  archive_.close();
}

} // namespace coffeecnc
