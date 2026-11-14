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
      # The tool change handshake between the control thread and the others. No HAL runs.
      add_executable(coffeecnc_hal_control_tests tests/hal_control_tests.cpp)
      target_link_libraries(coffeecnc_hal_control_tests PRIVATE coffeecnc_linuxcnc
        coffeecnc_project_options)
      coffeecnc_enable_analyzers(coffeecnc_hal_control_tests)
      add_test(NAME hal_control COMMAND coffeecnc_hal_control_tests)
      # The real Session on NmlPort against a running LinuxCNC on a simulated mill. Exists only
      # where the LinuxCNC adapter builds; `ctest -L linuxcnc` selects it, `-LE linuxcnc` skips it.
      add_executable(coffeecnc_sim_tests tests/sim/sim_tests.cpp)
      target_link_libraries(coffeecnc_sim_tests PRIVATE coffeecnc_app coffeecnc_linuxcnc
        coffeecnc_project_options nlohmann_json::nlohmann_json)
      coffeecnc_enable_analyzers(coffeecnc_sim_tests)
      foreach(units mm inch)
        add_test(NAME linuxcnc_sim_${units} COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
          -u ${units} -t 300 $<TARGET_FILE:coffeecnc_sim_tests>)
        # One LinuxCNC per host at a time.
        set_tests_properties(linuxcnc_sim_${units} PROPERTIES LABELS linuxcnc RUN_SERIAL TRUE
          RESOURCE_LOCK linuxcnc TIMEOUT 400)
      endforeach()
      # The same on a mill with the G0759's INI values: unhomed MDI and runs, per-axis jog
      # ceilings, optional signals.
      add_test(NAME linuxcnc_sim_g0759 COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/run.sh
        -p g0759 -t 300 $<TARGET_FILE:coffeecnc_sim_tests> g0759)
      add_test(NAME linuxcnc_sim_runner COMMAND ${PROJECT_SOURCE_DIR}/tests/sim/runner_tests.sh
        ${PROJECT_BINARY_DIR})
      set_tests_properties(linuxcnc_sim_g0759 linuxcnc_sim_runner PROPERTIES LABELS linuxcnc
        RUN_SERIAL TRUE RESOURCE_LOCK linuxcnc TIMEOUT 400)
    endif()
  endif()
endif()
