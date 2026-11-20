# Native compiler admission test: finance float/Region syntax MUST NOT
# generate LLVM IR until real typed binary64 and Region lowering is implemented.
# This runs the production oreslang-llvmc rather than a hand-written host model.
cmake_minimum_required(VERSION 3.20)
if(NOT EXISTS "${ORES_FINANCE_LLVM_CLI}")
    message(FATAL_ERROR "finance native compiler binary is missing")
endif()
set(bad_sources
    "pub fnc main():float { return 1.0; }"
    "pub fnc main():int { return 1.25; }"
    "pub compute fnc risk_gate(Region<int> orders):void { return; }"
)
set(counter 0)
foreach(source IN LISTS bad_sources)
    math(EXPR counter "${counter}+1")
    set(path "${CMAKE_CURRENT_BINARY_DIR}/hpc_finance_reject_${counter}.ores")
    file(WRITE "${path}" "${source}\n")
    execute_process(
        COMMAND "${ORES_FINANCE_LLVM_CLI}" "${path}"
        RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr
        TIMEOUT 10)
    file(REMOVE "${path}")
    if("${status}" STREQUAL "0")
        message(FATAL_ERROR "unimplemented finance float or Region guest was admitted")
    endif()
    if(stderr STREQUAL "")
        message(FATAL_ERROR "native compiler rejection emitted no diagnostic")
    endif()
    if(stdout MATCHES "define [^\n]*@" OR stdout MATCHES "target triple")
        message(FATAL_ERROR "negative finance compilation emitted guest LLVM IR")
    endif()
endforeach()
message(STATUS "LLVM_HPC_FINANCE_FLOAT_REGION_SOURCE_NEGATIVE=3 (compiler rejection only)")
