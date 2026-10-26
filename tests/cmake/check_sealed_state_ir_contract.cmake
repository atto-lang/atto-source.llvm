# Exact source -> LLVM IR contract for canonical sealed actor state.
# This deliberately uses only CMake's execute_process/string primitives so the
# parity gate does not introduce a Python runtime dependency.
if(NOT DEFINED ORES_COMPILER OR NOT DEFINED ORES_FIXTURES)
    message(FATAL_ERROR "missing native compiler or sealed-state fixture directory")
endif()

function(compile_fixture fixture out_var)
    execute_process(
        COMMAND "${ORES_COMPILER}" "${ORES_FIXTURES}/${fixture}"
        RESULT_VARIABLE exit_code
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
        TIMEOUT 30)
    if(NOT "${exit_code}" STREQUAL "0")
        message(FATAL_ERROR "${fixture}: compiler exited ${exit_code}: ${stderr}")
    endif()
    if(NOT "${stderr}" STREQUAL "")
        message(FATAL_ERROR "${fixture}: unexpected stderr: ${stderr}")
    endif()
    string(FIND "${stdout}" "; ModuleID" module_id)
    if(NOT module_id EQUAL 0)
        message(FATAL_ERROR "${fixture}: missing LLVM module")
    endif()
    set(${out_var} "${stdout}" PARENT_SCOPE)
endfunction()

function(require_contains value needle detail)
    string(FIND "${value}" "${needle}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "${detail}")
    endif()
endfunction()

function(require_absent value needle detail)
    string(FIND "${value}" "${needle}" found)
    if(NOT found EQUAL -1)
        message(FATAL_ERROR "${detail}")
    endif()
endfunction()

compile_fixture("sealed-state-empty-positive.ores" empty_ir)
require_contains("${empty_ir}" "oreslang_actor_Idle_on_message"
    "zero-schema actor missing compiler-generated message handler")
require_absent("${empty_ir}" "call i64 @ores_actor_i64_heap_field_read"
    "zero-schema actor emitted a hidden state read")
require_absent("${empty_ir}" "call void @ores_actor_i64_heap_field_write"
    "zero-schema actor emitted a hidden state write")
string(REGEX MATCHALL "[^\n]*@oreslang_source_actor_spawn\\([^\n]*" spawn_lines "${empty_ir}")
list(LENGTH spawn_lines spawn_count)
if(NOT spawn_count EQUAL 1)
    message(FATAL_ERROR "empty actor must emit exactly one source actor spawn")
endif()
list(GET spawn_lines 0 spawn_line)
string(FIND "${spawn_line}" "i64 1" one_slot)
if(one_slot EQUAL -1)
    message(FATAL_ERROR "empty actor must receive a one-slot runtime-only physical heap")
endif()

compile_fixture("sealed-state-read-positive.ores" read_ir)
require_contains("${read_ir}" "call i64 @ores_actor_i64_heap_field_read"
    "read callback emitted no actor state read")
require_absent("${read_ir}" "call void @ores_actor_i64_heap_field_write"
    "read callback unexpectedly emitted actor state write")

compile_fixture("sealed-state-write-positive.ores" write_ir)
require_contains("${write_ir}" "call i64 @ores_actor_i64_heap_field_read"
    "read-modify-write callback emitted no state read")
require_contains("${write_ir}" "call void @ores_actor_i64_heap_field_write"
    "read-modify-write callback emitted no state write")

compile_fixture("sealed-state-write-only-positive.ores" write_only_ir)
require_contains("${write_only_ir}" "call void @ores_actor_i64_heap_field_write"
    "write-only callback emitted no state write")
require_absent("${write_only_ir}" "call i64 @ores_actor_i64_heap_field_read"
    "write-only callback emitted a hidden state read")

compile_fixture("sealed-state-using-write-positive.ores" lexical_write_ir)
require_contains("${lexical_write_ir}" "call void @ores_actor_i64_heap_field_write"
    "lexical using state.write emitted no state write")
require_absent("${lexical_write_ir}" "call i64 @ores_actor_i64_heap_field_read"
    "lexical write-only using acquired a hidden state read")
