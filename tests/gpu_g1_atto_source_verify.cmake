# Execute the restricted real .atto -> checked G1 IR compiler, then validate
# emitted IR with a real LLVM IR parser and negative mutations. HOST ONLY.
foreach(key IN ITEMS G1_COMPILER G1_SOURCE G1_EMITTER G1_CLANG G1_WORK)
    if(NOT DEFINED ${key} OR "${${key}}" STREQUAL "")
        message(FATAL_ERROR "GPU-G1-SOURCE-TEST-001: missing ${key}")
    endif()
endforeach()
file(MAKE_DIRECTORY "${G1_WORK}")
set(ir_path "${G1_WORK}/source-g1.ll")
set(parsed_path "${G1_WORK}/source-g1-checked.ll")
file(REMOVE "${ir_path}" "${parsed_path}")
execute_process(COMMAND "${G1_COMPILER}" "${G1_SOURCE}"
    OUTPUT_VARIABLE actual ERROR_VARIABLE compile_error RESULT_VARIABLE compile_rc)
if(NOT compile_rc EQUAL 0 OR actual STREQUAL "")
    message(FATAL_ERROR "GPU-G1-SOURCE-TEST-002: source compilation failed: ${compile_error}")
endif()
execute_process(COMMAND "${G1_EMITTER}" --emit-llvm-ir
    OUTPUT_VARIABLE reference ERROR_VARIABLE emit_error RESULT_VARIABLE emit_rc)
if(NOT emit_rc EQUAL 0 OR NOT actual STREQUAL reference)
    message(FATAL_ERROR "GPU-G1-SOURCE-TEST-003: .atto source IR disagrees with checked G1 IR backend")
endif()
# Native top-level oreslang-llvmc must use its secure source-identity
# reader before reaching the exact same restricted G1 compiler proof.
if(DEFINED G1_MAIN_COMPILER AND NOT G1_MAIN_COMPILER STREQUAL "")
    execute_process(COMMAND "${G1_MAIN_COMPILER}" --gpu-g1-kernel-ir "${G1_SOURCE}"
        OUTPUT_VARIABLE main_ir ERROR_VARIABLE main_error RESULT_VARIABLE main_rc)
    if(NOT main_rc EQUAL 0 OR NOT main_ir STREQUAL actual)
        message(FATAL_ERROR "GPU-G1-SOURCE-TEST-008: native main compiler disagrees with restricted .atto compiler: ${main_error}")
    endif()
    execute_process(COMMAND "${G1_MAIN_COMPILER}" --target=host --gpu-g1-kernel-ir "${G1_SOURCE}"
        OUTPUT_VARIABLE mixed_stdout ERROR_VARIABLE mixed_error RESULT_VARIABLE mixed_rc)
    if(mixed_rc EQUAL 0 OR NOT mixed_stdout STREQUAL "")
        message(FATAL_ERROR "GPU-G1-SOURCE-TEST-009: target/compiler flags bypassed isolated G1 source mode")
    endif()
endif()
file(WRITE "${ir_path}" "${actual}")
execute_process(COMMAND "${G1_CLANG}" -x ir -S -emit-llvm -Wno-override-module
    -o "${parsed_path}" "${ir_path}"
    RESULT_VARIABLE clang_rc ERROR_VARIABLE clang_error)
if(NOT clang_rc EQUAL 0 OR NOT EXISTS "${parsed_path}")
    message(FATAL_ERROR "GPU-G1-SOURCE-TEST-004: LLVM IR parser rejected compiled .atto kernel: ${clang_error}")
endif()
file(READ "${G1_SOURCE}" source)
string(REPLACE "if i >= n" "if i > n" mutated "${source}")
if(mutated STREQUAL source)
    message(FATAL_ERROR "GPU-G1-SOURCE-TEST-005: bounds mutation did not apply")
endif()
set(invalid_path "${G1_WORK}/invalid-g1.atto")
file(WRITE "${invalid_path}" "${mutated}")
execute_process(COMMAND "${G1_COMPILER}" "${invalid_path}"
    OUTPUT_VARIABLE rejected_stdout ERROR_VARIABLE rejected_stderr RESULT_VARIABLE rejected_rc)
if(rejected_rc EQUAL 0 OR NOT rejected_stdout STREQUAL "")
    message(FATAL_ERROR "GPU-G1-SOURCE-TEST-006: invalid bounds accepted or emitted LLVM IR")
endif()
if(DEFINED G1_MAIN_COMPILER AND NOT G1_MAIN_COMPILER STREQUAL "")
    execute_process(COMMAND "${G1_MAIN_COMPILER}" --gpu-g1-kernel-ir "${invalid_path}"
        OUTPUT_VARIABLE main_negative_stdout ERROR_VARIABLE main_negative_error
        RESULT_VARIABLE main_negative_rc)
    if(main_negative_rc EQUAL 0 OR NOT main_negative_stdout STREQUAL "")
        message(FATAL_ERROR "GPU-G1-SOURCE-TEST-010: native main compiler admitted mutated bounds")
    endif()
endif()
file(WRITE "${invalid_path}" "${source}\nsuspends fnc hostile() => void {}\n")
execute_process(COMMAND "${G1_COMPILER}" "${invalid_path}"
    OUTPUT_VARIABLE trailing_stdout ERROR_VARIABLE trailing_stderr RESULT_VARIABLE trailing_rc)
if(trailing_rc EQUAL 0 OR NOT trailing_stdout STREQUAL "")
    message(FATAL_ERROR "GPU-G1-SOURCE-TEST-007: extra executable source accepted")
endif()
if(DEFINED G1_MAIN_COMPILER AND NOT G1_MAIN_COMPILER STREQUAL "")
    execute_process(COMMAND "${G1_MAIN_COMPILER}" --gpu-g1-kernel-ir "${invalid_path}"
        OUTPUT_VARIABLE trailing_main_stdout ERROR_VARIABLE trailing_main_error
        RESULT_VARIABLE trailing_main_rc)
    if(trailing_main_rc EQUAL 0 OR NOT trailing_main_stdout STREQUAL "")
        message(FATAL_ERROR "GPU-G1-SOURCE-TEST-011: native main compiler accepted hidden host source")
    endif()
endif()
message(STATUS "PASS G1 .atto source -> canonical checked LLVM IR -> Clang; two invalid source mutations rejected without artifact (NO PTX OR GPU)")
