# Production LLVM compiler must still lower and emit the existing native
# scalar actor for a new .atto guest file. This does NOT certify Bytes/Map.
cmake_minimum_required(VERSION 3.20)
if(NOT EXISTS "${ORE_ACTOR_LLVM_CLI}")
  message(FATAL_ERROR "missing actual native oreslang-llvmc compiler")
endif()
if(NOT EXISTS "${ORE_ACTOR_SCALAR_ATTO}")
  message(FATAL_ERROR "missing native scalar .atto guest fixture")
endif()
execute_process(
  COMMAND "${ORE_ACTOR_LLVM_CLI}" "${ORE_ACTOR_SCALAR_ATTO}"
  RESULT_VARIABLE status OUTPUT_VARIABLE ir ERROR_VARIABLE diagnostic
  TIMEOUT 30)
if(NOT "${status}" STREQUAL "0")
  message(FATAL_ERROR "production LLVM compiler rejected known-good scalar .atto actor: ${diagnostic}")
endif()
foreach(symbol "oreslang_source_actor_spawn" "oreslang_source_actor_send"
               "oreslang_source_actor_stop" "oreslang_actor_NativeScalarRedis_on_message")
  if(NOT ir MATCHES "${symbol}")
    message(FATAL_ERROR "scalar actor native LLVM IR missing required ${symbol}")
  endif()
endforeach()
message(STATUS "NATIVE_ATTO_SCALAR_ACTOR_LLVM_IR=PRODUCTION_COMPILER_PASS (native link/run separate)")
