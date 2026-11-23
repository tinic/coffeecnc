# The coffeecnc executable: the application's wiring of runtime, LinuxCNC adapter and window.
# Built where both the adapter and the shell are.

if(NOT TARGET coffeecnc_shell OR NOT TARGET coffeecnc_linuxcnc)
  return()
endif()

# GCC warns under ThreadSanitizer (-Wtsan, an error with -Werror) that it does not model
# std::atomic_thread_fence, which Boost.Asio calls in detail/std_fenced_block.hpp and
# detail/atomic_count.hpp. Each io_context here runs on one thread only (the server's has
# concurrency hint 1, the test client's is the test's own): those fences order nothing between
# threads of this program. Only that warning is off, only for the sources that include Asio.
set(COFFEECNC_ASIO_OPTIONS "")
if("thread" IN_LIST COFFEECNC_SANITIZERS AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(COFFEECNC_ASIO_OPTIONS -Wno-tsan)
endif()
set(COFFEECNC_ASIO_OPTIONS "${COFFEECNC_ASIO_OPTIONS}" CACHE INTERNAL
  "Compile options of sources that include Boost.Asio")

add_library(coffeecnc_application application.cpp config_check.cpp remote_server.cpp)
target_include_directories(coffeecnc_application SYSTEM PRIVATE ${COFFEECNC_BOOST_INCLUDE_DIR})
set_source_files_properties(remote_server.cpp PROPERTIES COMPILE_OPTIONS "${COFFEECNC_ASIO_OPTIONS}")
target_include_directories(coffeecnc_application PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(coffeecnc_application PUBLIC cxx_std_23)
# Where a development build finds the panel when none is installed beside it.
if(COFFEECNC_DEVELOPMENT_PATHS)
  target_compile_definitions(coffeecnc_application PRIVATE
    COFFEECNC_SOURCE_UI_DIRECTORY="${PROJECT_SOURCE_DIR}/../ui/dist")
endif()
if(TARGET coffeecnc_preview AND COFFEECNC_DEVELOPMENT_PATHS)
  # Where a development build finds the preview helper when none is installed beside it.
  target_compile_definitions(coffeecnc_application PRIVATE
    COFFEECNC_PREVIEW_HELPER_BUILT="$<TARGET_FILE:coffeecnc_preview>")
endif()
if(TARGET coffeecnc_preview)
  add_dependencies(coffeecnc_application coffeecnc_preview)
endif()
target_link_libraries(coffeecnc_application PUBLIC coffeecnc_app coffeecnc_linuxcnc coffeecnc_shell
  PRIVATE coffeecnc_project_options)
coffeecnc_enable_analyzers(coffeecnc_application)

# COFFEECNC_LSAN_SUPPRESSIONS: set in src/shell, a source of every program that opens the window.
add_executable(coffeecnc main.cpp ${COFFEECNC_LSAN_SUPPRESSIONS})
target_link_libraries(coffeecnc PRIVATE coffeecnc_application coffeecnc_project_options)
coffeecnc_enable_analyzers(coffeecnc)

include(GNUInstallDirs)
install(TARGETS coffeecnc RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# The built panel. Not optional: an installation without it does not start.
install(DIRECTORY ${PROJECT_SOURCE_DIR}/../ui/dist/ DESTINATION ${CMAKE_INSTALL_DATADIR}/coffeecnc/ui)
