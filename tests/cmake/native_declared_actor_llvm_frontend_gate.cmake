# Exact-source native AOT smoke and deliberately unsupported actor source.
# LLVM IR is written only for the supported scalar guest, never for an
# unverified declaration-only actor. No Java runtime or Python is used.
foreach(required COMPILER CLANG POSITIVE ARITHMETIC NEGATIVE BAD_NONLITERAL BAD_MULTIFUNCTION SOURCE_DIR RECEIVE OUTPUT_DIR)
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

# Actual source -> LLVM IR -> Clang O0/O2 -> native executable, not a host
# evaluator. Preserve the backend's signed overflow intrinsics.
execute_process(
  COMMAND "${COMPILER}" "${ARITHMETIC}"
  RESULT_VARIABLE arithmetic_compiled
  OUTPUT_VARIABLE arithmetic_ir
  ERROR_VARIABLE arithmetic_diagnostics
)
if(NOT "${arithmetic_compiled}" STREQUAL "0")
  message(FATAL_ERROR "checked namespaced arithmetic source compilation failed: ${arithmetic_diagnostics}")
endif()
foreach(op IN ITEMS sadd ssub smul)
  if(NOT arithmetic_ir MATCHES "llvm\\.${op}\\.with\\.overflow")
    message(FATAL_ERROR "missing checked signed arithmetic lowering for ${op}")
  endif()
endforeach()
foreach(opt IN ITEMS O0 O2)
  set(arithmetic_ll "${OUTPUT_DIR}/use-namespace-arithmetic-${opt}.ll")
  set(arithmetic_bin "${OUTPUT_DIR}/use-namespace-arithmetic-${opt}")
  file(WRITE "${arithmetic_ll}" "${arithmetic_ir}")
  execute_process(
    COMMAND "${CLANG}" "-${opt}" -x ir "${arithmetic_ll}" -o "${arithmetic_bin}"
    RESULT_VARIABLE arithmetic_linked
    ERROR_VARIABLE arithmetic_link_diagnostics
  )
  if(NOT "${arithmetic_linked}" STREQUAL "0")
    message(FATAL_ERROR "namespaced arithmetic AOT failed at ${opt}: ${arithmetic_link_diagnostics}")
  endif()
  execute_process(
    COMMAND "${arithmetic_bin}"
    RESULT_VARIABLE arithmetic_exit
    ERROR_VARIABLE arithmetic_runtime_diagnostics
  )
  if(NOT "${arithmetic_exit}" STREQUAL "42")
    message(FATAL_ERROR "namespaced arithmetic must exit 42 at ${opt}: got ${arithmetic_exit}; ${arithmetic_runtime_diagnostics}")
  endif()
endforeach()

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
# The namespace runtime slice remains narrow: literal-only signed arithmetic
# is allowed, but runtime locals, calls, and multiple functions stay gated.
foreach(source IN ITEMS "${BAD_NONLITERAL}" "${BAD_MULTIFUNCTION}")
  execute_process(
    COMMAND "${COMPILER}" "${source}"
    RESULT_VARIABLE rejected
    OUTPUT_VARIABLE leaked_ir
    ERROR_VARIABLE diagnostics
  )
  if("${rejected}" STREQUAL "0" OR NOT leaked_ir STREQUAL "")
    message(FATAL_ERROR "unsupported namespace function escaped first-stage gate: ${source}")
  endif()
  if(NOT diagnostics MATCHES "NAMESPACE_RUNTIME_BACKEND_UNSUPPORTED")
    message(FATAL_ERROR "missing explicit unsupported-namespace diagnostic: ${diagnostics}")
  endif()
endforeach()

# Canonical explicit external receive is a privileged actor runtime hook;
# neither a native free function nor an unverified host C entrypoint. It must
# reject at the precise declared-actor gate with NO LLVM IR output.
execute_process(
  COMMAND "${COMPILER}" "${RECEIVE}"
  RESULT_VARIABLE denied_receive
  OUTPUT_VARIABLE leaked_receive_ir
  ERROR_VARIABLE receive_diagnostic
)
if("${denied_receive}" STREQUAL "0" OR NOT leaked_receive_ir STREQUAL "")
  message(FATAL_ERROR "unverified external actor receive escaped native codegen gate")
endif()
if(NOT receive_diagnostic MATCHES "DECLARED_ACTOR_CODEGEN_UNSUPPORTED")
  message(FATAL_ERROR "missing typed external actor receive rejection: ${receive_diagnostic}")
endif()

# Every constituent is a separate same-namespace source unit. Until
# compiler-owned manifest/linking exists none may accidentally compile alone
# or emit partial IR. The nominal actor gets the precise diagnostic above.
foreach(unit actor contract state increment get_count)
  set(source "${SOURCE_DIR}/${unit}.atto")
  if(NOT EXISTS "${source}")
    message(FATAL_ERROR "missing cross-file declared actor source ${source}")
  endif()
  execute_process(
    COMMAND "${COMPILER}" "${source}"
    RESULT_VARIABLE rejected
    OUTPUT_VARIABLE leaked_ir
    ERROR_VARIABLE diagnostics
  )
  if("${rejected}" STREQUAL "0")
    message(FATAL_ERROR "unsafe standalone ${unit}.atto compiled without actor linker")
  endif()
  if(NOT leaked_ir STREQUAL "")
    message(FATAL_ERROR "partial LLVM IR leaked for ${unit}.atto")
  endif()
  if("${unit}" STREQUAL "actor"
          AND NOT diagnostics MATCHES "DECLARED_ACTOR_CODEGEN_UNSUPPORTED")
    message(FATAL_ERROR "nominal actor missing fail-closed diagnostic: ${diagnostics}")
  endif()
endforeach()
message(STATUS "LLVM namespace literal and checked arithmetic AOT PASS (exit 42 O0/O2); all five unlowered actor units rejected")

