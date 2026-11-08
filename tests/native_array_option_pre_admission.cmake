# Narrow native collection Option<int> source admission gate.
# An executable host CTest is necessary, but not proof of ReadLoan/NLL.
if(NOT DEFINED ORES_COMPILER OR NOT EXISTS "${ORES_COMPILER}")
  message(FATAL_ERROR "exact built oreslang-llvmc executable is required")
endif()

set(_dir "${CMAKE_CURRENT_BINARY_DIR}/native-array-option-pre-admission")
file(MAKE_DIRECTORY "${_dir}")
set(_legacy "${_dir}/legacy-array-get.ores")
file(WRITE "${_legacy}" "pub fnc main():int {\n  let mut Array<int> values = new Array<int>();\n  values.add(42);\n  return values.get(0);\n}\n")
execute_process(COMMAND "${ORES_COMPILER}" "${_legacy}"
  RESULT_VARIABLE _legacy_rc OUTPUT_VARIABLE _legacy_ir ERROR_VARIABLE _legacy_err TIMEOUT 30)
if(NOT "${_legacy_rc}" STREQUAL "0" OR
   NOT "${_legacy_ir}" MATCHES "@oreslang_int_array_get")
  message(FATAL_ERROR "legacy Array.get scalar admission failed: rc=${_legacy_rc}, err=${_legacy_err}")
endif()

set(_checked "${_dir}/typed-copy-option.ores")
file(WRITE "${_checked}" "pub fnc main():int {\n  let mut Array<int> values = new Array<int>();\n  values.add(42);\n  val Option<int> found = values.get_copy(0);\n  return found.unwrap_or(0);\n}\n")
execute_process(COMMAND "${ORES_COMPILER}" "${_checked}"
  RESULT_VARIABLE _checked_rc OUTPUT_VARIABLE _checked_ir ERROR_VARIABLE _checked_err TIMEOUT 30)
if(NOT "${_checked_rc}" STREQUAL "0" OR
   NOT "${_checked_ir}" MATCHES "@oreslang_int_array_try_get_copy" OR
   NOT "${_checked_ir}" MATCHES "@ores_record_new")
  message(FATAL_ERROR "typed Array<int>.get_copy compiler admission failed: rc=${_checked_rc}, err=${_checked_err}")
endif()

set(_untyped "${_dir}/untyped-copy.ores")
file(WRITE "${_untyped}" "pub fnc main():int {\n let mut Array<int> values = new Array<int>();\n values.add(42);\n return values.get_copy(0);\n}\n")
set(_wrong "${_dir}/wrong-option-type.ores")
file(WRITE "${_wrong}" "pub fnc main():int {\n let mut Array<int> values = new Array<int>();\n values.add(42);\n val Option<bool> found = values.get_copy(0);\n return 42;\n}\n")
set(_mutable "${_dir}/borrow-unsupported.ores")
file(WRITE "${_mutable}" "pub fnc main():int {\n let mut Array<int> values = new Array<int>();\n values.add(42);\n return values.get_mut(0);\n}\n")

foreach(_candidate IN ITEMS "${_untyped}" "${_wrong}" "${_mutable}")
  execute_process(COMMAND "${ORES_COMPILER}" "${_candidate}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _ir ERROR_VARIABLE _err TIMEOUT 30)
  if("${_rc}" STREQUAL "0" OR NOT "${_ir}" STREQUAL "" OR "${_err}" STREQUAL "")
    message(FATAL_ERROR "invalid collection Option/loan admission produced executable IR or no diagnostic: ${_candidate}; rc=${_rc}")
  endif()
endforeach()
message(STATUS "Legacy get and typed get_copy admitted; invalid Option/loan calls fail closed. No ReadLoan parity.")
