cmake_minimum_required(VERSION 3.18)
# Real Atto native linked guest test, not the source-free IR ABI fixture.
# Only run this under the full LLVM SDK CTest target after oreslang-llvmc builds.
foreach(required IN ITEMS ATTO_ROOT ATTO_COMPILER ATTO_CLANG ATTO_WORK)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "missing linked Atto guest proof setting")
  endif()
endforeach()
file(MAKE_DIRECTORY "${ATTO_WORK}")
set(ir "${ATTO_WORK}/guest-log.ll")
set(guest_obj "${ATTO_WORK}/guest-log.o")
set(runtime_obj "${ATTO_WORK}/native-telemetry.o")
set(proof_obj "${ATTO_WORK}/native-guest-proof.o")
set(binary "${ATTO_WORK}/verified-atto-guest")

execute_process(
  COMMAND "${ATTO_COMPILER}"
    "--source-log-ids=examples/telemetry/log-info-ids.json"
    "examples/telemetry/log-info-verified.atto"
  WORKING_DIRECTORY "${ATTO_ROOT}"
  OUTPUT_FILE "${ir}"
  RESULT_VARIABLE compile_result
  ERROR_QUIET)
if(NOT "${compile_result}" STREQUAL "0")
  message(FATAL_ERROR "Atto frontend/LLVM codegen failed: no guest proof")
endif()

execute_process(
  COMMAND "${ATTO_CLANG}" -x ir -O1 -c "${ir}" -o "${guest_obj}"
  RESULT_VARIABLE ir_result ERROR_QUIET)
if(NOT "${ir_result}" STREQUAL "0")
  message(FATAL_ERROR "Atto compiler emitted IR that Clang cannot assemble")
endif()

execute_process(
  COMMAND "${ATTO_CLANG}" -std=c11 -O1 -UNDEBUG -Wall -Wextra
    -Wpedantic -Werror -I "${ATTO_ROOT}/src/main/cpp"
    -c "${ATTO_ROOT}/src/main/cpp/oreslang/runtime/NativeTelemetrySpan.c"
    -o "${runtime_obj}"
  RESULT_VARIABLE runtime_result ERROR_QUIET)
if(NOT "${runtime_result}" STREQUAL "0")
  message(FATAL_ERROR "native telemetry runtime did not compile")
endif()
execute_process(
  COMMAND "${ATTO_CLANG}" -std=c11 -O1 -UNDEBUG -Wall -Wextra
    -Wpedantic -Werror -I "${ATTO_ROOT}/src/main/cpp"
    -c "${ATTO_ROOT}/tests/native_guest_log_info_full_aot_hook_v1.c"
    -o "${proof_obj}"
  RESULT_VARIABLE hook_result ERROR_QUIET)
if(NOT "${hook_result}" STREQUAL "0")
  message(FATAL_ERROR "linked guest proof hook did not compile")
endif()
execute_process(
  COMMAND "${ATTO_CLANG}"
    "${guest_obj}" "${runtime_obj}" "${proof_obj}" -o "${binary}"
  RESULT_VARIABLE link_result ERROR_QUIET)
if(NOT "${link_result}" STREQUAL "0")
  message(FATAL_ERROR "Atto-compiled guest native link failed")
endif()

execute_process(
  COMMAND "${binary}"
  RESULT_VARIABLE run_result
  OUTPUT_VARIABLE observed
  ERROR_QUIET)
if(NOT "${run_result}" STREQUAL "42" OR
   NOT "${observed}" MATCHES "LLVM_NATIVE_LOG_INFO_GENERATED_GUEST_PASS")
  message(FATAL_ERROR
    "Atto-compiled native guest did not return 42 with two verified site receipts")
endif()
message(STATUS "LLVM_NATIVE_LOG_INFO_LINKED_ATTO_GUEST_PASS:42")
