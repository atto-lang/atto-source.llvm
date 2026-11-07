# Always-executed HOST ONLY regression for GPU-G2 source-to-device artifact
# provenance; proves that stale PTX cannot satisfy a selected CTest run.
if(NOT DEFINED COMPILER OR NOT DEFINED FIXTURE OR NOT DEFINED WORKDIR OR
   NOT DEFINED CHAIN_SCRIPT OR NOT DEFINED OFFLINE_SCRIPT)
  message(FATAL_ERROR "GPU-G2-006: negative test missing arguments")
endif()
file(MAKE_DIRECTORY "${WORKDIR}")
execute_process(COMMAND "${COMPILER}" "${FIXTURE}"
  RESULT_VARIABLE compiler_result OUTPUT_VARIABLE ir ERROR_VARIABLE compiler_error
  TIMEOUT 15)
if(NOT compiler_result STREQUAL "0" OR NOT ir MATCHES "ptx_kernel void @saxpy")
  message(FATAL_ERROR "GPU-G2-006: positive .atto source compiler broken: ${compiler_error}")
endif()
file(READ "${FIXTURE}" source)
foreach(mode IN ITEMS mutated-source broken-llc)
  set(case_dir "${WORKDIR}/${mode}")
  file(MAKE_DIRECTORY "${case_dir}")
  file(WRITE "${case_dir}/kernel.ptx" "STALE PTX WHICH MUST NOT SURVIVE FAILURE")
  if(mode STREQUAL "mutated-source")
    set(input "${case_dir}/mutated.atto")
    file(WRITE "${input}" "${source}\n// negative mutation\n")
  else()
    set(input "${FIXTURE}")
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}"
    "-DCOMPILER=${COMPILER}" "-DFIXTURE=${input}"
    "-DWORKDIR=${case_dir}" "-DLLC=${CMAKE_COMMAND}"
    "-DHARDWARE=${CMAKE_COMMAND}" "-DOFFLINE_SCRIPT=${OFFLINE_SCRIPT}"
    -P "${CHAIN_SCRIPT}"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 35)
  if(status STREQUAL "0" OR EXISTS "${case_dir}/kernel.ptx")
    message(FATAL_ERROR "GPU-G2-006: ${mode}: stale PTX accepted after failed proof")
  endif()
  if(mode STREQUAL "mutated-source" AND
     NOT error MATCHES "canonical .atto fixture hash changed")
    message(FATAL_ERROR "GPU-G2-006: mutation not caught by source identity")
  endif()
  if(mode STREQUAL "broken-llc" AND
     NOT error MATCHES "fresh source -> PTX verification failed")
    message(FATAL_ERROR "GPU-G2-006: failing LLVM backend not rejected")
  endif()
endforeach()
message(STATUS "PASS 2 GPU-G2 HOST-ONLY stale-artifact and mutation negative cases")
