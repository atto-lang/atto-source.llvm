# Compiler-backed AttoLang (*.atto) NEGATIVE source admission only. The native LLVM backend
# must never reinterpret std.thread.spawn as an ordinary scalar class method,
# emit LLVM IR, or launch a blocking host pthread by accident.
if(NOT DEFINED COMPILER OR NOT DEFINED WORKDIR)
  message(FATAL_ERROR "COMPILER and WORKDIR are required")
endif()
file(MAKE_DIRECTORY "${WORKDIR}")
set(cases
    "unqualified-spawn"
    "qualified-spawn"
    "lexical-whitespace"
    "initializer-spawn"
    "generic-spawn"
    "generic-qualified-spawn"
    "qualified-spawn-whitespace"
    "spawn-with-comment"
    "bare-thread-spawn-value"
    "bare-qualified-spawn-value")
set(source_unqualified-spawn
    "fnc main(): int { return thread.spawn(1); }\n")
set(source_qualified-spawn
    "fnc main(): int { return std.thread.spawn(1); }\n")
set(source_lexical-whitespace
    "fnc main(): int { return thread . spawn (1); }\n")
set(source_initializer-spawn
    "fnc main(): int { let int result = thread.spawn(1); return result; }\n")


set(source_generic-spawn
    "fnc main(): int { return thread.spawn<int>(1); }\n")
set(source_generic-qualified-spawn
    "fnc main(): int { return std.thread.spawn<int>(1); }\n")
set(source_qualified-spawn-whitespace
    "fnc main(): int { return std . thread . spawn (1); }\n")
set(source_spawn-with-comment
    "fnc main(): int { return thread /* comment */ .spawn(1); }\n")

set(source_bare-thread-spawn-value
    "fnc main(): int { return thread.spawn; }\n")
set(source_bare-qualified-spawn-value
    "fnc main(): int { return std.thread.spawn; }\n")

# Shared conformance PR #346 carries the exact same canonical .atto files.
# Pin their raw SHA256 so guest admission cannot silently drift in a backend.
# These hashes are not proof that a compiler job executed.
set(expected_fixture_sha256_unqualified-spawn "e4c8aa98773d50cca989e4d8f77fe1401d6712b05f40f73c9f244aaf11690ee1")
set(expected_fixture_sha256_qualified-spawn "156ceb3fc9ee54d1b79355193001425a5a91e49d626ffb8de54add3fa5e207c3")
set(expected_fixture_sha256_lexical-whitespace "191d75ec32646d330443a5fb81d35eddabd3363a2b5bc31bc0f4fc596525066f")
set(expected_fixture_sha256_initializer-spawn "e9efa82f607fddd30d63867a3477245e8a91085e30d5cf09e581c7582f709bcd")
set(expected_fixture_sha256_generic-spawn "a71d5c4be754a8c1e2066bfc8104955b31de2a3946545f21d2d7c74251aee94f")
set(expected_fixture_sha256_generic-qualified-spawn "6b1479d2b0939051b4a9f3dbcd5c10caf0f15a41bd123fbc1bcd3843fce375f1")
set(expected_fixture_sha256_qualified-spawn-whitespace "65e28686f797d7d03a1281787c871ef79259f11195105732699ff18d1e1ccb72")
set(expected_fixture_sha256_spawn-with-comment "318400109296abc0974192f4c64a9219d226f23eed2b878dc510ac91857513ef")
set(expected_fixture_sha256_bare-thread-spawn-value "139fa88eb8529bbfd67c4a716343de3e8a4d8f27650b802be730c97cb667b449")
set(expected_fixture_sha256_bare-qualified-spawn-value "138727bd53e98d2907eeb99c984bb63dfd9e9c6930ce931637e101547a8ffe66")

foreach(case_name IN LISTS cases)
  # Isolate each attempt in a clean, case-specific directory. Otherwise
  # an earlier compiler artifact of the same name could survive a rerun and
  # escape a before/after filename comparison.
  set(case_dir "${WORKDIR}/${case_name}")
  file(REMOVE_RECURSE "${case_dir}")
  file(MAKE_DIRECTORY "${case_dir}")
  set(path "${case_dir}/${case_name}.atto")
  file(WRITE "${path}" "${source_${case_name}}")
  file(SHA256 "${path}" expected_source_hash)
  if(NOT expected_source_hash STREQUAL "${expected_fixture_sha256_${case_name}}")
    message(FATAL_ERROR "${case_name} differs from canonical shared .atto conformance fixture")
  endif()
  execute_process(
    COMMAND "${COMPILER}" "${path}"
    WORKING_DIRECTORY "${case_dir}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
    TIMEOUT 10)
  if("${result}" STREQUAL "0")
    message(FATAL_ERROR "${case_name} unexpectedly compiled thread.spawn")
  endif()
  if(NOT "${error}" MATCHES "THREAD_SPAWN_BACKEND_UNSUPPORTED")
    message(FATAL_ERROR "${case_name} missed fail-closed diagnostic: ${error}")
  endif()
  if(NOT "${output}" STREQUAL "")
    message(FATAL_ERROR "${case_name} emitted native stdout on failure")
  endif()
  file(GLOB after RELATIVE "${case_dir}" "${case_dir}/*" "${case_dir}/.*")
  if(NOT "${after}" STREQUAL "${case_name}.atto")
    message(FATAL_ERROR "${case_name} produced compiler artifacts: ${after}")
  endif()
  file(SHA256 "${path}" actual_source_hash)
  if(NOT expected_source_hash STREQUAL actual_source_hash)
    message(FATAL_ERROR "${case_name} modified its admitted source file")
  endif()
endforeach()

# Preserve contextual ordinary identifiers: a user function named thread is
# not itself the std.thread capability, and its ordinary scalar call remains
# legal. This positive control prevents reserving all uses of "thread".
set(positive_dir "${WORKDIR}/ordinary-thread-identifier")
file(REMOVE_RECURSE "${positive_dir}")
file(MAKE_DIRECTORY "${positive_dir}")
set(ordinary_source "${positive_dir}/ordinary-thread-identifier.atto")
file(WRITE "${ordinary_source}"
    "fnc thread(int value): int { return value; }\nfnc main(): int { return thread(7); }\n")
file(SHA256 "${ordinary_source}" expected_positive_hash)
if(NOT expected_positive_hash STREQUAL "462615576a254098159747b2b7b15fe89f0edb5c93f168e0ae67ef59f66a7c2e")
  message(FATAL_ERROR "positive .atto source differs from canonical conformance fixture")
endif()
execute_process(
  COMMAND "${COMPILER}" "${ordinary_source}"
  WORKING_DIRECTORY "${positive_dir}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE ir
  ERROR_VARIABLE error
  TIMEOUT 10)
if(NOT "${result}" STREQUAL "0" OR NOT "${ir}" MATCHES "define ")
  message(FATAL_ERROR "ordinary non-thread-spawn function unexpectedly rejected: ${error}")
endif()
file(GLOB after RELATIVE "${positive_dir}" "${positive_dir}/*" "${positive_dir}/.*")
if(NOT "${after}" STREQUAL "ordinary-thread-identifier.atto")
  message(FATAL_ERROR "ordinary control unexpectedly emitted an artifact: ${after}")
endif()
file(SHA256 "${ordinary_source}" actual_positive_hash)
if(NOT expected_positive_hash STREQUAL actual_positive_hash)
  message(FATAL_ERROR "ordinary control modified its admitted source")
endif()
