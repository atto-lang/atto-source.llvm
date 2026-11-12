# Exact-source native AOT smoke and deliberately unsupported actor source.
# LLVM IR is written only for the supported scalar guest, never for an
# unverified declaration-only actor. No Java runtime or Python is used.
foreach(required COMPILER CLANG POSITIVE NEGATIVE OUTPUT_DIR)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "missing mandatory ${required}")
  endif()
endforeach()

file(MAKE_DIRECTORY "${OUTPUT_DIR}")
set(ir "${OUTPUT_DIR}/use-namespace-scalar-main.ll")
set(executable "${OUTPUT_DIR}/use-namespace-scalar-main")

execute_process(
  COMMAND "${COMPILER}" "${POSITIVE}"
  RESULT_VARIABLE compiled
  OUTPUT_VARIABLE source_ir
  ERROR_VARIABLE compiler_stderr
)
if(NOT "${compiled}" STREQUAL "0")
  message(FATAL_ERROR "LLVM use namespace guest compile failed: ${compiler_stderr}")
endif()
if(NOT source_ir MATCHES "define .*main")
  message(FATAL_ERROR "LLVM use namespace guest did not emit a main function")
endif()
file(WRITE "${ir}" "${source_ir}")
execute_process(
  COMMAND "${CLANG}" "-x" "ir" "${ir}" "-o" "${executable}"
  RESULT_VARIABLE linked
  OUTPUT_VARIABLE link_stdout
  ERROR_VARIABLE link_stderr
)
if(NOT "${linked}" STREQUAL "0")
  message(FATAL_ERROR "LLVM IR failed native Clang AOT: ${link_stderr}")
endif()
execute_process(
  COMMAND "${executable}"
  RESULT_VARIABLE exit_code
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)
if(NOT "${exit_code}" STREQUAL "42")
  message(FATAL_ERROR "LLVM native guest must exit 42; got ${exit_code}, stderr=${stderr}")
endif()

execute_process(
  COMMAND "${COMPILER}" "${NEGATIVE}"
  RESULT_VARIABLE blocked
  OUTPUT_VARIABLE forbidden_ir
  ERROR_VARIABLE error
)
if("${blocked}" STREQUAL "0")
  message(FATAL_ERROR "LLVM improperly compiled declaration-only actor without MIR")
endif()
if(NOT error MATCHES "DECLARED_ACTOR_CODEGEN_UNSUPPORTED")
  message(FATAL_ERROR "missing explicit native declared actor gate: ${error}")
endif()
if(NOT forbidden_ir STREQUAL "")
  message(FATAL_ERROR "LLVM leaked partial IR for an unsupported actor")
endif()
message(STATUS "LLVM use namespace real scalar AOT PASS (exit 42); actor codegen still gated")
