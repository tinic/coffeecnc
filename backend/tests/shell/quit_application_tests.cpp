// Full application Quit against a simulation INI, through the real WebKit bridge.
// The test drives protocol requests only; use tests/sim/run.sh, never a machine INI.
#include "application.hpp"
#include "panel_window_impl.hpp"

#include <iostream>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

using namespace coffeecnc;

namespace {
struct Evidence {
  bool supported{}; bool accepted{}; bool failed{}; bool timed_out{}; bool coolant{};
  std::string expected_coolant;
};
constexpr const char* script = R"js((() => {
  let receiver;
  let phase = 'reset';
  const post = (m) => window.webkit.messageHandlers.coffeecnc.postMessage(JSON.stringify(m));
  const mark = (s) => window.webkit.messageHandlers.quitEvidence.postMessage(s);
  Object.defineProperty(window, 'coffeecncReceive', {
    configurable: true,
    get: () => receiver,
    set: (fn) => { receiver = (m) => {
      fn(m);
      if (m.type === 'hello' && m.quit_supported) mark('supported');
      if (m.type === 'hello') mark('coolant:' + JSON.stringify(m.coolant ?? null));
      if (m.type === 'result' && m.id >= 1000000 && m.outcome !== 'accepted') mark('failed');
      if (m.type === 'result' && m.id === 1000002 && m.outcome === 'accepted') mark('accepted');
      if (m.type !== 'status' || m.link !== 'live') return;
      if (phase === 'reset' && m.machine === 'estop') {
        phase = 'on'; post({type:'set_machine',state:'off',id:1000000});
      } else if (phase === 'on' && m.machine === 'off') {
        phase = 'quit'; post({type:'set_machine',state:'on',id:1000001});
      } else if (phase === 'quit' && m.machine === 'on' && m.interp === 'idle') {
        phase = 'done'; post({type:'quit',id:1000002});
      }
    }; }
  });
})();)js";
}

auto main(int argc, char** argv) -> int {
  Options options;
  options.ui_directory = COFFEECNC_SOURCE_UI_DIRECTORY;
  for (int i = 1; i + 1 < argc; ++i)
    if (std::string_view(argv[i]) == "-ini") options.ini_file = argv[i + 1];
  if (options.ini_file.empty()) return 1;
  std::string coolant_mode = "unwired";
  for (int i = 1; i < argc; ++i)
    if (std::string_view(argv[i]).starts_with("--coolant=")) coolant_mode = std::string(argv[i]).substr(10);
  if (coolant_mode != "unwired" && coolant_mode != "wired" &&
      coolant_mode != "fail-pin" && coolant_mode != "fail-signal") return 1;
  const auto config = load_machine_config(options.ini_file);
  // These mutations belong only to the runner's disposable loopback simulation.
  if (!config || config->machine_name != "CoffeeCNC-sim") return 1;
  if (coolant_mode != "unwired") {
    std::ofstream postgui(config->postgui_halfile, std::ios::app);
    postgui << "\nunlinkp iocontrol.0.coolant-flood\n"
               "net coffeecnc-test-coolant iocontrol.0.coolant-flood => motion.digital-in-00\n";
    if (!postgui) return 1;
  }
  if (coolant_mode.starts_with("fail-")) {
    gchar* real_halcmd = g_find_program_in_path("halcmd");
    if (!real_halcmd) return 1;
    const auto directory = std::filesystem::path(config->directory) / "probe-failure-bin";
    std::filesystem::create_directories(directory);
    const auto wrapper = directory / "halcmd";
    std::ofstream script_file(wrapper);
    script_file << "#!/bin/sh\nif [ \"$1\" = -s ] && [ \"$3\" = "
                << (coolant_mode == "fail-pin" ? "pin" : "sig")
                << " ]; then exit 7; fi\nexec '" << real_halcmd << "' \"$@\"\n";
    script_file.close();
    g_free(real_halcmd);
    if (!script_file) return 1;
    std::filesystem::permissions(wrapper, std::filesystem::perms::owner_all);
    const char* path = std::getenv("PATH");
    const std::string test_path = directory.string() + ":" + (path ? path : "");
    if (setenv("PATH", test_path.c_str(), 1) != 0) return 1;
  }
  configure_environment();
  Evidence evidence;
  evidence.expected_coolant = coolant_mode.starts_with("fail-") ? "null" :
      coolant_mode == "wired" ? "{\"flood\":true,\"mist\":false}" : "{\"flood\":false,\"mist\":false}";
  guint timer = 0;
  const int code = run_application(options, {
    .before_load = [&](PanelWindow& window, const MachineConfig&) -> void {
      auto* manager = webkit_web_view_get_user_content_manager(window.internals().view);
      webkit_user_content_manager_register_script_message_handler(manager, "quitEvidence");
      g_signal_connect(manager, "script-message-received::quitEvidence", G_CALLBACK(+[](
          WebKitUserContentManager*, WebKitJavascriptResult* result, gpointer data) -> void {
        auto& state = *static_cast<Evidence*>(data);
        gchar* text = jsc_value_to_string(webkit_javascript_result_get_js_value(result));
        if (text != nullptr) {
          const std::string_view value{text};
          if (value == "supported") state.supported = true;
          if (value == "accepted") state.accepted = true;
          if (value == "failed") state.failed = true;
          if (value.starts_with("coolant:")) state.coolant = value.substr(8) == state.expected_coolant;
        }
        g_free(text);
      }), &evidence);
      auto* injected = webkit_user_script_new(script, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
          WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
      webkit_user_content_manager_add_script(manager, injected);
      webkit_user_script_unref(injected);
      // Never wait indefinitely if the app fails to quit. Keep all callback state alive
      // until run_application returns; the timer is removed on the successful path.
      struct Timeout { PanelWindow* window; Evidence* evidence; };
      timer = g_timeout_add_full(G_PRIORITY_DEFAULT, 15000, [](gpointer data) -> gboolean {
        const auto& timeout = *static_cast<Timeout*>(data);
        timeout.evidence->timed_out = true;
        timeout.window->close();
        return G_SOURCE_REMOVE;
      }, new Timeout{&window, &evidence}, [](gpointer data) -> void { delete static_cast<Timeout*>(data); });
    },
  });
  if (timer != 0 && !evidence.timed_out) g_source_remove(timer);
  const bool passed = code == 0 && evidence.supported && evidence.accepted && evidence.coolant &&
                      !evidence.failed && !evidence.timed_out;
  std::cout << "check=native_quit_application result=" << (passed ? "pass" : "fail")
            << " supported=" << evidence.supported << " accepted_before_close=" << evidence.accepted
            << " failed=" << evidence.failed << " timeout=" << evidence.timed_out
            << " coolant_mode=" << coolant_mode << " coolant_verified=" << evidence.coolant << '\n'
            << "exit=" << (passed ? 0 : 1) << '\n';
  return passed ? 0 : 1;
}
