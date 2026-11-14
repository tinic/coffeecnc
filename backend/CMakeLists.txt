cmake_minimum_required(VERSION 3.25)
project(coffeecnc_backend VERSION 0.1.0 LANGUAGES CXX)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(FATAL_ERROR "Use an out-of-source build: cmake -S backend -B build/backend")
endif()

include(cmake/ProjectOptions.cmake)
include(cmake/StaticAnalyzers.cmake)
add_subdirectory(src/core)

include(CTest)
if(BUILD_TESTING)
  add_executable(coffeecnc_core_tests tests/core_tests.cpp)
  target_link_libraries(coffeecnc_core_tests PRIVATE coffeecnc_core coffeecnc_project_options)
  coffeecnc_enable_analyzers(coffeecnc_core_tests)
  add_test(NAME core_behavior COMMAND coffeecnc_core_tests)
endif()
