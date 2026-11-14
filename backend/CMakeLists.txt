cmake_minimum_required(VERSION 3.25)
project(coffeecnc_backend VERSION 0.1.0 LANGUAGES CXX)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(FATAL_ERROR "Use an out-of-source build: cmake -S backend -B build/backend")
endif()

include(cmake/ProjectOptions.cmake)
include(cmake/StaticAnalyzers.cmake)
add_subdirectory(src/core)
option(COFFEECNC_BUILD_APP "Build the app protocol layer (requires nlohmann_json)" ON)
if(COFFEECNC_BUILD_APP)
  add_subdirectory(src/app)
endif()
add_subdirectory(src/linuxcnc)
if(COFFEECNC_BUILD_APP)
  add_subdirectory(src/shell)
  add_subdirectory(src/coffeecnc)
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(coffeecnc_core_tests tests/core_tests.cpp)
  target_link_libraries(coffeecnc_core_tests PRIVATE coffeecnc_core coffeecnc_project_options)
  coffeecnc_enable_analyzers(coffeecnc_core_tests)
  add_test(NAME core_behavior COMMAND coffeecnc_core_tests)
  if(COFFEECNC_BUILD_APP)
    add_executable(coffeecnc_request_tests tests/request_tests.cpp)
    target_link_libraries(coffeecnc_request_tests PRIVATE coffeecnc_app coffeecnc_project_options)
    coffeecnc_enable_analyzers(coffeecnc_request_tests)
    add_test(NAME request_boundary COMMAND coffeecnc_request_tests)
    # The tests below read wire text back with the JSON library the app layer keeps private.
    find_package(nlohmann_json 3.11 REQUIRED)
    add_executable(coffeecnc_fixture_tests tests/fixture_tests.cpp)
    target_link_libraries(coffeecnc_fixture_tests PRIVATE coffeecnc_app coffeecnc_project_options
      nlohmann_json::nlohmann_json)
    # One valid example of every panel command, as the panel serialises it.
    target_compile_definitions(coffeecnc_fixture_tests PRIVATE
      COFFEECNC_COMMAND_FIXTURES="${PROJECT_SOURCE_DIR}/../protocol/fixtures/commands.json"
      # At least one valid example of every message and variant the panel is sent.
      COFFEECNC_MESSAGE_FIXTURES="${PROJECT_SOURCE_DIR}/../protocol/fixtures/messages.json")
    coffeecnc_enable_analyzers(coffeecnc_fixture_tests)
    add_test(NAME protocol_fixtures COMMAND coffeecnc_fixture_tests)
    add_executable(coffeecnc_response_tests tests/response_tests.cpp)
    target_link_libraries(coffeecnc_response_tests PRIVATE coffeecnc_app coffeecnc_project_options
      nlohmann_json::nlohmann_json)
    coffeecnc_enable_analyzers(coffeecnc_response_tests)
    add_test(NAME response_encoding COMMAND coffeecnc_response_tests)
    add_executable(coffeecnc_message_tests tests/message_tests.cpp)
    target_link_libraries(coffeecnc_message_tests PRIVATE coffeecnc_app coffeecnc_project_options
      nlohmann_json::nlohmann_json)
    coffeecnc_enable_analyzers(coffeecnc_message_tests)
    add_test(NAME message_encoding COMMAND coffeecnc_message_tests)
    add_executable(coffeecnc_publisher_tests tests/publisher_tests.cpp)
    target_link_libraries(coffeecnc_publisher_tests PRIVATE coffeecnc_app coffeecnc_project_options
      nlohmann_json::nlohmann_json)
    coffeecnc_enable_analyzers(coffeecnc_publisher_tests)
    add_test(NAME status_publishing COMMAND coffeecnc_publisher_tests)
    add_executable(coffeecnc_panel_assets_tests tests/panel_assets_tests.cpp)
    target_link_libraries(coffeecnc_panel_assets_tests PRIVATE coffeecnc_app
      coffeecnc_project_options)
    coffeecnc_enable_analyzers(coffeecnc_panel_assets_tests)
    add_test(NAME panel_assets COMMAND coffeecnc_panel_assets_tests ${PROJECT_BINARY_DIR})
    add_executable(coffeecnc_panel_feed_tests tests/panel_feed_tests.cpp)
    target_link_libraries(coffeecnc_panel_feed_tests PRIVATE coffeecnc_app coffeecnc_project_options
      nlohmann_json::nlohmann_json)
    coffeecnc_enable_analyzers(coffeecnc_panel_feed_tests)
    add_test(NAME panel_feed COMMAND coffeecnc_panel_feed_tests)
    add_executable(coffeecnc_program_store_tests tests/program_store_tests.cpp)
    target_link_libraries(coffeecnc_program_store_tests PRIVATE coffeecnc_app
      coffeecnc_project_options)
    coffeecnc_enable_analyzers(coffeecnc_program_store_tests)
    # The std::filesystem implementation is tested on a directory the test makes below this one.
    add_test(NAME program_store COMMAND coffeecnc_program_store_tests ${PROJECT_BINARY_DIR})
    add_executable(coffeecnc_session_tests tests/session_tests.cpp)
    target_link_libraries(coffeecnc_session_tests PRIVATE coffeecnc_app coffeecnc_project_options
      nlohmann_json::nlohmann_json)
    coffeecnc_enable_analyzers(coffeecnc_session_tests)
    add_test(NAME session_lifecycle COMMAND coffeecnc_session_tests)
    add_executable(coffeecnc_runtime_tests tests/runtime_tests.cpp)
    target_link_libraries(coffeecnc_runtime_tests PRIVATE coffeecnc_app coffeecnc_project_options
      nlohmann_json::nlohmann_json)
    coffeecnc_enable_analyzers(coffeecnc_runtime_tests)
    add_test(NAME runtime_threads COMMAND coffeecnc_runtime_tests)
    if(TARGET coffeecnc_linuxcnc)
      # INI parsing is in the adapter layer, so this exists where the adapter builds. It reads
      # files only: no LinuxCNC runs.
      add_executable(coffeecnc_machine_config_tests tests/machine_config_tests.cpp)
      target_link_libraries(coffeecnc_machine_config_tests PRIVATE coffeecnc_linuxcnc
        coffeecnc_project_options)
      coffeecnc_enable_analyzers(coffeecnc_machine_config_tests)
      add_test(NAME machine_config COMMAND coffeecnc_machine_config_tests
        ${PROJECT_SOURCE_DIR}/tests/sim/run.sh ${PROJECT_BINARY_DIR}/machine_config)
      # LinuxCNC's status as the panel's messages, on constructed readings. No LinuxCNC runs.
      add_executable(coffeecnc_panel_mapping_tests tests/panel_mapping_tests.cpp)
      target_link_libraries(coffeecnc_panel_mapping_tests PRIVATE coffeecnc_linuxcnc
        coffeecnc_project_options)
      coffeecnc_enable_analyzers(coffeecnc_panel_mapping_tests)
      add_test(NAME panel_mapping COMMAND coffeecnc_panel_mapping_tests ${PROJECT_BINARY_DIR})
      # The tool change handshake between the control thread and the others. No HAL runs.
      add_executable(coffeecnc_hal_control_tests tests/hal_control_tests.cpp)
      target_link_libraries(coffeecnc_hal_control_tests PRIVATE coffeecnc_linuxcnc
        coffeecnc_project_options)
      coffeecnc_enable_analyzers(coffeecnc_hal_control_tests)
      add_test(NAME hal_control COMMAND coffeecnc_hal_control_tests)
      # The real Session on NmlPort against a running LinuxCNC on a simulated mill. Exists only
      # where the LinuxCNC adapter builds; `ctest -L linuxcnc` selects it, `-LE linuxcnc` skips it.
      add_executable(coffeecnc_sim_tests tests/sim/sim_tests.cpp tests/sim/hal_probe.cpp)
      # hal_probe.cpp is a HAL component of the test's own and includes hal.h.
      target_include_directories(coffeecnc_sim_tests SYSTEM PRIVATE
        ${COFFEECNC_LINUXCNC_INCLUDE_DIR})
      target_compile_definitions(coffeecnc_sim_tests PRIVATE ULAPI)
      target_link_libraries(coffeecnc_sim_tests PRIVATE coffeecnc_app coffeecnc_linuxcnc
        coffeecnc_project_options nlohmann_json::nlohmann_json ${COFFEECNC_LINUXCNC_HAL})
      coffeecnc_enable_analyzers(coffeecnc_sim_tests)
      foreach(units mm inch)
        # Every simulation test has a run directory of its own, named after the test: its
        # linuxcnc.log there is that test's and no other's.
        add_test(NAME linuxcnc_sim_${units} COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
          -u ${units} -t 300 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_sim_${units}
          $<TARGET_FILE:coffeecnc_sim_tests>)
        # One LinuxCNC per host at a time.
        set_tests_properties(linuxcnc_sim_${units} PROPERTIES LABELS linuxcnc RUN_SERIAL TRUE
          RESOURCE_LOCK linuxcnc TIMEOUT 400)
      endforeach()
      # The same on a mill with the G0759's INI values: unhomed MDI and runs, per-axis jog
      # ceilings, optional signals.
      add_test(NAME linuxcnc_sim_g0759 COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
        -p g0759 -t 300 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_sim_g0759
        $<TARGET_FILE:coffeecnc_sim_tests> g0759)
      add_test(NAME linuxcnc_sim_runner COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/runner_tests.sh
        ${PROJECT_BINARY_DIR})
      set_tests_properties(linuxcnc_sim_g0759 linuxcnc_sim_runner PROPERTIES LABELS linuxcnc
        RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 400)
      if(TARGET coffeecnc)
        # The executable's command line and its start-up refusals. No LinuxCNC and no display.
        add_test(NAME coffeecnc_cli COMMAND ${PROJECT_SOURCE_DIR}/tests/shell/cli_tests.sh
          $<TARGET_FILE:coffeecnc> ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
          ${PROJECT_BINARY_DIR}/cli)
        # The panel the tests below run: ui/dist, built from the ui/ sources of this checkout
        # and stamped with their hashes (ui/build-stamp.ts). The tests need it, and each checks
        # the stamp again itself before it does anything.
        add_test(NAME panel_bundle COMMAND ${PROJECT_SOURCE_DIR}/tests/shell/build_panel.sh
          ${PROJECT_SOURCE_DIR}/..)
        set_tests_properties(panel_bundle PROPERTIES FIXTURES_SETUP panel TIMEOUT 600)
        # The stamp checker refuses a changed source, a changed bundle and an unbuilt source.
        add_test(NAME panel_stamp COMMAND ${PROJECT_SOURCE_DIR}/tests/shell/panel_stamp_tests.sh
          ${PROJECT_SOURCE_DIR}/.. ${PROJECT_BINARY_DIR})
        set_tests_properties(panel_stamp PROPERTIES FIXTURES_REQUIRED panel)
        # The application with the real panel in its window, as LinuxCNC's DISPLAY program on a
        # display of its own (Xvfb): the test drives the page and watches LinuxCNC.
        add_executable(coffeecnc_shell_tests tests/shell/shell_tests.cpp
          ${COFFEECNC_LSAN_SUPPRESSIONS})
        target_compile_definitions(coffeecnc_shell_tests PRIVATE
          COFFEECNC_SAMPLES_DIRECTORY="${PROJECT_SOURCE_DIR}/../samples"
          COFFEECNC_SOURCE_UI_DIRECTORY="${PROJECT_SOURCE_DIR}/../ui/dist"
          COFFEECNC_PANEL_STAMP_CHECK="node ${PROJECT_SOURCE_DIR}/tests/sim/check-panel-stamp.mjs ${PROJECT_SOURCE_DIR}/.."
          COFFEECNC_EVIDENCE_DIRECTORY="${PROJECT_SOURCE_DIR}/build/shell-evidence")
        target_link_libraries(coffeecnc_shell_tests PRIVATE coffeecnc_application
          coffeecnc_shell_test_hooks coffeecnc_project_options nlohmann_json::nlohmann_json)
        coffeecnc_enable_analyzers(coffeecnc_shell_tests)
        # Not registered under ThreadSanitizer: with the GUI the tests are UNQUALIFIED there, not
        # passing. GLib, GIO, GTK, WebKit and JavaScriptCore are not instrumented, and
        # ThreadSanitizer reports data races between their own threads. Run by hand at 4588b3b
        # (logs and the distinct stacks in build/sim-history/4588b3b/tsan-gui/): 133 reports in
        # the mm shell test, 140 in the inch one, 76 in the start-up test. In every one the top
        # frame of both accesses is in libglib-2.0, libgio-2.0, libjavascriptcoregtk-4.1,
        # libwebkit2gtk-4.1, libfontconfig, libpangoft2 or libc: no report has an access-origin
        # stack in CoffeeCNC code. (At ca88a0a 119 reports had one; they went when data began to
        # cross to the GTK thread through the shell's own queue.) Each of these programs then
        # exits 66 on the reports. All 202 checks of each shell test passed in those runs. No
        # suppression is in place. The programs still build with the preset.
        if(NOT "thread" IN_LIST COFFEECNC_SANITIZERS)
          foreach(units mm inch)
            add_test(NAME linuxcnc_shell_${units} COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
              -u ${units} -t 500 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_shell_${units}
              ${PROJECT_SOURCE_DIR}/tests/shell/under_xvfb.sh
              $<TARGET_FILE:coffeecnc_shell_tests>)
            set_tests_properties(linuxcnc_shell_${units} PROPERTIES LABELS linuxcnc RUN_SERIAL TRUE
              RESOURCE_LOCK linuxcnc TIMEOUT 600 FIXTURES_REQUIRED panel)
          endforeach()
          # The `coffeecnc` executable itself as the DISPLAY program: it starts, shows the panel,
          # and SIGTERM ends it and LinuxCNC with no process left.
          add_test(NAME linuxcnc_coffeecnc_start COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
            -t 120 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_coffeecnc_start
            ${PROJECT_SOURCE_DIR}/tests/shell/under_xvfb.sh --terminate-after=12
            $<TARGET_FILE:coffeecnc>)
          set_tests_properties(linuxcnc_coffeecnc_start PROPERTIES LABELS linuxcnc RUN_SERIAL TRUE
            RESOURCE_LOCK linuxcnc TIMEOUT 200 FIXTURES_REQUIRED panel)
        endif()
      endif()
    endif()
  endif()
endif()
