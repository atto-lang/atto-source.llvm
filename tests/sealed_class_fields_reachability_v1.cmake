# Fifteen compiler-backed additional AOT reachability and immutable class tests.
# None are evidence without exact-head nonzero-step native execution.
cmake_minimum_required(VERSION 3.20)
set(work "${BUILD}/sealed-class-fields-reachability-v1")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}")
set(failures 0)
set(cases 0)
if(NOT DEFINED CLANG OR "${CLANG}" STREQUAL "" OR "${CLANG}" MATCHES "-NOTFOUND$")
  message(FATAL_ERROR "sealed class-field AOT: Clang required; no silent test skipping")
endif()
if(NOT EXISTS "${BUILD}/oreslang-llvmc")
  message(FATAL_ERROR "sealed class-field AOT: native compiler must be built")
endif()

function(fail_case reason)
  message(SEND_ERROR "${reason}")
  math(EXPR next "${failures}+1")
  set(failures "${next}" PARENT_SCOPE)
endfunction()

function(native_positive name source expected)
  set(src "${work}/${name}.atto")
  set(bin "${work}/${name}")
  file(WRITE "${src}" "${source}")
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env
    "ORESLANG_LLVM_AOT_PROFILE=sealed-class-fields-v1"
    "ORESLANG_LLVMC=${BUILD}/oreslang-llvmc"
    "ORES_NO_GC_IR_AUDIT=${BUILD}/oreslang-no-gc-ir-audit"
    "CLANG=${CLANG}"
    bash "${ROOT}/scripts/compile-aot.sh" "${src}" "${bin}"
    RESULT_VARIABLE compiled OUTPUT_VARIABLE compile_out
    ERROR_VARIABLE compile_err TIMEOUT 60)
  if(NOT compiled STREQUAL "0" OR NOT EXISTS "${bin}")
    fail_case("${name}: compiler/Clang link failed (${compiled}): ${compile_err}")
  else()
    # Independently confirm real frontend IR and a concrete native artifact.
    execute_process(COMMAND "${BUILD}/oreslang-llvmc"
      --aot-profile=sealed-class-fields-v1 --target=host "${src}"
      RESULT_VARIABLE ir_status OUTPUT_VARIABLE ir_stdout
      ERROR_VARIABLE ir_stderr TIMEOUT 30)
    string(FIND "${ir_stdout}" "; ModuleID" module_mark)
    string(FIND "${ir_stdout}" "define " definition_mark)
    if(NOT ir_status STREQUAL "0" OR module_mark EQUAL -1 OR definition_mark EQUAL -1)
      fail_case("${name}: native LLVM IR evidence missing: ${ir_status} ${ir_stderr}")
    endif()
    file(SHA256 "${bin}" binary_sha256)
    string(LENGTH "${binary_sha256}" sha_length)
    if(NOT sha_length EQUAL 64)
      fail_case("${name}: native executable SHA-256 missing")
    endif()
    execute_process(COMMAND "${bin}" RESULT_VARIABLE status
      OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr TIMEOUT 20)
    if(NOT status STREQUAL "${expected}" OR NOT stdout STREQUAL "" OR
       NOT stderr STREQUAL "")
      fail_case("${name}: native exit/output mismatch: exit=${status} expected=${expected} stdout=[${stdout}] stderr=[${stderr}]")
    else()
      message(STATUS "PASS class native guest ${name}: exit=${status} sha256=${binary_sha256}")
    endif()
  endif()
  math(EXPR n "${cases}+1")
  set(cases "${n}" PARENT_SCOPE)
  set(failures "${failures}" PARENT_SCOPE)
endfunction()

function(fixture_positive name fixture expected)
  file(READ "${ROOT}/tests/fixtures/${fixture}" code)
  native_positive("${name}" "${code}" "${expected}")
  set(cases "${cases}" PARENT_SCOPE)
  set(failures "${failures}" PARENT_SCOPE)
endfunction()

function(compiler_negative name source diagnostic)
  set(dir "${work}/reject-${name}")
  file(MAKE_DIRECTORY "${dir}")
  file(WRITE "${dir}/main.atto" "${source}")
  execute_process(COMMAND "${BUILD}/oreslang-llvmc"
    --aot-profile=sealed-class-fields-v1 "${dir}/main.atto"
    WORKING_DIRECTORY "${dir}" RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr TIMEOUT 20)
  if(status STREQUAL "0" OR NOT status MATCHES "^[0-9]+$")
    fail_case("${name}: invalid source accepted or not executed: ${status}")
  elseif(NOT stdout STREQUAL "")
    fail_case("${name}: rejected source emitted executable IR on stdout")
  else()
    string(FIND "${stderr}" "${diagnostic}" found)
    if(found EQUAL -1)
      fail_case("${name}: expected diagnostic '${diagnostic}', got '${stderr}'")
    else()
      file(GLOB emitted "${dir}/*.ll" "${dir}/*.bc" "${dir}/*.o"
        "${dir}/*.obj" "${dir}/*.s" "${dir}/*.exe")
      if(emitted)
        fail_case("${name}: rejected source emitted artifacts: ${emitted}")
      else()
        message(STATUS "PASS sealed AOT source-negative ${name}")
      endif()
    endif()
  endif()
  math(EXPR n "${cases}+1")
  set(cases "${n}" PARENT_SCOPE)
  set(failures "${failures}" PARENT_SCOPE)
endfunction()

function(fixture_negative name fixture diagnostic)
  file(READ "${ROOT}/tests/fixtures/${fixture}" source)
  compiler_negative("${name}" "${source}" "${diagnostic}")
  set(cases "${cases}" PARENT_SCOPE)
  set(failures "${failures}" PARENT_SCOPE)
endfunction()

# Eight real-guest executable cases. No JavaScript or interpreted fallback.
fixture_positive(immutable_class "class-public-field-read-native.ores" 42)
native_positive(two_static_fields
  "define class Pair as\n  pub final int left: 20;\n  pub final int right: 22;\nend\npub fnc main(): int {\n  let Pair p = new Pair();\n  return p.left + p.right;\n}\n" 42)
native_positive(field_to_closed_helper
  "define class Box as\n  pub final int value: 41;\nend\nfnc plus_one(int x): int { return x + 1; }\npub fnc main(): int {\n  let Box b = new Box();\n  return plus_one(b.value);\n}\n" 42)
fixture_positive(forward_call "forward-call-main.ores" 42)
fixture_positive(overloaded_arity "native-name-arity-overloads.ores" 24)
fixture_positive(named_overload "named-call-arity-overload-main.ores" 43)
fixture_positive(call_from_guard "cfg-guard-call.ores" 42)
native_positive(static_user_eval_symbol
  "fnc eval(int x): int { return x; }\npub fnc main(): int { return eval(42); }\n" 42)

# Seven rejections: must fail before emitting IR or binary artifacts.
fixture_negative(mutable_class
  "class-multifield-independent-write.ores"
  "sealed class fields require immutable literal scalar defaults")
compiler_negative(borrowed_class_parameter
  "define class Box as\n  pub final int value: 42;\nend\nfnc read(borrow Box box): int { return box.value; }\npub fnc main(): int {\n  let Box box = new Box();\n  return read(rt borrow box);\n}\n"
  "sealed scalar AOT rejects non-scalar or borrowed parameter ABI")
fixture_negative(parameterized_constructor
  "class-scalar-constructor-arithmetic-native.ores"
  "sealed class-field AOT rejects methods or parameterized constructors")
compiler_negative(unknown_class_method_roots
  "define class Box as\n  pub final int value: 42;\n  pub answer(): int { return self.value; }\nend\npub fnc main(): int {\n  let Box box = new Box();\n  return box.value;\n}\n"
  "sealed class-field AOT rejects methods or parameterized constructors")
compiler_negative(unresolved_control_flow_target
  "fnc main(): int {\n  if 1 == 1 then return missing(0); fi\n  return 42;\n}\n"
  "missing")
compiler_negative(unknown_eval_target
  "fnc main(): int { return eval(42); }\n"
  "eval")
compiler_negative(no_known_entry
  "fnc helper(): int { return 42; }\n"
  "sealed scalar AOT requires a statically known main/0 entry")

if(NOT cases EQUAL 15)
  message(FATAL_ERROR "expected exactly 15 tests, recorded ${cases}")
endif()
if(failures GREATER 0)
  message(FATAL_ERROR "sealed AOT reachability: ${failures} / 15 test cases failed")
endif()
message(STATUS "sealed class-field reachability: all 15 cases executed and passed")
