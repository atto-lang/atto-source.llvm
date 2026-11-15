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
  "d0ec8a9bc9e73e016cf1c31e2cb35cb84ac281c7"
  "8dd86350bdfa4dc90e47b7dd5f44bd82b0266157"
  "0f431f23d40d46cacd07d81714780e0061b6413e"
  "038d5bfe66996efff91400a8d699dac3e6f43f54"
  "57a646240880dfc4b633c2f9eeb5b9278dd0022f"
  "7b674493d50e688666b1fa37f59b990a27f109e4"
  "6547de51bc2be9b52eca35359a00b692ed434cfb"
  "057544faa452e4942143503499c4a7b76072ff17"
  "c5a427450943ac32824e42c1261401777080724e")

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
