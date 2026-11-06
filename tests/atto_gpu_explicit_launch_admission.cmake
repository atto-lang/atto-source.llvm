# Compiler-backed native admission check. This must not be confused with
# successful execution of an AttoLang GPU kernel or a physical GPU provider.
if(NOT DEFINED COMPILER OR NOT DEFINED WORKDIR)
  message(FATAL_ERROR "COMPILER and WORKDIR are required")
endif()
file(MAKE_DIRECTORY "${WORKDIR}")

# A plain function literally called 'launch' is still legal; the new
# GPU expression must be contextual and must not reserve the identifier.
set(control_dir "${WORKDIR}/scalar-control")
file(MAKE_DIRECTORY "${control_dir}")
set(positive "${control_dir}/launch-identifier-control.ores")
file(WRITE "${positive}" "pub fnc launch(int x): int { return x; }\npub fnc main(): int { return launch(7); }\n")
execute_process(COMMAND "${COMPILER}" "${positive}"
  WORKING_DIRECTORY "${control_dir}"
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
  # Every rejected source gets an isolated directory. No prior positive
  # compilation artifact can conceal a same-name overwrite.
  set(case_dir "${WORKDIR}/${case_name}")
  file(MAKE_DIRECTORY "${case_dir}")
  set(input "${case_dir}/${case_name}.ores")
  file(WRITE "${input}" "${source_${case_name}}")
  file(SHA256 "${input}" input_digest_before)
  file(GLOB before RELATIVE "${case_dir}" "${case_dir}/*")
  execute_process(COMMAND "${COMPILER}" "${input}"
    WORKING_DIRECTORY "${case_dir}"
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
  file(SHA256 "${input}" input_digest_after)
  if(NOT "${input_digest_before}" STREQUAL "${input_digest_after}")
    message(FATAL_ERROR "${case_name}: compiler altered source bytes")
  endif()
  file(GLOB after RELATIVE "${case_dir}" "${case_dir}/*")
  if(NOT "${before}" STREQUAL "${after}")
    message(FATAL_ERROR "${case_name}: invalid GPU launch emitted a compiler artifact")
  endif()
endforeach()
