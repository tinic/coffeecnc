# The coffeecnc executable: the application's wiring of runtime, LinuxCNC adapter and window.
# Built where both the adapter and the shell are.

if(NOT TARGET coffeecnc_shell OR NOT TARGET coffeecnc_linuxcnc)
  return()
endif()

add_library(coffeecnc_application application.cpp)
target_include_directories(coffeecnc_application PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(coffeecnc_application PUBLIC cxx_std_23)
# Where a development build finds the panel when none is installed beside it.
target_compile_definitions(coffeecnc_application PRIVATE
  COFFEECNC_SOURCE_UI_DIRECTORY="${PROJECT_SOURCE_DIR}/../ui/dist")
if(TARGET coffeecnc_preview)
  # Where a development build finds the preview helper when none is installed beside it.
  target_compile_definitions(coffeecnc_application PRIVATE
    COFFEECNC_PREVIEW_HELPER_BUILT="$<TARGET_FILE:coffeecnc_preview>")
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
install(DIRECTORY ${PROJECT_SOURCE_DIR}/../ui/dist/ DESTINATION ${CMAKE_INSTALL_DATADIR}/coffeecnc/ui
  OPTIONAL)
