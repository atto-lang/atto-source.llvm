# Host-only LLVM IR parsing/admission. Does not emit PTX or execute a GPU.
# Call with -DG1_EMITTER=<built host program> -DG1_CLANG=<clang> -DG1_WORK=<build path>.
foreach(key IN ITEMS G1_EMITTER G1_CLANG G1_WORK)
    if(NOT DEFINED ${key} OR "${${key}}" STREQUAL "")
        message(FATAL_ERROR "GPU-G1-TEST-001: ${key} is required")
    endif()
endforeach()
file(MAKE_DIRECTORY "${G1_WORK}")
set(source "${G1_WORK}/gpu-g1-saxpy.ll")
set(verified "${G1_WORK}/gpu-g1-saxpy-verified.ll")
set(mutated "${G1_WORK}/gpu-g1-saxpy-mutated.ll")
set(rejected "${G1_WORK}/gpu-g1-saxpy-rejected.ll")
file(REMOVE "${source}" "${verified}" "${mutated}" "${rejected}")
execute_process(COMMAND "${G1_EMITTER}" --emit-llvm-ir
    OUTPUT_FILE "${source}" RESULT_VARIABLE emitter_rc ERROR_VARIABLE emitter_err)
if(NOT emitter_rc EQUAL 0)
    message(FATAL_ERROR "GPU-G1-TEST-002: typed IR emission failed: ${emitter_err}")
endif()
file(READ "${source}" ir)
if(NOT ir MATCHES "define ptx_kernel void @atto_g1_saxpy_f32\\(" OR
   NOT ir MATCHES "icmp ult i64 %index, %n" OR
   NOT ir MATCHES "ptr addrspace\\(1\\)")
    message(FATAL_ERROR "GPU-G1-TEST-003: emitted IR did not preserve target/guard/device memory")
endif()
execute_process(COMMAND "${G1_CLANG}" -x ir -S -emit-llvm -Wno-override-module
    -o "${verified}" "${source}"
    RESULT_VARIABLE verify_rc OUTPUT_VARIABLE verify_out ERROR_VARIABLE verify_err)
if(NOT verify_rc EQUAL 0 OR NOT EXISTS "${verified}")
    message(FATAL_ERROR "GPU-G1-TEST-004: LLVM IR parser rejected source: ${verify_err}")
endif()
# Negative mutation must be rejected by the real LLVM IR parser, not merely by
# regex source-shape tests. No artifact may survive the rejected compilation.
string(REPLACE "icmp ult i64 %index, %n" "icmp ult i64 %unbound, %n" mutated_ir "${ir}")
if(mutated_ir STREQUAL ir)
    message(FATAL_ERROR "GPU-G1-TEST-005: mutation did not change IR")
endif()
file(WRITE "${mutated}" "${mutated_ir}")
execute_process(COMMAND "${G1_CLANG}" -x ir -S -emit-llvm -Wno-override-module
    -o "${rejected}" "${mutated}"
    RESULT_VARIABLE mutate_rc OUTPUT_VARIABLE mutate_out ERROR_VARIABLE mutate_err)
if(mutate_rc EQUAL 0 OR EXISTS "${rejected}")
    message(FATAL_ERROR "GPU-G1-TEST-006: invalid SSA mutation was accepted or produced artifact")
endif()
message(STATUS "PASS GPU-G1 typed IR LLVM parser, invalid SSA negative mutation and no-artifact gate; HOST ONLY")
