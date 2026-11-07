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

# The actual native parser now creates checked source-only GPU MIR.
# Valid source must still fail with 008; malformed geometry/placement are
# rejected earlier with specific diagnostics and NO emitted artifact.
set(cases
  "explicit" "logical" "empty-logical" "invalid-block"
  "zero-grid" "negative-grid" "wrong-placement" "host-geometry"
  "zero-divisor" "mixed-geometry" "literal-argument")
set(source_explicit
  "pub fnc main(): int { return launch saxpy(x) on gpu(device) {grid: [1,1,1], block: [64,1,1], queue: queue}; }\n")
set(source_logical
  "pub fnc main(): int { return launch saxpy(x) on gpu(device) {over: [257], group: [256], queue: queue}; }\n")
set(source_empty-logical
  "pub fnc main(): int { return launch saxpy(x) on gpu(device) {over: [0], group: [256], queue: queue}; }\n")
set(source_invalid-block
  "pub fnc main(): int { return launch saxpy(x) on gpu(device) {grid: [1,1,1], block: [0,1,1], queue: queue}; }\n")
set(source_zero-grid
  "pub fnc main(): int { return launch saxpy(x) on gpu(device) {grid: [0,1,1], block: [64,1,1], queue: queue}; }\n")
set(source_negative-grid
  "pub fnc main(): int { return launch saxpy(x) on gpu(device) {grid: [-1,1,1], block: [64,1,1], queue: queue}; }\n")
set(source_wrong-placement
  "pub fnc main(): int { return launch saxpy(x) on cpu(device) {grid: [1,1,1], block: [64,1,1], queue: queue}; }\n")
set(source_host-geometry
  "pub fnc main(): int { return launch saxpy(x) on gpu(device) {grid: [hostCall(),1,1], block: [64,1,1], queue: queue}; }\n")
set(source_zero-divisor
  "pub fnc main(): int { return launch saxpy(x) on gpu(device) {grid: [ceil_div(n,0),1,1], block: [64,1,1], queue: queue}; }\n")
set(source_mixed-geometry
  "pub fnc main(): int { return launch saxpy(x) on gpu(device) {over: [16], block: [64], queue: queue}; }\n")
set(source_literal-argument
  "pub fnc main(): int { return launch saxpy(1) on gpu(device) {grid: [1,1,1], block: [64,1,1], queue: queue}; }\n")
set(expected_explicit "GPU-LAUNCH-008")
set(expected_logical "GPU-LAUNCH-008")
set(expected_empty-logical "GPU-LAUNCH-008")
set(expected_invalid-block "GPU-LAUNCH-003")
set(expected_zero-grid "GPU-LAUNCH-003")
set(expected_negative-grid "GPU-LAUNCH-003")
set(expected_wrong-placement "GPU-LAUNCH-004")
set(expected_host-geometry "GPU-LAUNCH-003")
set(expected_zero-divisor "GPU-LAUNCH-003")
set(expected_mixed-geometry "GPU-LAUNCH-003")
set(expected_literal-argument "GPU-LAUNCH-002")
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
  if(NOT "${error}" MATCHES "${expected_${case_name}}")
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
