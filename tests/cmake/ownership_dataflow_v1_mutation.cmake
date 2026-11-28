# Mutate an isolated HEADER COPY and compile the unchanged 54-case tests.
# A compile failure, signal/crash or unrelated test failure is NOT a kill.
cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED ORIGINAL_HEADER OR NOT DEFINED TEST_SOURCE OR
   NOT DEFINED BASELINE_EXECUTABLE OR NOT DEFINED CXX_COMPILER OR
   NOT DEFINED WORK_DIR)
    message(FATAL_ERROR "required arguments missing")
endif()
if(NOT EXISTS "${ORIGINAL_HEADER}" OR NOT EXISTS "${TEST_SOURCE}" OR
   NOT EXISTS "${BASELINE_EXECUTABLE}")
    message(FATAL_ERROR "baseline header/source/executable missing")
endif()
execute_process(
    COMMAND "${BASELINE_EXECUTABLE}"
    RESULT_VARIABLE baseline_status
    OUTPUT_VARIABLE baseline_out
    ERROR_VARIABLE baseline_err
    TIMEOUT 90)
set(baseline_receipt
    "OWNERSHIP_DATAFLOW_V1|positive=15|negative=39|attempted=54|failed=0|result=PASS")
string(FIND "${baseline_out}" "${baseline_receipt}" baseline_pos)
if(NOT "${baseline_status}" STREQUAL "0" OR baseline_pos EQUAL -1)
    message(FATAL_ERROR "baseline 54-case suite is not green: ${baseline_status}\n${baseline_out}\n${baseline_err}")
endif()
file(READ "${ORIGINAL_HEADER}" original)
file(MAKE_DIRECTORY "${WORK_DIR}")
set(mutants drop_guard mutable_read_guard join_guard cleanup_order)
set(killed 0)
foreach(mutant IN LISTS mutants)
    if(mutant STREQUAL "drop_guard")
        set(needle "case OwnershipV1OpKind::Drop:\n                require_owned(owner);\n                if (owner.shared_borrows || owner.mutable_borrow)")
        set(replacement "case OwnershipV1OpKind::Drop:\n                require_owned(owner);\n                if (false && (owner.shared_borrows || owner.mutable_borrow))")
        set(expected_case "rejects_borrowed_drop")
    elseif(mutant STREQUAL "mutable_read_guard")
        set(needle "if (owner.mutable_borrow) fail(\"read conflicts with mutable borrow\");")
        set(replacement "if (false && owner.mutable_borrow) fail(\"read conflicts with mutable borrow\");")
        set(expected_case "rejects_read_during_mutable_borrow")
    elseif(mutant STREQUAL "join_guard")
        set(needle "else if (!(incoming[successor] == state))")
        set(replacement "else if (false && !(incoming[successor] == state))")
        set(expected_case "rejects_join_mismatch")
    elseif(mutant STREQUAL "cleanup_order")
        set(needle "return state[lhs].acquisition_order >\n                                   state[rhs].acquisition_order;")
        set(replacement "return state[lhs].acquisition_order <\n                                   state[rhs].acquisition_order;")
        set(expected_case "normal_cleanup_reverse_order")
    else()
        message(FATAL_ERROR "unknown mutant name")
    endif()
    string(FIND "${original}" "${needle}" first_pos)
    string(FIND "${original}" "${needle}" last_pos REVERSE)
    if(first_pos EQUAL -1 OR NOT first_pos EQUAL last_pos)
        message(FATAL_ERROR "mutation anchor not found exactly once: ${mutant}")
    endif()
    string(REPLACE "${needle}" "${replacement}" mutated "${original}")
    if(mutated STREQUAL original)
        message(FATAL_ERROR "mutation was a no-op: ${mutant}")
    endif()
    set(dir "${WORK_DIR}/${mutant}")
    file(MAKE_DIRECTORY "${dir}/include/oreslang/backend")
    file(WRITE "${dir}/include/oreslang/backend/OwnershipDataflowV1.hpp" "${mutated}")
    set(exe "${dir}/ownership_v1_mutant")
    execute_process(
        COMMAND "${CXX_COMPILER}" -std=c++17
            -Wall -Wextra -Wpedantic -Werror
            "-I${dir}/include" "${TEST_SOURCE}" -o "${exe}"
        RESULT_VARIABLE compile_status
        OUTPUT_VARIABLE compile_out
        ERROR_VARIABLE compile_err
        TIMEOUT 120)
    if(NOT "${compile_status}" STREQUAL "0")
        message(FATAL_ERROR
            "mutant compile failure is NOT a kill: ${mutant}\n${compile_out}\n${compile_err}")
    endif()
    execute_process(
        COMMAND "${exe}"
        RESULT_VARIABLE mutant_status
        OUTPUT_VARIABLE mutant_out
        ERROR_VARIABLE mutant_err
        TIMEOUT 90)
    string(REGEX MATCH
        "OWNERSHIP_CFG\\|case=${expected_case}\\|kind=(positive|negative)\\|result=FAIL"
        expected_failure "${mutant_out}")
    # Require complete, explicitly failing 54-case output; crashes,
    # timeouts, partial suites, and unrelated failures are not mutant kills.
    string(REGEX MATCH
        "OWNERSHIP_DATAFLOW_V1\\|positive=15\\|negative=39\\|attempted=54\\|failed=[1-9][0-9]*\\|result=FAIL"
        aggregate_failure "${mutant_out}")
    if(NOT "${mutant_status}" MATCHES "^[1-9][0-9]*$" OR
       expected_failure STREQUAL "" OR
       aggregate_failure STREQUAL "")
        message(FATAL_ERROR
            "mutant survived or failed without targeted semantic regression: ${mutant}\n"
            "status=${mutant_status}\n${mutant_out}\n${mutant_err}")
    endif()
    math(EXPR killed "${killed} + 1")
    message(STATUS "OWNERSHIP_CFG_MUTATION|mutant=${mutant}|expected_case=${expected_case}|result=KILLED")
endforeach()
if(NOT killed EQUAL 4)
    message(FATAL_ERROR "expected all four implementation mutants killed")
endif()
message(STATUS "OWNERSHIP_CFG_MUTATION|baseline=PASS|mutants=4|killed=4|result=PASS")
