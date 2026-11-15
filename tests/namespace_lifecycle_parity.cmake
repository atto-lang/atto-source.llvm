# Source-only conformance gate: no ambient namespace package loading and no
# ordinary invocation/emission of once-only init while native lowering lacks it.
if(NOT DEFINED COMPILER OR NOT DEFINED WORK_DIR OR NOT DEFINED CLANG OR
   NOT EXISTS "${CLANG}" OR IS_DIRECTORY "${CLANG}")
  message(FATAL_ERROR "COMPILER, CLANG and WORK_DIR with a real Clang binary are required")
endif()
file(MAKE_DIRECTORY "${WORK_DIR}")

function(expect_rejection name source_text diagnostic)
  set(input "${WORK_DIR}/${name}.ores")
  file(WRITE "${input}" "${source_text}")
  # Rejected inputs must not leave native output on disk even when stdout is
  # empty. Check both common compiler output naming conventions; remove stale
  # outputs from prior CTest attempts so each case measures this invocation.
  set(artifact_suffixes ".ll" ".bc" ".o" ".obj" ".s" ".wasm" ".out" ".exe")
  foreach(stem "${WORK_DIR}/${name}" "${input}")
    foreach(suffix IN LISTS artifact_suffixes)
      file(REMOVE "${stem}${suffix}")
    endforeach()
  endforeach()
  # Snapshot the entire work tree, not only known LLVM output suffixes.
  # A rejected compiler must not create, delete, or mutate any file here.
  file(GLOB_RECURSE pre_files LIST_DIRECTORIES false RELATIVE "${WORK_DIR}" "${WORK_DIR}/*")
  # Include empty directories: a file-only snapshot cannot detect mkdir/rmdir.
  file(GLOB_RECURSE pre_entries LIST_DIRECTORIES true RELATIVE "${WORK_DIR}" "${WORK_DIR}/*")
  list(SORT pre_entries)
  set(pre_hashes)
  foreach(entry IN LISTS pre_files)
    file(SHA256 "${WORK_DIR}/${entry}" digest)
    list(APPEND pre_hashes "${digest}")
  endforeach()
  execute_process(
    COMMAND "${COMPILER}" "${input}"
    RESULT_VARIABLE status OUTPUT_VARIABLE ir ERROR_VARIABLE err
    WORKING_DIRECTORY "${WORK_DIR}"
    TIMEOUT 15)
  if(NOT "${status}" MATCHES "^[0-9]+$")
    message(FATAL_ERROR "${name} compiler did not exit normally: ${status}")
  endif()
  if(status EQUAL 0)
    message(FATAL_ERROR "${name} incorrectly admitted")
  endif()
  if(NOT ir STREQUAL "")
    message(FATAL_ERROR "${name} emitted IR on rejection")
  endif()
  foreach(stem "${WORK_DIR}/${name}" "${input}")
    foreach(suffix IN LISTS artifact_suffixes)
      if(EXISTS "${stem}${suffix}")
        message(FATAL_ERROR "${name} wrote ${stem}${suffix} on rejection")
      endif()
    endforeach()
  endforeach()
  # Detect unexpected filenames (including compiler-default a.out/IR names),
  # as well as mutation or deletion of existing source/provider fixtures.
  file(GLOB_RECURSE post_files LIST_DIRECTORIES false RELATIVE "${WORK_DIR}" "${WORK_DIR}/*")
  file(GLOB_RECURSE post_entries LIST_DIRECTORIES true RELATIVE "${WORK_DIR}" "${WORK_DIR}/*")
  list(SORT post_entries)
  if(NOT "${pre_entries}" STREQUAL "${post_entries}")
    message(FATAL_ERROR "${name} changed directory/file entries on rejection: before=[${pre_entries}], after=[${post_entries}]")
  endif()
  foreach(entry IN LISTS pre_files)
    if(NOT EXISTS "${WORK_DIR}/${entry}")
      message(FATAL_ERROR "${name} removed pre-existing ${entry} on rejection")
    endif()
  endforeach()
  foreach(entry IN LISTS post_files)
    list(FIND pre_files "${entry}" old_index)
    if(old_index EQUAL -1)
      message(FATAL_ERROR "${name} wrote unexpected ${entry} on rejection")
    endif()
    list(GET pre_hashes ${old_index} old_hash)
    file(SHA256 "${WORK_DIR}/${entry}" new_hash)
    if(NOT new_hash STREQUAL old_hash)
      message(FATAL_ERROR "${name} modified ${entry} on rejection")
    endif()
  endforeach()
  string(FIND "${err}" "${diagnostic}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "${name} expected '${diagnostic}' but saw: ${err}")
  endif()
endfunction()

expect_rejection(namespace-import
  "import namespace billing as payments from \"checkout\";\ndefine module App as pub fnc main(): int { return 0; } end\n"
  "verified manifest package index")
expect_rejection(lifecycle-init
  "define module App as pub fnc init(): int { return 1; } pub fnc main(): int { return 0; } end\n"
  "once-only reachable initialization lowering")


# A private init is still a once-only lifecycle hook, not an ordinary helper.
expect_rejection(lifecycle-private-init
  "define module App as fnc init(): int { return 1; } pub fnc main(): int { return 0; } end\n"
  "once-only reachable initialization lowering")

# Every reachable provider must be checked before the root's no-import/linked
# compiler paths can emit IR. The imported module's init must fail closed.
file(WRITE "${WORK_DIR}/provider-with-init.ores"
  "define module Provider as pub fnc init(): int { return 1; } end\n")
expect_rejection(lifecycle-imported-init
  "import module Provider from \"./provider-with-init.ores\";\ndefine module App as pub fnc main(): int { return 0; } end\n"
  "once-only reachable initialization lowering")

# A private imported hook must not bypass the same once-only lifecycle fence.
file(WRITE "${WORK_DIR}/private-provider.ores"
  "define module PrivateProvider as fnc init():int{return 1;} pub fnc answer():int{return 42;} end\n")
expect_rejection(lifecycle-imported-private-init
  "import module PrivateProvider from \"./private-provider.ores\"; pub fnc main():int{return PrivateProvider.answer();}\n"
  "once-only reachable initialization lowering")

# Reject hooks in transitive reachable dependencies, not only direct imports.
file(WRITE "${WORK_DIR}/deep-with-init.ores"
  "define module Deep as pub fnc init():int{return 1;} pub fnc answer():int{return 42;} end\n")
file(WRITE "${WORK_DIR}/middle.ores"
  "import module Deep from \"./deep-with-init.ores\"; define module Middle as pub fnc answer():int{return Deep.answer();} end\n")
expect_rejection(lifecycle-transitive-init
  "import module Middle from \"./middle.ores\"; pub fnc main():int{return Middle.answer();}\n"
  "once-only reachable initialization lowering")

# Positive cases must run real LLVM IR, not merely contain a matching
# "define @main" substring. Verify the linked guest exits with the expected
# observable value and produces no stdout/stderr.
function(expect_native_exit name source_file expected_exit)
  execute_process(
    COMMAND "${COMPILER}" "${source_file}"
    RESULT_VARIABLE compile_status OUTPUT_VARIABLE compiled_ir ERROR_VARIABLE compile_err
    WORKING_DIRECTORY "${WORK_DIR}" TIMEOUT 15)
  if(NOT "${compile_status}" STREQUAL "0" OR
     NOT compiled_ir MATCHES "define i32 @main\\(")
    message(FATAL_ERROR "${name} failed to emit native main: ${compile_status}: ${compile_err}")
  endif()
  set(ir_file "${WORK_DIR}/${name}.ll")
  set(native_file "${WORK_DIR}/${name}-native")
  if(WIN32)
    string(APPEND native_file ".exe")
  endif()
  file(WRITE "${ir_file}" "${compiled_ir}")
  # A successful linker exit must produce a fresh regular executable. A stale
  # binary from a prior CTest attempt must never satisfy the positive gate.
  file(REMOVE "${native_file}")
  if(EXISTS "${native_file}" OR IS_SYMLINK "${native_file}")
    message(FATAL_ERROR "${name} could not remove stale native output")
  endif()
  execute_process(
    COMMAND "${CLANG}" -O0 -x ir "${ir_file}" -o "${native_file}"
    RESULT_VARIABLE clang_status OUTPUT_VARIABLE clang_out ERROR_VARIABLE clang_err
    WORKING_DIRECTORY "${WORK_DIR}" TIMEOUT 20)
  if(NOT "${clang_status}" STREQUAL "0" OR NOT EXISTS "${native_file}"
     OR IS_DIRECTORY "${native_file}" OR IS_SYMLINK "${native_file}")
    message(FATAL_ERROR "${name} failed to link a fresh native executable: ${clang_status}: ${clang_err}")
  endif()
  execute_process(
    COMMAND "${native_file}"
    RESULT_VARIABLE guest_status OUTPUT_VARIABLE guest_out ERROR_VARIABLE guest_err
    WORKING_DIRECTORY "${WORK_DIR}" TIMEOUT 10)
  if(NOT "${guest_status}" STREQUAL "${expected_exit}" OR
     NOT "${guest_out}" STREQUAL "" OR NOT "${guest_err}" STREQUAL "")
    message(FATAL_ERROR "${name} native guest exit/output mismatch: ${guest_status}: ${guest_out}: ${guest_err}")
  endif()
endfunction()

# A source file in the same directory but not reached by an import must
# not enter the native compile graph or run an init hook.
file(WRITE "${WORK_DIR}/unrelated.ores"
  "define module Unrelated as pub fnc init(): int { return 1; } end\n")
file(WRITE "${WORK_DIR}/main.ores"
  "define module App as pub fnc main(): int { return 42; } end\n")
expect_native_exit(unrelated-source "${WORK_DIR}/main.ores" 42)

# A clean, explicitly imported provider must remain admitted; rejecting all
# imports would incorrectly satisfy the negative-only lifecycle cases.
file(WRITE "${WORK_DIR}/clean-provider.ores"
  "define module Clean as pub fnc answer():int{return 42;} end\n")
file(WRITE "${WORK_DIR}/clean-main.ores"
  "import module Clean from \"./clean-provider.ores\"; pub fnc main():int{return Clean.answer();}\n")
expect_native_exit(explicit-import "${WORK_DIR}/clean-main.ores" 42)
