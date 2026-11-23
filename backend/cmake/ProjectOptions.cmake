include_guard(GLOBAL)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(COFFEECNC_WARNINGS_AS_ERRORS "Treat warnings in CoffeeCNC code as errors" ON)
set(COFFEECNC_SANITIZERS "" CACHE STRING "Sanitizers: address;undefined, thread or memory")
set(COFFEECNC_MSAN_LIBCXX "" CACHE PATH
  "Prefix of a libc++ built for MemorySanitizer (tests/sanitizers/build_msan_libcxx.sh)")

add_library(coffeecnc_project_options INTERFACE)
target_compile_features(coffeecnc_project_options INTERFACE cxx_std_23)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(coffeecnc_project_options INTERFACE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
    -Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual -Wformat=2
    -Wnull-dereference -Wdouble-promotion -Wimplicit-fallthrough
    $<$<BOOL:${COFFEECNC_WARNINGS_AS_ERRORS}>:-Werror>)
elseif(MSVC)
  target_compile_options(coffeecnc_project_options INTERFACE
    /W4 $<$<BOOL:${COFFEECNC_WARNINGS_AS_ERRORS}>:/WX>)
endif()

if(COFFEECNC_SANITIZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR "The sanitizer presets require GCC or Clang")
  endif()
  foreach(sanitizer IN LISTS COFFEECNC_SANITIZERS)
    if(NOT sanitizer MATCHES "^(address|undefined|thread|memory)$")
      message(FATAL_ERROR "Unsupported sanitizer: ${sanitizer}")
    endif()
  endforeach()
  if("address" IN_LIST COFFEECNC_SANITIZERS AND "thread" IN_LIST COFFEECNC_SANITIZERS)
    message(FATAL_ERROR "AddressSanitizer and ThreadSanitizer require separate builds")
  endif()
  if("memory" IN_LIST COFFEECNC_SANITIZERS)
    list(LENGTH COFFEECNC_SANITIZERS sanitizer_count)
    if(NOT sanitizer_count EQUAL 1)
      message(FATAL_ERROR "MemorySanitizer requires a build of its own")
    endif()
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      message(FATAL_ERROR "MemorySanitizer requires Clang")
    endif()
    # MemorySanitizer reports every read of memory an uninstrumented library wrote. The build has
    # only what is instrumented: our code, header-only libraries, and a libc++ built for it.
    # LinuxCNC, GTK and WebKit are not, so neither the adapter nor the shell is built.
    foreach(layer COFFEECNC_LINUXCNC COFFEECNC_SHELL)
      if(NOT "${${layer}}" STREQUAL "OFF")
        message(FATAL_ERROR "MemorySanitizer: set ${layer}=OFF (its libraries are not instrumented)")
      endif()
    endforeach()
    set(libcxx "${COFFEECNC_MSAN_LIBCXX}")
    if(NOT EXISTS "${libcxx}/include/c++/v1/vector" OR NOT EXISTS "${libcxx}/lib/libc++.so")
      message(FATAL_ERROR "MemorySanitizer: no instrumented libc++ in COFFEECNC_MSAN_LIBCXX="
        "'${libcxx}'. Build one: tests/sanitizers/build_msan_libcxx.sh <clang++> <prefix>")
    endif()
    # Every C++ translation unit and link of the build, not only those of the project options:
    # one standard library for the whole program.
    add_compile_options(-nostdinc++ "SHELL:-isystem ${libcxx}/include/c++/v1"
      -fsanitize-memory-track-origins=2)
    add_link_options(-stdlib=libc++ "-L${libcxx}/lib" "-Wl,-rpath,${libcxx}/lib")
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
  if("memory" IN_LIST COFFEECNC_SANITIZERS)
    target_compile_definitions(coffeecnc_project_options INTERFACE COFFEECNC_MEMORY_SANITIZER)
  endif()
endif()
