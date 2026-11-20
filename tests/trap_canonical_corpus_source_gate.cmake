# Exact-byte cross-backend Atto caller-trap gate for the REAL LLVM compiler.
# Only 01 is admitted in this restricted native slice. Negative rejection is
# not runtime parity; never mark this whole suite as Java/LLVM/.NET equivalent.
cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED ORES_TRAP_LLVM_CLI OR NOT EXISTS "${ORES_TRAP_LLVM_CLI}")
  message(FATAL_ERROR "real source LLVM compiler required")
endif()
if(NOT DEFINED ORES_TRAP_CORPUS OR NOT IS_DIRECTORY "${ORES_TRAP_CORPUS}")
  message(FATAL_ERROR "exact shared caller-trap source corpus missing")
endif()

set(fixture_names
  "01-some-int.atto" "02-some-result-ok.atto" "03-some-result-err.atto"
  "04-some-none.atto" "05-ordinary-throw.atto" "06-panic-bypass.atto"
  "07-exactly-once.atto" "08-type-mismatch.atto" "09-result-is-not-throw.atto")
set(expected_blobs
  "a83015bcbc66ed075440fc15972f6933189c2eee"
  "f2c4eb66f37ad239cc06b05f4be9aa8cac4712f5"
  "3930a526cc7f1c94005354f1f4d94327d7106069"
  "cb60075bf1ae15e86ff8013878f5da669269e610"
  "1f3e6b694b3147a63a2528052cf5ed7c0b7a1a59"
  "1e1247eb310efea99a6a28aa8fd7013e76e98e7e"
  "af0d97a1df81ad879529983b21ea2fb5da52824e"
  "740eb4a196f450ba834918b9fb1c41338177e46f"
  "fcc43cbc495b90b642c4881ae79f5d4a75a58258")

set(passed 0)
foreach(idx RANGE 0 8)
  list(GET fixture_names ${idx} name)
  list(GET expected_blobs ${idx} expected)
  set(path "${ORES_TRAP_CORPUS}/${name}")
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "missing canonical shared source: ${name}")
  endif()
  execute_process(COMMAND git hash-object "${path}"
    RESULT_VARIABLE git_status OUTPUT_VARIABLE blob ERROR_VARIABLE git_error
    OUTPUT_STRIP_TRAILING_WHITESPACE TIMEOUT 15)
  if(NOT "${git_status}" STREQUAL "0" OR NOT blob STREQUAL expected)
    message(FATAL_ERROR "canonical source Git blob mismatch: ${name} (${blob})")
  endif()
  execute_process(COMMAND "${ORES_TRAP_LLVM_CLI}" "${path}"
    RESULT_VARIABLE status OUTPUT_VARIABLE ir ERROR_VARIABLE diagnostic TIMEOUT 20)
  if(idx EQUAL 0)
    if(NOT "${status}" STREQUAL "0" OR NOT ir MATCHES "define [^\n]*@" OR NOT diagnostic STREQUAL "")
      message(FATAL_ERROR "real LLVM compiler must admit the canonical pure caller trap guest: ${status} ${diagnostic}")
    endif()
    math(EXPR passed "${passed}+1")
  else()
    if("${status}" STREQUAL "0" OR diagnostic STREQUAL "" OR
        ir MATCHES "define [^\n]*@" OR ir MATCHES "target triple")
      message(FATAL_ERROR "unimplemented native trap/Result/throw source incorrectly admitted or emitted LLVM IR: ${name}")
    endif()
  endif()
endforeach()
if(NOT passed EQUAL 1)
  message(FATAL_ERROR "canonical LLVM positive control was not executed")
endif()
message(STATUS "LLVM_CANONICAL_CALLER_TRAP=1_REAL_SOURCE_ADMITTED_8_REJECTED (NOT full parity)")
