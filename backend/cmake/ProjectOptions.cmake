include_guard(GLOBAL)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(COFFEECNC_WARNINGS_AS_ERRORS "Treat warnings in CoffeeCNC code as errors" ON)
set(COFFEECNC_SANITIZERS "" CACHE STRING "Sanitizers: address;undefined or thread")

add_library(coffeecnc_project_options INTERFACE)
target_compile_features(coffeecnc_project_options INTERFACE cxx_std_23)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(coffeecnc_project_options INTERFACE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
    -Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual -Wformat=2
    -Wnull-dereference -Wdouble-promotion -Wimplicit-fallthrough
    -pedantic-errors -Wcast-qual -Wmissing-declarations -Wextra-semi
    $<$<BOOL:${COFFEECNC_WARNINGS_AS_ERRORS}>:-Werror>)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(coffeecnc_project_options INTERFACE
      -Wuseless-cast -Wduplicated-cond -Wduplicated-branches -Wlogical-op)
  endif()
elseif(MSVC)
  target_compile_options(coffeecnc_project_options INTERFACE
    /W4 $<$<BOOL:${COFFEECNC_WARNINGS_AS_ERRORS}>:/WX>)
endif()

if(COFFEECNC_SANITIZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR "The sanitizer presets require GCC or Clang")
  endif()
  foreach(sanitizer IN LISTS COFFEECNC_SANITIZERS)
    if(NOT sanitizer MATCHES "^(address|undefined|thread)$")
      message(FATAL_ERROR "Unsupported sanitizer: ${sanitizer}")
    endif()
  endforeach()
  if("address" IN_LIST COFFEECNC_SANITIZERS AND "thread" IN_LIST COFFEECNC_SANITIZERS)
    message(FATAL_ERROR "AddressSanitizer and ThreadSanitizer require separate builds")
  endif()
  list(JOIN COFFEECNC_SANITIZERS "," sanitizers)
  target_compile_options(coffeecnc_project_options INTERFACE
    -fsanitize=${sanitizers} -fno-sanitize-recover=all -fno-omit-frame-pointer)
  target_link_options(coffeecnc_project_options INTERFACE -fsanitize=${sanitizers})
  if("address" IN_LIST COFFEECNC_SANITIZERS)
    target_compile_definitions(coffeecnc_project_options INTERFACE COFFEECNC_ADDRESS_SANITIZER)
  endif()
  if("thread" IN_LIST COFFEECNC_SANITIZERS)
    target_compile_definitions(coffeecnc_project_options INTERFACE COFFEECNC_THREAD_SANITIZER)
  endif()
endif()
