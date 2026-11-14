# Exact-source native AOT smoke and deliberately unsupported actor source.
# LLVM IR is written only for the supported scalar guest, never for an
# unverified declaration-only actor. No Java runtime or Python is used.
foreach(required COMPILER CLANG POSITIVE NEGATIVE BAD_NONLITERAL BAD_MULTIFUNCTION SOURCE_DIR RECEIVE OUTPUT_DIR)
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
# The first namespace runtime slice must remain narrow; ordinary calls,
# arithmetic and multiple functions still need checked namespace lowering.
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
# Syntax-only admission uses the same production native lexer and checks the
# canonical read-loan release before the later mailbox send. It must never
# emit LLVM IR or imply full actor execution.
execute_process(
  COMMAND "${COMPILER}" "--check-declared-receive-syntax" "${RECEIVE}"
  RESULT_VARIABLE receive_syntax_result
  OUTPUT_VARIABLE receive_syntax_receipt
  ERROR_VARIABLE receive_syntax_error
)
if(NOT "${receive_syntax_result}" STREQUAL "0"
        OR NOT receive_syntax_receipt MATCHES
        "LLVM_DECLARED_COUNTER_RECEIVE_SYNTAX_PASS\\|namespace=counter\\|actor=Counter\\|field=total\\|stop=4\\|guest_not_executed")
  message(FATAL_ERROR "Native lexer did not admit canonical detached-read syntax: ${receive_syntax_error}")
endif()

function(expect_denied_receive mutation from into)
  file(READ "${RECEIVE}" canonical_receive)
  string(REPLACE "${from}" "${into}" modified_receive "${canonical_receive}")
  if("${canonical_receive}" STREQUAL "${modified_receive}")
    message(FATAL_ERROR "mutation ${mutation} did not actually change the source")
  endif()
  set(bad "${OUTPUT_DIR}/counter-receive-negative-${mutation}.atto")
  file(WRITE "${bad}" "${modified_receive}")
  execute_process(
    COMMAND "${COMPILER}" "--check-declared-receive-syntax" "${bad}"
    RESULT_VARIABLE denied
    OUTPUT_VARIABLE untrusted_receipt
    ERROR_VARIABLE denied_diagnostic
  )
  if("${denied}" STREQUAL "0" OR NOT "${untrusted_receipt}" STREQUAL ""
          OR NOT denied_diagnostic MATCHES "DECLARED_ACTOR_RECEIVE_REJECTED")
    message(FATAL_ERROR "untrusted actor receive passed syntax admission: ${mutation}")
  endif()
endfunction()

expect_denied_receive(borrowed_send "self.send(snapshot)" "self.send(s.total)")
expect_denied_receive(read_effect "critical-read |s| -> s.total"
  "critical-read |s| -> { self.send(s.total); }")
expect_denied_receive(read_is_write "critical-read" "critical-write")
expect_denied_receive(spaced_mode "critical-read" "critical -read")
expect_denied_receive(mutable_snapshot "val int snapshot" "let int snapshot")
expect_denied_receive(unowned_delta "s.total + mail.value" "s.total + 1")
expect_denied_receive(spoofed_receiver "external mtd" "pub mtd")
expect_denied_receive(no_actor_stop "self.end()" "self.send(5)")

message(STATUS "LLVM canonical declared receive syntax PASS, 8 negative mutations rejected; full actor guest NOT executed")
message(STATUS "LLVM use namespace real scalar AOT PASS (exit 42); all five unlowered actor units rejected")

