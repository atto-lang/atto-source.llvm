# This negative is a compiler-executable admission, NOT native guest proof.
if(NOT DEFINED COMPILER OR NOT EXISTS "${COMPILER}"
   OR NOT DEFINED SOURCE OR NOT EXISTS "${SOURCE}")
    message(FATAL_ERROR "NOT_EXECUTED: missing native compiler/Bytes source")
endif()
execute_process(
    COMMAND "${COMPILER}" "${SOURCE}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE generated
    ERROR_VARIABLE diagnostics
    TIMEOUT 20)
if("${status}" STREQUAL "0" OR NOT "${generated}" STREQUAL ""
   OR "${diagnostics}" STREQUAL "")
    message(FATAL_ERROR
        "Native ActorMail<Bytes> unsupported source incorrectly accepted, emitted LLVM IR, or failed without diagnostic")
endif()
message(STATUS "NATIVE_ACTOR_BYTES_SOURCE=UNSUPPORTED_FAIL_CLOSED; real guest NOT_EXECUTED")
