if(NOT DEFINED COMPILER OR NOT DEFINED FIXTURE)
    message(FATAL_ERROR "COMPILER and FIXTURE are required")
endif()

execute_process(
    COMMAND "${COMPILER}" "${FIXTURE}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout_text
    ERROR_VARIABLE stderr_text
    TIMEOUT 20)

if(NOT result EQUAL 1)
    message(FATAL_ERROR "macro rejection must exit 1, got '${result}'")
endif()

# Default to the preexisting strict macro diagnostic. A source-level
# unresolved runtime name may fail earlier in ordinary binding; that case
# still must fail before emitting any IR.
if(NOT DEFINED DIAGNOSTIC)
    set(DIAGNOSTIC "NATIVE_MACRO_EXPANDER_REQUIRED")
endif()
if(NOT DIAGNOSTIC STREQUAL "ANY")
    string(FIND "${stderr_text}" "${DIAGNOSTIC}" diagnostic_index)
    if(diagnostic_index EQUAL -1)
        message(FATAL_ERROR
            "macro rejection missed '${DIAGNOSTIC}': ${stderr_text}")
    endif()
elseif(stderr_text STREQUAL "")
    message(FATAL_ERROR "macro rejection produced no diagnostic")
endif()

if(NOT stdout_text STREQUAL "")
    message(FATAL_ERROR
        "rejected macro source emitted stdout/partial LLVM IR: ${stdout_text}")
endif()
