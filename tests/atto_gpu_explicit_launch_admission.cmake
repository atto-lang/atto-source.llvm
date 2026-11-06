# Compiler-backed native admission check. This must not be confused with
# successful execution of an AttoLang GPU kernel or a physical GPU provider.
if(NOT DEFINED COMPILER OR NOT DEFINED WORKDIR)
  message(FATAL_ERROR "COMPILER and WORKDIR are required")
endif()
file(MAKE_DIRECTORY "${WORKDIR}")

# A plain function literally called 'launch' is still legal; the new
# GPU expression must be contextual and must not reserve the identifier.
set(positive "${WORKDIR}/launch-identifier-control.ores")
file(WRITE "${positive}" "pub fnc launch(int x): int { return x; }\npub fnc main(): int { return launch(7); }\n")
execute_process(COMMAND "${COMPILER}" "${positive}"
  WORKING_DIRECTORY "${WORKDIR}"
  RESULT_VARIABLE positive_result OUTPUT_VARIABLE positive_stdout ERROR_VARIABLE positive_stderr
  TIMEOUT 15)
if(NOT "${positive_result}" STREQUAL "0")
  message(FATAL_ERROR "ordinary launch() scalar control did not compile: ${positive_stderr}")
endif()

set(cases "explicit" "invalid-block" "wrong-placement")
set(source_explicit
  "pub fnc main(): int { return launch saxpy(1) on gpu(device) {grid: [1,1,1], block: [64,1,1], queue: queue}; }\n")
set(source_invalid-block
  "pub fnc main(): int { return launch saxpy(1) on gpu(device) {grid: [1,1,1], block: [0,1,1], queue: queue}; }\n")
set(source_wrong-placement
  "pub fnc main(): int { return launch saxpy(1) on cpu(device) {grid: [1,1,1], block: [64,1,1], queue: queue}; }\n")
foreach(case_name IN LISTS cases)
  set(input "${WORKDIR}/${case_name}.ores")
  file(WRITE "${input}" "${source_${case_name}}")
  file(GLOB before RELATIVE "${WORKDIR}" "${WORKDIR}/*")
  execute_process(COMMAND "${COMPILER}" "${input}"
    WORKING_DIRECTORY "${WORKDIR}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error
    TIMEOUT 15)
  if("${result}" STREQUAL "0")
    message(FATAL_ERROR "${case_name}: unsupported physical GPU launch was compiled")
  endif()
  if(NOT "${error}" MATCHES "GPU-LAUNCH-008")
    message(FATAL_ERROR "${case_name}: missing explicit GPU admission diagnostic: ${error}")
  endif()
  if(NOT "${error}" MATCHES "no implicit CPU fallback")
    message(FATAL_ERROR "${case_name}: missing no-fallback guarantee: ${error}")
  endif()
  if(NOT "${output}" STREQUAL "")
    message(FATAL_ERROR "${case_name}: invalid GPU launch emitted standard output")
  endif()
  file(GLOB after RELATIVE "${WORKDIR}" "${WORKDIR}/*")
  if(NOT "${before}" STREQUAL "${after}")
    message(FATAL_ERROR "${case_name}: invalid GPU launch emitted a compiler artifact")
  endif()
endforeach()
