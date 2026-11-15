# Source-free negative gate for the mutation script's destructive workspace setup.
cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED ROOT OR NOT IS_DIRECTORY "${ROOT}" OR
   NOT DEFINED BUILD OR NOT IS_DIRECTORY "${BUILD}")
  message(FATAL_ERROR "guard selftest requires source ROOT and build BUILD")
endif()
set(probe "${BUILD}/sealed-aot-guard-selftest")
if(IS_SYMLINK "${probe}")
  message(FATAL_ERROR "symlinked probe workspace denied")
endif()
file(REMOVE_RECURSE "${probe}")
file(MAKE_DIRECTORY "${probe}/source/sealed-aot-profile-mutations")
file(WRITE "${probe}/source/sealed-aot-profile-mutations/keep.txt" "MUST_NOT_BE_DELETED")
# Unsafe BUILD == ROOT must fail before any recursive removal.
execute_process(COMMAND "${CMAKE_COMMAND}"
  "-DROOT=${probe}/source" "-DBUILD=${probe}/source"
  -P "${ROOT}/tests/sealed_aot_profile_mutations.cmake"
  RESULT_VARIABLE outcome OUTPUT_QUIET ERROR_VARIABLE diag TIMEOUT 15)
if(outcome STREQUAL "0" OR
   NOT EXISTS "${probe}/source/sealed-aot-profile-mutations/keep.txt")
  message(FATAL_ERROR "unsafe build/source alias was accepted or deleted a sentinel")
endif()
string(FIND "${diag}" "canonical separate build directory required" reason)
if(reason EQUAL -1)
  message(FATAL_ERROR "unsafe alias did not hit the intended fail-closed gate: ${diag}")
endif()
# Missing BUILD must also fail before touching the sentinel.
execute_process(COMMAND "${CMAKE_COMMAND}"
  "-DROOT=${probe}/source"
  -P "${ROOT}/tests/sealed_aot_profile_mutations.cmake"
  RESULT_VARIABLE absent OUTPUT_QUIET ERROR_VARIABLE absent_diag TIMEOUT 15)
if(absent STREQUAL "0" OR
   NOT EXISTS "${probe}/source/sealed-aot-profile-mutations/keep.txt")
  message(FATAL_ERROR "missing BUILD was accepted or removed a sentinel")
endif()
message(STATUS "PASS source-free sealed-AOT workspace deletion guards")
file(REMOVE_RECURSE "${probe}")
