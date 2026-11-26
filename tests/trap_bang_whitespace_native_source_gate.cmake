# Real LLVM compiler lexical admission: the bang suffix requires ASCII
# whitespace immediately after it. Reject malformed source with no LLVM IR.
cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED ORES_TRAP_LLVM_CLI OR NOT EXISTS "${ORES_TRAP_LLVM_CLI}")
    message(FATAL_ERROR "missing real LLVM CLI")
endif()
set(work "${CMAKE_CURRENT_BINARY_DIR}/native_trap_bang_whitespace_source_gate")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}")

# Positive compiler control must emit native LLVM IR.
set(good [=[pub fnc main(): int { val Result<int,Error> value = trap! (21 + 21); return value.unwrap_or(0); }]=])
file(WRITE "${work}/valid.atto" "${good}\n")
execute_process(COMMAND "${ORES_TRAP_LLVM_CLI}" "${work}/valid.atto"
    RESULT_VARIABLE accepted OUTPUT_VARIABLE positive_ir ERROR_VARIABLE positive_err TIMEOUT 20)
if(NOT "${accepted}" STREQUAL "0" OR NOT positive_ir MATCHES "define [^\n]*@" OR NOT positive_err STREQUAL "")
    message(FATAL_ERROR "positive trap! LLVM compiler control failed: ${accepted} ${positive_err}")
endif()

set(negative_1 [=[pub fnc main():int{val Result<int,Error> v=trap!(21+21);return v.unwrap_or(0);}]=])
set(negative_2 [=[pub fnc main():int{val Result<int,Error> v=trap!21;return v.unwrap_or(0);}]=])
set(negative_3 [=[pub fnc main():int{val Result<int,Error> v=trap!/*gap*/ (21+21);return v.unwrap_or(0);}]=])
set(negative_4 [=[pub fnc main():int{val Result<int,Error> v=trap!;return v.unwrap_or(0);}]=])
foreach(idx RANGE 1 4)
    file(WRITE "${work}/invalid_${idx}.atto" "${negative_${idx}}\n")
    execute_process(COMMAND "${ORES_TRAP_LLVM_CLI}" "${work}/invalid_${idx}.atto"
        RESULT_VARIABLE result OUTPUT_VARIABLE emitted ERROR_VARIABLE diagnostic TIMEOUT 20)
    if("${result}" STREQUAL "0" OR NOT diagnostic MATCHES "E_BANG_KEYWORD_WHITESPACE" OR
       emitted MATCHES "define [^\n]*@" OR emitted MATCHES "target triple")
        message(FATAL_ERROR
          "malformed trap! #${idx} must fail with precise code and no LLVM IR: status=${result} diagnostic=${diagnostic}")
    endif()
endforeach()
message(STATUS "LLVM_TRAP_BANG_WHITESPACE=1_REAL_COMPILER_POSITIVE_4_PRECISE_NEGATIVES")
