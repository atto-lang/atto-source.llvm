if(NOT DEFINED ORES_LLVM_COMPILER OR NOT DEFINED ORES_SOURCE OR NOT DEFINED ORES_OUTPUT)
    message(FATAL_ERROR "missing OresVM compile fixture arguments")
endif()
execute_process(COMMAND "${ORES_LLVM_COMPILER}" "--isolate-entry=tenant_step" "${ORES_SOURCE}"
    RESULT_VARIABLE status OUTPUT_FILE "${ORES_OUTPUT}" ERROR_VARIABLE details)
if(NOT status STREQUAL "0")
    file(REMOVE "${ORES_OUTPUT}")
    message(FATAL_ERROR "OresVM isolate compilation failed: ${details}")
endif()
