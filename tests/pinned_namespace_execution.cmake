if(NOT DEFINED HELPER OR NOT EXISTS "${HELPER}" OR
   NOT DEFINED CLANG OR NOT EXISTS "${CLANG}" OR
   NOT DEFINED WORK_DIR)
  message(FATAL_ERROR "HELPER, CLANG and WORK_DIR are required")
endif()

file(MAKE_DIRECTORY "${WORK_DIR}")
set(ir_file "${WORK_DIR}/pinned-namespace.ll")
set(native_file "${WORK_DIR}/pinned-namespace-native")
if(WIN32)
  string(APPEND native_file ".exe")
endif()

execute_process(
  COMMAND "${HELPER}"
  RESULT_VARIABLE helper_status
  OUTPUT_VARIABLE ir
  ERROR_VARIABLE helper_err
  WORKING_DIRECTORY "${WORK_DIR}"
  TIMEOUT 20)

if(NOT "${helper_status}" STREQUAL "0")
  message(FATAL_ERROR "pinned namespace helper failed: ${helper_status}: ${helper_err}")
endif()
if(NOT "${helper_err}" STREQUAL "")
  message(FATAL_ERROR "pinned namespace helper wrote stderr: ${helper_err}")
endif()
if(NOT ir MATCHES "define i32 @main\\(")
  message(FATAL_ERROR "pinned namespace lowering did not emit a native main")
endif()

file(WRITE "${ir_file}" "${ir}")
file(REMOVE "${native_file}")
if(EXISTS "${native_file}" OR IS_SYMLINK "${native_file}")
  message(FATAL_ERROR "could not clear stale native namespace executable")
endif()

execute_process(
  COMMAND "${CLANG}" -O0 -x ir "${ir_file}" -o "${native_file}"
  RESULT_VARIABLE clang_status
  OUTPUT_VARIABLE clang_out
  ERROR_VARIABLE clang_err
  WORKING_DIRECTORY "${WORK_DIR}"
  TIMEOUT 20)

if(NOT "${clang_status}" STREQUAL "0" OR
   NOT EXISTS "${native_file}" OR IS_DIRECTORY "${native_file}" OR
   IS_SYMLINK "${native_file}")
  message(FATAL_ERROR "Clang failed to link fresh namespace guest: ${clang_status}: ${clang_err}")
endif()
if(NOT "${clang_out}" STREQUAL "")
  message(FATAL_ERROR "Clang unexpectedly wrote stdout: ${clang_out}")
endif()

execute_process(
  COMMAND "${native_file}"
  RESULT_VARIABLE guest_status
  OUTPUT_VARIABLE guest_out
  ERROR_VARIABLE guest_err
  WORKING_DIRECTORY "${WORK_DIR}"
  TIMEOUT 10)

if(NOT "${guest_status}" STREQUAL "43")
  message(FATAL_ERROR "pinned namespace guest exit mismatch: ${guest_status}")
endif()
if(NOT "${guest_out}" STREQUAL "" OR NOT "${guest_err}" STREQUAL "")
  message(FATAL_ERROR "pinned namespace guest produced output: stdout=[${guest_out}] stderr=[${guest_err}]")
endif()

message(STATUS "PINNED_NAMESPACE_NATIVE_EXIT_43=PASS")
