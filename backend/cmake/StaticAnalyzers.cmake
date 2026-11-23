include_guard(GLOBAL)

option(COFFEECNC_ENABLE_ANALYZERS "Require clang-tidy and cppcheck during compilation" OFF)
if(COFFEECNC_ENABLE_ANALYZERS)
  find_program(COFFEECNC_CLANG_TIDY NAMES clang-tidy clang-tidy-22 clang-tidy-21 clang-tidy-20
    HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin REQUIRED)
  find_program(COFFEECNC_CPPCHECK NAMES cppcheck REQUIRED)
endif()

function(coffeecnc_enable_analyzers target)
  if(COFFEECNC_ENABLE_ANALYZERS)
    set_target_properties(${target} PROPERTIES
      CXX_CLANG_TIDY "${CMAKE_COMMAND};-P;${CMAKE_CURRENT_FUNCTION_LIST_DIR}/RunClangTidy.cmake;--;${COFFEECNC_CLANG_TIDY};--warnings-as-errors=*"
      CXX_CPPCHECK "${COFFEECNC_CPPCHECK};--enable=warning,style,performance,portability;--error-exitcode=2;--std=c++20;--inline-suppr;--suppress=missingIncludeSystem;--suppress=normalCheckLevelMaxBranches")
  endif()
endfunction()
