cmake_minimum_required(VERSION 3.20)
# Explicit modes: validating the fixture never implies an LLVM guest was run.
if(NOT DEFINED MODE OR NOT (MODE STREQUAL "validate" OR MODE STREQUAL "native"))
  message(FATAL_ERROR "MODE must be explicitly validate or native")
endif()
if(NOT DEFINED CORPUS OR NOT EXISTS "${CORPUS}" OR IS_DIRECTORY "${CORPUS}")
  message(FATAL_ERROR "shared scalar50 corpus is missing")
endif()
set(PINNED_CORPUS_SHA256 "210dbe7363e5388e4f82c4de9fe7f8136ee6a7dca698ce72165e006fd5ecb0db")
file(SHA256 "${CORPUS}" corpus_sha256)
if(NOT corpus_sha256 STREQUAL PINNED_CORPUS_SHA256)
  message(FATAL_ERROR "scalar50 corpus bytes differ from the canonical cross-backend fixture")
endif()
file(STRINGS "${CORPUS}" lines ENCODING UTF-8)
list(LENGTH lines line_count)
if(NOT line_count EQUAL 52)
  message(FATAL_ERROR "scalar50 must contain two headers and exactly fifty programs")
endif()
list(GET lines 0 header1)
list(GET lines 1 header2)
if(NOT header1 STREQUAL "# oreslang-scalar-50-v1 | exact same source bytes across Java, LLVM, WASM, Cranelift, .NET" OR
   NOT header2 STREQUAL "# id<TAB>expected_signed_i64<TAB>complete_Oreslang_source")
  message(FATAL_ERROR "scalar50 header profile mismatch")
endif()
if(MODE STREQUAL "native")
  if(NOT DEFINED COMPILER OR NOT EXISTS "${COMPILER}" OR IS_DIRECTORY "${COMPILER}" OR
     NOT DEFINED CLANG OR NOT EXISTS "${CLANG}" OR IS_DIRECTORY "${CLANG}")
    message(FATAL_ERROR "native LLVM guest test requires built oreslang-llvmc and Clang")
  endif()
  if(NOT DEFINED WORKDIR OR WORKDIR STREQUAL "" OR WORKDIR STREQUAL "/")
    message(FATAL_ERROR "native guest test requires an isolated work directory")
  endif()
  if(NOT DEFINED SOURCE_DIR OR NOT EXISTS "${SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "source repository identity is required")
  endif()
  execute_process(COMMAND git -C "${SOURCE_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE source_head OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE head_status TIMEOUT 5)
  execute_process(COMMAND git -C "${SOURCE_DIR}" rev-parse "HEAD^{tree}"
    OUTPUT_VARIABLE source_tree OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE tree_status TIMEOUT 5)
  if(NOT head_status STREQUAL "0" OR NOT tree_status STREQUAL "0" OR
     NOT source_head MATCHES "^[0-9a-f]+$" OR
     NOT source_tree MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "missing valid native source Git head/tree")
  endif()
  if(DEFINED EXPECTED_SOURCE_SHA AND NOT "${EXPECTED_SOURCE_SHA}" STREQUAL "" AND NOT "${EXPECTED_SOURCE_SHA}" STREQUAL "${source_head}")
    message(FATAL_ERROR "source checkout differs from expected exact commit")
  endif()
  file(MAKE_DIRECTORY "${WORKDIR}")
  # Do not allow an old successful receipt to survive a failed rerun and be
  # uploaded by the always() artifact step as if it belonged to this HEAD.
  if(IS_SYMLINK "${WORKDIR}")
    message(FATAL_ERROR "native receipt workdir may not be a symlink")
  endif()
  set(canonical_report "${WORKDIR}/llvm-scalar50-native-receipt.json")
  if(NOT DEFINED REPORT OR REPORT STREQUAL "")
    set(REPORT "${canonical_report}")
  elseif(NOT REPORT STREQUAL canonical_report)
    message(FATAL_ERROR "native receipt path must remain inside its designated workdir")
  endif()
  if(IS_DIRECTORY "${REPORT}")
    message(FATAL_ERROR "native receipt path may not be a directory")
  endif()
  file(REMOVE "${REPORT}")
  if(EXISTS "${REPORT}")
    message(FATAL_ERROR "unable to remove stale native receipt")
  endif()
  file(SHA256 "${COMPILER}" compiler_sha256)
  file(SHA256 "${CLANG}" clang_sha256)
  set(observations "")
endif()
set(count 0)
foreach(line IN LISTS lines)
  if(line MATCHES "^#")
    continue()
  endif()
  if(NOT "${line}" MATCHES "^scalar-([0-9][0-9][0-9])\t([0-9]+)\t(pub fnc main\\(\\): int \\{ return [-+*()0-9 ]+; \\})$")
    message(FATAL_ERROR "invalid scalar50 ID, numeric expectation, or source syntax")
  endif()
  set(ordinal "${CMAKE_MATCH_1}")
  set(expected "${CMAKE_MATCH_2}")
  set(source "${CMAKE_MATCH_3}")
  math(EXPR count "${count} + 1")
  if(count LESS 10)
    set(expected_ordinal "00${count}")
  elseif(count LESS 100)
    set(expected_ordinal "0${count}")
  else()
    set(expected_ordinal "${count}")
  endif()
  if(NOT ordinal STREQUAL expected_ordinal OR expected GREATER 255 OR expected LESS 0)
    message(FATAL_ERROR "out-of-order scalar50 case or nonrepresentable native exit")
  endif()
  if(MODE STREQUAL "native")
    set(prefix "${WORKDIR}/scalar-${ordinal}")
    set(source_file "${prefix}.atto")
    set(ir_file "${prefix}.ll")
    file(REMOVE "${ir_file}" "${prefix}.O0.native" "${prefix}.O2.native")
    # No trailing newline: the full source field is byte-identical across backends.
    file(WRITE "${source_file}" "${source}")
    file(SHA256 "${source_file}" source_sha256)
    execute_process(COMMAND "${COMPILER}" "${source_file}"
      RESULT_VARIABLE compile_code OUTPUT_FILE "${ir_file}"
      ERROR_VARIABLE compile_stderr TIMEOUT 10)
    if(NOT "${compile_code}" STREQUAL "0" OR NOT EXISTS "${ir_file}")
      message(FATAL_ERROR "oreslang-llvmc did not compile scalar-${ordinal}: ${compile_code}: ${compile_stderr}")
    endif()
    file(SIZE "${ir_file}" ir_bytes)
    if(ir_bytes LESS 16 OR ir_bytes GREATER 131072)
      message(FATAL_ERROR "empty or unbounded LLVM IR for scalar-${ordinal}")
    endif()
    # Optimize the SAME checked native IR at both O0 and O2. This detects
    # optimizer-dependent differences, not only frontend/codegen acceptance.
    foreach(opt IN ITEMS O0 O2)
      set(native_file "${prefix}.${opt}.native")
      execute_process(COMMAND "${CLANG}" "-${opt}" -Wno-override-module -x ir "${ir_file}" -o "${native_file}"
        RESULT_VARIABLE link_code OUTPUT_VARIABLE link_stdout
        ERROR_VARIABLE link_stderr TIMEOUT 20)
      if(NOT "${link_code}" STREQUAL "0" OR NOT EXISTS "${native_file}" OR
         NOT "${link_stdout}" STREQUAL "")
        message(FATAL_ERROR "Clang ${opt} native link failed for scalar-${ordinal}: ${link_code}: ${link_stderr}")
      endif()
      execute_process(COMMAND "${native_file}"
        RESULT_VARIABLE observed OUTPUT_VARIABLE guest_stdout
        ERROR_VARIABLE guest_stderr TIMEOUT 5)
      if(NOT "${observed}" STREQUAL "${expected}" OR
         NOT "${guest_stdout}" STREQUAL "" OR NOT "${guest_stderr}" STREQUAL "")
        message(FATAL_ERROR "LLVM native guest scalar-${ordinal} ${opt}: expected exit ${expected}; got ${observed}; stdout=${guest_stdout}; stderr=${guest_stderr}")
      endif()
      set("observed_${opt}" "${observed}")
    endforeach()
    if(NOT observations STREQUAL "")
      string(APPEND observations ",\n")
    endif()
    string(APPEND observations
      "    {\"id\":\"scalar-${ordinal}\",\"expected_exit\":${expected},\"observed_exit\":${observed_O0},\"observed_exit_o2\":${observed_O2},\"source_sha256\":\"${source_sha256}\"}")
  endif()
endforeach()
if(NOT count EQUAL 50)
  message(FATAL_ERROR "native scalar50 suite must contain fifty source programs")
endif()
if(MODE STREQUAL "native")
  # Pin the exact Git revision and the real compiler, Clang and corpus bytes
  # through completion; a mid-run replacement cannot inherit this receipt.
  execute_process(COMMAND git -C "${SOURCE_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE after_source_head OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE after_head_status TIMEOUT 5)
  execute_process(COMMAND git -C "${SOURCE_DIR}" rev-parse "HEAD^{tree}"
    OUTPUT_VARIABLE after_source_tree OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE after_tree_status TIMEOUT 5)
  file(SHA256 "${CORPUS}" after_corpus_sha256)
  file(SHA256 "${COMPILER}" after_compiler_sha256)
  file(SHA256 "${CLANG}" after_clang_sha256)
  if(NOT after_head_status STREQUAL "0" OR NOT after_tree_status STREQUAL "0" OR
     NOT after_source_head STREQUAL source_head OR
     NOT after_source_tree STREQUAL source_tree OR
     NOT after_corpus_sha256 STREQUAL corpus_sha256 OR
     NOT after_compiler_sha256 STREQUAL compiler_sha256 OR
     NOT after_clang_sha256 STREQUAL clang_sha256)
    message(FATAL_ERROR "scalar50 source revision or compiler inputs changed during native execution")
  endif()
  if(NOT DEFINED REPORT OR REPORT STREQUAL "")
    set(REPORT "${WORKDIR}/llvm-scalar50-native-receipt.json")
  endif()
  file(WRITE "${REPORT}" "{\n  \"schema\":\"atto-native-scalar50-v1\",\n  \"verification\":\"LLVM_NATIVE_GUEST_EXECUTED\",\n  \"source_head\":\"${source_head}\",\n  \"source_tree\":\"${source_tree}\",\n  \"corpus_sha256\":\"${corpus_sha256}\",\n  \"compiler_sha256\":\"${compiler_sha256}\",\n  \"clang_sha256\":\"${clang_sha256}\",\n  \"optimization_modes\":[\"O0\",\"O2\"],\n  \"executed_guest_processes\":100,\n  \"executed_cases\":${count},\n  \"results\":[\n${observations}\n  ]\n}\n")
  message(STATUS "LLVM native source-to-IR-to-Clang guest execution: ${count}/${count} passed; receipt ${REPORT}")
else()
  message(STATUS "HOST-ONLY scalar50 manifest validation: ${count} source rows, no guest executed")
endif()
