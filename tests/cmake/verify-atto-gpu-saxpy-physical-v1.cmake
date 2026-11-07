# Physical hardware CTest must consume device source emitted by
# oreslang-llvmc from the exact .atto fixture, never synthesize it locally.
if(NOT DEFINED COMPILER OR NOT DEFINED PROBE OR NOT DEFINED SOURCE)
  message(FATAL_ERROR "physical GPU gate requires compiler, probe, source")
endif()
if(NOT EXISTS "${COMPILER}" OR NOT EXISTS "${PROBE}" OR NOT EXISTS "${SOURCE}")
  message(FATAL_ERROR "physical GPU gate missing executable or fixture")
endif()
execute_process(
  COMMAND "${COMPILER}" --emit-opencl-gpu-v1 "${SOURCE}"
  RESULT_VARIABLE compiled OUTPUT_VARIABLE artifact
  ERROR_VARIABLE compiler_errors TIMEOUT 30)
if(NOT "${compiled}" STREQUAL "0" OR "${artifact}" STREQUAL "")
  message(FATAL_ERROR "physical GPU gate has no native-compiler artifact: ${compiler_errors}")
endif()
string(FIND "${artifact}" "__kernel void atto_saxpy_f32" canonical)
if(canonical EQUAL -1)
  message(FATAL_ERROR "physical GPU gate rejected unexpected emitted kernel")
endif()
string(SHA256 name "${SOURCE}|${PROBE}|${COMPILER}")
set(artifact_path "${CMAKE_CURRENT_BINARY_DIR}/atto-saxpy-${name}.cl")
file(WRITE "${artifact_path}" "${artifact}")
execute_process(
  COMMAND "${PROBE}" "${SOURCE}" "${artifact_path}"
  RESULT_VARIABLE physical OUTPUT_VARIABLE device_log
  ERROR_VARIABLE device_errors TIMEOUT 50)
file(REMOVE "${artifact_path}")
if("${physical}" STREQUAL "77")
  message(STATUS "SKIP:no-compatible-physical-opencl-gpu")
  return()
endif()
if(NOT "${physical}" STREQUAL "0")
  message(FATAL_ERROR "physical GPU launch or oracle failed: ${device_log} ${device_errors}")
endif()
string(FIND "${device_log}" "ATTO_SAXPY_PHYSICAL_GPU_PASSED device=" marker)
if(marker EQUAL -1)
  message(FATAL_ERROR "physical GPU probe returned without a device completion marker")
endif()
message(STATUS "PHYSICAL_GPU_EXECUTED: native AttoLang compiler artifact matched GPU probe, device: ${device_log}")
