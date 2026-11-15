# Compile the real AttoLang trap-int guest via the production oreslang-llvmc
# and source-owned audited AOT driver, then execute at Clang O0 and O2.
# This proves ONLY the restricted, nonthrowing Option<int> guest slice.
cmake_minimum_required(VERSION 3.20)
foreach(var ORES_TRAP_LLVM_CLI ORES_TRAP_SOURCE ORES_TRAP_AOT_SCRIPT ORES_TRAP_AUDITOR)
    if(NOT DEFINED ${var} OR NOT EXISTS "${${var}}")
        message(FATAL_ERROR "missing required real-native test input: ${var}")
    endif()
endforeach()
if(NOT DEFINED ORES_TRAP_CLANG OR ORES_TRAP_CLANG STREQUAL "")
    message(FATAL_ERROR "missing Clang for actual native guest execution")
endif()

set(work "${CMAKE_CURRENT_BINARY_DIR}/native_trap_int_aot_exec")
file(MAKE_DIRECTORY "${work}")
foreach(level IN ITEMS 0 2)
    set(out "${work}/trap_guest_O${level}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            "ORESLANG_LLVMC=${ORES_TRAP_LLVM_CLI}"
            "ORES_NO_GC_IR_AUDIT=${ORES_TRAP_AUDITOR}"
            "ORESLANG_LLVM_AOT_OPT_LEVEL=${level}"
            "CLANG=${ORES_TRAP_CLANG}"
            bash "${ORES_TRAP_AOT_SCRIPT}" "${ORES_TRAP_SOURCE}" "${out}"
        RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr
        TIMEOUT 120)
    if(NOT "${status}" STREQUAL "0" OR NOT EXISTS "${out}")
        message(FATAL_ERROR "real native trap guest AOT O${level} failed: ${status} ${stderr}")
    endif()
    # Oreslang main(): int returns the native process's actual exit status.
    # A fake host evaluator, absent binary or scalar-only compiler will fail.
    execute_process(
        COMMAND "${out}"
        RESULT_VARIABLE guest_exit OUTPUT_VARIABLE guest_stdout
        ERROR_VARIABLE guest_stderr TIMEOUT 15)
    if(NOT "${guest_exit}" STREQUAL "42")
        message(FATAL_ERROR
            "compiled native trap guest O${level} expected exit 42, got '${guest_exit}' stdout='${guest_stdout}' stderr='${guest_stderr}'")
    endif()
endforeach()
message(STATUS "LLVM_TRAP_INT_NATIVE_GUEST_O0_O2_PASS (NOT nested Result/throw parity)")
