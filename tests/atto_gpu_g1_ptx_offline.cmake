# Offline NVPTX backend gate. This is compiler machine-code generation, not
# a CUDA Driver API launch or proof of device memory/queue/fence execution.
if(NOT DEFINED COMPILER OR NOT DEFINED FIXTURE OR NOT DEFINED WORKDIR OR
   NOT DEFINED LLC)
  message(FATAL_ERROR "COMPILER, FIXTURE, WORKDIR and LLC required")
endif()
file(MAKE_DIRECTORY "${WORKDIR}")
file(REMOVE "${WORKDIR}/kernel.ll" "${WORKDIR}/kernel.ptx")
execute_process(COMMAND "${COMPILER}" "${FIXTURE}"
  RESULT_VARIABLE parse_status OUTPUT_VARIABLE ir ERROR_VARIABLE parse_error TIMEOUT 15)
if(NOT parse_status STREQUAL "0" OR NOT parse_error STREQUAL "")
  message(FATAL_ERROR "G1 canonical source-to-IR failed: ${parse_error}")
endif()
file(WRITE "${WORKDIR}/kernel.ll" "${ir}")
execute_process(
  COMMAND "${LLC}" -march=nvptx64 -mcpu=sm_70 -filetype=asm
          "${WORKDIR}/kernel.ll" -o "${WORKDIR}/kernel.ptx"
  RESULT_VARIABLE ptx_status OUTPUT_VARIABLE ptx_stdout ERROR_VARIABLE ptx_error
  TIMEOUT 30)
if(NOT ptx_status STREQUAL "0" OR NOT ptx_stdout STREQUAL "")
  message(FATAL_ERROR "offline LLVM NVPTX lowering failed: ${ptx_error}")
endif()
file(READ "${WORKDIR}/kernel.ptx" ptx)
foreach(fragment IN ITEMS ".entry saxpy" "ld.global.f32" "st.global.f32"
                          "mul.rn.f32" "add.rn.f32")
  string(FIND "${ptx}" "${fragment}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "missing NVPTX backend PTX operation: ${fragment}")
  endif()
endforeach()
file(SHA256 "${FIXTURE}" source_digest)
file(SHA256 "${WORKDIR}/kernel.ll" llvm_digest)
file(SHA256 "${WORKDIR}/kernel.ptx" ptx_digest)
message(STATUS "GPU-G1 OFFLINE PTX (NO DEVICE): source=${source_digest} llvm=${llvm_digest} ptx=${ptx_digest}")
