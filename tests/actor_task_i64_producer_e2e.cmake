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
        OR NOT emitted MATCHES "define i32 @atto_actor_task_i64_resume_probe"
        OR NOT emitted MATCHES "llvm\\.s(add|sub|mul)\\.with\\.overflow\\.i64")
    message(FATAL_ERROR "missing checked compiler-generated Atto Task producer ABI")
endif()
if(emitted MATCHES "llvm\\.trap" OR emitted MATCHES "oreslang_source_actor_"
        OR emitted MATCHES "SourceTaskRuntime" OR emitted MATCHES "pthread")
    message(FATAL_ERROR "producer-only IR illegally includes a host actor/Task fallback")
endif()
foreach(opt IN ITEMS O0 O2)
    set(object "${OUTPUT_DIR}/atto-producer-${opt}.o")
    set(binary "${OUTPUT_DIR}/atto-producer-${opt}")
    execute_process(
        COMMAND "${CLANG}" "-${opt}" -c -x ir "${ir}" -o "${object}"
        ERROR_VARIABLE ir_errors RESULT_VARIABLE ir_result)
    if(NOT ir_result STREQUAL "0")
        message(FATAL_ERROR "Clang ${opt} Task LLVM IR object generation failed: ${ir_errors}")
    endif()
    execute_process(
        COMMAND "${CLANG}" "-${opt}" -std=c11 -Wall -Wextra -Wpedantic -Werror
            "${HARNESS}" "${object}" -o "${binary}"
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
    if(TEST_BRIDGE)
        if(NOT DEFINED SOURCE_ROOT OR "${SOURCE_ROOT}" STREQUAL "")
            message(FATAL_ERROR "compiled Task/WAKE bridge probe requires SOURCE_ROOT")
        endif()
        set(bridge "${OUTPUT_DIR}/atto-producer-native-wake-${opt}")
        execute_process(
            COMMAND "${CLANG}" "-${opt}" -std=c11 -Wall -Wextra -Wpedantic -Werror
                -UNDEBUG -pthread
                "-I${SOURCE_ROOT}/src/main/cpp"
                -DATTO_ACTOR_TASK_PRODUCER_PROBE=1
                "-DATTO_ACTOR_TASK_PRODUCER_EXPECTED=${EXPECTED}"
                "${SOURCE_ROOT}/tests/native_actor_owned_task_wake_test.c"
                "${SOURCE_ROOT}/src/main/cpp/oreslang/runtime/NativeActorTaskBridge.c"
                "${SOURCE_ROOT}/src/main/cpp/oreslang/runtime/NativeActorTaskRegistry.c"
                "${SOURCE_ROOT}/src/main/cpp/oreslang/runtime/NativeActorScheduler.c"
                "${object}" -o "${bridge}"
            ERROR_VARIABLE bridge_errors RESULT_VARIABLE bridge_compile_result
            TIMEOUT 60)
        if(NOT bridge_compile_result STREQUAL "0")
            message(FATAL_ERROR "compiled Atto producer + native actor WAKE link failed: ${bridge_compile_result} ${bridge_errors}")
        endif()
        execute_process(
            COMMAND "${bridge}"
            RESULT_VARIABLE bridge_result OUTPUT_VARIABLE bridge_output
            ERROR_VARIABLE bridge_run_errors TIMEOUT 35)
        if(NOT bridge_result STREQUAL "0"
                OR NOT bridge_output MATCHES "NATIVE_ACTOR_OWNED_TASK_WAKE_PASS")
            message(FATAL_ERROR "compiled Atto producer native actor WAKE failed ${opt}: ${bridge_result} ${bridge_run_errors}")
        endif()
    endif()
endforeach()
message(STATUS "ATTO_TASK_I64_PRODUCER_IR_ONLY_O0_O2_PASS; NOT actor await parity")
