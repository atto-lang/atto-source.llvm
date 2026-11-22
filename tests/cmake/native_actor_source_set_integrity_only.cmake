# Real oreslang-llvmc read-only source-set integrity admission.
# This is NOT a compiled guest, actor linker, AST, IR or runtime test.
foreach(required COMPILER NODE GIT TOOL MANIFEST EXPECTED_SHA OUTPUT_DIR)
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

# A second compiler mode parses only cross-file declaration *headers*
# from the same already-hashed source snapshot. It cannot emit guest IR.
execute_process(
  COMMAND "${COMPILER}" "--verify-actor-headers-only"
    "--expected-source-set-git-sha=${EXPECTED_SHA}" "${manifest_copy}"
  RESULT_VARIABLE checked_headers
  OUTPUT_VARIABLE header_receipt
  ERROR_VARIABLE header_diagnostic
)
if(NOT "${checked_headers}" STREQUAL "0" OR NOT header_diagnostic STREQUAL ""
   OR NOT header_receipt STREQUAL "ACTOR_SOURCE_HEADERS_ONLY units=7 namespace=counter actor=Counter contract=CounterContract field=total NO_BODY_TYPECHECK_NO_IR_NO_GUEST\n")
  message(FATAL_ERROR "source-verified declared-actor header check failed: ${header_diagnostic} ${header_receipt}")
endif()

# Prove that genuine *new* trusted source blobs can pass the integrity
# gate while invalid declared actor headers still fail at header admission.
# Every test rewrites the source hash + manifest hash (Git blob semantics);
# this is not an ordinary tamper/checksum negative in disguise.
function(require_actor_header_negative scenario relative needle replacement)
  set(variant "${OUTPUT_DIR}/header-${nonce}-${scenario}")
  execute_process(
    COMMAND "${NODE}" "${TOOL}" materialize "${variant}"
    RESULT_VARIABLE materialized_variant
    OUTPUT_QUIET ERROR_VARIABLE variant_error
  )
  if(NOT "${materialized_variant}" STREQUAL "0")
    message(FATAL_ERROR "cannot materialize ${scenario}: ${variant_error}")
  endif()
  set(target "${variant}/${relative}")
  file(READ "${target}" old_bytes)
  string(FIND "${old_bytes}" "${needle}" found)
  if(found LESS 0)
    message(FATAL_ERROR "missing ${scenario} mutation anchor")
  endif()
  string(REPLACE "${needle}" "${replacement}" new_bytes "${old_bytes}")
  if(new_bytes STREQUAL old_bytes)
    message(FATAL_ERROR "no actual ${scenario} source mutation")
  endif()
  file(WRITE "${target}" "${new_bytes}")
  execute_process(
    COMMAND "${GIT}" hash-object "${package}/${relative}"
    RESULT_VARIABLE original_status
    OUTPUT_VARIABLE original_blob OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  execute_process(
    COMMAND "${GIT}" hash-object "${target}"
    RESULT_VARIABLE edited_status
    OUTPUT_VARIABLE edited_blob OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if(NOT "${original_status}" STREQUAL "0" OR
     NOT "${edited_status}" STREQUAL "0" OR
     original_blob STREQUAL edited_blob)
    message(FATAL_ERROR "cannot pin actual ${scenario} source Git blobs")
  endif()
  file(READ "${MANIFEST}" manifest_data)
  string(REPLACE "${original_blob}" "${edited_blob}" new_manifest "${manifest_data}")
  if(new_manifest STREQUAL manifest_data)
    message(FATAL_ERROR "manifest did not authenticate edited ${scenario} source")
  endif()
  set(edited_manifest "${variant}/actor-source-set.v1")
  file(WRITE "${edited_manifest}" "${new_manifest}")
  execute_process(
    COMMAND "${GIT}" hash-object "${edited_manifest}"
    RESULT_VARIABLE manifest_status
    OUTPUT_VARIABLE edited_manifest_blob OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if(NOT "${manifest_status}" STREQUAL "0")
    message(FATAL_ERROR "cannot pin ${scenario} updated trusted manifest")
  endif()
  execute_process(
    COMMAND "${COMPILER}" "--verify-actor-headers-only"
      "--expected-source-set-git-sha=${edited_manifest_blob}" "${edited_manifest}"
    RESULT_VARIABLE syntax_denied
    OUTPUT_VARIABLE syntax_stdout
    ERROR_VARIABLE syntax_error
  )
  if("${syntax_denied}" STREQUAL "0" OR NOT syntax_stdout STREQUAL ""
     OR NOT syntax_error MATCHES "ACTOR_DECL_HEADER_REJECT")
    message(FATAL_ERROR "source-verified ${scenario} header not denied: ${syntax_error} ${syntax_stdout}")
  endif()
endfunction()

require_actor_header_negative("wrong-namespace" "src/counter/actor.atto"
  "namespace counter" "namespace impostor")
require_actor_header_negative("wrong-owner" "src/counter/state.atto"
  "state on actor Counter" "state on actor Phantom")
require_actor_header_negative("spoof-receive" "src/counter/receive.atto"
  "external mtd (Counter c)" "pub external mtd (Counter c)")
require_actor_header_negative("mail-type" "src/counter/receive.atto"
  "ActorMail<int>" "ActorMail<String>")
require_actor_header_negative("contract-method" "src/counter/method.atto"
  "echo(int n)" "different(int n)")

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
