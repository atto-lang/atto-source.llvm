if(NOT DEFINED COMPILER OR NOT DEFINED FIXTURE OR NOT DEFINED STAGING OR NOT DEFINED CLANG)
    message(FATAL_ERROR "COMPILER, FIXTURE, STAGING and CLANG are required")
endif()
file(MAKE_DIRECTORY "${STAGING}")
execute_process(COMMAND "${COMPILER}" "${FIXTURE}"
    RESULT_VARIABLE emitted_status OUTPUT_VARIABLE guest_ir ERROR_VARIABLE diagnostic TIMEOUT 25)
if(NOT emitted_status EQUAL 0)
    message(FATAL_ERROR "typed macro failed before native IR: ${diagnostic}")
endif()
if(NOT guest_ir MATCHES "ret i64 42")
    message(FATAL_ERROR "compiled macro must lower to native i64 return 42")
endif()
if(guest_ir MATCHES "@quote\\(|@unquote\\(|@eval\\(")
    message(FATAL_ERROR "compile-time macro leaked into native guest IR")
endif()
set(ir_file "${STAGING}/guest.ll")
set(binary "${STAGING}/guest")
# Never let a previous passing native guest mask a broken/rejected rebuild.
file(REMOVE "${ir_file}" "${binary}")
file(WRITE "${ir_file}" "${guest_ir}")
execute_process(COMMAND "${CLANG}" -x ir "${ir_file}" -o "${binary}"
    RESULT_VARIABLE link_status OUTPUT_VARIABLE linker_out ERROR_VARIABLE linker_err TIMEOUT 40)
if(NOT link_status EQUAL 0)
    message(FATAL_ERROR "native macro AOT executable linking failed: ${linker_out} ${linker_err}")
endif()
execute_process(COMMAND "${binary}" RESULT_VARIABLE guest_status
    OUTPUT_VARIABLE guest_stdout ERROR_VARIABLE guest_stderr TIMEOUT 10)
if(NOT guest_status EQUAL 42)
    message(FATAL_ERROR "native macro AOT guest should return exit 42; status=${guest_status}: ${guest_stderr}")
endif()
if(NOT guest_stdout STREQUAL "" OR NOT guest_stderr STREQUAL "")
    message(FATAL_ERROR
        "native macro AOT exit-value ABI must not emit stdout/stderr: stdout='${guest_stdout}' stderr='${guest_stderr}'")
endif()

# An actual compiled guest receipt binds the test to source, emitted IR,
# compiler and independently linked native executable bytes. A source-only
# test must never emit this success marker.
file(SHA256 "${FIXTURE}" source_sha256)
file(SHA256 "${COMPILER}" compiler_sha256)
file(SHA256 "${ir_file}" ir_sha256)
file(SHA256 "${binary}" guest_sha256)
message(STATUS
    "MACRO_LLVM_LINKED_GUEST_AOT_PASS value=42 exit=42 source_sha256=${source_sha256} compiler_sha256=${compiler_sha256} ir_sha256=${ir_sha256} guest_sha256=${guest_sha256}")
