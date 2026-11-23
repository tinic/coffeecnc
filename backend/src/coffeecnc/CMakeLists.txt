# The coffeecnc executable: the application's wiring of runtime, LinuxCNC adapter and window.
# Built where both the adapter and the shell are.

if(NOT TARGET coffeecnc_shell OR NOT TARGET coffeecnc_linuxcnc)
  return()
endif()

add_library(coffeecnc_application application.cpp config_check.cpp remote_server.cpp)
# GCC 14 diagnoses inlined Boost.Asio executor/reactor code at -O3, even in SYSTEM headers.
# Boost's exception locations also retain __FILE__. Keep those independent of the host path.
set_property(SOURCE remote_server.cpp APPEND PROPERTY COMPILE_OPTIONS
  "$<$<CXX_COMPILER_ID:GNU>:-Wno-null-dereference>"
  "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-fmacro-prefix-map=${COFFEECNC_BOOST_INCLUDE_DIR}/boost=boost>")
target_include_directories(coffeecnc_application SYSTEM PRIVATE ${COFFEECNC_BOOST_INCLUDE_DIR})
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

# LeakSanitizer's suppressions are looked up in the executable: the file is a source of every
# program that opens the window, and is empty outside AddressSanitizer builds.
set(COFFEECNC_LSAN_SUPPRESSIONS "${CMAKE_CURRENT_SOURCE_DIR}/lsan_suppressions.cpp"
  CACHE INTERNAL "Source with the LeakSanitizer suppressions of the window's libraries")
add_executable(coffeecnc main.cpp ${COFFEECNC_LSAN_SUPPRESSIONS})
target_link_libraries(coffeecnc PRIVATE coffeecnc_application coffeecnc_project_options)
coffeecnc_enable_analyzers(coffeecnc)

include(GNUInstallDirs)
install(TARGETS coffeecnc RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# The built panel. Not optional: an installation without it does not start.
install(DIRECTORY ${PROJECT_SOURCE_DIR}/../ui/dist/ DESTINATION ${CMAKE_INSTALL_DATADIR}/coffeecnc/ui)
