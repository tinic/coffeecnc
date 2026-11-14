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
      COFFEECNC_COMMAND_FIXTURES="${PROJECT_SOURCE_DIR}/../protocol/fixtures/commands.json")
    coffeecnc_enable_analyzers(coffeecnc_fixture_tests)
    add_test(NAME protocol_fixtures COMMAND coffeecnc_fixture_tests)
    add_executable(coffeecnc_response_tests tests/response_tests.cpp)
    target_link_libraries(coffeecnc_response_tests PRIVATE coffeecnc_app coffeecnc_project_options
      nlohmann_json::nlohmann_json)
    coffeecnc_enable_analyzers(coffeecnc_response_tests)
    add_test(NAME response_encoding COMMAND coffeecnc_response_tests)
    add_executable(coffeecnc_session_tests tests/session_tests.cpp)
    target_link_libraries(coffeecnc_session_tests PRIVATE coffeecnc_app coffeecnc_project_options
      nlohmann_json::nlohmann_json)
    coffeecnc_enable_analyzers(coffeecnc_session_tests)
    add_test(NAME session_lifecycle COMMAND coffeecnc_session_tests)
  endif()
endif()
