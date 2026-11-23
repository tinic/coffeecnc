// Full application Quit against a simulation INI, through the real WebKit bridge.
// The test drives protocol requests only; use tests/sim/run.sh, never a machine INI.
#include "application.hpp"
#include "panel_window_impl.hpp"

#include <array>
#include <cstdio>
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
  bool tool_prompt{}; bool tool_loaded{};
  std::string expected_coolant;
};
auto tool_change_pin(const char* command) -> std::string {
  std::string output;
  if (auto* pipe = popen(command, "r")) {
    std::array<char, 512> line{};
    while (fgets(line.data(), static_cast<int>(line.size()), pipe))
      output += line.data();
    if (pclose(pipe) != 0)
      return {};
  }
  return output;
}
constexpr const char* script = R"js((() => {
  let receiver;
  let phase = 'reset';
  let firstFrame;
  let drawing = false;
  function waitForDrawing(now) {
    firstFrame ??= now;
    // Deliver the simulated operator's first press after the panel has drawn steadily.
    drawing = now - firstFrame >= 250;
    if (!drawing) requestAnimationFrame(waitForDrawing);
  }
  requestAnimationFrame(waitForDrawing);
  const post = (m) => window.webkit.messageHandlers.coffeecnc.postMessage(JSON.stringify(m));
  const mark = (s) => window.webkit.messageHandlers.quitEvidence.postMessage(s);
  Object.defineProperty(window, 'coffeecncReceive', {
    configurable: true,
    get: () => receiver,
    set: (fn) => { receiver = (m) => {
      fn(m);
      if (m.type === 'hello' && m.quit_supported) mark('supported');
      if (m.type === 'hello') mark('coolant:' + JSON.stringify(m.coolant ?? null));
      if (m.type === 'result' && m.id >= 1000000 && m.outcome !== 'accepted')
        mark('failed:' + m.id + ':' + m.reason);
      if (m.type === 'result' && [1000002, 1000006].includes(m.id) && m.outcome === 'accepted') mark('accepted');
      if (!drawing || m.type !== 'status' || m.link !== 'live') return;
      if (phase === 'reset' && m.machine === 'estop') {
        phase = 'on'; post({type:'set_machine',state:'off',id:1000000});
      } else if (phase === 'on' && m.machine === 'off') {
        phase = 'quit'; post({type:'set_machine',state:'on',id:1000001});
      } else if (phase === 'quit' && m.machine === 'on' && m.interp === 'idle') {
        if (window.toolChangeCase) {
          phase = 'home'; post({type:'home',axis:'all',id:1000003});
        } else {
          phase = 'done'; post({type:'quit',id:1000002});
        }
      } else if (phase === 'home' && m.axes.every(axis => axis.homed && !axis.homing)) {
        phase = 'change'; post({type:'mdi',text:'T1 M6',id:1000004});
      } else if (phase === 'change' && m.wait.kind === 'tool_change' && m.wait.tool === 1) {
        mark('tool-prompt');
        phase = 'confirmed'; post({type:'tool_changed',id:1000005});
      } else if ((phase === 'change' || phase === 'confirmed') && m.tool.in_spindle === 1 && m.interp === 'idle') {
        mark('tool-loaded');
        phase = 'done'; post({type:'quit',id:1000006});
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
  std::string tool_change_mode;
  for (int i = 1; i < argc; ++i) {
    if (std::string_view(argv[i]).starts_with("--coolant=")) coolant_mode = std::string(argv[i]).substr(10);
    if (std::string_view(argv[i]).starts_with("--tool-change=")) tool_change_mode = std::string(argv[i]).substr(14);
  }
  if (coolant_mode != "unwired" && coolant_mode != "wired" &&
      coolant_mode != "fail-pin" && coolant_mode != "fail-signal") return 1;
  const auto config = load_machine_config(options.ini_file);
  // These mutations belong only to the runner's disposable loopback simulation.
  if (!config || config->machine_name != "CoffeeCNC-sim") return 1;
  if (!tool_change_mode.empty()) {
    if (tool_change_mode != "external" && tool_change_mode != "manual" &&
        tool_change_mode != "conflict") return 1;
    std::ofstream ini(options.ini_file, std::ios::app);
    ini << "\n[COFFEECNC]\nMANUAL_TOOLCHANGE = " << (tool_change_mode == "external" ? "no" : "yes") << '\n';
    std::ofstream postgui(config->postgui_halfile);
    postgui << "source coffeecnc_postgui.hal\n";
    // Keep LinuxCNC's own changer loop for the external case. In the conflict case leave
    // the acknowledgement pin owned so the app must roll back its earlier links.
    if (tool_change_mode != "external")
      postgui << "unlinkp iocontrol.0.tool-change\n";
    if (tool_change_mode == "manual")
      postgui << "unlinkp iocontrol.0.tool-changed\n";
    if (!ini || !postgui) return 1;
  }
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
        gchar* text = jsc_value_to_string(webkit_javascript_result_get_js_value(result));
        if (text != nullptr) {
          auto& state = *static_cast<Evidence*>(data);
          const std::string_view value{text};
          if (value == "supported") state.supported = true;
          if (value == "accepted") state.accepted = true;
          if (value.starts_with("failed:")) {
            state.failed = true;
            std::cout << "fact=quit_request_" << value << '\n';
          }
          if (value == "tool-prompt") state.tool_prompt = true;
          if (value == "tool-loaded") state.tool_loaded = true;
          if (value.starts_with("coolant:")) state.coolant = value.substr(8) == state.expected_coolant;
        }
        g_free(text);
      }), &evidence);
      const std::string panel_script = std::string(tool_change_mode.empty() ? "" : "window.toolChangeCase = true;") + script;
      auto* injected = webkit_user_script_new(panel_script.c_str(), WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
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
  bool passed = code == 0 && evidence.supported && evidence.accepted && evidence.coolant &&
                !evidence.failed && !evidence.timed_out;
  if (!tool_change_mode.empty()) {
    const auto changed = tool_change_pin("halcmd -s show pin iocontrol.0.tool-changed");
    const auto signals = tool_change_pin("halcmd -s show sig coffeecnc-manual");
    if (tool_change_mode == "conflict") {
      passed = code == 1 && timer == 0 && changed.find("tool-change-loop") != std::string::npos &&
               signals.find("coffeecnc-manual-") == std::string::npos;
    } else {
      passed = passed && evidence.tool_loaded &&
               evidence.tool_prompt == (tool_change_mode == "manual") &&
               (tool_change_mode != "external" || changed.find("tool-change-loop") != std::string::npos);
    }
    std::cout << "check=tool_change_case result=" << (passed ? "pass" : "fail")
              << " mode=" << tool_change_mode << " prompt=" << evidence.tool_prompt
              << " loaded=" << evidence.tool_loaded << " app_code=" << code
              << " existing=" << (changed.find("tool-change-loop") != std::string::npos) << '\n';
  }
  std::cout << "check=native_quit_application result=" << (passed ? "pass" : "fail")
            << " supported=" << evidence.supported << " accepted_before_close=" << evidence.accepted
            << " failed=" << evidence.failed << " timeout=" << evidence.timed_out
            << " coolant_mode=" << coolant_mode << " coolant_verified=" << evidence.coolant << '\n'
            << "exit=" << (passed ? 0 : 1) << '\n';
  return passed ? 0 : 1;
}
