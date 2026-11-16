#include "panel_window_impl.hpp"

#include <coffeecnc/app/panel_assets.hpp>

#include <algorithm>
#include <csignal>
#include <cstddef>
#include <iterator>
#include <string_view>

#include <glib-unix.h>
#include <libsoup/soup.h>

namespace coffeecnc {
namespace {

constexpr guint reload_delay_ms = 500;
constexpr int ok = 200;
constexpr int method_not_allowed = 405;
constexpr int not_found = 404;

// Only the panel's own origin: no network, no other scheme. Scripts are the panel's files
// only. 'unsafe-eval' is for the STEP importer: its WebAssembly glue (occt-import-js, built
// with Emscripten's embind) makes its bindings with new Function, and without the word the
// import fails in its worker ("Refused to evaluate a string as JavaScript"). The panel's
// components set inline styles; canvases use blob and data images. `blob:` workers are not
// needed: the worker script is a file of the panel.
constexpr const char* content_security_policy =
    "default-src 'self'; script-src 'self' 'wasm-unsafe-eval' 'unsafe-eval'; "
    "style-src 'self' 'unsafe-inline'; "
    "img-src 'self' data: blob:; font-src 'self'; connect-src 'self'; worker-src 'self'; "
    "object-src 'none'; base-uri 'none'; form-action 'none'; frame-src 'none'";

// Runs first in every document of the top frame, before any script of the page. The document
// gives itself an identity that scripts of the page cannot change and tells the shell. Then it
// puts its own function in the place of the `coffeecnc` handler's postMessage: every message
// the page posts leaves as `<identity>:<text>`, so the shell knows which document sent it.
// The handler object itself is WebKit's and was there before this ran; the page's check that
// `window.webkit.messageHandlers.coffeecnc.postMessage` is a function sees the same object with
// a function in it. The last character tells the shell whether that is in place.
constexpr const char* document_start_script = R"js((() => {
  const bytes = new Uint8Array(16);
  crypto.getRandomValues(bytes);
  const identity = Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');
  Object.defineProperty(window, '__coffeecncPage', { value: identity });
  const handlers = window.webkit && window.webkit.messageHandlers;
  const native = handlers && handlers.coffeecnc;
  let bridge = false;
  if (native && typeof native.postMessage === 'function') {
    const post = native.postMessage.bind(native);
    const marked = (text) => post(identity + ':' + String(text));
    try {
      Object.defineProperty(native, 'postMessage', { value: marked, writable: true, configurable: true });
    } catch (error) { /* reported below */ }
    bridge = native.postMessage === marked;
  }
  if (handlers && handlers.coffeecncPage) handlers.coffeecncPage.postMessage(identity + (bridge ? ':1' : ':0'));
})();)js";

constexpr std::size_t identity_digits = 32;

auto text_of(WebKitJavascriptResult* result) -> std::optional<std::string> {
  JSCValue* value = webkit_javascript_result_get_js_value(result);
  if (jsc_value_is_string(value) == FALSE)
    return {};
  gchar* text = jsc_value_to_string(value);
  if (text == nullptr)
    return {};
  std::string copy{text};
  g_free(text);
  return copy;
}

// Answers a request with `length` bytes that WebKit reads from `stream` when it wants them.
void respond(WebKitURISchemeRequest* request, int status, std::string_view content_type,
             GInputStream* stream, gint64 length) {
  WebKitURISchemeResponse* response = webkit_uri_scheme_response_new(stream, length);
  webkit_uri_scheme_response_set_status(response, static_cast<guint>(status), nullptr);
  webkit_uri_scheme_response_set_content_type(response, std::string(content_type).c_str());
  SoupMessageHeaders* headers = soup_message_headers_new(SOUP_MESSAGE_HEADERS_RESPONSE);
  // The page reads the type from the headers (fetch, WebAssembly streaming), WebKit from the
  // response: both say the same.
  soup_message_headers_append(headers, "Content-Type", std::string(content_type).c_str());
  soup_message_headers_append(headers, "Content-Security-Policy", content_security_policy);
  soup_message_headers_append(headers, "X-Content-Type-Options", "nosniff");
  soup_message_headers_append(headers, "Cache-Control", "no-store");
  webkit_uri_scheme_response_set_http_headers(response, headers); // takes the headers
  webkit_uri_scheme_request_finish_with_response(request, response);
  g_object_unref(response);
}

// A refusal: a status and a line of text.
void refuse(WebKitURISchemeRequest* request, int status, std::string_view text) {
  GBytes* bytes = g_bytes_new(text.data(), text.size());
  GInputStream* stream = g_memory_input_stream_new_from_bytes(bytes);
  respond(request, status, "text/plain", stream, static_cast<gint64>(text.size()));
  g_object_unref(stream);
  g_bytes_unref(bytes);
}

// On the GTK thread, like every signal. The file is only opened here: WebKit reads the stream
// in pieces, off this thread, so that a large file (the STEP importer's WebAssembly is 7.6 MB)
// does not keep the main loop from delivering the machine's status to the page.
void on_scheme(WebKitURISchemeRequest* request, gpointer data) {
  const auto* self = static_cast<PanelWindow::Impl*>(data);
  const std::string_view method = webkit_uri_scheme_request_get_http_method(request);
  if (method != "GET") {
    refuse(request, method_not_allowed, "Only GET.");
    return;
  }
  const auto asset =
      resolve_asset(self->options.ui_directory, webkit_uri_scheme_request_get_uri(request));
  if (!asset) {
    refuse(request, asset.error(), "Not a file of the panel.");
    return;
  }
  GFile* file = g_file_new_for_path(asset->file.c_str());
  GFileInfo* info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_SIZE, G_FILE_QUERY_INFO_NONE,
                                      nullptr, nullptr);
  GFileInputStream* stream = info != nullptr ? g_file_read(file, nullptr, nullptr) : nullptr;
  if (stream == nullptr) {
    refuse(request, not_found, "Not a file of the panel.");
  } else {
    respond(request, ok, asset->content_type, G_INPUT_STREAM(stream),
            static_cast<gint64>(g_file_info_get_size(info)));
    g_object_unref(stream);
  }
  if (info != nullptr)
    g_object_unref(info);
  g_object_unref(file);
}

void on_message(WebKitUserContentManager* /*manager*/, WebKitJavascriptResult* result,
                gpointer data) {
  if (const auto text = text_of(result))
    static_cast<PanelWindow::Impl*>(data)->receive(*text);
}

// The document's identity: 32 hexadecimal digits, then `:1` or `:0` for the bridge.
void on_identity(WebKitUserContentManager* /*manager*/, WebKitJavascriptResult* result,
                 gpointer data) {
  auto* self = static_cast<PanelWindow::Impl*>(data);
  const auto text = text_of(result);
  constexpr std::size_t digits = identity_digits;
  if (!text || !self->page || !self->identity.empty() || text->size() != digits + 2 ||
      !std::ranges::all_of(text->substr(0, digits), [](unsigned char c) -> bool {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      }))
    return;
  self->identity = text->substr(0, digits);
  self->handler_at_start = text->ends_with(":1");
}

void on_load_changed(WebKitWebView* /*view*/, WebKitLoadEvent event, gpointer data) {
  auto* self = static_cast<PanelWindow::Impl*>(data);
  if (event != WEBKIT_LOAD_COMMITTED)
    return;
  self->end_page();
  self->begin_page();
}

auto on_load_failed(WebKitWebView* /*view*/, WebKitLoadEvent /*event*/, const gchar* /*uri*/,
                    GError* error, gpointer data) -> gboolean {
  auto* self = static_cast<PanelWindow::Impl*>(data);
  // A navigation that was refused or cancelled replaces nothing.
  if (error != nullptr &&
      (error->domain == WEBKIT_POLICY_ERROR ||
       g_error_matches(error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED) != FALSE))
    return TRUE;
  self->end_page();
  self->reload_later();
  return TRUE; // No error page: it would be a document of its own.
}

void on_process_terminated(WebKitWebView* /*view*/, WebKitWebProcessTerminationReason /*reason*/,
                           gpointer data) {
  auto* self = static_cast<PanelWindow::Impl*>(data);
  self->end_page();
  self->reload_later();
}

auto on_policy(WebKitWebView* /*view*/, WebKitPolicyDecision* decision,
               WebKitPolicyDecisionType type, gpointer /*data*/) -> gboolean {
  if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE)
    return FALSE;
  bool allowed = false;
  if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
    WebKitNavigationAction* action = webkit_navigation_policy_decision_get_navigation_action(
        WEBKIT_NAVIGATION_POLICY_DECISION(decision));
    const gchar* uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(action));
    allowed = uri != nullptr && is_panel_url(uri);
  }
  if (allowed)
    webkit_policy_decision_use(decision);
  else
    webkit_policy_decision_ignore(decision);
  return TRUE;
}

auto on_context_menu(WebKitWebView* /*view*/, WebKitContextMenu* /*menu*/, GdkEvent* /*event*/,
                     WebKitHitTestResult* /*hit*/, gpointer /*data*/) -> gboolean {
  return TRUE; // No menu.
}

auto on_create(WebKitWebView* /*view*/, WebKitNavigationAction* /*action*/, gpointer /*data*/)
    -> GtkWidget* {
  return nullptr; // No second window.
}

auto on_permission(WebKitWebView* /*view*/, WebKitPermissionRequest* request, gpointer /*data*/)
    -> gboolean {
  webkit_permission_request_deny(request);
  return TRUE;
}

void on_destroy(GtkWidget* /*widget*/, gpointer data) {
  auto* self = static_cast<PanelWindow::Impl*>(data);
  self->end_page();
  {
    const std::scoped_lock lock{self->outbox->mutex};
    self->outbox->closed = true;
    self->outbox->waiting.clear();
    self->outbox->work.clear();
  }
  self->window = nullptr;
  self->view = nullptr;
  if (self->running)
    gtk_main_quit();
}

auto on_signal(gpointer data) -> gboolean {
  auto* self = static_cast<PanelWindow::Impl*>(data);
  if (self->window != nullptr)
    gtk_widget_destroy(self->window);
  return G_SOURCE_CONTINUE;
}

auto on_reload(gpointer data) -> gboolean {
  auto* self = static_cast<PanelWindow::Impl*>(data);
  self->reload_source = 0;
  if (self->view != nullptr)
    webkit_web_view_load_uri(self->view, std::string(panel_url).c_str());
  return G_SOURCE_REMOVE;
}

// The window of the process, for the wake-up below: one window at a time.
std::mutex window_mutex;
PanelWindow::Impl* the_window{};

// The wake-up of the GTK thread. It is given no data: what there is to do is in the outbox.
auto on_wake(gpointer /*data*/) -> gboolean {
  PanelWindow::Impl* window = nullptr;
  {
    const std::scoped_lock lock{window_mutex};
    window = the_window;
  }
  // The GTK thread is the one that makes and destroys the window: it is there for this call.
  if (window != nullptr)
    window->drain();
  return G_SOURCE_REMOVE;
}

// Any thread. Always through the main loop, never g_main_context_invoke: while no loop runs
// that would call on this thread, and WebKit is the GTK thread's.
void wake() { g_idle_add_full(G_PRIORITY_DEFAULT, on_wake, nullptr, nullptr); }

} // namespace

void PanelWindow::Impl::receive(std::string_view raw) {
  // `<identity>:<text>`, and the identity must be that of the document that shows: a message
  // of a document that has been replaced is not the present page's, whenever it arrives.
  if (!page || identity.empty() || raw.size() <= identity_digits || raw[identity_digits] != ':' ||
      raw.substr(0, identity_digits) != identity) {
    ++rejected;
    return;
  }
  if (events.message)
    events.message(*page, raw.substr(identity_digits + 1));
}

void PanelWindow::Impl::drain() {
  std::vector<std::pair<PageToken, std::vector<std::string>>> waiting;
  std::vector<std::function<void()>> work;
  std::optional<PageToken> close_now;
  {
    const std::scoped_lock lock{outbox->mutex};
    waiting.swap(outbox->waiting);
    work.swap(outbox->work);
    outbox->scheduled = false;
    close_now = std::exchange(outbox->close_requested, std::nullopt);
  }
  // Work that waits behind work that closed the window is dropped, not run.
  for (const auto& task : work) {
    {
      const std::scoped_lock lock{outbox->mutex};
      if (outbox->closed)
        break;
    }
    task();
  }
  for (const auto& [token, messages] : waiting)
    deliver(token, messages);
  if (close_now && page == close_now)
    close_when_delivered = close_now;
  finish_close();
}

auto PanelWindow::Impl::on_gtk_thread(std::vector<std::function<void()>> work) -> bool {
  {
    const std::scoped_lock lock{outbox->mutex};
    if (outbox->closed)
      return false;
    std::ranges::move(work, std::back_inserter(outbox->work));
    if (std::exchange(outbox->scheduled, true))
      return true;
  }
  wake();
  return true;
}

void PanelWindow::Impl::begin_page() {
  delivery_failed = false;
  close_when_delivered.reset();
  identity.clear();
  handler_at_start.reset();
  ++commits;
  if (events.begin_page)
    page = events.begin_page();
}

void PanelWindow::Impl::end_page() {
  close_when_delivered.reset();
  if (!page)
    return;
  const auto ended = *page;
  page.reset();
  identity.clear();
  if (events.end_page)
    events.end_page(ended);
}

void PanelWindow::Impl::reload_later() {
  if (reload_source == 0 && view != nullptr)
    reload_source = g_timeout_add(reload_delay_ms, on_reload, this);
}

void PanelWindow::Impl::finish_close() {
  if (!close_when_delivered || pending_deliveries != 0)
    return;
  const auto requested = std::exchange(close_when_delivered, std::nullopt);
  if (page == requested && !delivery_failed && window != nullptr)
    gtk_widget_destroy(window);
}

void PanelWindow::Impl::deliver(PageToken token, const std::vector<std::string>& messages) {
  if (view == nullptr || !page || *page != token || identity.empty()) {
    dropped += messages.size();
    return;
  }
  // The callback, not submission to WebKit, marks a delivery complete. Returning true
  // proves that this document's receiver ran for every message without throwing.
  std::string script = "(()=>{if(window.__coffeecncPage!==\"" + identity +
                       "\")return false;const r=window.coffeecncReceive;if(!r)return false;"
                       "let ok=true;for(const m of[";
  for (const auto& message : messages)
    script.append(message).append(",");
  script.append("]){try{r(m)}catch(e){ok=false;console.error(e)}}return ok;})()");
  struct Delivery {
    std::shared_ptr<Outbox> outbox;
    PageToken page;
    std::size_t count;
  };
  ++pending_deliveries;
  webkit_web_view_evaluate_javascript(
      view, script.c_str(), static_cast<gssize>(script.size()), nullptr, nullptr, nullptr,
      [](GObject* source, GAsyncResult* result, gpointer data) -> void {
        const std::unique_ptr<Delivery> delivery{static_cast<Delivery*>(data)};
        GError* error = nullptr;
        JSCValue* value = webkit_web_view_evaluate_javascript_finish(
            WEBKIT_WEB_VIEW(source), result, &error);
        const bool success = value != nullptr && jsc_value_is_boolean(value) &&
                             jsc_value_to_boolean(value);
        g_clear_object(&value);
        g_clear_error(&error);
        // A completion can outlive the window (or arrive during a later window's
        // main loop). The outbox identity avoids retaining or dereferencing Impl.
        Impl* self = nullptr;
        {
          const std::scoped_lock lock{window_mutex};
          if (the_window != nullptr && the_window->outbox == delivery->outbox)
            self = the_window;
        }
        if (self == nullptr)
          return;
        --self->pending_deliveries;
        if (self->page == delivery->page) {
          if (success)
            self->delivered += delivery->count;
          else {
            self->delivery_failed = true;
            g_warning("CoffeeCNC message delivery failed; pending Quit will not close the window.");
          }
        }
        self->finish_close();
      }, new Delivery{outbox, token, messages.size()});
}

auto initialise_display() -> bool { return gtk_init_check(nullptr, nullptr) != FALSE; }

PanelWindow::PanelWindow(PanelOptions options, PanelEvents events)
    : impl_(std::make_unique<Impl>()) {
  Impl& impl = *impl_;
  impl.options = std::move(options);
  impl.events = std::move(events);
  {
    const std::scoped_lock lock{window_mutex};
    the_window = &impl;
  }

  // Nothing of the panel is kept on disk: no cache, no storage.
  impl.context = webkit_web_context_new_ephemeral();
  webkit_web_context_register_uri_scheme(impl.context, std::string(panel_scheme).c_str(), on_scheme,
                                         &impl, nullptr);
  WebKitSecurityManager* security = webkit_web_context_get_security_manager(impl.context);
  webkit_security_manager_register_uri_scheme_as_secure(security,
                                                        std::string(panel_scheme).c_str());
  webkit_security_manager_register_uri_scheme_as_cors_enabled(security,
                                                              std::string(panel_scheme).c_str());

  // The handlers exist before the view does, so before any document.
  WebKitUserContentManager* content = webkit_user_content_manager_new();
  g_signal_connect(content, "script-message-received::coffeecnc", G_CALLBACK(on_message), &impl);
  g_signal_connect(content, "script-message-received::coffeecncPage", G_CALLBACK(on_identity),
                   &impl);
  webkit_user_content_manager_register_script_message_handler(content, "coffeecnc");
  webkit_user_content_manager_register_script_message_handler(content, "coffeecncPage");
  WebKitUserScript* script =
      webkit_user_script_new(document_start_script, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                             WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
  webkit_user_content_manager_add_script(content, script);
  webkit_user_script_unref(script);

  impl.view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "web-context", impl.context,
                                           "user-content-manager", content, nullptr));
  g_object_unref(content);
  WebKitSettings* settings = webkit_web_view_get_settings(impl.view);
  webkit_settings_set_enable_developer_extras(settings,
                                              impl.options.developer_tools ? TRUE : FALSE);
  webkit_settings_set_enable_back_forward_navigation_gestures(settings, FALSE);
  webkit_settings_set_javascript_can_open_windows_automatically(settings, FALSE);
  g_signal_connect(impl.view, "load-changed", G_CALLBACK(on_load_changed), &impl);
  g_signal_connect(impl.view, "load-failed", G_CALLBACK(on_load_failed), &impl);
  g_signal_connect(impl.view, "web-process-terminated", G_CALLBACK(on_process_terminated), &impl);
  g_signal_connect(impl.view, "decide-policy", G_CALLBACK(on_policy), &impl);
  g_signal_connect(impl.view, "context-menu", G_CALLBACK(on_context_menu), &impl);
  g_signal_connect(impl.view, "create", G_CALLBACK(on_create), &impl);
  g_signal_connect(impl.view, "permission-request", G_CALLBACK(on_permission), &impl);

  impl.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(impl.window), "CoffeeCNC");
  gtk_container_add(GTK_CONTAINER(impl.window), GTK_WIDGET(impl.view));
  g_signal_connect(impl.window, "destroy", G_CALLBACK(on_destroy), &impl);
  if (impl.options.windowed) {
    gtk_window_set_default_size(GTK_WINDOW(impl.window), impl.options.window_width,
                                impl.options.window_height);
  } else {
    // The size of the screen as well: without a window manager nothing else makes it so.
    GdkRectangle screen{};
    GdkDisplay* display = gdk_display_get_default();
    GdkMonitor* monitor = display != nullptr ? gdk_display_get_primary_monitor(display) : nullptr;
    if (monitor == nullptr && display != nullptr)
      monitor = gdk_display_get_monitor(display, 0);
    if (monitor != nullptr) {
      gdk_monitor_get_geometry(monitor, &screen);
      gtk_window_set_default_size(GTK_WINDOW(impl.window), screen.width, screen.height);
      gtk_window_move(GTK_WINDOW(impl.window), screen.x, screen.y);
    }
    gtk_window_set_decorated(GTK_WINDOW(impl.window), FALSE);
    gtk_window_fullscreen(GTK_WINDOW(impl.window));
  }
}

PanelWindow::~PanelWindow() {
  Impl& impl = *impl_;
  if (impl.reload_source != 0)
    g_source_remove(impl.reload_source);
  if (impl.window != nullptr)
    gtk_widget_destroy(impl.window);
  {
    const std::scoped_lock lock{impl.outbox->mutex};
    impl.outbox->closed = true;
    impl.outbox->waiting.clear();
    impl.outbox->work.clear();
  }
  // A wake-up that is still on its way finds no window and does nothing.
  {
    const std::scoped_lock lock{window_mutex};
    the_window = nullptr;
  }
  g_object_unref(impl.context);
}

void PanelWindow::run() {
  Impl& impl = *impl_;
  if (impl.window == nullptr)
    return;
  constexpr std::array<int, 3> terminating{SIGINT, SIGTERM, SIGHUP};
  for (std::size_t i = 0; i < terminating.size(); ++i)
    impl.signal_sources.at(i) = g_unix_signal_add(terminating.at(i), on_signal, &impl);
  gtk_widget_show_all(impl.window);
  webkit_web_view_load_uri(impl.view, std::string(panel_url).c_str());
  impl.running = true;
  gtk_main();
  impl.running = false;
  for (auto& source : impl.signal_sources)
    if (source != 0)
      g_source_remove(std::exchange(source, 0));
}

void PanelWindow::close() {
  if (impl_->window != nullptr)
    gtk_widget_destroy(impl_->window);
}

void PanelWindow::close_after_posts(PageToken page) {
  const auto& outbox = impl_->outbox;
  {
    const std::scoped_lock lock{outbox->mutex};
    if (outbox->closed)
      return;
    outbox->close_requested = page;
    if (std::exchange(outbox->scheduled, true))
      return;
  }
  wake();
}

void PanelWindow::post(PageToken page, std::vector<std::string> messages) {
  if (messages.empty())
    return;
  const auto& outbox = impl_->outbox;
  {
    const std::scoped_lock lock{outbox->mutex};
    if (outbox->closed)
      return;
    outbox->waiting.emplace_back(page, std::move(messages));
    if (std::exchange(outbox->scheduled, true))
      return;
  }
  wake();
}

} // namespace coffeecnc
