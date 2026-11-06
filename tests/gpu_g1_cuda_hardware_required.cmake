# Strict GPU-G1 hardware-only proof. Must NEVER be treated as .atto guest parity.
foreach(key IN ITEMS G1_GIT G1_REPO G1_HEAD G1_EMITTER G1_LLC G1_PROBE G1_WORK G1_PTX_GATE)
    if(NOT DEFINED ${key} OR "${${key}}" STREQUAL "")
        message(FATAL_ERROR "GPU-G1-HW-001: missing required ${key}")
    endif()
endforeach()
string(LENGTH "${G1_HEAD}" head_len)
if(NOT head_len EQUAL 40 OR G1_HEAD MATCHES "[^0-9a-f]")
    message(FATAL_ERROR "GPU-G1-HW-002: exact 40-hex source head required")
endif()
execute_process(COMMAND "${G1_GIT}" -C "${G1_REPO}" rev-parse HEAD
    OUTPUT_VARIABLE actual_head OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_VARIABLE git_err RESULT_VARIABLE git_rc)
if(NOT git_rc EQUAL 0 OR NOT actual_head STREQUAL G1_HEAD)
    message(FATAL_ERROR "GPU-G1-HW-003: checkout differs from expected immutable PR head")
endif()
execute_process(COMMAND "${G1_GIT}" -C "${G1_REPO}" status --porcelain --untracked-files=no
    OUTPUT_VARIABLE dirty OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE dirty_rc)
if(NOT dirty_rc EQUAL 0 OR NOT dirty STREQUAL "")
    message(FATAL_ERROR "GPU-G1-HW-004: tracked source modifications invalidate the receipt")
endif()
execute_process(COMMAND "${G1_GIT}" -C "${G1_REPO}" rev-parse HEAD^{tree}
    OUTPUT_VARIABLE source_tree OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE tree_rc)
if(NOT tree_rc EQUAL 0)
    message(FATAL_ERROR "GPU-G1-HW-005: cannot bind source tree")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}"
    "-DG1_EMITTER=${G1_EMITTER}" "-DG1_LLC=${G1_LLC}"
    "-DG1_WORK=${G1_WORK}" -P "${G1_PTX_GATE}"
    RESULT_VARIABLE ptx_rc OUTPUT_VARIABLE ptx_log ERROR_VARIABLE ptx_err)
if(NOT ptx_rc EQUAL 0)
    message(FATAL_ERROR "GPU-G1-HW-006: actual PTX translation failed: ${ptx_err}")
endif()
set(ptx "${G1_WORK}/gpu-g1-saxpy.ptx")
if(NOT EXISTS "${ptx}")
    message(FATAL_ERROR "GPU-G1-HW-006: no translated PTX artifact")
endif()
file(SHA256 "${ptx}" ptx_sha)
file(SHA256 "${G1_WORK}/gpu-g1-saxpy.ll" ir_sha)
execute_process(COMMAND "${G1_PROBE}" "${ptx}"
    RESULT_VARIABLE device_rc OUTPUT_VARIABLE device_log ERROR_VARIABLE device_err)
if(NOT device_rc EQUAL 0 OR NOT device_log MATCHES "GPU_G1_PHYSICAL_PASS 6/6")
    message(FATAL_ERROR "GPU-G1-HW-007: physical CUDA execution unavailable or incorrect: ${device_err}")
endif()
message(STATUS "${device_log}")
message(STATUS "GPU_G1_DEVICE_RECEIPT source_head=${actual_head} source_tree=${source_tree} ir_sha256=${ir_sha} ptx_sha256=${ptx_sha} class=PHYSICAL_PTX_TYPED_IR_ONLY_NOT_ATTO_GUEST")
