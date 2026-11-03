# Compiler-backed NEGATIVE source admission only. The native LLVM backend
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
    "initializer-spawn")
set(source_unqualified-spawn
    "fnc main(): int { return thread.spawn(1); }\n")
set(source_qualified-spawn
    "fnc main(): int { return std.thread.spawn(1); }\n")
set(source_lexical-whitespace
    "fnc main(): int { return thread . spawn (1); }\n")
set(source_initializer-spawn
    "fnc main(): int { let int result = thread.spawn(1); return result; }\n")

foreach(case_name IN LISTS cases)
  set(path "${WORKDIR}/${case_name}.ores")
  file(WRITE "${path}" "${source_${case_name}}")
  file(GLOB before RELATIVE "${WORKDIR}" "${WORKDIR}/*")
  execute_process(
    COMMAND "${COMPILER}" "${path}"
    WORKING_DIRECTORY "${WORKDIR}"
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
  file(GLOB after RELATIVE "${WORKDIR}" "${WORKDIR}/*")
  if(NOT "${before}" STREQUAL "${after}")
    message(FATAL_ERROR "${case_name} produced compiler artifacts")
  endif()
endforeach()
