cmake_minimum_required(VERSION 3.25)
project(coffeecnc_backend VERSION 0.1.4 LANGUAGES CXX)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(FATAL_ERROR "Use an out-of-source build: cmake -S backend -B build/backend")
endif()

include(cmake/ProjectOptions.cmake)
# Directories an installation or package creates: not writable by the group, whatever the
# umask of the build is.
set(CMAKE_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
  GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
include(cmake/StaticAnalyzers.cmake)
include(CTest)
add_subdirectory(src/core)
option(COFFEECNC_BUILD_APP "Build the app protocol layer (requires nlohmann_json)" ON)
# ON: an executable that finds no panel or preview helper installed beside it uses the panel of
# this checkout (ui/dist) and the helper of this build tree; both paths are compiled in. OFF,
# for a build that is packaged: only the installed places are looked at.
option(COFFEECNC_DEVELOPMENT_PATHS
  "Compile in this checkout's panel and this build tree's preview helper as fallbacks" ON)
if(COFFEECNC_BUILD_APP)
  add_subdirectory(src/app)
endif()
add_subdirectory(src/linuxcnc)
if(COFFEECNC_BUILD_APP)
  add_subdirectory(src/shell)
  add_subdirectory(src/coffeecnc)
endif()
if(TARGET coffeecnc)
  include(cmake/Packaging.cmake)
endif()

if(BUILD_TESTING)
  add_executable(coffeecnc_core_tests tests/core_tests.cpp)
  target_link_libraries(coffeecnc_core_tests PRIVATE coffeecnc_core coffeecnc_project_options)
  coffeecnc_enable_analyzers(coffeecnc_core_tests)
  add_test(NAME core_behavior COMMAND coffeecnc_core_tests)
  # The one way a file is taken into memory: whole, bounded, read in blocks.
  add_executable(coffeecnc_text_file_tests tests/text_file_tests.cpp)
  target_link_libraries(coffeecnc_text_file_tests PRIVATE coffeecnc_core coffeecnc_project_options)
  coffeecnc_enable_analyzers(coffeecnc_text_file_tests)
  add_test(NAME text_file COMMAND coffeecnc_text_file_tests ${PROJECT_BINARY_DIR})
  # How the adapter replaces the tool table and its records: POSIX only, built from its source
  # so that it runs wherever the core does.
  add_executable(coffeecnc_durable_file_tests tests/durable_file_tests.cpp
    src/linuxcnc/src/durable_file.cpp)
  target_link_libraries(coffeecnc_durable_file_tests PRIVATE coffeecnc_core
    coffeecnc_project_options)
  coffeecnc_enable_analyzers(coffeecnc_durable_file_tests)
  add_test(NAME durable_file COMMAND coffeecnc_durable_file_tests ${PROJECT_BINARY_DIR})
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
    add_executable(coffeecnc_program_progress_tests tests/program_progress_tests.cpp)
    target_link_libraries(coffeecnc_program_progress_tests PRIVATE coffeecnc_app
      coffeecnc_project_options nlohmann_json::nlohmann_json)
    coffeecnc_enable_analyzers(coffeecnc_program_progress_tests)
    add_test(NAME program_progress COMMAND coffeecnc_program_progress_tests
      ${PROJECT_BINARY_DIR}/program_progress_tests)
    add_executable(coffeecnc_offset_history_tests tests/offset_history_tests.cpp)
    target_link_libraries(coffeecnc_offset_history_tests PRIVATE coffeecnc_app
      coffeecnc_project_options nlohmann_json::nlohmann_json)
    coffeecnc_enable_analyzers(coffeecnc_offset_history_tests)
    add_test(NAME offset_history COMMAND coffeecnc_offset_history_tests)
    if(TARGET coffeecnc_backup)
      add_executable(coffeecnc_machine_data_tests tests/machine_data_tests.cpp)
      target_link_libraries(coffeecnc_machine_data_tests PRIVATE coffeecnc_backup
        coffeecnc_project_options nlohmann_json::nlohmann_json LibArchive::LibArchive)
      coffeecnc_enable_analyzers(coffeecnc_machine_data_tests)
      add_test(NAME machine_data COMMAND coffeecnc_machine_data_tests ${PROJECT_BINARY_DIR})
    endif()
    add_executable(coffeecnc_alarm_log_tests tests/alarm_log_tests.cpp)
    target_link_libraries(coffeecnc_alarm_log_tests PRIVATE coffeecnc_app
      coffeecnc_project_options nlohmann_json::nlohmann_json)
    coffeecnc_enable_analyzers(coffeecnc_alarm_log_tests)
    add_test(NAME alarm_log COMMAND coffeecnc_alarm_log_tests ${PROJECT_BINARY_DIR})
    # The preview in the app layer: composer on a service, worker thread on an engine, both the
    # test's own. No interpreter.
    add_executable(coffeecnc_preview_tests tests/preview_tests.cpp)
    target_link_libraries(coffeecnc_preview_tests PRIVATE coffeecnc_app coffeecnc_project_options
      nlohmann_json::nlohmann_json)
    coffeecnc_enable_analyzers(coffeecnc_preview_tests)
    add_test(NAME preview_service COMMAND coffeecnc_preview_tests)
    # The text formats between the executable and the preview helper, and the text checks on
    # a program: pure code of the adapter layer, built from its sources so that it runs
    # wherever the app layer does.
    add_executable(coffeecnc_preview_wire_tests tests/preview_wire_tests.cpp
      src/linuxcnc/src/preview_wire.cpp src/linuxcnc/src/program_watch.cpp)
    target_link_libraries(coffeecnc_preview_wire_tests PRIVATE coffeecnc_core
      coffeecnc_project_options)
    coffeecnc_enable_analyzers(coffeecnc_preview_wire_tests)
    add_test(NAME preview_wire COMMAND coffeecnc_preview_wire_tests)
    add_executable(coffeecnc_program_edit_tests tests/program_edit_tests.cpp)
    target_link_libraries(coffeecnc_program_edit_tests PRIVATE coffeecnc_app
      coffeecnc_project_options)
    coffeecnc_enable_analyzers(coffeecnc_program_edit_tests)
    add_test(NAME program_edit COMMAND coffeecnc_program_edit_tests ${PROJECT_BINARY_DIR})
    add_executable(coffeecnc_program_store_tests tests/program_store_tests.cpp)
    target_link_libraries(coffeecnc_program_store_tests PRIVATE coffeecnc_app
      coffeecnc_project_options)
    coffeecnc_enable_analyzers(coffeecnc_program_store_tests)
    # The std::filesystem implementation is tested on a directory the test makes below this one.
    add_test(NAME program_store COMMAND coffeecnc_program_store_tests ${PROJECT_BINARY_DIR})
    add_executable(coffeecnc_tool_model_tests tests/tool_model_tests.cpp)
    target_link_libraries(coffeecnc_tool_model_tests PRIVATE coffeecnc_app
      coffeecnc_project_options nlohmann_json::nlohmann_json)
    coffeecnc_enable_analyzers(coffeecnc_tool_model_tests)
    add_test(NAME tool_models COMMAND coffeecnc_tool_model_tests ${PROJECT_BINARY_DIR})
    add_executable(coffeecnc_operator_settings_tests tests/operator_settings_tests.cpp)
    target_link_libraries(coffeecnc_operator_settings_tests PRIVATE coffeecnc_app
      coffeecnc_project_options nlohmann_json::nlohmann_json)
    coffeecnc_enable_analyzers(coffeecnc_operator_settings_tests)
    add_test(NAME operator_settings COMMAND coffeecnc_operator_settings_tests)
    add_executable(coffeecnc_session_tests tests/session_tests.cpp)
    target_link_libraries(coffeecnc_session_tests PRIVATE coffeecnc_app coffeecnc_project_options
      nlohmann_json::nlohmann_json)
    coffeecnc_enable_analyzers(coffeecnc_session_tests)
    add_test(NAME session_lifecycle COMMAND coffeecnc_session_tests)
    # The diagnostics view (#251): its wire text and the feed that reads only while watched.
    add_executable(coffeecnc_diagnostics_tests tests/diagnostics_tests.cpp)
    target_link_libraries(coffeecnc_diagnostics_tests PRIVATE coffeecnc_app
      coffeecnc_project_options nlohmann_json::nlohmann_json)
    coffeecnc_enable_analyzers(coffeecnc_diagnostics_tests)
    add_test(NAME diagnostics_feed COMMAND coffeecnc_diagnostics_tests)
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
        ${PROJECT_SOURCE_DIR}/tests/sim/run.sh ${PROJECT_BINARY_DIR}/machine_config
        ${PROJECT_SOURCE_DIR}/share/coffeecnc.ini.example)
      # LinuxCNC's status as the panel's messages, on constructed readings. No LinuxCNC runs.
      add_executable(coffeecnc_panel_mapping_tests tests/panel_mapping_tests.cpp)
      target_link_libraries(coffeecnc_panel_mapping_tests PRIVATE coffeecnc_linuxcnc
        coffeecnc_project_options)
      target_include_directories(coffeecnc_panel_mapping_tests PRIVATE src/linuxcnc/src)
      target_include_directories(coffeecnc_panel_mapping_tests SYSTEM PRIVATE
        ${COFFEECNC_LINUXCNC_INCLUDE_DIR} ${COFFEECNC_BOOST_INCLUDE_DIR})
      coffeecnc_enable_analyzers(coffeecnc_panel_mapping_tests)
      add_test(NAME panel_mapping COMMAND coffeecnc_panel_mapping_tests ${PROJECT_BINARY_DIR})
      # The tool change handshake between the control thread and the others. No HAL runs.
      add_executable(coffeecnc_hal_control_tests tests/hal_control_tests.cpp)
      target_link_libraries(coffeecnc_hal_control_tests PRIVATE coffeecnc_linuxcnc
        coffeecnc_project_options)
      coffeecnc_enable_analyzers(coffeecnc_hal_control_tests)
      add_test(NAME hal_control COMMAND coffeecnc_hal_control_tests)
      # The diagnostics view's HAL readings on a lookup of the test's own, and the HAL tool
      # launcher on programs the test writes. No HAL runs.
      add_executable(coffeecnc_hal_diagnostics_tests tests/hal_diagnostics_tests.cpp)
      target_link_libraries(coffeecnc_hal_diagnostics_tests PRIVATE coffeecnc_linuxcnc
        coffeecnc_project_options)
      coffeecnc_enable_analyzers(coffeecnc_hal_diagnostics_tests)
      add_test(NAME hal_diagnostics COMMAND coffeecnc_hal_diagnostics_tests
        ${PROJECT_BINARY_DIR})
      if(TARGET coffeecnc_preview)
        # The preview engine with the real helper and LinuxCNC's interpreter on programs the
        # test writes. No LinuxCNC runs.
        add_executable(coffeecnc_preview_engine_tests tests/preview_engine_tests.cpp)
        target_link_libraries(coffeecnc_preview_engine_tests PRIVATE coffeecnc_linuxcnc
          coffeecnc_project_options)
        target_compile_definitions(coffeecnc_preview_engine_tests PRIVATE
          COFFEECNC_SAMPLES_DIRECTORY="${PROJECT_SOURCE_DIR}/../samples"
          COFFEECNC_CAM_SAMPLES_DIRECTORY="${PROJECT_SOURCE_DIR}/tests/cam")
        coffeecnc_enable_analyzers(coffeecnc_preview_engine_tests)
        add_test(NAME preview_engine COMMAND coffeecnc_preview_engine_tests
          $<TARGET_FILE:coffeecnc_preview> ${PROJECT_BINARY_DIR})
        set_tests_properties(preview_engine PROPERTIES TIMEOUT 300)
      endif()
      # The real Session on NmlPort against a running LinuxCNC on a simulated mill. Exists only
      # where the LinuxCNC adapter builds; `ctest -L linuxcnc` selects it, `-LE linuxcnc` skips it.
      add_executable(coffeecnc_sim_tests tests/sim/sim_tests.cpp tests/sim/hal_probe.cpp)
      # The fixture reads the source INI and owns a HAL component.
      target_include_directories(coffeecnc_sim_tests SYSTEM PRIVATE
        ${COFFEECNC_LINUXCNC_INCLUDE_DIR} ${COFFEECNC_BOOST_INCLUDE_DIR})
      target_compile_definitions(coffeecnc_sim_tests PRIVATE ULAPI
        COFFEECNC_SAMPLES_DIRECTORY="${PROJECT_SOURCE_DIR}/../samples"
        COFFEECNC_CAM_SAMPLES_DIRECTORY="${PROJECT_SOURCE_DIR}/tests/cam"
        COFFEECNC_PREVIEW_HELPER_BUILT="$<$<TARGET_EXISTS:coffeecnc_preview>:$<TARGET_FILE:coffeecnc_preview>>")
      if(TARGET coffeecnc_preview)
        add_dependencies(coffeecnc_sim_tests coffeecnc_preview)
      endif()
      target_link_libraries(coffeecnc_sim_tests PRIVATE coffeecnc_app coffeecnc_linuxcnc
        coffeecnc_project_options nlohmann_json::nlohmann_json ${COFFEECNC_LINUXCNC_HAL}
        ${COFFEECNC_LINUXCNC_INI})
      coffeecnc_enable_analyzers(coffeecnc_sim_tests)
      add_executable(coffeecnc_fixed_setter_tests tests/sim/fixed_setter_tests.cpp)
      target_link_libraries(coffeecnc_fixed_setter_tests PRIVATE
        coffeecnc_app coffeecnc_linuxcnc coffeecnc_project_options nlohmann_json::nlohmann_json)
      if(TARGET coffeecnc_preview)
        target_compile_definitions(coffeecnc_fixed_setter_tests PRIVATE
          COFFEECNC_PREVIEW_HELPER_BUILT="$<TARGET_FILE:coffeecnc_preview>")
        add_dependencies(coffeecnc_fixed_setter_tests coffeecnc_preview)
      endif()
      # The TIMEOUT of every simulation test includes 300 s it may wait for the host's
      # simulation lock (run.sh -w): another checkout may be running one.
      foreach(units mm inch)
        # Every simulation test has a run directory of its own, named after the test: its
        # linuxcnc.log there is that test's and no other's.
        add_test(NAME linuxcnc_sim_${units} COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
          -u ${units} -t 300 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_sim_${units}
          $<TARGET_FILE:coffeecnc_sim_tests>)
        # One LinuxCNC per host at a time.
        set_tests_properties(linuxcnc_sim_${units} PROPERTIES LABELS linuxcnc RUN_SERIAL TRUE
          RESOURCE_LOCK linuxcnc TIMEOUT 700)
        add_test(NAME linuxcnc_velocity_limit_${units} COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
          -u ${units} -t 65 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_velocity_limit_${units}
          $<TARGET_FILE:coffeecnc_sim_tests> velocity-limit)
        set_tests_properties(linuxcnc_velocity_limit_${units} PROPERTIES LABELS linuxcnc
          RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 395)
        add_test(NAME linuxcnc_tool_probe_${units} COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
          -u ${units} -t 45 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_tool_probe_${units}
          ${PROJECT_SOURCE_DIR}/tests/sim/tool_probe_facts.py)
        set_tests_properties(linuxcnc_tool_probe_${units} PROPERTIES LABELS linuxcnc RUN_SERIAL TRUE
          RESOURCE_LOCK linuxcnc TIMEOUT 375)
        add_test(NAME linuxcnc_fixed_setter_${units} COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
          -u ${units} -t 140 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_fixed_setter_${units}
          $<TARGET_FILE:coffeecnc_fixed_setter_tests>)
        # Include the consecutive tolerance checks at the configured probe speeds.
        set_tests_properties(linuxcnc_fixed_setter_${units} PROPERTIES LABELS linuxcnc RUN_SERIAL TRUE
          RESOURCE_LOCK linuxcnc TIMEOUT 470)
        if(TARGET coffeecnc_preview)
          set(change_position)
          if(units STREQUAL "inch")
            set(change_position -c)
          endif()
          add_test(NAME linuxcnc_m6_measure_${units} COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
            -u ${units} ${change_position} -t 130 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_m6_measure_${units}
            $<TARGET_FILE:coffeecnc_fixed_setter_tests> measure-m6)
          set_tests_properties(linuxcnc_m6_measure_${units} PROPERTIES LABELS linuxcnc RUN_SERIAL TRUE
            RESOURCE_LOCK linuxcnc TIMEOUT 460)
        endif()
        add_test(NAME linuxcnc_material_probe_${units} COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
          -u ${units} -t 420 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_material_probe_${units}
          $<TARGET_FILE:coffeecnc_fixed_setter_tests> material)
        # The corner, ball calibration, boss and bore each run several probe sequences.
        set_tests_properties(linuxcnc_material_probe_${units} PROPERTIES LABELS linuxcnc
          RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 840)
        add_test(NAME linuxcnc_feature_probe_${units} COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
          -u ${units} -t 280 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_feature_probe_${units}
          $<TARGET_FILE:coffeecnc_fixed_setter_tests> features)
        set_tests_properties(linuxcnc_feature_probe_${units} PROPERTIES LABELS linuxcnc
          RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 610)
        # Every feature cycle and the edge angle in a rotated G55: +7.5° in mm, -30° in inch.
        add_test(NAME linuxcnc_rotated_probe_${units} COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
          -u ${units} -t 480 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_rotated_probe_${units}
          $<TARGET_FILE:coffeecnc_fixed_setter_tests> rotated)
        set_tests_properties(linuxcnc_rotated_probe_${units} PROPERTIES LABELS linuxcnc
          RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 810)
        # Stylus runout (#247), the ring part: calibration, corrected bores (also rotated +7.5° in mm,
        # -30° in inch) and ring features, refusals, an abort.
        add_test(NAME linuxcnc_probe_runout_${units} COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
          -u ${units} -t 480 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_probe_runout_${units}
          $<TARGET_FILE:coffeecnc_fixed_setter_tests> runout)
        set_tests_properties(linuxcnc_probe_runout_${units} PROPERTIES LABELS linuxcnc
          RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 810)
        # The pin part of it: pin features, measure-only, the search travel, a miss, ball, Clear.
        add_test(NAME linuxcnc_probe_runout_pin_${units} COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
          -u ${units} -t 480 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_probe_runout_pin_${units}
          $<TARGET_FILE:coffeecnc_fixed_setter_tests> runout-pin)
        set_tests_properties(linuxcnc_probe_runout_pin_${units} PROPERTIES LABELS linuxcnc
          RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 810)
        # The three-point bore and boss with a known correction, also in G56 rotated.
        add_test(NAME linuxcnc_probe_runout_arc_${units} COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
          -u ${units} -t 480 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_probe_runout_arc_${units}
          $<TARGET_FILE:coffeecnc_fixed_setter_tests> runout-arc)
        set_tests_properties(linuxcnc_probe_runout_arc_${units} PROPERTIES LABELS linuxcnc
          RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 810)
        # Three-point bore and boss (#247): unrotated in mm, then +7.5° in mm and -30° in inch.
        if(units STREQUAL "mm")
          add_test(NAME linuxcnc_arc_probe_mm COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
            -u mm -t 300 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_arc_probe_mm
            $<TARGET_FILE:coffeecnc_fixed_setter_tests> arc)
          set_tests_properties(linuxcnc_arc_probe_mm PROPERTIES LABELS linuxcnc
            RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 630)
          # Misses, early trips, refused geometry and a stop mid-cycle, at +7.5°.
          add_test(NAME linuxcnc_arc_probe_faults_mm COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
            -u mm -t 300 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_arc_probe_faults_mm
            $<TARGET_FILE:coffeecnc_fixed_setter_tests> arc-faults)
          set_tests_properties(linuxcnc_arc_probe_faults_mm PROPERTIES LABELS linuxcnc
            RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 630)
        endif()
        add_test(NAME linuxcnc_arc_probe_rotated_${units} COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
          -u ${units} -t 300 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_arc_probe_rotated_${units}
          $<TARGET_FILE:coffeecnc_fixed_setter_tests> arc-rotated)
        set_tests_properties(linuxcnc_arc_probe_rotated_${units} PROPERTIES LABELS linuxcnc
          RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 630)
        # Every feature cycle measure-only, then applied (#247): +7.5° in mm, -30° in inch.
        add_test(NAME linuxcnc_probe_measure_${units} COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
          -u ${units} -t 480 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_probe_measure_${units}
          $<TARGET_FILE:coffeecnc_fixed_setter_tests> measure)
        set_tests_properties(linuxcnc_probe_measure_${units} PROPERTIES LABELS linuxcnc
          RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 810)
        add_test(NAME linuxcnc_jog_contact_${units} COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
          -u ${units} -t 25 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_jog_contact_${units}
          $<TARGET_FILE:coffeecnc_sim_tests> jog-contact)
        set_tests_properties(linuxcnc_jog_contact_${units} PROPERTIES LABELS linuxcnc
          RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 355)
        add_test(NAME linuxcnc_offset_history_${units} COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
          -u ${units} -t 120 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_offset_history_${units}
          $<TARGET_FILE:coffeecnc_sim_tests> offset-history)
        set_tests_properties(linuxcnc_offset_history_${units} PROPERTIES LABELS linuxcnc
          RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 450)
        # The inactive work systems from the parameter file, and when they are current.
        add_test(NAME linuxcnc_work_offsets_${units} COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
          -u ${units} -t 120 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_work_offsets_${units}
          $<TARGET_FILE:coffeecnc_sim_tests> work-offsets)
        set_tests_properties(linuxcnc_work_offsets_${units} PROPERTIES LABELS linuxcnc
          RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 450)
        # When LinuxCNC writes the parameter file: the facts the reading above relies on.
        add_test(NAME linuxcnc_parameter_file_${units} COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
          -u ${units} -t 90 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_parameter_file_${units}
          ${PROJECT_SOURCE_DIR}/tests/sim/work_offset_facts.py)
        set_tests_properties(linuxcnc_parameter_file_${units} PROPERTIES LABELS linuxcnc
          RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 420)
        # The offset editor's XY rotation and G92/G52 clear, with history and refusals.
        add_test(NAME linuxcnc_offset_edits_${units} COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
          -u ${units} -t 120 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_offset_edits_${units}
          $<TARGET_FILE:coffeecnc_sim_tests> offset-edits)
        set_tests_properties(linuxcnc_offset_edits_${units} PROPERTIES LABELS linuxcnc
          RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 450)
        # Go to and power feed: G53 blocks under tool, work and G92 offsets end where saved.
        add_test(NAME linuxcnc_machine_moves_${units} COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
          -u ${units} -t 150 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_machine_moves_${units}
          $<TARGET_FILE:coffeecnc_sim_tests> machine-moves)
        set_tests_properties(linuxcnc_machine_moves_${units} PROPERTIES LABELS linuxcnc
          RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 480)
        add_test(NAME linuxcnc_restore_spindle_tool_${units}
          COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
          -u ${units} -t 70 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_restore_spindle_tool_${units}
          $<TARGET_FILE:coffeecnc_sim_tests> restore-spindle-tool)
        set_tests_properties(linuxcnc_restore_spindle_tool_${units} PROPERTIES LABELS linuxcnc
          RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 400)
      endforeach()
      add_test(NAME linuxcnc_probe_tool_interlock COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
        -u mm -t 60 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_probe_tool_interlock
        $<TARGET_FILE:coffeecnc_sim_tests> probe-interlock)
      set_tests_properties(linuxcnc_probe_tool_interlock PROPERTIES LABELS linuxcnc
        RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 390)
      # [COFFEECNC] MACRO: the INI keys, hello, and each macro's block run as MDI (#211).
      add_test(NAME linuxcnc_macros_mm COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
        -u mm -t 60 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_macros_mm
        $<TARGET_FILE:coffeecnc_sim_tests> macros)
      set_tests_properties(linuxcnc_macros_mm PROPERTIES LABELS linuxcnc
        RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 390)
      # A restore staged while LinuxCNC runs, written by the helper after LinuxCNC has stopped
      # and written its .var file, and reported by the next start (#252).
      if(TARGET coffeecnc_backup AND TARGET coffeecnc)
        add_executable(coffeecnc_restore_sim_tests tests/sim/restore_sim_tests.cpp)
        target_link_libraries(coffeecnc_restore_sim_tests PRIVATE coffeecnc_backup
          coffeecnc_linuxcnc coffeecnc_project_options nlohmann_json::nlohmann_json)
        coffeecnc_enable_analyzers(coffeecnc_restore_sim_tests)
        add_test(NAME linuxcnc_restore COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/restore_tests.sh
          ${PROJECT_BINARY_DIR}/sim $<TARGET_FILE:coffeecnc_restore_sim_tests>
          $<TARGET_FILE:coffeecnc>)
        set_tests_properties(linuxcnc_restore PROPERTIES LABELS linuxcnc
          RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 520)
      endif()
      # The same on a mill with the G0759's INI values: unhomed MDI and runs, per-axis jog
      # ceilings, optional signals.
      add_test(NAME linuxcnc_edge_rotation_g0759 COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
        -p g0759 -t 110 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_edge_rotation_g0759
        $<TARGET_FILE:coffeecnc_fixed_setter_tests> edge-rotation)
      add_test(NAME linuxcnc_edge_rotation_inch COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
        -u inch -t 110 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_edge_rotation_inch
        $<TARGET_FILE:coffeecnc_fixed_setter_tests> edge-rotation)
      set_tests_properties(linuxcnc_edge_rotation_g0759 linuxcnc_edge_rotation_inch PROPERTIES
        LABELS linuxcnc RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 450)
      # The HAL diagnostics view (#251): limit, home, probe and E-stop rows follow halcmd.
      add_test(NAME linuxcnc_diagnostics_g0759 COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
        -p g0759 -t 60 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_diagnostics_g0759
        $<TARGET_FILE:coffeecnc_sim_tests> g0759 diagnostics)
      set_tests_properties(linuxcnc_diagnostics_g0759 PROPERTIES LABELS linuxcnc
        RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 390)
      add_test(NAME linuxcnc_sim_g0759 COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
        -p g0759 -t 300 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_sim_g0759
        $<TARGET_FILE:coffeecnc_sim_tests> g0759)
      set_tests_properties(linuxcnc_sim_g0759 PROPERTIES LABELS linuxcnc
        RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 700)
      foreach(units mm inch)
        add_test(NAME linuxcnc_sim_rotary_${units} COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
          -p rotary -u ${units} -t 50 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_sim_rotary_${units}
          $<TARGET_FILE:coffeecnc_sim_tests> rotary)
        set_tests_properties(linuxcnc_sim_rotary_${units} PROPERTIES LABELS linuxcnc
          RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 380)
      endforeach()
      add_test(NAME linuxcnc_offset_history_rotary COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
        -p rotary -t 120 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_offset_history_rotary
        $<TARGET_FILE:coffeecnc_sim_tests> offset-history)
      set_tests_properties(linuxcnc_offset_history_rotary PROPERTIES LABELS linuxcnc
        RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 450)
      # G92.1 from the offset editor is refused where it would clear an A, B or C shift.
      add_test(NAME linuxcnc_offset_edits_rotary COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
        -p rotary -t 120 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_offset_edits_rotary
        $<TARGET_FILE:coffeecnc_sim_tests> offset-edits)
      set_tests_properties(linuxcnc_offset_edits_rotary PROPERTIES LABELS linuxcnc
        RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 450)
      # A gantry router: two motors on Y (COORDINATES X Y Y Z).
      add_test(NAME linuxcnc_sim_gantry COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
        -p gantry -t 120 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_sim_gantry
        $<TARGET_FILE:coffeecnc_sim_tests> gantry)
      set_tests_properties(linuxcnc_sim_gantry PROPERTIES LABELS linuxcnc
        RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 420)
      if(TARGET coffeecnc_preview)
        add_test(NAME linuxcnc_whole_job_g0759 COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
          -p g0759 -t 310 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_whole_job_g0759
          $<TARGET_FILE:coffeecnc_sim_tests> g0759 whole-job)
        set_tests_properties(linuxcnc_whole_job_g0759 PROPERTIES LABELS linuxcnc
          RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 655)
        add_test(NAME linuxcnc_air_cuts_g0759 COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
          -p g0759 -t 420 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_air_cuts_g0759
          $<TARGET_FILE:coffeecnc_sim_tests> g0759 air-cuts)
        set_tests_properties(linuxcnc_air_cuts_g0759 PROPERTIES LABELS linuxcnc
          RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 750)
        add_test(NAME linuxcnc_restart_cuts_g0759 COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
          -p g0759 -t 220 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_restart_cuts_g0759
          $<TARGET_FILE:coffeecnc_sim_tests> g0759 restart-cuts)
        set_tests_properties(linuxcnc_restart_cuts_g0759 PROPERTIES LABELS linuxcnc
          RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 550)
        add_test(NAME linuxcnc_program_preflight_g0759 COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
          -p g0759 -t 55 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_program_preflight_g0759
          $<TARGET_FILE:coffeecnc_sim_tests> g0759 program-preflight)
        set_tests_properties(linuxcnc_program_preflight_g0759 PROPERTIES LABELS linuxcnc
          RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 385)
        add_test(NAME linuxcnc_paused_spindle_g0759 COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
          -p g0759 -t 105 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_paused_spindle_g0759
          $<TARGET_FILE:coffeecnc_sim_tests> g0759 paused-spindle)
        set_tests_properties(linuxcnc_paused_spindle_g0759 PROPERTIES LABELS linuxcnc
          RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 440)
        add_test(NAME linuxcnc_paused_z_g0759 COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
          -p g0759 -t 70 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_paused_z_g0759
          $<TARGET_FILE:coffeecnc_sim_tests> g0759 paused-z)
        set_tests_properties(linuxcnc_paused_z_g0759 PROPERTIES LABELS linuxcnc
          RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 400)
        add_test(NAME linuxcnc_program_breakpoint_g0759 COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
          -p g0759 -t 70 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_program_breakpoint_g0759
          $<TARGET_FILE:coffeecnc_sim_tests> g0759 program-breakpoint)
        set_tests_properties(linuxcnc_program_breakpoint_g0759 PROPERTIES LABELS linuxcnc
          RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 400)
        add_test(NAME linuxcnc_cam_programs_g0759 COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
          -p g0759 -t 240 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_cam_programs_g0759
          $<TARGET_FILE:coffeecnc_sim_tests> g0759 cam-programs)
        set_tests_properties(linuxcnc_cam_programs_g0759 PROPERTIES LABELS linuxcnc
          RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 570)
      endif()
      add_test(NAME linuxcnc_restore_spindle_tool_g0759 COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
        -p g0759 -t 30 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_restore_spindle_tool_g0759
        $<TARGET_FILE:coffeecnc_sim_tests> g0759 restore-spindle-tool)
      set_tests_properties(linuxcnc_restore_spindle_tool_g0759 PROPERTIES LABELS linuxcnc
        RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 360)
      # The runner's own test calls run.sh many times. Eleven of its runs may each wait for a
      # simulation of another checkout; two wait on the test's own run for fixed times. The
      # time limit is made of those parts, and the script is handed the same lock wait.
      set(runner_lock_wait 300)   # run.sh -w of each of the eleven runs
      set(runner_waiting_runs 11)
      set(runner_own_waits 123)   # the refused run (3 s) and the run that waits its turn (120 s)
      set(runner_run_time 300)    # everything else the test does; about 160 s measured
      math(EXPR runner_timeout
        "${runner_waiting_runs} * ${runner_lock_wait} + ${runner_own_waits} + ${runner_run_time}")
      add_test(NAME linuxcnc_sim_runner COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/runner_tests.sh
        ${PROJECT_BINARY_DIR} ${runner_lock_wait})
      set_tests_properties(linuxcnc_sim_runner PROPERTIES LABELS linuxcnc
        RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT ${runner_timeout})
      # run.sh -m refuses a machine INI file that keeps its data outside the run directory.
      # Nothing is started: no label, no lock.
      add_test(NAME sim_data_directory COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/data_directory_tests.sh
        ${PROJECT_BINARY_DIR})
      set_tests_properties(sim_data_directory PROPERTIES TIMEOUT 60)
      # The G0759 scenario on the machine's own INI and HAL files, hardware simulated (run.sh
      # -m). The files are in a checkout of the machine configurations (tinic/linuxcnc), which
      # this repository does not carry: without one the tests are skipped.
      set(COFFEECNC_MACHINES_DIRECTORY "" CACHE PATH
        "Checkout of the machine configurations for the tests on a machine's own files; the environment variable of the same name at ctest time when empty")
      set(machine_g0759 ${PROJECT_SOURCE_DIR}/tests/sim/with_machine.sh
        machines=${COFFEECNC_MACHINES_DIRECTORY} configs/G0759/G0759-coffeecnc.ini
        ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
        -s ${PROJECT_SOURCE_DIR}/tests/sim/coffeecnc_g0759_machine_hardware.hal)
      add_test(NAME linuxcnc_machine_g0759 COMMAND ${machine_g0759}
        -t 300 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_machine_g0759
        $<TARGET_FILE:coffeecnc_sim_tests> g0759-machine)
      set_tests_properties(linuxcnc_machine_g0759 PROPERTIES LABELS linuxcnc RUN_SERIAL TRUE
        RESOURCE_LOCK linuxcnc TIMEOUT 700 SKIP_RETURN_CODE 77)
      if(TARGET coffeecnc)
        add_executable(coffeecnc_remote_server_tests tests/remote_server_tests.cpp)
        # The Beast test instantiates the same Asio code as remote_server.cpp.
        set_property(SOURCE tests/remote_server_tests.cpp APPEND PROPERTY COMPILE_OPTIONS
          "$<$<CXX_COMPILER_ID:GNU>:-Wno-null-dereference>")
        target_include_directories(coffeecnc_remote_server_tests SYSTEM PRIVATE ${COFFEECNC_BOOST_INCLUDE_DIR})
        target_link_libraries(coffeecnc_remote_server_tests PRIVATE coffeecnc_application
          coffeecnc_project_options nlohmann_json::nlohmann_json)
        target_compile_options(coffeecnc_remote_server_tests PRIVATE ${COFFEECNC_ASIO_OPTIONS})
        coffeecnc_enable_analyzers(coffeecnc_remote_server_tests)
        add_test(NAME remote_server COMMAND coffeecnc_remote_server_tests ${PROJECT_BINARY_DIR})
        set_tests_properties(remote_server PROPERTIES TIMEOUT 20)
        add_test(NAME remote_server_dns COMMAND sh ${PROJECT_SOURCE_DIR}/tests/remote_server_dns.sh
          $<TARGET_FILE:coffeecnc_remote_server_tests> ${PROJECT_BINARY_DIR})
        set_tests_properties(remote_server_dns PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)

        set(paths_kind $<IF:$<BOOL:${COFFEECNC_DEVELOPMENT_PATHS}>,development,installed-only>)
        # The executable's command line and its start-up refusals. No LinuxCNC and no display.
        add_test(NAME coffeecnc_cli COMMAND ${PROJECT_SOURCE_DIR}/tests/shell/cli_tests.sh
          $<TARGET_FILE:coffeecnc> ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
          ${PROJECT_BINARY_DIR}/cli ${paths_kind})
        # The Xvfb wrapper of the tests below: each failure it prints is a nonzero exit code.
        add_test(NAME xvfb_wrapper COMMAND ${PROJECT_SOURCE_DIR}/tests/shell/wrapper_tests.sh
          ${PROJECT_BINARY_DIR})
        set_tests_properties(xvfb_wrapper PROPERTIES TIMEOUT 120)
        # The panel the tests below run: ui/dist, built from the ui/ sources of this checkout
        # and stamped with their hashes (ui/build-stamp.ts). The tests need it, and each checks
        # the stamp again itself before it does anything.
        add_test(NAME panel_bundle COMMAND ${PROJECT_SOURCE_DIR}/tests/shell/build_panel.sh
          ${PROJECT_SOURCE_DIR}/..)
        set_tests_properties(panel_bundle PROPERTIES FIXTURES_SETUP panel TIMEOUT 600)
        # The CLI's development-path check also needs the checkout's built panel.
        set_tests_properties(coffeecnc_cli PROPERTIES FIXTURES_REQUIRED panel)
        # The stamp checker refuses a changed source, a changed bundle and an unbuilt source.
        add_test(NAME panel_stamp COMMAND ${PROJECT_SOURCE_DIR}/tests/shell/panel_stamp_tests.sh
          ${PROJECT_SOURCE_DIR}/.. ${PROJECT_BINARY_DIR})
        set_tests_properties(panel_stamp PROPERTIES FIXTURES_REQUIRED panel)
        # The application with the real panel in its window, as LinuxCNC's DISPLAY program on a
        # display of its own (Xvfb): the test drives the page and watches LinuxCNC.
        add_executable(coffeecnc_shell_tests tests/shell/shell_tests.cpp
          ${COFFEECNC_SANITIZER_SUPPRESSIONS})
        target_compile_definitions(coffeecnc_shell_tests PRIVATE
          COFFEECNC_SAMPLES_DIRECTORY="${PROJECT_SOURCE_DIR}/../samples"
          COFFEECNC_SOURCE_UI_DIRECTORY="${PROJECT_SOURCE_DIR}/../ui/dist"
          COFFEECNC_PANEL_STAMP_CHECK="node ${PROJECT_SOURCE_DIR}/tests/sim/check-panel-stamp.mjs ${PROJECT_SOURCE_DIR}/.."
          COFFEECNC_EVIDENCE_DIRECTORY="${PROJECT_SOURCE_DIR}/build/shell-evidence")
        target_link_libraries(coffeecnc_shell_tests PRIVATE coffeecnc_application
          coffeecnc_shell_test_hooks coffeecnc_project_options nlohmann_json::nlohmann_json)
        coffeecnc_enable_analyzers(coffeecnc_shell_tests)
        # Under ThreadSanitizer the window's programs ignore what GLib, GTK and WebKitGTK do
        # in the runtime's interceptors (src/shell/sanitizer_suppressions.cpp): those libraries
        # are not instrumented. Every access of CoffeeCNC's code is checked.
        # `cmake --install` into a prefix below the build directory: the layout, the one HAL
        # fragment, nothing else installed.
        set(installed ${PROJECT_BINARY_DIR}/installed)
        add_test(NAME install_tree COMMAND ${PROJECT_SOURCE_DIR}/tests/shell/install_tree.sh
          ${PROJECT_BINARY_DIR} ${installed}/prefix ${PROJECT_SOURCE_DIR} ${paths_kind})
        set_tests_properties(install_tree PROPERTIES FIXTURES_REQUIRED panel
          FIXTURES_SETUP installed_prefix TIMEOUT 300)
        set(installed_programs prefix:${installed}/prefix/bin/coffeecnc)
        if(CPACK_GENERATOR)
          # The Debian package: control fields, dependencies, contents; unpacked with
          # dpkg-deb -x below the build directory, never installed.
          add_test(NAME package_deb COMMAND ${PROJECT_SOURCE_DIR}/tests/shell/package_deb.sh
            ${PROJECT_BINARY_DIR} ${installed}/package ${PROJECT_SOURCE_DIR} ${PROJECT_VERSION}
            ${paths_kind})
          set_tests_properties(package_deb PROPERTIES FIXTURES_REQUIRED panel
            FIXTURES_SETUP installed_package TIMEOUT 600)
          list(APPEND installed_programs package:${installed}/package/root/usr/bin/coffeecnc)
        endif()
        # The build tree's GUI tests need the fallbacks: the test program is not installed.
        if(COFFEECNC_DEVELOPMENT_PATHS)
          add_executable(coffeecnc_quit_application_tests tests/shell/quit_application_tests.cpp
            ${COFFEECNC_SANITIZER_SUPPRESSIONS})
          target_include_directories(coffeecnc_quit_application_tests PRIVATE src/coffeecnc src/shell/src)
          target_include_directories(coffeecnc_quit_application_tests SYSTEM PRIVATE
            $<TARGET_PROPERTY:coffeecnc_shell,INCLUDE_DIRECTORIES>)
          target_compile_definitions(coffeecnc_quit_application_tests PRIVATE
            COFFEECNC_SOURCE_UI_DIRECTORY="${PROJECT_SOURCE_DIR}/../ui/dist")
          target_link_libraries(coffeecnc_quit_application_tests PRIVATE coffeecnc_application
            coffeecnc_project_options)
          add_test(NAME linuxcnc_quit_application COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
            -u mm -t 40 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_quit_application
            ${PROJECT_SOURCE_DIR}/tests/shell/under_xvfb.sh
            $<TARGET_FILE:coffeecnc_quit_application_tests>)
          set_tests_properties(linuxcnc_quit_application PROPERTIES LABELS linuxcnc RUN_SERIAL TRUE
            RESOURCE_LOCK linuxcnc TIMEOUT 400 FIXTURES_REQUIRED panel)
          foreach(coolant_mode wired fail-pin fail-signal)
            add_test(NAME linuxcnc_coolant_${coolant_mode} COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
              -u mm -t 40 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_coolant_${coolant_mode}
              ${PROJECT_SOURCE_DIR}/tests/shell/under_xvfb.sh
              $<TARGET_FILE:coffeecnc_quit_application_tests> --coolant=${coolant_mode})
            set_tests_properties(linuxcnc_coolant_${coolant_mode} PROPERTIES LABELS linuxcnc RUN_SERIAL TRUE
              RESOURCE_LOCK linuxcnc TIMEOUT 400 FIXTURES_REQUIRED panel)
          endforeach()
          foreach(change_mode external manual conflict)
            add_test(NAME linuxcnc_tool_change_${change_mode} COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
              -u mm -t 40 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_tool_change_${change_mode}
              ${PROJECT_SOURCE_DIR}/tests/shell/under_xvfb.sh
              $<TARGET_FILE:coffeecnc_quit_application_tests> --tool-change=${change_mode})
            set_tests_properties(linuxcnc_tool_change_${change_mode} PROPERTIES LABELS linuxcnc RUN_SERIAL TRUE
              RESOURCE_LOCK linuxcnc TIMEOUT 400 FIXTURES_REQUIRED panel)
          endforeach()
          foreach(units mm inch)
            add_test(NAME linuxcnc_shell_${units} COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
              -u ${units} -t 500 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_shell_${units}
              ${PROJECT_SOURCE_DIR}/tests/shell/under_xvfb.sh
              $<TARGET_FILE:coffeecnc_shell_tests>)
            set_tests_properties(linuxcnc_shell_${units} PROPERTIES LABELS linuxcnc RUN_SERIAL TRUE
              RESOURCE_LOCK linuxcnc TIMEOUT 900 FIXTURES_REQUIRED panel)
          endforeach()
          # The tool models through the real editor, on a sim machine that is given an identity
          # (run.sh -i): a defaults save and an assignment save reach the disk; LinuxCNC and the
          # application are then started again in the same run directory (run.sh -k), and the
          # panel shows what was saved.
          # Two runner calls, each of which may wait for the host's lock, run and clean up: the
          # time limit is made of those parts, and the script is handed the same numbers.
          set(tool_models_lock_wait 300) # run.sh -w, per call
          set(tool_models_run_time 200)  # run.sh -t, per call
          set(tool_models_cleanup 100)   # run.sh ending LinuxCNC, per call
          math(EXPR tool_models_timeout
            "2 * (${tool_models_lock_wait} + ${tool_models_run_time} + ${tool_models_cleanup})")
          add_test(NAME linuxcnc_shell_tool_models
            COMMAND ${PROJECT_SOURCE_DIR}/tests/shell/tool_models_restart.sh
            ${PROJECT_SOURCE_DIR}/tests/sim/run.sh ${PROJECT_SOURCE_DIR}/tests/shell/under_xvfb.sh
            $<TARGET_FILE:coffeecnc_shell_tests>
            ${PROJECT_BINARY_DIR}/sim/linuxcnc_shell_tool_models
            ${tool_models_lock_wait} ${tool_models_run_time})
          set_tests_properties(linuxcnc_shell_tool_models PROPERTIES LABELS linuxcnc
            RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT ${tool_models_timeout}
            FIXTURES_REQUIRED panel)
          # The `coffeecnc` executable itself as the DISPLAY program: it starts, shows the panel,
          # and SIGTERM ends it and LinuxCNC with no process left.
          add_test(NAME linuxcnc_coffeecnc_start COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
            -t 120 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_coffeecnc_start
            ${PROJECT_SOURCE_DIR}/tests/shell/under_xvfb.sh --terminate-after=12
            $<TARGET_FILE:coffeecnc>)
          set_tests_properties(linuxcnc_coffeecnc_start PROPERTIES LABELS linuxcnc RUN_SERIAL TRUE
            RESOURCE_LOCK linuxcnc TIMEOUT 500 FIXTURES_REQUIRED panel)
          # The same on the G0759's own INI and HAL files, and a program's preview with the
          # machine's [RS274NGC] and tool table becomes ready.
          add_test(NAME linuxcnc_machine_g0759_start COMMAND ${machine_g0759}
            -t 120 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_machine_g0759_start
            ${PROJECT_SOURCE_DIR}/tests/shell/under_xvfb.sh --terminate-after=6
            --check=${PROJECT_SOURCE_DIR}/tests/shell/running_checks.sh $<TARGET_FILE:coffeecnc>)
          set_tests_properties(linuxcnc_machine_g0759_start PROPERTIES LABELS linuxcnc
            RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 500 FIXTURES_REQUIRED panel
            SKIP_RETURN_CODE 77)
        endif()
        # The INSTALLED executable as the DISPLAY program, from the prefix and from the
        # unpacked package: no --ui, no COFFEECNC_* setting. It names the installed panel and
        # the installed helper, a program's preview becomes ready, and SIGTERM ends it and
        # LinuxCNC with no process left (run.sh).
        foreach(entry IN LISTS installed_programs)
          string(REGEX MATCH "^[a-z]+" tree ${entry})
          string(REGEX REPLACE "^[a-z]+:" "" program ${entry})
          add_test(NAME linuxcnc_installed_${tree}_start
            COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
            -t 120 -d ${PROJECT_BINARY_DIR}/sim/linuxcnc_installed_${tree}_start
            ${PROJECT_SOURCE_DIR}/tests/shell/under_xvfb.sh --terminate-after=6
            --check=${PROJECT_SOURCE_DIR}/tests/shell/running_checks.sh,installed ${program})
          set_tests_properties(linuxcnc_installed_${tree}_start PROPERTIES LABELS linuxcnc
            RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 500
            FIXTURES_REQUIRED installed_${tree})
        endforeach()
      endif()
    endif()
  endif()
endif()
