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

string(FIND "${stderr_text}" "NATIVE_MACRO_EXPANDER_REQUIRED" diagnostic_index)
if(diagnostic_index EQUAL -1)
    message(FATAL_ERROR
        "macro rejection missed NATIVE_MACRO_EXPANDER_REQUIRED: ${stderr_text}")
endif()

if(NOT stdout_text STREQUAL "")
    message(FATAL_ERROR
        "rejected macro source emitted stdout/partial LLVM IR: ${stdout_text}")
endif()
