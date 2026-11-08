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
"define class Point as\n  pub final int x: 20;\nend\npub fnc main(): int {\n  let Point point = new Point();\n  if true {\n    const Point alias = point;\n  } fi\n  return point.x;\n}\n")
execute_process(
  COMMAND "${COMPILER}" --aot-profile=actor-region-v1 "${branch_negative}"
  RESULT_VARIABLE branch_status OUTPUT_VARIABLE branch_ir
  ERROR_VARIABLE branch_diagnostic TIMEOUT 30)
if(branch_status STREQUAL "0" OR NOT branch_ir STREQUAL "")
  message(FATAL_ERROR "actor-region-v1 accepted a branch-scoped class alias or emitted partial IR")
endif()
string(FIND "${branch_diagnostic}" "branch-scoped class owners" branch_expected)
if(branch_expected EQUAL -1)
  message(FATAL_ERROR "actor-region-v1 branch-owner negative returned unexpected diagnostic: ${branch_diagnostic}")
endif()

message(STATUS "actor-region-v1 compiler IR: allocation/read/write/drop and helper/branch owner rejection PASS (NOT guest execution)")
