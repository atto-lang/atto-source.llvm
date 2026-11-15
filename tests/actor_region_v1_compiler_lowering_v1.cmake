if(NOT DEFINED COMPILER OR NOT EXISTS "${COMPILER}")
  message(FATAL_ERROR "actor-region-v1 compiler test requires COMPILER")
endif()
if(NOT DEFINED ROOT OR NOT EXISTS "${ROOT}/tests/fixtures/actor-region-v1-point-main.atto")
  message(FATAL_ERROR "actor-region-v1 compiler test requires ROOT fixtures")
endif()

set(source "${ROOT}/tests/fixtures/actor-region-v1-point-main.atto")
execute_process(
  COMMAND "${COMPILER}" --aot-profile=actor-region-v1 "${source}"
  RESULT_VARIABLE status OUTPUT_VARIABLE ir ERROR_VARIABLE diagnostic
  TIMEOUT 30)
if(NOT status STREQUAL "0")
  message(FATAL_ERROR "actor-region-v1 positive compiler fixture rejected: ${diagnostic}")
endif()

foreach(symbol IN ITEMS
    ores_actor_region_guest_v1_begin
    ores_actor_region_guest_v1_alloc
    ores_actor_region_guest_v1_read_i64
    ores_actor_region_guest_v1_write_i64
    ores_actor_region_guest_v1_drop
    ores_actor_region_guest_v1_end)
  string(FIND "${ir}" "@${symbol}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "actor-region-v1 generated IR lacks runtime symbol @${symbol}")
  endif()
endforeach()
string(FIND "${ir}" "actor.region.drop.offset" cleanup_at)
if(cleanup_at EQUAL -1)
  message(FATAL_ERROR "actor-region-v1 generated IR lacks explicit destructor lowering")
endif()

# A class owner in a helper function is outside the initial single-root-frame
# ABI. It must fail before emitting LLVM IR.
set(negative "${CMAKE_CURRENT_BINARY_DIR}/actor-region-v1-helper-owner.atto")
file(WRITE "${negative}"
"define class Point as\n  pub final int x: 20;\nend\nfnc helper(): int {\n  let Point point = new Point();\n  return point.x;\n}\npub fnc main(): int { return helper(); }\n")
execute_process(
  COMMAND "${COMPILER}" --aot-profile=actor-region-v1 "${negative}"
  RESULT_VARIABLE negative_status OUTPUT_VARIABLE negative_ir
  ERROR_VARIABLE negative_diagnostic TIMEOUT 30)
if(negative_status STREQUAL "0" OR NOT negative_ir STREQUAL "")
  message(FATAL_ERROR "actor-region-v1 accepted a non-root class owner or emitted partial IR")
endif()
string(FIND "${negative_diagnostic}" "class owners are restricted to main/0" expected_diagnostic)
if(expected_diagnostic EQUAL -1)
  message(FATAL_ERROR "actor-region-v1 negative returned an unexpected diagnostic: ${negative_diagnostic}")
endif()

# Branch-scoped class aliases require path-sensitive drops and must also
# be rejected before any IR is printed, even though the root object is valid.
set(branch_negative "${CMAKE_CURRENT_BINARY_DIR}/actor-region-v1-branch-owner.atto")
file(WRITE "${branch_negative}"
"define class Point as\n  pub final int x: 20;\nend\npub fnc main(): int {\n  let Point point = new Point();\n  if true then\n    const Point alias = point;\n  fi\n  return point.x;\n}\n")
execute_process(
  COMMAND "${COMPILER}" --aot-profile=actor-region-v1 "${branch_negative}"
  RESULT_VARIABLE branch_status OUTPUT_VARIABLE branch_ir
  ERROR_VARIABLE branch_diagnostic TIMEOUT 30)
if(branch_status STREQUAL "0" OR NOT branch_ir STREQUAL "")
  message(FATAL_ERROR "actor-region-v1 accepted a branch-scoped class alias or emitted partial IR")
endif()
# Main's ownership checker now rejects the non-Copy class alias itself
# (explicit borrow required) before the actor-region branch-owner gate; either
# fail-closed diagnostic proves no branch-scoped owner reaches IR.
if(NOT branch_diagnostic MATCHES "branch-scoped class owners|class is non-Copy; use an explicit borrow")
  message(FATAL_ERROR "actor-region-v1 branch-owner negative returned unexpected diagnostic: ${branch_diagnostic}")
endif()

# Class-typed parameters and returns would transfer an owner across the
# root frame. The initial profile has no ABI for that transfer and must reject.
set(parameter_negative "${CMAKE_CURRENT_BINARY_DIR}/actor-region-v1-class-parameter.atto")
file(WRITE "${parameter_negative}"
"define class Point as\n  pub final int x: 20;\nend\nfnc read(Point value): int { return value.x; }\npub fnc main(): int {\n  let Point point = new Point();\n  return read(point);\n}\n")
execute_process(
  COMMAND "${COMPILER}" --aot-profile=actor-region-v1 "${parameter_negative}"
  RESULT_VARIABLE parameter_status OUTPUT_VARIABLE parameter_ir
  ERROR_VARIABLE parameter_diagnostic TIMEOUT 30)
if(parameter_status STREQUAL "0" OR NOT parameter_ir STREQUAL "")
  message(FATAL_ERROR "actor-region-v1 accepted a class-valued helper parameter or emitted partial IR")
endif()
# Main's ownership checker rejects a by-value class parameter before the
# profile gate; the profile itself says "class-valued parameters and returns".
if(NOT parameter_diagnostic MATCHES "class-valued parameters and returns|by-value class parameter 'value' requires exclusive borrow")
  message(FATAL_ERROR "actor-region-v1 class-parameter negative returned unexpected diagnostic: ${parameter_diagnostic}")
endif()

set(return_negative "${CMAKE_CURRENT_BINARY_DIR}/actor-region-v1-class-return.atto")
file(WRITE "${return_negative}"
"define class Point as\n  pub final int x: 20;\nend\nfnc make(): Point {\n  let Point point = new Point();\n  return point;\n}\npub fnc main(): int {\n  let Point point = new Point();\n  return point.x;\n}\n")
execute_process(
  COMMAND "${COMPILER}" --aot-profile=actor-region-v1 "${return_negative}"
  RESULT_VARIABLE return_status OUTPUT_VARIABLE return_ir
  ERROR_VARIABLE return_diagnostic TIMEOUT 30)
if(return_status STREQUAL "0" OR NOT return_ir STREQUAL "")
  message(FATAL_ERROR "actor-region-v1 accepted a class-valued return or emitted partial IR")
endif()
# The reused sealed class-field checker rejects class returns first.
if(NOT return_diagnostic MATCHES "class-valued parameters and returns|sealed scalar AOT rejects unmodeled function, ownership or suspension roots")
  message(FATAL_ERROR "actor-region-v1 class-return negative returned unexpected diagnostic: ${return_diagnostic}")
endif()

message(STATUS "actor-region-v1 compiler IR: alloc/read/write/drop + helper, branch, parameter and return rejection PASS (NOT guest execution)")
