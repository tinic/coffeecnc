# What `cmake --install` adds to the programs and the panel, and the Debian package
# (`cpack -G DEB` in the build directory, or `cpack --preset package`).
#
#   <prefix>/bin/coffeecnc, coffeecnc-preview        install rules beside the targets
#   <prefix>/share/coffeecnc/ui/                     the built panel (ui/dist)
#   <prefix>/share/coffeecnc/hal/coffeecnc_postgui.hal
#   <prefix>/share/coffeecnc/coffeecnc.ini.example
#
# The HAL fragment installed is the file the simulation tests run (tests/sim/run.sh copies
# share/hal/coffeecnc_postgui.hal into every run directory): there is one.
include(GNUInstallDirs)
install(FILES ${PROJECT_SOURCE_DIR}/share/hal/coffeecnc_postgui.hal
  DESTINATION ${CMAKE_INSTALL_DATADIR}/coffeecnc/hal)
# Examples for the optional functions, sourced after the fragment by the machine's own file.
install(FILES ${PROJECT_SOURCE_DIR}/share/hal/examples/paused_z.hal
  ${PROJECT_SOURCE_DIR}/share/hal/examples/fixed_setter.hal
  DESTINATION ${CMAKE_INSTALL_DATADIR}/coffeecnc/hal/examples)
install(FILES ${PROJECT_SOURCE_DIR}/share/coffeecnc.ini.example
  DESTINATION ${CMAKE_INSTALL_DATADIR}/coffeecnc)

# Licenses: CoffeeCNC's own (MIT) as the Debian copyright file, and the notice of everything the
# package bundles: the panel's runtime libraries (in ui/dist), nlohmann/json (compiled in) and
# the SM-101 sample model the simulator offers. occt-import-js is LGPL-2.1: its WebAssembly is a
# separate file of the panel, and its source is https://github.com/kovacsv/occt-import-js.
set(coffeecnc_doc ${CMAKE_INSTALL_DATADIR}/doc/coffeecnc)
install(FILES ${PROJECT_SOURCE_DIR}/../LICENSE DESTINATION ${coffeecnc_doc} RENAME copyright)
set(coffeecnc_ui_modules ${PROJECT_SOURCE_DIR}/../ui/node_modules)
foreach(notice IN ITEMS "vue:LICENSE" "three:LICENSE" "occt-import-js:LICENSE.md")
  string(REPLACE ":" ";" parts "${notice}")
  list(GET parts 0 module)
  list(GET parts 1 file)
  install(FILES ${coffeecnc_ui_modules}/${module}/${file}
    DESTINATION ${coffeecnc_doc}/third-party/${module})
endforeach()
install(FILES ${PROJECT_SOURCE_DIR}/../samples/upstream/instant-machinist/LICENSE
  DESTINATION ${coffeecnc_doc}/third-party/instant-machinist-sm-101)
if(EXISTS /usr/share/doc/nlohmann-json3-dev/copyright)
  install(FILES /usr/share/doc/nlohmann-json3-dev/copyright
    DESTINATION ${coffeecnc_doc}/third-party/nlohmann-json)
endif()

# The package is for the Debian release and architecture it is built on. Nothing here is
# looked up on a network: the LinuxCNC version is the installed package's.
find_program(COFFEECNC_DPKG_QUERY dpkg-query)
find_program(COFFEECNC_DPKG dpkg)
if(NOT COFFEECNC_DPKG_QUERY OR NOT COFFEECNC_DPKG)
  message(STATUS "dpkg not found: no Debian package is configured")
  return()
endif()
execute_process(COMMAND ${COFFEECNC_DPKG} --print-architecture
  OUTPUT_VARIABLE architecture OUTPUT_STRIP_TRAILING_WHITESPACE)
# The NML messages are encoded by the liblinuxcnc.a linked into the programs, and their encoding
# can change within a series: 2.9.11 adds a field to EMC_SPINDLE_OFF and an argument to the
# interpreter's STOP_SPINDLE_TURNING. So the package depends on exactly the LinuxCNC it was built
# against; a LinuxCNC update needs a CoffeeCNC built for it.
execute_process(COMMAND ${COFFEECNC_DPKG_QUERY} -W -f=\${Version} linuxcnc-uspace
  OUTPUT_VARIABLE linuxcnc_version OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT linuxcnc_version MATCHES "^(([0-9]+):)?([0-9]+)\\.([0-9]+)\\.([0-9]+)")
  message(STATUS "linuxcnc-uspace is not an installed package: no Debian package is configured")
  return()
endif()

set(COFFEECNC_PACKAGE_MAINTAINER "tinic <tinic@users.noreply.github.com>" CACHE STRING
  "Maintainer field of the Debian package")
set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_NAME coffeecnc)
# The one place the version is written: project() in the top CMakeLists.txt.
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Operator panel for LinuxCNC milling machines")
set(CPACK_PACKAGE_DESCRIPTION
  "CoffeeCNC is a DISPLAY program of LinuxCNC for XYZ mills: a touch panel in a GTK/WebKit window. Set [DISPLAY] DISPLAY = coffeecnc in the machine's INI file; see /usr/share/coffeecnc/coffeecnc.ini.example and /usr/share/coffeecnc/hal/coffeecnc_postgui.hal.")
set(CPACK_PACKAGE_CONTACT "${COFFEECNC_PACKAGE_MAINTAINER}")
set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
set(CPACK_STRIP_FILES ON)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE ${architecture})
set(CPACK_DEBIAN_PACKAGE_SECTION misc)
set(CPACK_DEBIAN_PACKAGE_PRIORITY optional)
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/tinic/coffeecnc")
# Library packages come from dpkg-shlibdeps. LinuxCNC's libraries have no shlibs file, and
# halcmd is run as a program, so that package is named here.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "linuxcnc-uspace (= ${linuxcnc_version})")
# The panel's Chinese and Japanese text needs CJK fonts, which a GNOME install may not have
# (Droid Sans Fallback alone gives Japanese the Chinese forms of Han characters). apt installs
# recommended packages by default; without them the other five languages are unaffected.
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "fonts-noto-cjk")
include(CPack)
