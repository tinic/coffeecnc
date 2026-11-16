# The only layer that includes GTK's and WebKit's headers: the window with the web view. They
# are not warning-clean, so they are SYSTEM headers and included from src/ only. Built only
# where the development files of GTK 3 and WebKitGTK 4.1 (Debian: libgtk-3-dev,
# libwebkit2gtk-4.1-dev) are installed.

set(COFFEECNC_SHELL "AUTO" CACHE STRING
  "GTK/WebKit shell: AUTO builds it when GTK 3 and WebKitGTK 4.1 are found, ON requires them")
set_property(CACHE COFFEECNC_SHELL PROPERTY STRINGS AUTO ON OFF)
if(NOT COFFEECNC_SHELL MATCHES "^(AUTO|ON|OFF)$")
  message(FATAL_ERROR "COFFEECNC_SHELL must be AUTO, ON or OFF")
endif()
if(COFFEECNC_SHELL STREQUAL "OFF")
  return()
endif()

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(COFFEECNC_WEBKIT QUIET webkit2gtk-4.1 gtk+-3.0)
endif()
if(NOT COFFEECNC_WEBKIT_FOUND)
  set(missing "GTK 3 and WebKitGTK 4.1 development files not found (libgtk-3-dev, libwebkit2gtk-4.1-dev)")
  if(COFFEECNC_SHELL STREQUAL "ON")
    message(FATAL_ERROR "${missing}")
  endif()
  message(STATUS "${missing}: the shell and the coffeecnc executable are not built")
  return()
endif()
message(STATUS "Shell: WebKitGTK ${COFFEECNC_WEBKIT_webkit2gtk-4.1_VERSION}, "
  "GTK ${COFFEECNC_WEBKIT_gtk+-3.0_VERSION}")

add_library(coffeecnc_shell src/panel_window.cpp)
target_include_directories(coffeecnc_shell PUBLIC include)
target_include_directories(coffeecnc_shell SYSTEM PRIVATE ${COFFEECNC_WEBKIT_INCLUDE_DIRS})
target_compile_features(coffeecnc_shell PUBLIC cxx_std_23)
target_compile_options(coffeecnc_shell PRIVATE ${COFFEECNC_WEBKIT_CFLAGS_OTHER})
target_link_libraries(coffeecnc_shell PUBLIC coffeecnc_app
  PRIVATE coffeecnc_project_options ${COFFEECNC_WEBKIT_LINK_LIBRARIES})
coffeecnc_enable_analyzers(coffeecnc_shell)

# What tests do to the window. Built only for a test that links it; never part of the coffeecnc
# executable.
add_library(coffeecnc_shell_test_hooks EXCLUDE_FROM_ALL src/test_hooks.cpp)
target_include_directories(coffeecnc_shell_test_hooks SYSTEM PRIVATE
  ${COFFEECNC_WEBKIT_INCLUDE_DIRS})
target_compile_options(coffeecnc_shell_test_hooks PRIVATE ${COFFEECNC_WEBKIT_CFLAGS_OTHER})
target_link_libraries(coffeecnc_shell_test_hooks PUBLIC coffeecnc_shell
  PRIVATE coffeecnc_project_options ${COFFEECNC_WEBKIT_LINK_LIBRARIES})
coffeecnc_enable_analyzers(coffeecnc_shell_test_hooks)

if(BUILD_TESTING AND NOT "thread" IN_LIST COFFEECNC_SANITIZERS)
  add_executable(coffeecnc_quit_delivery_tests ../../tests/shell/quit_delivery_tests.cpp)
  target_include_directories(coffeecnc_quit_delivery_tests PRIVATE src)
  target_include_directories(coffeecnc_quit_delivery_tests SYSTEM PRIVATE ${COFFEECNC_WEBKIT_INCLUDE_DIRS})
  target_link_libraries(coffeecnc_quit_delivery_tests PRIVATE coffeecnc_shell
    coffeecnc_project_options ${COFFEECNC_WEBKIT_LINK_LIBRARIES})
  add_test(NAME quit_delivery COMMAND xvfb-run -a $<TARGET_FILE:coffeecnc_quit_delivery_tests>
    ${PROJECT_BINARY_DIR}/quit-delivery-panel)
  set_tests_properties(quit_delivery PROPERTIES TIMEOUT 20)
endif()
