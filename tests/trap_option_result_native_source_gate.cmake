# Compiler-backed native source ADMISSION gate for caller-side trap/ADT effects.
# Passing this test means LLVM does NOT miscompile unsupported guest syntax.
# It is NOT a positive Option<Result> / ordinary-throw native parity result.
cmake_minimum_required(VERSION 3.20)
if(NOT EXISTS "${ORES_TRAP_LLVM_CLI}")
    message(FATAL_ERROR "real oreslang-llvmc binary is missing")
endif()

# Positive control ensures a broken/absent compiler cannot pass by rejecting all.
set(good "${CMAKE_CURRENT_BINARY_DIR}/ores_trap_gate_control.atto")
file(WRITE "${good}" "pub fnc main():int { return 7; }\n")
execute_process(
    COMMAND "${ORES_TRAP_LLVM_CLI}" "${good}"
    RESULT_VARIABLE good_status OUTPUT_VARIABLE good_ir ERROR_VARIABLE good_error
    TIMEOUT 15)
file(REMOVE "${good}")
if(NOT "${good_status}" STREQUAL "0" OR NOT good_ir MATCHES "define [^\n]*@")
    message(FATAL_ERROR "positive compiler control failed: ${good_error}")
endif()

# Exact Java/Graal guest fixture. LLVM may reject it until real native
# tagged payload, ownership-safe throw and caller-side Option lowering exist.
if(NOT EXISTS "${ORES_TRAP_SHARED_SOURCE}")
    message(FATAL_ERROR "shared AttoLang trap fixture is missing")
endif()
execute_process(
    COMMAND "${ORES_TRAP_LLVM_CLI}" "${ORES_TRAP_SHARED_SOURCE}"
    RESULT_VARIABLE target_status OUTPUT_VARIABLE target_ir
    ERROR_VARIABLE target_error TIMEOUT 15)
if("${target_status}" STREQUAL "0")
    message(FATAL_ERROR "LLVM admitted shared trap guest without parity proof")
endif()
if(target_error STREQUAL "" OR
        target_ir MATCHES "define [^\n]*@" OR target_ir MATCHES "target triple")
    message(FATAL_ERROR "shared trap guest failed to reject safely")
endif()

# Do not put semicolon-containing source text into a CMake list: foreach
# would split each guest program at ';' and silently test broken fragments.
set(case_1 [=[pub fnc main():int { return trap 7; }]=])
set(case_2 [=[fnc checked():int { return 7; } pub fnc main():int { return trap checked(); }]=])
set(case_3 [=[trap fnc checked():int { return 7; } pub fnc main():int { return 7; }]=])
set(case_4 [=[pub fnc main():int { val Option<int> outcome = trap true; return 7; }]=])
set(case_5 [=[pub fnc main():int { throw Error("ordinary"); return 7; }]=])
set(case_6 [=[pub fnc main():int { val Option<int> outcome = trap Some(7); return 7; }]=])
foreach(n RANGE 1 6)
    set(source "${case_${n}}")
    set(path "${CMAKE_CURRENT_BINARY_DIR}/ores_trap_gate_reject_${n}.atto")
    file(WRITE "${path}" "${source}\n")
    execute_process(
        COMMAND "${ORES_TRAP_LLVM_CLI}" "${path}"
        RESULT_VARIABLE status OUTPUT_VARIABLE ir ERROR_VARIABLE diagnostic
        TIMEOUT 15)
    file(REMOVE "${path}")
    if("${status}" STREQUAL "0")
        message(FATAL_ERROR "unimplemented trap guest was accepted in case ${n}")
    endif()
    if(diagnostic STREQUAL "")
        message(FATAL_ERROR "missing negative source diagnostic in case ${n}")
    endif()
    if(ir MATCHES "define [^\n]*@" OR ir MATCHES "target triple")
        message(FATAL_ERROR "rejected trap source produced LLVM IR in case ${n}")
    endif()
endforeach()
message(STATUS "LLVM_TRAP_OPTION_RESULT_NATIVE_SOURCE_GATE=6_REJECTED_1_POSITIVE (not semantic parity)")
