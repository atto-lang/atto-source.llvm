# Compile an admitted .atto Task expression through the real oreslang-llvmc
# frontend, emit a standalone *producer-only* IR function, compile the output
# with Clang at O0/O2 and execute the resulting native binary.
# This is NOT an actor TASK/WAKE/await test and must not certify parity.
foreach(required IN ITEMS SOURCE_COMPILER CLANG FIXTURE HARNESS EXPECTED OUTPUT_DIR)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "missing exact producer probe input: ${required}")
    endif()
endforeach()
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
set(ir "${OUTPUT_DIR}/atto-actor-task-producer.ll")
execute_process(
    COMMAND "${SOURCE_COMPILER}" --emit-actor-task-producer-ir-only "${FIXTURE}"
    OUTPUT_FILE "${ir}" ERROR_VARIABLE compiler_errors
    RESULT_VARIABLE compiler_result)
if(NOT compiler_result STREQUAL "0")
    message(FATAL_ERROR "compiler did not emit typed actor producer IR: ${compiler_errors}")
endif()
file(READ "${ir}" emitted)
if(NOT emitted MATCHES "define i32 @atto_actor_task_i64_producer_probe"
        OR NOT emitted MATCHES "llvm\\.s(add|sub|mul)\\.with\\.overflow\\.i64")
    message(FATAL_ERROR "missing checked compiler-generated Atto Task producer ABI")
endif()
if(emitted MATCHES "llvm\\.trap" OR emitted MATCHES "oreslang_source_actor_"
        OR emitted MATCHES "SourceTaskRuntime" OR emitted MATCHES "pthread")
    message(FATAL_ERROR "producer-only IR illegally includes a host actor/Task fallback")
endif()
foreach(opt IN ITEMS O0 O2)
    set(binary "${OUTPUT_DIR}/atto-producer-${opt}")
    execute_process(
        COMMAND "${CLANG}" "-${opt}" -x ir "${ir}"
                -x c "${HARNESS}" -o "${binary}"
        ERROR_VARIABLE clang_errors RESULT_VARIABLE clang_result)
    if(NOT clang_result STREQUAL "0")
        message(FATAL_ERROR "Clang ${opt} native Task probe failed: ${clang_errors}")
    endif()
    execute_process(
        COMMAND "${binary}" "${EXPECTED}"
        OUTPUT_VARIABLE native_output
        ERROR_VARIABLE native_errors
        RESULT_VARIABLE native_result)
    if(NOT native_result STREQUAL "0"
            OR NOT native_output MATCHES "ATTO_TASK_I64_PRODUCER_IR_ONLY_PASS")
        message(FATAL_ERROR
            "compiler-generated Task producer failed native ${opt} execution: ${native_result} ${native_errors}")
    endif()
endforeach()
message(STATUS "ATTO_TASK_I64_PRODUCER_IR_ONLY_O0_O2_PASS; NOT actor await parity")
