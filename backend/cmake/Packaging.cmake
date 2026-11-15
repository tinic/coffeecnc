# What `cmake --install` adds to the programs and the panel, and the Debian package
# (`cpack -G DEB` in the build directory, or `cpack --preset package`).
#
#   <prefix>/bin/coffeecnc, coffeecnc-preview        install rules beside the targets
#   <prefix>/share/coffeecnc/ui/                     the built panel (ui/dist)
#   <prefix>/share/coffeecnc/hal/coffeecnc_postgui.hal
#   <prefix>/share/coffeecnc/coffeecnc.ini.example
#
# The HAL fragment installed is the file the simulation tests run (tests/sim/run.sh copies the
# same file into every run directory): there is one.
include(GNUInstallDirs)
install(FILES ${PROJECT_SOURCE_DIR}/tests/sim/coffeecnc_postgui.hal
  DESTINATION ${CMAKE_INSTALL_DATADIR}/coffeecnc/hal)
install(FILES ${PROJECT_SOURCE_DIR}/share/coffeecnc.ini.example
  DESTINATION ${CMAKE_INSTALL_DATADIR}/coffeecnc)

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
# NML passes EMC_STAT as bytes and its layout changes between LinuxCNC releases: the programs
# run with the release series they were built against, from the built version on.
execute_process(COMMAND ${COFFEECNC_DPKG_QUERY} -W -f=\${Version} linuxcnc-uspace
  OUTPUT_VARIABLE linuxcnc_version OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT linuxcnc_version MATCHES "^(([0-9]+):)?([0-9]+)\\.([0-9]+)\\.([0-9]+)")
  message(STATUS "linuxcnc-uspace is not an installed package: no Debian package is configured")
  return()
endif()
set(epoch "${CMAKE_MATCH_1}")
set(series_floor "${epoch}${CMAKE_MATCH_3}.${CMAKE_MATCH_4}.${CMAKE_MATCH_5}")
math(EXPR next_minor "${CMAKE_MATCH_4} + 1")
set(series_ceiling "${epoch}${CMAKE_MATCH_3}.${next_minor}")

set(COFFEECNC_PACKAGE_MAINTAINER "CoffeeCNC <coffeecnc@invalid>" CACHE STRING
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
set(CPACK_DEBIAN_PACKAGE_DEPENDS
  "linuxcnc-uspace (>= ${series_floor}), linuxcnc-uspace (<< ${series_ceiling})")
include(CPack)
