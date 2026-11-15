# Strict GPU-G1 hardware-only proof. Must NEVER be treated as .atto guest parity.
foreach(key IN ITEMS G1_GIT G1_REPO G1_HEAD G1_EMITTER G1_LLC G1_PROBE G1_WORK G1_PTX_GATE G1_SOURCE)
    if(NOT DEFINED ${key} OR "${${key}}" STREQUAL "")
        message(FATAL_ERROR "GPU-G1-HW-001: missing required ${key}")
    endif()
endforeach()
if(NOT DEFINED G1_SOURCE_FLAG OR NOT G1_SOURCE_FLAG STREQUAL "--gpu-g1-kernel-ir")
    message(FATAL_ERROR "GPU-G1-HW-012: physical source claim requires main compiler G1 mode")
endif()
string(LENGTH "${G1_HEAD}" head_len)
if(NOT head_len EQUAL 40 OR G1_HEAD MATCHES "[^0-9a-f]")
    message(FATAL_ERROR "GPU-G1-HW-002: exact 40-hex source head required")
endif()
# All executable/file identities are provenance, not an attestation of a
# trusted runner. Never report a local script run as independently certified.
foreach(binary IN ITEMS G1_GIT G1_EMITTER G1_LLC G1_PROBE G1_PTX_GATE)
    if(NOT EXISTS "${${binary}}" OR IS_DIRECTORY "${${binary}}")
        message(FATAL_ERROR "GPU-G1-HW-008: executable or gate missing: ${binary}")
    endif()
endforeach()
file(SHA256 "${G1_GIT}" git_sha)
file(SHA256 "${G1_EMITTER}" emitter_sha)
file(SHA256 "${G1_LLC}" llc_sha)
file(SHA256 "${G1_PROBE}" probe_sha)
file(SHA256 "${G1_PTX_GATE}" ptx_gate_sha)
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" receipt_gate_sha)

function(g1_require_unchanged_tools phase)
    foreach(item IN ITEMS G1_GIT G1_EMITTER G1_LLC G1_PROBE G1_PTX_GATE)
        if(NOT EXISTS "${${item}}" OR IS_DIRECTORY "${${item}}")
            message(FATAL_ERROR "GPU-G1-HW-009: tool disappeared at ${phase}: ${item}")
        endif()
    endforeach()
    file(SHA256 "${G1_GIT}" now_git)
    file(SHA256 "${G1_EMITTER}" now_emitter)
    file(SHA256 "${G1_LLC}" now_llc)
    file(SHA256 "${G1_PROBE}" now_probe)
    file(SHA256 "${G1_PTX_GATE}" now_ptx_gate)
    file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" now_receipt_gate)
    if(NOT now_git STREQUAL git_sha OR NOT now_emitter STREQUAL emitter_sha OR
       NOT now_llc STREQUAL llc_sha OR NOT now_probe STREQUAL probe_sha OR
       NOT now_ptx_gate STREQUAL ptx_gate_sha OR
       NOT now_receipt_gate STREQUAL receipt_gate_sha)
        message(FATAL_ERROR "GPU-G1-HW-009: tool or receipt gate bytes changed at ${phase}")
    endif()
endfunction()

function(g1_require_immutable_checkout phase)
    execute_process(COMMAND "${G1_GIT}" -C "${G1_REPO}" rev-parse HEAD
        OUTPUT_VARIABLE actual_head OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_VARIABLE git_err RESULT_VARIABLE git_rc)
    if(NOT git_rc EQUAL 0 OR NOT actual_head STREQUAL G1_HEAD)
        message(FATAL_ERROR "GPU-G1-HW-003: source head changed at ${phase}")
    endif()
    execute_process(COMMAND "${G1_GIT}" -C "${G1_REPO}" status --porcelain --untracked-files=normal
        OUTPUT_VARIABLE dirty OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE dirty_rc)
    if(NOT dirty_rc EQUAL 0 OR NOT dirty STREQUAL "")
        message(FATAL_ERROR "GPU-G1-HW-004: dirty tracked/untracked source at ${phase}")
    endif()
    execute_process(COMMAND "${G1_GIT}" -C "${G1_REPO}" rev-parse HEAD^{tree}
        OUTPUT_VARIABLE tree OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE tree_rc)
    string(LENGTH "${tree}" tree_len)
    if(NOT tree_rc EQUAL 0 OR NOT tree_len EQUAL 40 OR tree MATCHES "[^0-9a-f]")
        message(FATAL_ERROR "GPU-G1-HW-005: invalid Git tree identity at ${phase}")
    endif()
    set(source_tree "${tree}" PARENT_SCOPE)
    set(actual_head "${actual_head}" PARENT_SCOPE)
endfunction()
g1_require_immutable_checkout("pre-PTX")
# Source must be part of this immutable Git tree: no external unreviewed
# .atto file can be passed into a source-to-hardware certification receipt.
if(NOT EXISTS "${G1_SOURCE}" OR IS_DIRECTORY "${G1_SOURCE}")
    message(FATAL_ERROR "GPU-G1-HW-011: source file does not exist")
endif()
execute_process(COMMAND "${G1_GIT}" -C "${G1_REPO}" ls-files --error-unmatch -- "${G1_SOURCE}"
    RESULT_VARIABLE source_tracked_rc OUTPUT_QUIET ERROR_QUIET)
if(NOT source_tracked_rc EQUAL 0)
    message(FATAL_ERROR "GPU-G1-HW-011: source file is not tracked in immutable PR tree")
endif()
# A Git-tracked pathname is insufficient: symlink targets and worktree
# filters can disagree with immutable tree bytes. Bind to the actual index
# blob at stage zero and reject anything except a regular file.
if(IS_SYMLINK "${G1_SOURCE}")
    message(FATAL_ERROR "GPU-G1-HW-011: source may not be a symlink")
endif()
execute_process(COMMAND "${G1_GIT}" -C "${G1_REPO}" ls-files --stage -- "${G1_SOURCE}"
    OUTPUT_VARIABLE index_record OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE index_rc)
if(NOT index_rc EQUAL 0 OR NOT index_record MATCHES "^10064[04] ([0-9a-f]+) 0[\t]")
    message(FATAL_ERROR "GPU-G1-HW-011: required stage-zero regular source blob missing")
endif()
set(source_index_blob "${CMAKE_MATCH_1}")
string(LENGTH "${source_index_blob}" blob_len)
if(NOT blob_len EQUAL 40)
    message(FATAL_ERROR "GPU-G1-HW-011: malformed indexed source blob")
endif()
execute_process(COMMAND "${G1_GIT}" -C "${G1_REPO}" hash-object -- "${G1_SOURCE}"
    OUTPUT_VARIABLE actual_blob OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE actual_blob_rc)
if(NOT actual_blob_rc EQUAL 0 OR NOT actual_blob STREQUAL source_index_blob)
    message(FATAL_ERROR "GPU-G1-HW-011: .atto worktree bytes differ from immutable Git source blob")
endif()
file(SHA256 "${G1_SOURCE}" atto_source_sha)
execute_process(COMMAND "${CMAKE_COMMAND}"
    "-DG1_EMITTER=${G1_EMITTER}" "-DG1_SOURCE=${G1_SOURCE}"
    "-DG1_SOURCE_FLAG=${G1_SOURCE_FLAG}" "-DG1_REQUIRE_ATTO_SOURCE=ON" "-DG1_LLC=${G1_LLC}"
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
g1_require_immutable_checkout("post-PTX")
g1_require_unchanged_tools("post-PTX")
execute_process(COMMAND "${G1_PROBE}" "${ptx}"
    RESULT_VARIABLE device_rc OUTPUT_VARIABLE device_log ERROR_VARIABLE device_err)
if(NOT device_rc EQUAL 0 OR NOT device_log MATCHES "GPU_G1_PHYSICAL_PASS 6/6")
    message(FATAL_ERROR "GPU-G1-HW-007: physical CUDA execution unavailable or incorrect: ${device_err}")
endif()
g1_require_immutable_checkout("post-CUDA")
g1_require_unchanged_tools("post-CUDA")
file(SHA256 "${ptx}" post_ptx_sha)
file(SHA256 "${G1_WORK}/gpu-g1-saxpy.ll" post_ir_sha)
file(SHA256 "${G1_SOURCE}" post_atto_sha)
if(NOT post_atto_sha STREQUAL atto_source_sha)
    message(FATAL_ERROR "GPU-G1-HW-011: source bytes changed during device execution")
endif()
if(NOT post_ptx_sha STREQUAL ptx_sha OR NOT post_ir_sha STREQUAL ir_sha)
    message(FATAL_ERROR "GPU-G1-HW-010: kernel input artifacts changed during physical execution")
endif()
message(STATUS "${device_log}")
message(STATUS "GPU_G1_DEVICE_RECEIPT source_head=${actual_head} source_tree=${source_tree} ir_sha256=${ir_sha} ptx_sha256=${ptx_sha} emitter_sha256=${emitter_sha} llc_sha256=${llc_sha} probe_sha256=${probe_sha} gate_sha256=${ptx_gate_sha} receipt_gate_sha256=${receipt_gate_sha} git_sha256=${git_sha} atto_source_sha256=${atto_source_sha} atto_blob_sha1=${source_index_blob} class=PHYSICAL_RESTRICTED_ATTO_MAIN_COMPILER_KERNEL_NOT_HOST_LAUNCH runner_trust=EXTERNAL_REVIEW_REQUIRED")
