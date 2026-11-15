# Source-free negative model of the exact-head hardware receipt, NOT CUDA.
if(NOT DEFINED G1_NEGATIVE_GIT OR NOT DEFINED G1_NEGATIVE_GATE OR
   NOT DEFINED G1_NEGATIVE_WORK)
    message(FATAL_ERROR "GPU-G1-RECEIPT-001: missing test arguments")
endif()
set(root "${G1_NEGATIVE_WORK}")
set(repo "${root}/repo")
set(work "${root}/ptx")
set(gate "${root}/fake_ptx_gate.cmake")
set(probe "${root}/fake_probe.sh")
file(REMOVE_RECURSE "${root}")
file(MAKE_DIRECTORY "${repo}" "${work}")
file(WRITE "${repo}/fixture.atto" "unchanged\n")
execute_process(COMMAND "${G1_NEGATIVE_GIT}" -C "${repo}" init -q
    RESULT_VARIABLE init_rc)
execute_process(COMMAND "${G1_NEGATIVE_GIT}" -C "${repo}" add fixture.atto
    RESULT_VARIABLE add_rc)
execute_process(COMMAND "${G1_NEGATIVE_GIT}" -C "${repo}"
    -c user.name=G1Negative -c user.email=g1-negative@example.invalid
    -c commit.gpgsign=false commit -qm source-fixture
    RESULT_VARIABLE commit_rc)
if(NOT init_rc EQUAL 0 OR NOT add_rc EQUAL 0 OR NOT commit_rc EQUAL 0)
    message(FATAL_ERROR "GPU-G1-RECEIPT-002: failed to create isolated Git source")
endif()
execute_process(COMMAND "${G1_NEGATIVE_GIT}" -C "${repo}" rev-parse HEAD
    OUTPUT_VARIABLE head OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE head_rc)
if(NOT head_rc EQUAL 0)
    message(FATAL_ERROR "GPU-G1-RECEIPT-003: missing immutable fixture head")
endif()
set(pass "GPU_G1_PHYSICAL_PASS 6/6 driver=9999 device=fake sm=99.9 evidence=PTX_TYPED_IR_ONLY_NOT_ATTO_GUEST")

# CMake invokes this script INSTEAD of a real PTX codegen gate. Deliberately
# write false evidence, then mutate tracked source after pre-PTX identity.
file(WRITE "${gate}" "file(MAKE_DIRECTORY \"\${G1_WORK}\")\nfile(WRITE \"\${G1_WORK}/gpu-g1-saxpy.ll\" \"synthetic\\n\")\nfile(WRITE \"\${G1_WORK}/gpu-g1-saxpy.ptx\" \"synthetic\\n\")\nfile(APPEND \"${repo}/fixture.atto\" \"tampered\\n\")\n")
file(WRITE "${probe}" "#!/bin/sh\nprintf '%s\\n' '${pass}'\n")
file(CHMOD "${probe}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)

function(expect_identity_failure phase expected)
    execute_process(COMMAND "${CMAKE_COMMAND}"
        "-DG1_GIT=${G1_NEGATIVE_GIT}" "-DG1_REPO=${repo}"
        "-DG1_HEAD=${head}" "-DG1_EMITTER=${CMAKE_COMMAND}"
        "-DG1_SOURCE=${repo}/fixture.atto"
        "-DG1_SOURCE_FLAG=--gpu-g1-kernel-ir"
        "-DG1_LLC=${CMAKE_COMMAND}" "-DG1_PROBE=${probe}"
        "-DG1_WORK=${work}" "-DG1_PTX_GATE=${gate}"
        -P "${G1_NEGATIVE_GATE}"
        RESULT_VARIABLE rc OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(rc EQUAL 0 OR NOT "${error}" MATCHES "${expected}")
        message(FATAL_ERROR "GPU-G1-RECEIPT-004: ${phase} mutation not rejected: ${output} / ${error}")
    endif()
endfunction()
expect_identity_failure("post-PTX" "GPU-G1-HW-004.*post-PTX")
# Repeat with a non-mutating fake PTX gate, and a fake success probe that
# modifies the checked-out source after codegen.
execute_process(COMMAND "${G1_NEGATIVE_GIT}" -C "${repo}"
    checkout -- fixture.atto RESULT_VARIABLE restore_rc)
if(NOT restore_rc EQUAL 0)
    message(FATAL_ERROR "GPU-G1-RECEIPT-005: restore failed")
endif()
file(WRITE "${gate}" "file(MAKE_DIRECTORY \"\${G1_WORK}\")\nfile(WRITE \"\${G1_WORK}/gpu-g1-saxpy.ll\" \"synthetic\\n\")\nfile(WRITE \"\${G1_WORK}/gpu-g1-saxpy.ptx\" \"synthetic\\n\")\n")
file(WRITE "${probe}" "#!/bin/sh\nprintf 'tampered\\n' >> '${repo}/fixture.atto'\nprintf '%s\\n' '${pass}'\n")
file(CHMOD "${probe}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
expect_identity_failure("post-CUDA" "GPU-G1-HW-004.*post-CUDA")
# A forged success message is not sufficient if the physical probe rewrites
# the PTX artifact after its first receipt hash was computed.
execute_process(COMMAND "${G1_NEGATIVE_GIT}" -C "${repo}"
    checkout -- fixture.atto RESULT_VARIABLE restore_rc)
if(NOT restore_rc EQUAL 0)
    message(FATAL_ERROR "GPU-G1-RECEIPT-005: restore failed")
endif()
file(WRITE "${probe}" "#!/bin/sh\nprintf 'tampered\\n' >> '${work}/gpu-g1-saxpy.ptx'\nprintf '%s\\n' '${pass}'\n")
file(CHMOD "${probe}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
expect_identity_failure("post-CUDA artifact" "GPU-G1-HW-010")

# A codegen gate that edits its own file cannot retain its pre-execution hash.
file(WRITE "${probe}" "#!/bin/sh\nprintf '%s\\n' '${pass}'\n")
file(CHMOD "${probe}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
file(WRITE "${gate}" "file(MAKE_DIRECTORY \"\${G1_WORK}\")\nfile(WRITE \"\${G1_WORK}/gpu-g1-saxpy.ll\" \"synthetic\\n\")\nfile(WRITE \"\${G1_WORK}/gpu-g1-saxpy.ptx\" \"synthetic\\n\")\nfile(APPEND \"${gate}\" \"# mutated after codegen\\n\")\n")
expect_identity_failure("post-PTX tool" "GPU-G1-HW-009")
message(STATUS "PASS host-only G1 receipt TOCTOU negatives: source post-PTX, source post-CUDA, PTX artifact, gate self-mutation")
