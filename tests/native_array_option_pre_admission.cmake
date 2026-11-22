# Candidate safe-collection-access-v1 pre-admission, NOT an Option/loan parity test.
# The compiler's legacy Array<int>.get(index) is scalar and trapping.
# get_copy/get_mut must fail closed until true typed Option/ReadLoan lowering.
if(NOT DEFINED ORES_COMPILER OR NOT EXISTS "${ORES_COMPILER}")
  message(FATAL_ERROR "exact built oreslang-llvmc executable is required")
endif()

set(_dir "${CMAKE_CURRENT_BINARY_DIR}/native-array-option-pre-admission")
file(MAKE_DIRECTORY "${_dir}")
set(_legacy "${_dir}/legacy-array-get.ores")
file(WRITE "${_legacy}" "pub fnc main():int {\n  let mut Array<int> values = new Array<int>();\n  values.add(42);\n  return values.get(0);\n}\n")
execute_process(COMMAND "${ORES_COMPILER}" "${_legacy}"
  RESULT_VARIABLE _legacy_rc OUTPUT_VARIABLE _legacy_ir ERROR_VARIABLE _legacy_err
  TIMEOUT 30)
if(NOT "${_legacy_rc}" STREQUAL "0" OR
   NOT "${_legacy_ir}" MATCHES "@oreslang_int_array_get")
  message(FATAL_ERROR "legacy direct-value Array.get compiler admission failed: rc=${_legacy_rc}, err=${_legacy_err}")
endif()

foreach(_member get_copy get_mut)
  set(_candidate "${_dir}/candidate-${_member}.ores")
  file(WRITE "${_candidate}" "pub fnc main():int {\n  let mut Array<int> values = new Array<int>();\n  values.add(42);\n  return values.${_member}(0);\n}\n")
  execute_process(COMMAND "${ORES_COMPILER}" "${_candidate}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _ir ERROR_VARIABLE _err
    TIMEOUT 30)
  if("${_rc}" STREQUAL "0")
    message(FATAL_ERROR "unimplemented ${_member} unexpectedly emitted successful executable IR")
  endif()
  if(NOT "${_ir}" STREQUAL "")
    message(FATAL_ERROR "unimplemented ${_member} leaked partial compiler stdout/IR")
  endif()
  if("${_err}" STREQUAL "")
    message(FATAL_ERROR "unimplemented ${_member} produced no diagnostic")
  endif()
endforeach()
message(STATUS "legacy scalar Array.get admitted, candidate Option/loan APIs fail closed; NO guest parity")
