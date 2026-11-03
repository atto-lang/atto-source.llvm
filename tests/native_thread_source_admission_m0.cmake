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
    "spawn-with-comment")
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
