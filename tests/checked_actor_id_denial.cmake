# Strict native negative-case runner: an unrelated crash must not count as
# successful stale-identity rejection. CTest WILL_FAIL alone is insufficient.
cmake_minimum_required(VERSION 3.20)
foreach(required IN ITEMS EXECUTABLE MODE EXPECTED_DIAGNOSTIC)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "missing ${required} for checked actor-ID denial")
    endif()
endforeach()
execute_process(
    COMMAND "${EXECUTABLE}" "${MODE}"
    RESULT_VARIABLE exit_status
    OUTPUT_VARIABLE captured_stdout
    ERROR_VARIABLE captured_stderr
    TIMEOUT 25)
if("${exit_status}" STREQUAL "0" OR
        "${exit_status}" MATCHES "[Tt]imeout|[Nn]o such file|[Nn]ot found")
    message(FATAL_ERROR
        "checked actor-ID mode ${MODE} unexpectedly succeeded or did not execute: ${exit_status}")
endif()
string(FIND "${captured_stderr}" "${EXPECTED_DIAGNOSTIC}" expected_offset)
if(expected_offset EQUAL -1)
    message(FATAL_ERROR
        "checked actor-ID mode ${MODE} failed without its exact denial: ${exit_status}\n${captured_stderr}")
endif()
if(captured_stdout MATCHES "SOURCE_ACTOR_CHECKED_ID_STALE_ACCEPTED|SOURCE_ACTOR_CHECKED_ID_WRAP_ACCEPTED|SOURCE_ACTOR_LEGACY_STALE_DEREF_ACCEPTED")
    message(FATAL_ERROR
        "checked actor-ID mode ${MODE} printed a forbidden accepted marker")
endif()
message(STATUS "checked actor-ID denial verified for ${MODE}: ${EXPECTED_DIAGNOSTIC}")
