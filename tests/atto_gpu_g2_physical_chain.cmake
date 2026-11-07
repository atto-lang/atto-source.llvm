# GPU-G2 hardware test must produce its own fresh PTX from the exact pinned
# .atto fixture. CTest DEPENDS is not sufficient for selected test subsets.
if(NOT DEFINED COMPILER OR NOT DEFINED FIXTURE OR NOT DEFINED WORKDIR OR
   NOT DEFINED LLC OR NOT DEFINED HARDWARE OR NOT DEFINED OFFLINE_SCRIPT)
  message(FATAL_ERROR "GPU-G2-006: missing source-to-device proof parameters")
endif()
if(NOT EXISTS "${COMPILER}" OR NOT EXISTS "${FIXTURE}" OR
   NOT EXISTS "${LLC}" OR NOT EXISTS "${HARDWARE}" OR
   NOT EXISTS "${OFFLINE_SCRIPT}")
  message(FATAL_ERROR "GPU-G2-006: compiler, source, LLVM target, hardware test or gate missing")
endif()
# Rejected or stale fixture identities must not retain any old PTX artifact.
file(MAKE_DIRECTORY "${WORKDIR}")
file(REMOVE "${WORKDIR}/kernel.ll" "${WORKDIR}/kernel.ptx")
file(SHA256 "${FIXTURE}" source_sha256)
# The SHA is the canonical GitHub G1 .atto fixture. Changing the source
# requires a reviewed update to this expected identity, never a silent fallback.
set(expected_source_sha256 "c39562871d7287634c44180dcf0bc63e6602f7b261f28895eedfed5e29671ec3")
if(NOT source_sha256 STREQUAL expected_source_sha256)
  message(FATAL_ERROR "GPU-G2-006: canonical .atto fixture hash changed")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}"
    "-DCOMPILER=${COMPILER}" "-DFIXTURE=${FIXTURE}"
    "-DWORKDIR=${WORKDIR}" "-DLLC=${LLC}" -P "${OFFLINE_SCRIPT}"
  RESULT_VARIABLE offline_result OUTPUT_VARIABLE offline_stdout
  ERROR_VARIABLE offline_stderr TIMEOUT 60)
if(NOT offline_result STREQUAL "0")
  message(FATAL_ERROR "GPU-G2-006: fresh source -> PTX verification failed: ${offline_stderr}")
endif()
if(NOT EXISTS "${WORKDIR}/kernel.ll" OR NOT EXISTS "${WORKDIR}/kernel.ptx")
  message(FATAL_ERROR "GPU-G2-006: trusted source pipeline emitted no PTX")
endif()
file(SIZE "${WORKDIR}/kernel.ptx" ptx_bytes)
if(ptx_bytes LESS 256 OR ptx_bytes GREATER 524288)
  message(FATAL_ERROR "GPU-G2-006: generated PTX has invalid size")
endif()
file(SHA256 "${WORKDIR}/kernel.ll" ir_sha256)
file(SHA256 "${WORKDIR}/kernel.ptx" ptx_sha256)
message(STATUS "GPU-G2 SOURCE->PTX VERIFIED (NOT HARDWARE PROOF): source=${source_sha256} ir=${ir_sha256} ptx=${ptx_sha256}")
execute_process(
  COMMAND "${HARDWARE}" "${WORKDIR}/kernel.ptx"
  RESULT_VARIABLE hardware_result OUTPUT_VARIABLE hardware_stdout
  ERROR_VARIABLE hardware_stderr TIMEOUT 180)
if(NOT hardware_result STREQUAL "0")
  message(FATAL_ERROR "GPU-G2 PHYSICAL CUDA: execution or oracle failed: ${hardware_stderr}")
endif()
if(NOT hardware_stdout MATCHES "PASS 75 PHYSICAL CUDA SAXPY/cancellation cases")
  message(FATAL_ERROR "GPU-G2 PHYSICAL CUDA: expected 75 verified hardware cases")
endif()
message(STATUS "GPU-G2 PHYSICAL CUDA: ${hardware_stdout}")
