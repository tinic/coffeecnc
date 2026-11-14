#include <coffeecnc/shell/test_hooks.hpp>

#include "panel_window_impl.hpp"

#include <coffeecnc/app/panel_assets.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <functional>
#include <future>
#include <iterator>
#include <map>
#include <string_view>
#include <thread>
#include <utility>

#include <cairo.h>

namespace coffeecnc {

struct ShellTestHooks::Impl {
  PanelWindow& window;
  std::vector<std::string> resources;
  std::vector<std::string> foreign_resources;
  std::uint64_t process_terminations{};
  std::uint64_t file_choosers{};
  std::vector<std::string> chooser_answer;
  guint32 event_time{1000};

  explicit Impl(PanelWindow& panel) : window(panel) {}
  [[nodiscard]] auto view() const -> WebKitWebView* { return window.internals().view; }
};

namespace {

using Text = std::expected<std::string, std::string>;

// Runs `work` on the GTK thread and waits for it. The work and its result cross between the
// threads through the window's outbox and a promise: under locks of this program's own, never
// as data of a GLib source.
template <typename Result>
auto on_gtk(PanelWindow& window, std::function<Result()> work) -> Result {
  auto done = std::make_shared<std::promise<Result>>();
  auto result = done->get_future();
  window.internals().on_gtk_thread([done, work = std::move(work)]() -> void {
    if constexpr (std::is_void_v<Result>) {
      work();
      done->set_value();
    } else {
      done->set_value(work());
    }
  });
  return result.get();
}

void on_resource(WebKitWebView* /*view*/, WebKitWebResource* /*resource*/,
                 WebKitURIRequest* request, gpointer data) {
  auto* self = static_cast<ShellTestHooks::Impl*>(data);
  const gchar* uri = webkit_uri_request_get_uri(request);
  if (uri == nullptr)
    return;
  const std::string_view text{uri};
  self->resources.emplace_back(text);
  if (!is_panel_url(text) && !text.starts_with("data:") && !text.starts_with("blob:"))
    self->foreign_resources.emplace_back(text);
}

void on_terminated(WebKitWebView* /*view*/, WebKitWebProcessTerminationReason /*reason*/,
                   gpointer data) {
  ++static_cast<ShellTestHooks::Impl*>(data)->process_terminations;
}

auto on_file_chooser(WebKitWebView* /*view*/, WebKitFileChooserRequest* request, gpointer data)
    -> gboolean {
  auto* self = static_cast<ShellTestHooks::Impl*>(data);
  ++self->file_choosers;
  if (self->chooser_answer.empty()) {
    webkit_file_chooser_request_cancel(request);
    return TRUE;
  }
  std::vector<const gchar*> files;
  files.reserve(self->chooser_answer.size() + 1);
  std::ranges::transform(self->chooser_answer, std::back_inserter(files),
                         [](const std::string& file) -> const gchar* { return file.c_str(); });
  files.push_back(nullptr);
  webkit_file_chooser_request_select_files(request, files.data());
  self->chooser_answer.clear();
  return TRUE;
}

struct Pending {
  std::promise<Text> done;
};

void on_evaluated(GObject* source, GAsyncResult* result, gpointer data) {
  std::unique_ptr<std::shared_ptr<Pending>> pending{static_cast<std::shared_ptr<Pending>*>(data)};
  GError* error = nullptr;
  JSCValue* value = webkit_web_view_call_async_javascript_function_finish(
      WEBKIT_WEB_VIEW(source), result, &error);
  if (value == nullptr) {
    (*pending)->done.set_value(
        std::unexpected(std::string(error != nullptr ? error->message : "no result")));
    if (error != nullptr)
      g_error_free(error);
    return;
  }
  gchar* text = jsc_value_to_string(value);
  (*pending)->done.set_value(std::string(text != nullptr ? text : "null"));
  g_free(text);
  g_object_unref(value);
}

struct Picture {
  std::string file;
  std::promise<std::expected<void, std::string>> done;
};

void on_snapshot(GObject* source, GAsyncResult* result, gpointer data) {
  std::unique_ptr<std::shared_ptr<Picture>> picture{static_cast<std::shared_ptr<Picture>*>(data)};
  GError* error = nullptr;
  cairo_surface_t* surface =
      webkit_web_view_get_snapshot_finish(WEBKIT_WEB_VIEW(source), result, &error);
  if (surface == nullptr) {
    (*picture)->done.set_value(
        std::unexpected(std::string(error != nullptr ? error->message : "no snapshot")));
    if (error != nullptr)
      g_error_free(error);
    return;
  }
  const auto written = cairo_surface_write_to_png(surface, (*picture)->file.c_str());
  cairo_surface_destroy(surface);
  if (written == CAIRO_STATUS_SUCCESS)
    (*picture)->done.set_value({});
  else
    (*picture)->done.set_value(std::unexpected(std::string(cairo_status_to_string(written))));
}

void mouse(ShellTestHooks::Impl& hooks, GdkEventType type, double x, double y) {
  auto* view = GTK_WIDGET(hooks.view());
  if (view == nullptr || gtk_widget_get_window(view) == nullptr)
    return;
  GdkWindow* surface = gtk_widget_get_window(view);
  GdkDevice* pointer = gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default()));
  GdkEvent* event = gdk_event_new(type);
  gint origin_x = 0;
  gint origin_y = 0;
  gdk_window_get_origin(surface, &origin_x, &origin_y);
  hooks.event_time += 20;
  if (type == GDK_MOTION_NOTIFY) {
    event->motion.window = GDK_WINDOW(g_object_ref(surface));
    event->motion.time = hooks.event_time;
    event->motion.x = x;
    event->motion.y = y;
    event->motion.x_root = x + origin_x;
    event->motion.y_root = y + origin_y;
  } else {
    event->button.window = GDK_WINDOW(g_object_ref(surface));
    event->button.time = hooks.event_time;
    event->button.x = x;
    event->button.y = y;
    event->button.x_root = x + origin_x;
    event->button.y_root = y + origin_y;
    event->button.button = 1;
    event->button.state = type == GDK_BUTTON_RELEASE ? GDK_BUTTON1_MASK : 0;
  }
  gdk_event_set_device(event, pointer);
  gtk_main_do_event(event);
  gdk_event_free(event);
}

} // namespace

auto ShellTestHooks::install(PanelWindow& window, const std::string& document_start_script)
    -> std::unique_ptr<ShellTestHooks> {
  auto impl = std::make_unique<Impl>(window);
  WebKitWebView* view = impl->view();
  g_signal_connect(view, "resource-load-started", G_CALLBACK(on_resource), impl.get());
  g_signal_connect(view, "web-process-terminated", G_CALLBACK(on_terminated), impl.get());
  g_signal_connect(view, "run-file-chooser", G_CALLBACK(on_file_chooser), impl.get());
  webkit_settings_set_enable_write_console_messages_to_stdout(webkit_web_view_get_settings(view),
                                                              TRUE);
  if (!document_start_script.empty()) {
    WebKitUserScript* script = webkit_user_script_new(
        document_start_script.c_str(), WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
    webkit_user_content_manager_add_script(webkit_web_view_get_user_content_manager(view),
                                           script);
    webkit_user_script_unref(script);
  }
  return std::unique_ptr<ShellTestHooks>(new ShellTestHooks(std::move(impl)));
}

ShellTestHooks::ShellTestHooks(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ShellTestHooks::~ShellTestHooks() = default;

auto ShellTestHooks::state() -> State {
  return on_gtk<State>(impl_->window, [this]() -> State {
    const auto& window = impl_->window.internals();
    State state;
    state.page = window.page ? window.page->value : 0;
    state.commits = window.commits;
    state.identified = !window.identity.empty();
    state.handler_at_start = window.handler_at_start;
    state.delivered = window.delivered;
    state.dropped = window.dropped;
    state.rejected = window.rejected;
    state.identity = window.identity;
    if (window.view != nullptr) {
      const gchar* uri = webkit_web_view_get_uri(window.view);
      state.uri = uri != nullptr ? uri : "";
      state.width = gtk_widget_get_allocated_width(GTK_WIDGET(window.view));
      state.height = gtk_widget_get_allocated_height(GTK_WIDGET(window.view));
    }
    state.resources = impl_->resources;
    state.foreign_resources = impl_->foreign_resources;
    state.process_terminations = impl_->process_terminations;
    state.file_choosers = impl_->file_choosers;
    return state;
  });
}

auto ShellTestHooks::evaluate(const std::string& body, std::chrono::milliseconds timeout) -> Text {
  auto pending = std::make_shared<Pending>();
  auto result = pending->done.get_future();
  const std::string script = "const __value = await (async () => {" + body +
                             "\n})(); return JSON.stringify(__value === undefined ? null : __value);";
  const bool started = on_gtk<bool>(impl_->window, [&]() -> bool {
    WebKitWebView* view = impl_->view();
    if (view == nullptr)
      return false;
    webkit_web_view_call_async_javascript_function(
        view, script.c_str(), static_cast<gssize>(script.size()), nullptr, nullptr, nullptr,
        nullptr, on_evaluated, new std::shared_ptr<Pending>(pending));
    return true;
  });
  if (!started)
    return std::unexpected(std::string("The window is closed."));
  if (result.wait_for(timeout) != std::future_status::ready)
    return std::unexpected(std::string("The script did not finish in time."));
  return result.get();
}

auto ShellTestHooks::snapshot(const std::string& file) -> std::expected<void, std::string> {
  auto picture = std::make_shared<Picture>();
  picture->file = file;
  auto result = picture->done.get_future();
  const bool started = on_gtk<bool>(impl_->window, [&]() -> bool {
    WebKitWebView* view = impl_->view();
    if (view == nullptr)
      return false;
    webkit_web_view_get_snapshot(view, WEBKIT_SNAPSHOT_REGION_VISIBLE,
                                 WEBKIT_SNAPSHOT_OPTIONS_NONE, nullptr, on_snapshot,
                                 new std::shared_ptr<Picture>(picture));
    return true;
  });
  if (!started)
    return std::unexpected(std::string("The window is closed."));
  if (result.wait_for(std::chrono::seconds{15}) != std::future_status::ready)
    return std::unexpected(std::string("No snapshot in time."));
  return result.get();
}

auto ShellTestHooks::pixels(int x, int y, int width, int height)
    -> std::expected<Pixels, std::string> {
  struct Region {
    int x{};
    int y{};
    int width{};
    int height{};
    std::promise<std::expected<Pixels, std::string>> done;
  };
  auto region = std::make_shared<Region>();
  *region = Region{x, y, width, height, {}};
  auto result = region->done.get_future();
  const bool started = on_gtk<bool>(impl_->window, [&]() -> bool {
    WebKitWebView* view = impl_->view();
    if (view == nullptr)
      return false;
    webkit_web_view_get_snapshot(
        view, WEBKIT_SNAPSHOT_REGION_VISIBLE, WEBKIT_SNAPSHOT_OPTIONS_NONE, nullptr,
        [](GObject* source, GAsyncResult* async, gpointer data) -> void {
          std::unique_ptr<std::shared_ptr<Region>> asked{
              static_cast<std::shared_ptr<Region>*>(data)};
          Region& wanted = **asked;
          cairo_surface_t* surface =
              webkit_web_view_get_snapshot_finish(WEBKIT_WEB_VIEW(source), async, nullptr);
          if (surface == nullptr) {
            wanted.done.set_value(std::unexpected(std::string("no snapshot")));
            return;
          }
          cairo_surface_t* image = cairo_surface_map_to_image(surface, nullptr);
          const int stride = cairo_image_surface_get_stride(image);
          const unsigned char* data_start = cairo_image_surface_get_data(image);
          const auto format = cairo_image_surface_get_format(image);
          std::expected<Pixels, std::string> answer =
              std::unexpected(std::string("the snapshot does not hold the rectangle"));
          if ((format == CAIRO_FORMAT_ARGB32 || format == CAIRO_FORMAT_RGB24) &&
              data_start != nullptr && wanted.x >= 0 && wanted.y >= 0 && wanted.width > 0 &&
              wanted.height > 0 &&
              wanted.x + wanted.width <= cairo_image_surface_get_width(image) &&
              wanted.y + wanted.height <= cairo_image_surface_get_height(image)) {
            const auto at = [&](int column, int row) -> std::uint32_t {
              std::uint32_t pixel{};
              std::memcpy(&pixel,
                          data_start + static_cast<std::ptrdiff_t>(row) * stride +
                              static_cast<std::ptrdiff_t>(column) * 4,
                          sizeof(pixel));
              return pixel & 0x00FFFFFFU;
            };
            std::map<std::uint32_t, std::uint64_t> colours;
            for (int row = wanted.y; row < wanted.y + wanted.height; ++row)
              for (int column = wanted.x; column < wanted.x + wanted.width; ++column)
                ++colours[at(column, row)];
            std::uint32_t background{};
            std::uint64_t most{};
            for (const auto& [colour, count] : colours)
              if (count > most) {
                most = count;
                background = colour;
              }
            constexpr int a_little = 24;
            constexpr std::array<int, 3> channels{0, 8, 16};
            const auto differs = [&](std::uint32_t pixel) -> bool {
              return std::ranges::any_of(channels, [&](int shift) -> bool {
                const int one = static_cast<int>((pixel >> shift) & 0xFFU);
                const int other = static_cast<int>((background >> shift) & 0xFFU);
                return one - other > a_little || other - one > a_little;
              });
            };
            Pixels counted;
            for (const auto& [colour, count] : colours) {
              counted.total += count;
              if (differs(colour))
                counted.drawn += count;
            }
            answer = counted;
          }
          cairo_surface_unmap_image(surface, image);
          cairo_surface_destroy(surface);
          wanted.done.set_value(std::move(answer));
        },
        new std::shared_ptr<Region>(region));
    return true;
  });
  if (!started)
    return std::unexpected(std::string("The window is closed."));
  if (result.wait_for(std::chrono::seconds{15}) != std::future_status::ready)
    return std::unexpected(std::string("No snapshot in time."));
  return result.get();
}

void ShellTestHooks::mouse_move(double x, double y) {
  on_gtk<void>(impl_->window, [&]() -> void { mouse(*impl_, GDK_MOTION_NOTIFY, x, y); });
}
void ShellTestHooks::mouse_press(double x, double y) {
  on_gtk<void>(impl_->window, [&]() -> void { mouse(*impl_, GDK_BUTTON_PRESS, x, y); });
}
void ShellTestHooks::mouse_release(double x, double y) {
  on_gtk<void>(impl_->window, [&]() -> void { mouse(*impl_, GDK_BUTTON_RELEASE, x, y); });
}

void ShellTestHooks::answer_file_chooser(std::vector<std::string> files) {
  on_gtk<void>(impl_->window, [&]() -> void { impl_->chooser_answer = std::move(files); });
}

void ShellTestHooks::reload() {
  on_gtk<void>(impl_->window, [this]() -> void {
    if (impl_->view() != nullptr)
      webkit_web_view_reload(impl_->view());
  });
}

void ShellTestHooks::load(const std::string& uri) {
  on_gtk<void>(impl_->window, [&]() -> void {
    if (impl_->view() != nullptr)
      webkit_web_view_load_uri(impl_->view(), uri.c_str());
  });
}

void ShellTestHooks::close() {
  on_gtk<void>(impl_->window, [this]() -> void { impl_->window.close(); });
}

auto ShellTestHooks::graphics_report() -> std::expected<std::string, std::string> {
  struct Report {
    GtkWidget* window{};
    WebKitWebView* view{};
  };
  auto report = on_gtk<Report>(impl_->window, []() -> Report {
    Report made;
    made.window = gtk_offscreen_window_new();
    made.view = WEBKIT_WEB_VIEW(webkit_web_view_new());
    gtk_container_add(GTK_CONTAINER(made.window), GTK_WIDGET(made.view));
    gtk_widget_show_all(made.window);
    webkit_web_view_load_uri(made.view, "webkit://gpu");
    return made;
  });
  Text text = std::unexpected(std::string("webkit://gpu did not load."));
  for (int i = 0; i < 100; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    auto pending = std::make_shared<Pending>();
    auto result = pending->done.get_future();
    on_gtk<void>(impl_->window, [&]() -> void {
      constexpr std::string_view script =
          "return document.readyState === 'complete' ? document.body.innerText : '';";
      webkit_web_view_call_async_javascript_function(
          report.view, script.data(), static_cast<gssize>(script.size()), nullptr, nullptr,
          nullptr, nullptr, on_evaluated, new std::shared_ptr<Pending>(pending));
    });
    if (result.wait_for(std::chrono::seconds{5}) != std::future_status::ready)
      break;
    if (auto got = result.get(); got && !got->empty()) {
      text = std::move(got);
      break;
    }
  }
  on_gtk<void>(impl_->window, [&]() -> void { gtk_widget_destroy(report.window); });
  return text;
}

void ShellTestHooks::deliver_from_page(const std::string& raw) {
  on_gtk<void>(impl_->window, [&]() -> void { impl_->window.internals().receive(raw); });
}

void ShellTestHooks::post(std::uint64_t page, std::vector<std::string> messages) {
  impl_->window.post(PageToken{page}, std::move(messages));
}

} // namespace coffeecnc
