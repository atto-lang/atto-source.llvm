# Verify that unsupported String client contract signatures are rejected by
# the real native compiler with the intended type diagnostic, not any failure.
if(NOT DEFINED ORES_COMPILER OR NOT DEFINED ORES_FIXTURE)
    message(FATAL_ERROR "missing native compiler or fixture")
endif()
execute_process(COMMAND "${ORES_COMPILER}" "${ORES_FIXTURE}"
    RESULT_VARIABLE exit_code OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if("${exit_code}" STREQUAL "0")
    message(FATAL_ERROR "String client contract was unexpectedly admitted")
endif()
string(FIND "${stderr}" "expected 'int'" expected_type)
if(expected_type EQUAL -1)
    message(FATAL_ERROR "wrong rejection reason: ${stderr}")
endif()
