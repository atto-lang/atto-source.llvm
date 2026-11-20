# Exact-source native compiler + LLVM/Clang guest execution for the restricted
# scalar T? -> Option<T> ABI.  Host-only parser checks are not sufficient.
cmake_minimum_required(VERSION 3.20)
foreach(required ATTO_OPTION_LLVM_CLI ATTO_OPTION_FIXTURE_DIR ATTO_OPTION_CLANG)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "Missing ${required}")
  endif()
endforeach()
if(NOT EXISTS "${ATTO_OPTION_LLVM_CLI}")
  message(FATAL_ERROR "Actual compiled oreslang-llvmc is absent")
endif()

set(work "${CMAKE_CURRENT_BINARY_DIR}/atto-option-suffix-native")
file(MAKE_DIRECTORY "${work}")
set(positive_cases positive-explicit positive-some positive-none)
set(negative_cases
  reject-duplicate reject-void reject-nested reject-unknown-class
  reject-omittable-formal reject-unsupported-alias reject-nested-result)
set(executed_guests 0)
foreach(name IN LISTS positive_cases)
  set(source "${ATTO_OPTION_FIXTURE_DIR}/${name}.atto")
  if(NOT EXISTS "${source}")
    message(FATAL_ERROR "Missing positive fixture: ${source}")
  endif()
  execute_process(
    COMMAND "${ATTO_OPTION_LLVM_CLI}" "${source}"
    RESULT_VARIABLE compile_status OUTPUT_VARIABLE llvm_ir
    ERROR_VARIABLE compiler_error TIMEOUT 20)
  if(NOT "${compile_status}" STREQUAL "0" OR
      NOT llvm_ir MATCHES "define [^\\n]*@")
    message(FATAL_ERROR "${name}: compiler did not emit native function IR: ${compiler_error}")
  endif()
  set(ir_path "${work}/${name}.ll")
  file(WRITE "${ir_path}" "${llvm_ir}")
  foreach(opt 0 2)
    set(executable "${work}/${name}-O${opt}")
    execute_process(
      COMMAND "${ATTO_OPTION_CLANG}" -x ir "-O${opt}" "${ir_path}" -o "${executable}"
      RESULT_VARIABLE link_status OUTPUT_VARIABLE link_out ERROR_VARIABLE link_err TIMEOUT 30)
    if(NOT "${link_status}" STREQUAL "0")
      message(FATAL_ERROR "${name} O${opt}: Clang link failure: ${link_err}")
    endif()
    execute_process(
      COMMAND "${executable}"
      RESULT_VARIABLE run_status OUTPUT_VARIABLE run_out ERROR_VARIABLE run_err TIMEOUT 15)
    if(NOT "${run_status}" STREQUAL "42" OR
        NOT "${run_out}" STREQUAL "" OR NOT "${run_err}" STREQUAL "")
      message(FATAL_ERROR "${name} O${opt}: expected exit 42 with no output; got ${run_status}, ${run_out}, ${run_err}")
    endif()
    math(EXPR executed_guests "${executed_guests} + 1")
  endforeach()
endforeach()

set(negative_count 0)
foreach(name IN LISTS negative_cases)
  set(source "${ATTO_OPTION_FIXTURE_DIR}/${name}.atto")
  if(NOT EXISTS "${source}")
    message(FATAL_ERROR "Missing negative fixture: ${source}")
  endif()
  execute_process(
    COMMAND "${ATTO_OPTION_LLVM_CLI}" "${source}"
    RESULT_VARIABLE status OUTPUT_VARIABLE ir ERROR_VARIABLE diagnostic TIMEOUT 20)
  if("${status}" STREQUAL "0" OR "${diagnostic}" STREQUAL "" OR
      NOT "${ir}" STREQUAL "")
    message(FATAL_ERROR "${name}: unsupported/invalid optional type was admitted or emitted LLVM IR")
  endif()
  math(EXPR negative_count "${negative_count} + 1")
endforeach()
if(NOT executed_guests EQUAL 6 OR NOT negative_count EQUAL 7)
  message(FATAL_ERROR "Incomplete native Option suffix gate")
endif()
message(STATUS "ATTO_LLVM_OPTION_SUFFIX_PASS guests=${executed_guests} negatives=${negative_count}")
