# Real oreslang-llvmc read-only source-set integrity admission.
# This is NOT a compiled guest, actor linker, AST, IR or runtime test.
foreach(required COMPILER NODE TOOL MANIFEST EXPECTED_SHA OUTPUT_DIR)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "missing ${required}")
  endif()
endforeach()

file(MAKE_DIRECTORY "${OUTPUT_DIR}")
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef nonce)
set(package "${OUTPUT_DIR}/counter-${nonce}")
execute_process(
  COMMAND "${NODE}" "${TOOL}" materialize "${package}"
  RESULT_VARIABLE materialized
  OUTPUT_VARIABLE materialized_note
  ERROR_VARIABLE materialized_error
)
if(NOT "${materialized}" STREQUAL "0" OR
   NOT materialized_note MATCHES "MATERIALIZED 7 pinned sources; GUEST_NOT_EXECUTED")
  message(FATAL_ERROR "canonical Counter materializer failed: ${materialized_error}")
endif()
set(manifest_copy "${package}/actor-source-set.v1")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E copy "${MANIFEST}" "${manifest_copy}"
  RESULT_VARIABLE copied
)
if(NOT "${copied}" STREQUAL "0")
  message(FATAL_ERROR "cannot copy trusted actor source-set manifest")
endif()

set(verify_args "--verify-actor-source-set-only"
  "--expected-source-set-git-sha=${EXPECTED_SHA}" "${manifest_copy}")
execute_process(
  COMMAND "${COMPILER}" ${verify_args}
  RESULT_VARIABLE verified
  OUTPUT_VARIABLE receipt
  ERROR_VARIABLE diagnostic
)
if(NOT "${verified}" STREQUAL "0" OR NOT diagnostic STREQUAL ""
   OR NOT receipt STREQUAL "ACTOR_SOURCE_SET_INTEGRITY_ONLY units=7 namespace=counter actor=Counter NO_AST_NO_IR_NO_GUEST\n")
  message(FATAL_ERROR "real compiler source-set integrity admission failed: ${diagnostic} ${receipt}")
endif()

# The compiler must never silently accept an untrusted manifest hash.
execute_process(
  COMMAND "${COMPILER}" "--verify-actor-source-set-only"
    "--expected-source-set-git-sha=0000000000000000000000000000000000000000"
    "${manifest_copy}"
  RESULT_VARIABLE bad_hash
  OUTPUT_VARIABLE bad_hash_stdout
  ERROR_VARIABLE bad_hash_error
)
if("${bad_hash}" STREQUAL "0" OR NOT bad_hash_stdout STREQUAL ""
   OR NOT bad_hash_error MATCHES "untrusted manifest Git blob")
  message(FATAL_ERROR "incorrect trusted SHA was not rejected before source admission")
endif()

# Changing the actual Atto actor source must invalidate a manifest-pinned blob.
file(APPEND "${package}/src/counter/receive.atto" "\n")
execute_process(
  COMMAND "${COMPILER}" ${verify_args}
  RESULT_VARIABLE tampered
  OUTPUT_VARIABLE tampered_stdout
  ERROR_VARIABLE tampered_error
)
if("${tampered}" STREQUAL "0" OR NOT tampered_stdout STREQUAL ""
   OR NOT tampered_error MATCHES "source Git blob mismatch")
  message(FATAL_ERROR "tampered receive source escaped compiler manifest verification")
endif()

# A symlink must not become an unchecked file-origin authority.
file(REMOVE "${package}/src/counter/receive.atto")
file(CREATE_LINK "${package}/src/counter/actor.atto"
  "${package}/src/counter/receive.atto" SYMBOLIC RESULT created_link)
if(NOT created_link STREQUAL "0")
  message(FATAL_ERROR "symlink negative could not be created: ${created_link}")
endif()
execute_process(
  COMMAND "${COMPILER}" ${verify_args}
  RESULT_VARIABLE symlinked
  OUTPUT_VARIABLE symlink_stdout
  ERROR_VARIABLE symlink_error
)
if("${symlinked}" STREQUAL "0" OR NOT symlink_stdout STREQUAL ""
   OR NOT symlink_error MATCHES "source symlink")
  message(FATAL_ERROR "source symlink escaped compiler admission")
endif()

# Rewriting the manifest itself cannot authorize a new source inventory.
file(APPEND "${manifest_copy}" "\n")
execute_process(
  COMMAND "${COMPILER}" ${verify_args}
  RESULT_VARIABLE rewritten
  OUTPUT_VARIABLE rewritten_stdout
  ERROR_VARIABLE rewritten_error
)
if("${rewritten}" STREQUAL "0" OR NOT rewritten_stdout STREQUAL ""
   OR NOT rewritten_error MATCHES "untrusted manifest Git blob")
  message(FATAL_ERROR "rewritten manifest escaped trusted Git blob requirement")
endif()
message(STATUS "LLVM source-set compiler integrity-only gate PASS; NO_AST_NO_IR_NO_GUEST")
