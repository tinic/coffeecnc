// PanelComposer on constructed readings, and the PanelFeed thread on a source of the test's own.

#include <coffeecnc/app/panel_feed.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace coffeecnc;
using namespace std::chrono_literals;

namespace {
using Json = nlohmann::json;
using Types = std::vector<std::string>;

int checks = 0;
void require(bool condition, const std::string& message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

auto hello() -> Hello {
  Hello h;
  h.machine_name = "Mill";
  h.axes = {Axis::x, Axis::y, Axis::z};
  h.axis_max_jog_velocity = {1500, 1500, 750};
  h.max_jog_velocity = 1500;
  return h;
}

// A live, idle machine at rest in G54 with an offset.
auto reading() -> PanelReading {
  PanelReading r;
  r.status.link = Link::live;
  r.status.machine = MachineState::on;
  r.status.axes = {{.axis = Axis::x}, {.axis = Axis::y}, {.axis = Axis::z}};
  r.status.offsets.wcs = 1;
  r.status.offsets.g5x = {10, 20, 30};
  r.status.active_codes = {"G1", "G54"};
  r.tool_table = {1, {{1, 1, 40, 6, "End mill"}}};
  r.work_systems.at(0) = WorkSystem{{10, 20, 30}, 0, true};
  return r;
}

struct Sent {
  Types types;
  std::vector<Json> messages;
  [[nodiscard]] auto last() const -> const Json& { return messages.back(); }
  [[nodiscard]] auto context() const -> std::uint64_t {
    return last().at("preview_context").get<std::uint64_t>();
  }
};

auto compose(PanelComposer& composer, const PanelReading& r, std::uint64_t page = 1) -> Sent {
  const auto messages = composer.compose(r, PageToken{page}, false, 1234ms);
  require(messages.has_value(), "Composing succeeds: " + messages.error_or(""));
  Sent sent;
  for (const auto& text : messages.value_or(std::vector<std::string>{})) {
    sent.messages.push_back(Json::parse(text));
    sent.types.push_back(sent.messages.back().at("type").get<std::string>());
  }
  return sent;
}

void pages_and_snapshots() {
  PanelComposer composer{hello()};
  auto r = reading();
  auto sent = compose(composer, r);
  require(sent.types == Types{"hello", "tool_table", "work_offsets", "preview", "status"},
          "A page's first step: hello and every snapshot, status last; no program without one");
  require(sent.last().at("seq") == 1, "A page's statuses are numbered from 1");
  const auto& systems = sent.messages.at(2).at("systems");
  bool others_unavailable = systems.size() == 9;
  for (std::size_t i = 1; i < systems.size(); ++i)
    others_unavailable = others_unavailable && systems.at(i).at("available") == false &&
                         systems.at(i).at("offset") == Json::array({0.0, 0.0, 0.0}) &&
                         systems.at(i).at("rotation_xy") == 0.0;
  require(sent.messages.at(2).at("revision") == 1 && systems.at(0).at("available") == true &&
              systems.at(0).at("offset") == Json::array({10.0, 20.0, 30.0}) &&
              others_unavailable,
          "work_offsets: nine rows, the active system available with its live offset, the "
          "others not available and zero");
  const auto& preview = sent.messages.at(3);
  require(preview.at("state") == "unavailable" && preview.at("segments").empty() &&
              preview.at("context") == sent.context() &&
              preview.at("error").at("text") == PanelComposer::no_preview_reason,
          "The preview is unavailable for the status's context, and says why");
  sent = compose(composer, r);
  require(sent.types == Types{"status"} && sent.last().at("seq") == 2,
          "Then a status per step while nothing changes");

  r.program =
      std::make_shared<const ProgramText>(ProgramText{1, "/nc/a.ngc", {"G0 X1"}, false, "0"});
  r.status.program.file = "/nc/a.ngc";
  r.status.program.revision = 1;
  sent = compose(composer, r);
  require(sent.types == Types{"program", "preview", "status"},
          "A loaded program is sent, and the preview of the new context");

  sent = compose(composer, r, 2);
  require(sent.types ==
                  Types{"hello", "tool_table", "work_offsets", "program", "preview", "status"} &&
              sent.last().at("seq") == 1,
          "Another page starts clean: every snapshot again, numbered from 1");
  require(sent.messages.at(2).at("revision") == 1, "Unchanged work offsets keep their revision");

  // A second `ready` of the same page: everything again, the numbering goes on.
  const auto again = composer.compose(r, PageToken{2}, true, 1234ms);
  require(again.has_value(), "Composing after a repeated ready succeeds");
  Types types;
  std::ranges::transform(again.value_or(std::vector<std::string>{}), std::back_inserter(types),
                         [](const std::string& text) -> std::string {
                           return Json::parse(text).at("type").get<std::string>();
                         });
  require(types == Types{"hello", "tool_table", "work_offsets", "program", "preview", "status"} &&
              Json::parse(again->back()).at("seq") == 2,
          "A repeated ready on the same page sends hello and every snapshot again");
  require(compose(composer, r, 2).types == Types{"status"}, "Once per ready");
}

void preview_context_follows_live_inputs() {
  PanelComposer composer{hello()};
  auto r = reading();
  const auto first = compose(composer, r).context();
  require(first >= 1 && compose(composer, r).context() == first, "The same inputs keep it");

  r.status.offsets.g5x = {11, 20, 30};
  r.work_systems.at(0) = WorkSystem{{11, 20, 30}, 0, true};
  auto sent = compose(composer, r);
  auto context = sent.context();
  require(context == first + 1, "A changed offset of the active system changes the context");
  require(sent.types == Types{"work_offsets", "preview", "status"} &&
              sent.messages.front().at("revision") == 2 &&
              sent.messages.front().at("systems").at(0).at("offset").at(0) == 11.0,
          "And the revision of the work offsets, which are sent again");

  // G55: the same numbers in another system are another table.
  r.status.offsets.wcs = 2;
  r.work_systems.at(0) = WorkSystem{{}, 0, false};
  r.work_systems.at(1) = WorkSystem{{11, 20, 30}, 0, true};
  sent = compose(composer, r);
  require(sent.context() > context && sent.types.front() == "work_offsets" &&
              sent.messages.front().at("revision") == 3 &&
              sent.messages.front().at("systems").at(0).at("available") == false &&
              sent.messages.front().at("systems").at(1).at("available") == true,
          "Another active system changes context and revision; the system left is not "
          "available any more");
  r.work_systems.at(1).rotation_xy = 30;
  r.status.offsets.rotation_xy = 30;
  sent = compose(composer, r);
  require(sent.types.front() == "work_offsets" && sent.messages.front().at("revision") == 4,
          "A changed rotation of the active system too");
  context = sent.context();
  // An inactive system read from the parameter file: a value is an input, whether it is
  // known to be current is not.
  r.work_systems.at(4) = WorkSystem{{1, 2, 3}, 0, true, false};
  sent = compose(composer, r);
  require(sent.context() == context + 1 && sent.messages.front().at("revision") == 5 &&
              sent.messages.front().at("systems").at(4).at("current") == false,
          "An inactive value read is an input");
  context = sent.context();
  r.work_systems.at(4).current = true;
  sent = compose(composer, r);
  require(sent.context() == context && sent.types.front() == "work_offsets" &&
              sent.messages.front().at("revision") == 6 &&
              sent.messages.front().at("systems").at(4).at("current") == true,
          "Becoming current sends the work offsets again and leaves the preview context");

  // A tool table withheld from the panel (its file cannot be read) is still the machine's:
  // the rows it would show count, and the context stays.
  const auto shown = r.tool_table;
  r.accepted_tools = shown.tools;
  r.tool_table = {shown.revision + 1, {}};
  sent = compose(composer, r);
  require(sent.context() == context && sent.types.front() == "tool_table" &&
              sent.messages.front().at("tools").empty(),
          "A withheld tool table is sent empty and leaves the preview context as it is");
  r.accepted_tools.reset();
  r.tool_table = {shown.revision + 2, shown.tools};
  sent = compose(composer, r);
  require(sent.context() == context && sent.types.front() == "tool_table" &&
              sent.messages.front().at("tools").size() == shown.tools.size(),
          "Shown again: sent again, and still the same context");

  // A jog: the position counts only once the machine rests again.
  r.status.motion.in_position = false;
  r.status.axes.at(0).machine = 5;
  require(compose(composer, r).context() == context, "A position in motion is no start state");
  r.status.motion.in_position = true;
  require(compose(composer, r).context() == context + 1, "The position it comes to rest at is");
  context += 1;

  // A program run: its own modal changes do not count until it has ended.
  r.status.interp = InterpState::reading;
  r.status.active_codes = {"G0", "G55", "G91"};
  r.status.axes.at(0).machine = 50;
  require(compose(composer, r).context() == context, "The start state is frozen during a run");
  r.status.program.block_delete = true;
  require(compose(composer, r).context() == context + 1, "Block delete counts at once");
  r.status.interp = InterpState::idle;
  require(compose(composer, r).context() == context + 2, "After the run its state is a new start");

  // The same file loaded again is a new program: its text is sent and the context changes.
  r.program =
      std::make_shared<const ProgramText>(ProgramText{1, "/nc/a.ngc", {"G0 X1"}, false, "0"});
  r.status.program.file = "/nc/a.ngc";
  r.status.program.revision = 1;
  context = compose(composer, r).context();
  r.program =
      std::make_shared<const ProgramText>(ProgramText{2, "/nc/a.ngc", {"G0 X1"}, false, "0"});
  r.status.program.revision = 2;
  sent = compose(composer, r);
  require(sent.types == Types{"program", "preview", "status"} && sent.context() == context + 1 &&
              sent.messages.front().at("revision") == 2,
          "A reload of the same unchanged file: the text again, and a new context");
  context += 1;

  // Readings without a page count too.
  r.status.tool.in_spindle = 3;
  composer.observe(r);
  require(compose(composer, r).context() == context + 1,
          "A change seen while no page was ready has counted once");
  context += 1;

  PanelReading down;
  down.status.axes = r.status.axes;
  require(compose(composer, down).context() == context,
          "Without a status nothing is known: the context stays");
  require(compose(composer, r).context() == context, "And is the same when the status is back");
}

void messages_wait_for_a_page() {
  PanelComposer composer{hello()};
  auto r = reading();
  r.messages = {{true, "Cannot read tool table /t."}};
  composer.observe(r);
  r.messages.clear();
  auto sent = compose(composer, r);
  require(sent.types.back() == "message" && sent.last().at("level") == "error" &&
              sent.last().at("text") == "Cannot read tool table /t." &&
              sent.last().at("time") == 1234,
          "What the source reported before a page was ready reaches the first page");
  require(compose(composer, r).types == Types{"status"}, "Once");
  r.messages.assign(PanelComposer::max_waiting + 5, {false, "note"});
  composer.observe(r);
  r.messages.clear();
  sent = compose(composer, r);
  require(sent.types.size() == 1 + PanelComposer::max_waiting, "Only the last messages wait");
}

void a_status_that_cannot_be_sent() {
  PanelComposer composer{hello()};
  auto r = reading();
  r.status.axes.at(2).machine = std::numeric_limits<double>::quiet_NaN();
  const auto refused = composer.compose(r, PageToken{1}, false, 0ms);
  require(!refused && refused.error().find("axes.machine") != std::string::npos,
          "A number that is not finite is refused, naming the field");
  r.status.axes.at(2).machine = 0;
  const auto sent = compose(composer, r);
  require(sent.types.front() == "hello" && sent.last().at("seq") == 1,
          "Nothing counted as sent: the snapshots are still due");
}

// Counts readings and hands out the same one.
class Source final : public PanelSource {
public:
  [[nodiscard]] auto hello() const -> Hello override { return ::hello(); }
  [[nodiscard]] auto read() -> PanelReading override {
    const std::scoped_lock lock{mutex_};
    ++reads_;
    return reading_;
  }
  auto reads() -> int {
    const std::scoped_lock lock{mutex_};
    return reads_;
  }
  void set(PanelReading r) {
    const std::scoped_lock lock{mutex_};
    reading_ = std::move(r);
  }

private:
  std::mutex mutex_;
  PanelReading reading_{reading()};
  int reads_{};
};

struct Delivered {
  std::mutex mutex;
  std::vector<std::pair<std::uint64_t, Json>> messages;
  auto count(std::uint64_t page, const std::string& type) -> std::size_t {
    const std::scoped_lock lock{mutex};
    std::size_t found = 0;
    for (const auto& [token, message] : messages)
      if (token == page && message.at("type") == type)
        ++found;
    return found;
  }
  void clear() {
    const std::scoped_lock lock{mutex};
    messages.clear();
  }
  auto total() -> std::size_t {
    const std::scoped_lock lock{mutex};
    return messages.size();
  }
};

void feed_thread() {
  Source source;
  Delivered delivered;
  {
    PanelFeed feed{source, [&](PageToken page, const std::vector<std::string>& messages) -> void {
                     const std::scoped_lock lock{delivered.mutex};
                     for (const auto& text : messages)
                       delivered.messages.emplace_back(page.value, Json::parse(text));
                   }};
    std::this_thread::sleep_for(200ms);
    require(delivered.total() == 0, "Nothing is delivered before a page is ready");
    require(source.reads() >= 2, "The source is read without a page: the context stays current");

    const auto asked = std::chrono::steady_clock::now();
    feed.page_ready(PageToken{7});
    while (delivered.count(7, "hello") == 0 && std::chrono::steady_clock::now() - asked < 2s)
      std::this_thread::sleep_for(1ms);
    const auto waited = std::chrono::steady_clock::now() - asked;
    require(delivered.count(7, "hello") == 1 && waited < 40ms,
            "A ready page gets its snapshots at once, not a period later");
    // Every ready asks for the snapshots, also a second one on the same page.
    feed.page_ready(PageToken{7});
    const auto repeat = std::chrono::steady_clock::now();
    while (delivered.count(7, "hello") < 2 && std::chrono::steady_clock::now() - repeat < 300ms)
      std::this_thread::sleep_for(1ms);
    require(delivered.count(7, "hello") == 2 && delivered.count(7, "tool_table") == 2 &&
                delivered.count(7, "work_offsets") == 2 && delivered.count(7, "preview") == 2,
            "A repeated ready on the same page resends hello and the snapshots");
    delivered.clear();
    std::this_thread::sleep_for(500ms);
    const auto statuses = delivered.count(7, "status");
    require(statuses >= 8 && statuses <= 12,
            "A status every 50 ms: " + std::to_string(statuses) + " in half a second");

    feed.page_ended(PageToken{6});
    std::this_thread::sleep_for(120ms);
    require(delivered.count(7, "status") > statuses, "The end of another page changes nothing");
    feed.page_ended(PageToken{7});
    std::this_thread::sleep_for(60ms); // A step that had begun may still deliver.
    const auto after = delivered.total();
    std::this_thread::sleep_for(200ms);
    require(delivered.total() == after, "Nothing is delivered once the page has ended");

    feed.page_ready(PageToken{8});
    std::this_thread::sleep_for(120ms);
    require(delivered.count(8, "hello") == 1 && delivered.count(7, "hello") == 0,
            "The next page gets snapshots of its own, under its own token");

    auto bad = reading();
    bad.status.motion.velocity = std::numeric_limits<double>::infinity();
    source.set(bad);
    std::this_thread::sleep_for(300ms);
    require(delivered.count(8, "message") == 1,
            "A status that cannot be encoded is reported once, not every period");
  }
  const auto after = delivered.total();
  std::this_thread::sleep_for(120ms);
  require(delivered.total() == after, "The destructor has ended the thread");
}

// Ending the feed while it sleeps between two readings begins no further reading: a reading
// can block in the tool data memory or the file system, and shutdown must not wait for one
// that nobody needs.
// What must follow a page's hello is sent from the hook, after hello was handed over.
void snapshots_sent_follow_hello() {
  Source source;
  Delivered delivered;
  std::mutex mutex;
  // The page of each call, and the hellos that page had been delivered by then.
  std::vector<std::pair<std::uint64_t, std::size_t>> calls;
  const auto called = [&]() -> std::vector<std::pair<std::uint64_t, std::size_t>> {
    const std::scoped_lock lock{mutex};
    return calls;
  };
  const auto wait_for = [&](std::size_t count) -> void {
    const auto begun = std::chrono::steady_clock::now();
    while (called().size() < count && std::chrono::steady_clock::now() - begun < 2s)
      std::this_thread::sleep_for(1ms);
  };
  PanelFeed feed{source,
                 [&](PageToken page, const std::vector<std::string>& messages) -> void {
                   const std::scoped_lock lock{delivered.mutex};
                   for (const auto& text : messages)
                     delivered.messages.emplace_back(page.value, Json::parse(text));
                 },
                 nullptr, [&](PageToken page) -> void {
                   const auto hellos = delivered.count(page.value, "hello");
                   const std::scoped_lock lock{mutex};
                   calls.emplace_back(page.value, hellos);
                 }};
  std::this_thread::sleep_for(120ms);
  require(called().empty(), "Without a ready page nothing is announced");
  feed.page_ready(PageToken{3});
  wait_for(1);
  std::this_thread::sleep_for(170ms);
  require(called() == std::vector<std::pair<std::uint64_t, std::size_t>>{{3, 1}},
          "Once per ready, after that page's hello was delivered; not with every status");
  feed.page_ready(PageToken{3});
  wait_for(2);
  feed.page_ready(PageToken{4});
  wait_for(3);
  std::this_thread::sleep_for(120ms);
  require(called() ==
              std::vector<std::pair<std::uint64_t, std::size_t>>{{3, 1}, {3, 2}, {4, 1}},
          "Again for a repeated ready and for the next page");
}

void feed_stops_without_another_reading() {
  for (int round = 0; round < 10; ++round) {
    Source source;
    int before = 0;
    std::chrono::steady_clock::duration took{};
    {
      const PanelFeed feed{source, [](PageToken, const std::vector<std::string>&) -> void {}};
      // Just after a reading: the feed now sleeps for most of a period.
      const int seen = source.reads();
      while (source.reads() < seen + 2)
        std::this_thread::sleep_for(1ms);
      std::this_thread::sleep_for(5ms);
      before = source.reads();
      const auto asked = std::chrono::steady_clock::now();
      // The destructor runs here.
      took = asked.time_since_epoch();
    }
    took = std::chrono::steady_clock::now().time_since_epoch() - took;
    std::this_thread::sleep_for(60ms);
    require(source.reads() == before,
            "No reading is begun once the feed was told to stop: " + std::to_string(before) +
                " before, " + std::to_string(source.reads()) + " after");
    require(took < 25ms, "A sleeping feed ends at once, not at the end of its period");
  }
}

} // namespace

auto main() -> int {
  try {
    pages_and_snapshots();
    preview_context_follows_live_inputs();
    messages_wait_for_a_page();
    a_status_that_cannot_be_sent();
    feed_thread();
    snapshots_sent_follow_hello();
    feed_stops_without_another_reading();
    std::cout << checks << " panel feed checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
