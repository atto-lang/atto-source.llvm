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
# Production fail-stop paths use abort(): accepting any nonzero result lets
# an unrelated process exit, segfault, tool failure or timeout masquerade as
# an intentional checked-ID rejection. CMake reports SIGABRT this way on
# Unix (verified on Linux CMake 3.25); review if a host uses another spelling.
if(NOT "${exit_status}" STREQUAL "Subprocess aborted")
    message(FATAL_ERROR
        "checked actor-ID mode ${MODE} did not fail-stop with SIGABRT: ${exit_status}\\n${captured_stderr}")
endif()
string(FIND "${captured_stderr}" "${EXPECTED_DIAGNOSTIC}" expected_offset)
if(expected_offset EQUAL -1)
    message(FATAL_ERROR
        "checked actor-ID mode ${MODE} failed without its exact denial: ${exit_status}\n${captured_stderr}")
endif()
# Forbidden acceptance markers must be absent from BOTH output streams.
# Regressions often print the marker to stderr immediately before aborting.
set(all_output "${captured_stdout}\\n${captured_stderr}")
if(all_output MATCHES "SOURCE_ACTOR_CHECKED_ID_STALE_ACCEPTED|SOURCE_ACTOR_CHECKED_ID_WRAP_ACCEPTED|SOURCE_ACTOR_LEGACY_STALE_DEREF_ACCEPTED|SOURCE_ACTOR_STALE_AWAIT_CONTINUATION_EXECUTED")
    message(FATAL_ERROR
        "checked actor-ID mode ${MODE} emitted a forbidden acceptance/continuation marker")
endif()
message(STATUS "checked actor-ID denial verified for ${MODE}: ${EXPECTED_DIAGNOSTIC}")
