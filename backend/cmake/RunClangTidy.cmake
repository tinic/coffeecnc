cmake_minimum_required(VERSION 3.25)

# CMake supplies the GCC compile command after '--'. Keep GCC's warnings in the
# real compilation and omit only the warning switches Clang cannot parse.
set(gcc_only -Wuseless-cast -Wduplicated-cond -Wduplicated-branches -Wlogical-op)
set(arguments)
math(EXPR last "${CMAKE_ARGC} - 1")
foreach(i RANGE 4 ${last})
  set(arg "${CMAKE_ARGV${i}}")
  if(NOT arg IN_LIST gcc_only)
    string(REPLACE ";" "\\;" arg "${arg}")
    list(APPEND arguments "${arg}")
  endif()
endforeach()
execute_process(COMMAND ${arguments} COMMAND_ERROR_IS_FATAL ANY)
