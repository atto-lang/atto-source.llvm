if(NOT DEFINED HELPER OR NOT EXISTS "${HELPER}" OR
   NOT DEFINED CLANG OR NOT EXISTS "${CLANG}" OR
   NOT DEFINED WORK_DIR)
  message(FATAL_ERROR "HELPER, CLANG and WORK_DIR are required")
endif()

# A second all-provider program shares the same exact 12 source units but
# executes namespace b's four otherwise-unused providers too.
if(NOT DEFINED EXPECTED_GUEST_STATUS)
  set(EXPECTED_GUEST_STATUS 43)
endif()
file(MAKE_DIRECTORY "${WORK_DIR}")
set(ir_file "${WORK_DIR}/pinned-namespace.ll")
set(native_file "${WORK_DIR}/pinned-namespace-native")
if(WIN32)
  string(APPEND native_file ".exe")
endif()

# Never retain artifacts from an earlier invocation, including when this
# invocation fails before emitting IR. Other steps must not mistake a stale
# executable or .ll file for fresh evidence from the current exact head.
function(reject_namespace_execution reason)
  file(REMOVE "${ir_file}" "${native_file}")
  message(FATAL_ERROR "${reason}")
endfunction()
file(REMOVE "${ir_file}" "${native_file}")
if(EXISTS "${ir_file}" OR IS_SYMLINK "${ir_file}" OR
   EXISTS "${native_file}" OR IS_SYMLINK "${native_file}")
  reject_namespace_execution("could not clear stale namespace IR/executable")
endif()
if(NOT "${EXPECTED_GUEST_STATUS}" MATCHES "^(43|53)$")
  reject_namespace_execution("unexpected pinned namespace guest oracle")
endif()

execute_process(
  COMMAND "${HELPER}" ${HELPER_ARGS}
  RESULT_VARIABLE helper_status
  OUTPUT_VARIABLE ir
  ERROR_VARIABLE helper_err
  WORKING_DIRECTORY "${WORK_DIR}"
  TIMEOUT 20)

if(NOT "${helper_status}" STREQUAL "0")
  reject_namespace_execution("pinned namespace helper failed: ${helper_status}: ${helper_err}")
endif()
if(NOT "${helper_err}" STREQUAL "")
  reject_namespace_execution("pinned namespace helper wrote stderr: ${helper_err}")
endif()
if(NOT ir MATCHES "define i32 @main\\(")
  reject_namespace_execution("pinned namespace lowering did not emit a native main")
endif()

file(WRITE "${ir_file}" "${ir}")

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
  reject_namespace_execution("Clang failed to link fresh namespace guest: ${clang_status}: ${clang_err}")
endif()
if(NOT "${clang_out}" STREQUAL "")
  reject_namespace_execution("Clang unexpectedly wrote stdout: ${clang_out}")
endif()

execute_process(
  COMMAND "${native_file}"
  RESULT_VARIABLE guest_status
  OUTPUT_VARIABLE guest_out
  ERROR_VARIABLE guest_err
  WORKING_DIRECTORY "${WORK_DIR}"
  TIMEOUT 10)

if(NOT "${guest_status}" STREQUAL "${EXPECTED_GUEST_STATUS}")
  reject_namespace_execution("pinned namespace guest exit mismatch: ${guest_status}")
endif()
if(NOT "${guest_out}" STREQUAL "" OR NOT "${guest_err}" STREQUAL "")
  reject_namespace_execution("pinned namespace guest produced output: stdout=[${guest_out}] stderr=[${guest_err}]")
endif()

message(STATUS "PINNED_NAMESPACE_NATIVE_EXIT_${EXPECTED_GUEST_STATUS}=PASS")
