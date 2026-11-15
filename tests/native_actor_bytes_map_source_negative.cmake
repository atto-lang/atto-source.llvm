# Fails closed on the pending typed native actor/Bytes Map source target.
# This is a real production-compiler negative, never positive guest proof.
cmake_minimum_required(VERSION 3.20)
if(NOT EXISTS "${ORE_BYTES_MAP_COMPILER}")
  message(FATAL_ERROR "missing production oreslang-llvmc compiler")
endif()
if(NOT EXISTS "${ORE_BYTES_MAP_SOURCE}")
  message(FATAL_ERROR "missing .atto source fixture")
endif()
execute_process(
  COMMAND "${ORE_BYTES_MAP_COMPILER}" "${ORE_BYTES_MAP_SOURCE}"
  RESULT_VARIABLE exitcode OUTPUT_VARIABLE generated ERROR_VARIABLE diagnostic
  TIMEOUT 15)
if("${exitcode}" STREQUAL "0")
  message(FATAL_ERROR "unsupported native actor Bytes/Map source compiled as scalar guest")
endif()
if(NOT diagnostic MATCHES "native actor state.*requires sealed handle_state lowering")
  message(FATAL_ERROR "expected explicit sealed-state capability rejection, got: ${diagnostic}")
endif()
if(generated MATCHES "target triple" OR generated MATCHES "define [^\n]*@")
  message(FATAL_ERROR "unsupported Bytes/Map actor emitted partial LLVM IR")
endif()
message(STATUS "NATIVE_ACTOR_BYTES_MAP_ATTO=COMPILER_REJECTED_UNTIL_TYPED_LOWERING")
