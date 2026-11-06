if(NOT DEFINED COMPILER OR NOT DEFINED POSITIVE OR NOT DEFINED NEGATIVE)
  message(FATAL_ERROR "GPU v1 CLI gate requires compiler and both fixtures")
endif()
execute_process(
  COMMAND "${COMPILER}" --emit-opencl-gpu-v1 "${POSITIVE}"
  RESULT_VARIABLE success OUTPUT_VARIABLE artifact ERROR_VARIABLE diagnostic
  TIMEOUT 30)
if(NOT "${success}" STREQUAL "0")
  message(FATAL_ERROR "Atto GPU source lowering did not execute: ${diagnostic}")
endif()
foreach(required IN ITEMS
  "__kernel void atto_saxpy_f32"
  "get_global_id(0)"
  "if (i >= n) return"
  "alpha * x[i]")
  string(FIND "${artifact}" "${required}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "Atto GPU source artifact missing: ${required}")
  endif()
endforeach()
execute_process(
  COMMAND "${COMPILER}" --emit-opencl-gpu-v1 "${NEGATIVE}"
  RESULT_VARIABLE rejected OUTPUT_VARIABLE invalid_artifact
  ERROR_VARIABLE rejection TIMEOUT 30)
if("${rejected}" STREQUAL "0" OR NOT "${invalid_artifact}" STREQUAL "")
  message(FATAL_ERROR "Host-effect GPU source must reject with no artifact")
endif()
execute_process(
  COMMAND "${COMPILER}" "${POSITIVE}"
  RESULT_VARIABLE ordinary_status OUTPUT_VARIABLE ordinary_ir
  ERROR_VARIABLE ordinary_diagnostic TIMEOUT 30)
if("${ordinary_status}" STREQUAL "0" OR NOT "${ordinary_ir}" STREQUAL "")
  message(FATAL_ERROR "Normal LLVM scalar mode must not silently admit GPU source")
endif()
message(STATUS "Atto GPU SAXPY source compiler CLI: positive artifact and negative gates passed; no physical GPU dispatch")
