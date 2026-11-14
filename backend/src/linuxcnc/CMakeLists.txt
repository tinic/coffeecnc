# The only layer that includes LinuxCNC's headers: NML task channels, the HAL component and the
# INI file. They are not warning-clean, so they are SYSTEM headers and included from src/ only.
# Built only where LinuxCNC's development files (Debian: linuxcnc-uspace-dev) are installed.

set(COFFEECNC_LINUXCNC "AUTO" CACHE STRING
  "LinuxCNC adapter: AUTO builds it when LinuxCNC's development files are found, ON requires them")
set_property(CACHE COFFEECNC_LINUXCNC PROPERTY STRINGS AUTO ON OFF)
if(NOT COFFEECNC_LINUXCNC MATCHES "^(AUTO|ON|OFF)$")
  message(FATAL_ERROR "COFFEECNC_LINUXCNC must be AUTO, ON or OFF")
endif()
if(COFFEECNC_LINUXCNC STREQUAL "OFF")
  return()
endif()

find_path(COFFEECNC_LINUXCNC_INCLUDE_DIR emc_nml.hh PATH_SUFFIXES linuxcnc)
find_library(COFFEECNC_LINUXCNC_EMC NAMES liblinuxcnc.a linuxcnc)
find_library(COFFEECNC_LINUXCNC_NML nml)
find_library(COFFEECNC_LINUXCNC_HAL linuxcnchal)
find_library(COFFEECNC_LINUXCNC_INI linuxcncini)
# LinuxCNC's headers include Boost's, but linuxcnc-uspace-dev does not depend on libboost-dev.
# Headers unpacked below build/deps (apt-get download libboost1.83-dev; dpkg -x) are accepted
# where the package cannot be installed.
find_path(COFFEECNC_BOOST_INCLUDE_DIR boost/noncopyable.hpp
  HINTS "${PROJECT_SOURCE_DIR}/build/deps/usr/include")
set(missing "")
if(NOT (COFFEECNC_LINUXCNC_INCLUDE_DIR AND COFFEECNC_LINUXCNC_EMC AND COFFEECNC_LINUXCNC_NML
        AND COFFEECNC_LINUXCNC_HAL AND COFFEECNC_LINUXCNC_INI))
  set(missing "LinuxCNC development files not found (linuxcnc-uspace-dev)")
elseif(NOT COFFEECNC_BOOST_INCLUDE_DIR)
  set(missing "Boost headers not found; LinuxCNC's headers need them (libboost-dev)")
endif()
if(missing)
  if(COFFEECNC_LINUXCNC STREQUAL "ON")
    message(FATAL_ERROR "${missing}")
  endif()
  message(STATUS "${missing}: the LinuxCNC adapter is not built")
  return()
endif()
message(STATUS "LinuxCNC adapter: headers in ${COFFEECNC_LINUXCNC_INCLUDE_DIR}")

add_library(coffeecnc_linuxcnc src/hal_component.cpp src/hal_control.cpp src/machine_config.cpp
  src/nml_port.cpp src/nml_status.cpp src/tool_table.cpp)
target_include_directories(coffeecnc_linuxcnc PUBLIC include)
target_include_directories(coffeecnc_linuxcnc SYSTEM PRIVATE ${COFFEECNC_LINUXCNC_INCLUDE_DIR}
  ${COFFEECNC_BOOST_INCLUDE_DIR})
target_compile_features(coffeecnc_linuxcnc PUBLIC cxx_std_23)
# The user-space (not realtime) side of the RTAPI and HAL headers.
target_compile_definitions(coffeecnc_linuxcnc PRIVATE ULAPI)
target_link_libraries(coffeecnc_linuxcnc PUBLIC coffeecnc_core
  PRIVATE coffeecnc_project_options ${COFFEECNC_LINUXCNC_EMC} ${COFFEECNC_LINUXCNC_NML}
          ${COFFEECNC_LINUXCNC_HAL} ${COFFEECNC_LINUXCNC_INI})
coffeecnc_enable_analyzers(coffeecnc_linuxcnc)

# Throwaway-grade tool that records how the installed LinuxCNC behaves; see tests/sim/run.sh.
add_executable(coffeecnc_linuxcnc_probe probe/probe.cpp)
target_include_directories(coffeecnc_linuxcnc_probe SYSTEM PRIVATE
  ${COFFEECNC_LINUXCNC_INCLUDE_DIR} ${COFFEECNC_BOOST_INCLUDE_DIR})
target_compile_definitions(coffeecnc_linuxcnc_probe PRIVATE ULAPI)
target_compile_features(coffeecnc_linuxcnc_probe PRIVATE cxx_std_23)
target_link_libraries(coffeecnc_linuxcnc_probe PRIVATE coffeecnc_linuxcnc ${COFFEECNC_LINUXCNC_EMC}
  ${COFFEECNC_LINUXCNC_NML} ${COFFEECNC_LINUXCNC_HAL} ${COFFEECNC_LINUXCNC_INI})
