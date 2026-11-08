# Real compiler + native executable admission. This test cannot certify
# process zero-GC, actor heaps, full guest AOT or Java/.NET/Wasm parity.
cmake_minimum_required(VERSION 3.20)
set(work "${BUILD}/sealed-scalar-aot-v1")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}")
set(failures 0)
function(fail_case reason)
  message(SEND_ERROR "${reason}")
  math(EXPR next "${failures}+1")
  set(failures "${next}" PARENT_SCOPE)
endfunction()

function(positive name source exit_code)
  set(src "${work}/${name}.atto")
  set(bin "${work}/${name}")
  file(WRITE "${src}" "${source}")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
      "ORESLANG_LLVM_AOT_PROFILE=sealed-scalar-v1"
      "ORESLANG_LLVMC=${BUILD}/oreslang-llvmc"
      "ORES_NO_GC_IR_AUDIT=${BUILD}/oreslang-no-gc-ir-audit"
      "CLANG=${CLANG}"
      bash "${ROOT}/scripts/compile-aot.sh" "${src}" "${bin}"
    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 45)
  if(NOT result STREQUAL "0" OR NOT EXISTS "${bin}")
    fail_case("${name}: sealed scalar AOT link failed: ${result} ${err}")
    set(failures "${failures}" PARENT_SCOPE)
    return()
  endif()
  execute_process(COMMAND "${bin}" RESULT_VARIABLE actual
    OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr TIMEOUT 20)
  if(NOT actual STREQUAL "${exit_code}")
    fail_case("${name}: executed exit ${actual}, expected ${exit_code}; ${stderr}")
  else()
    message(STATUS "PASS sealed AOT linked and executed: ${name}")
  endif()
  set(failures "${failures}" PARENT_SCOPE)
endfunction()

function(negative name source needle)
  set(dir "${work}/reject-${name}")
  file(MAKE_DIRECTORY "${dir}")
  file(WRITE "${dir}/main.atto" "${source}")
  execute_process(
    COMMAND "${BUILD}/oreslang-llvmc" --aot-profile=sealed-scalar-v1 "${dir}/main.atto"
    WORKING_DIRECTORY "${dir}" RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr TIMEOUT 20)
  if(status STREQUAL "0" OR NOT status MATCHES "^[0-9]+$")
    fail_case("${name}: compiler did not reject: ${status}")
  elseif(NOT stdout STREQUAL "")
    fail_case("${name}: rejected program leaked executable LLVM IR")
  else()
    string(FIND "${stderr}" "${needle}" match)
    if(match EQUAL -1)
      fail_case("${name}: expected diagnostic ${needle}, got ${stderr}")
    else()
      file(GLOB artifact "${dir}/*.ll" "${dir}/*.bc" "${dir}/*.o" "${dir}/*.s")
      if(artifact)
        fail_case("${name}: rejected source emitted ${artifact}")
      else()
        message(STATUS "PASS sealed AOT rejection: ${name}")
      endif()
    endif()
  endif()
  set(failures "${failures}" PARENT_SCOPE)
endfunction()

# Positive: static source calls, including a nested transitive edge.
positive(constant "fnc main(): int { return 42; }\n" 42)
positive(direct_call
  "fnc double(int x): int { return x + x; }\nfnc main(): int { return double(21); }\n" 42)
positive(transitive_call
  "fnc add_one(int x): int { return x + 1; }\nfnc double(int x): int { return x + x; }\nfnc main(): int { return double(add_one(20)); }\n" 42)

# Rejected source must not be silently lowered using interpreter or codegen.
negative(class_root
  "define class Public as\n  pub final int token: 42;\nend\npub fnc main(): int {\n  let Public value = new Public();\n  return value.token;\n}\n"
  "sealed scalar AOT rejects non-scalar declarations")
negative(unmodeled_actor
  "define actor Worker as {\n  let total: int = 0;\n  pub on_message(int n): int { total = total + n; return total; }\n}\npub fnc main(): int {\n  const worker = spawn Worker();\n  worker.send(1);\n  stop worker;\n  return 43;\n}\n"
  "sealed scalar AOT rejects non-scalar declarations")
negative(unknown_callable
  "fnc main(): int { return missing(1); }\n"
  "missing")
negative(runtime_eval
  "fnc main(): int { return eval(42); }\n"
  "eval")
negative(no_entry
  "fnc helper(): int { return 1; }\n"
  "sealed scalar AOT requires a statically known main/0 entry")
negative(side_effecting_io
  "fnc main(): int { stdio.stdout.println(42); return 0; }\n"
  "sealed scalar AOT requires fully modeled pure scalar effects and calls")

# Option validation is independent of source parsing; never fall back to a
# different execution profile because of invalid or duplicated parameters.
execute_process(COMMAND "${BUILD}/oreslang-llvmc"
  --aot-profile=unsupported "${work}/constant.atto"
  RESULT_VARIABLE invalid OUTPUT_VARIABLE invalid_out
  ERROR_VARIABLE invalid_err TIMEOUT 20)
if(NOT invalid STREQUAL "2" OR NOT invalid_out STREQUAL "")
  fail_case("invalid profile was accepted or emitted output")
endif()
execute_process(COMMAND "${BUILD}/oreslang-llvmc"
  --aot-profile=sealed-scalar-v1 --aot-profile=sealed-scalar-v1
  "${work}/constant.atto"
  RESULT_VARIABLE duplicate OUTPUT_VARIABLE duplicate_out
  ERROR_VARIABLE duplicate_err TIMEOUT 20)
if(NOT duplicate STREQUAL "2" OR NOT duplicate_out STREQUAL "")
  fail_case("duplicated profile was accepted or emitted output")
endif()
# The AOT wrapper must reject bad profiles before attempting a compiler or
# producing an executable, rather than dropping the requested profile.
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
    "ORESLANG_LLVM_AOT_PROFILE=unverified"
    "ORESLANG_LLVMC=${BUILD}/oreslang-llvmc"
    bash "${ROOT}/scripts/compile-aot.sh"
    "${work}/constant.atto" "${work}/bad-profile"
  RESULT_VARIABLE bad_wrapper OUTPUT_VARIABLE bad_wrapper_out
  ERROR_VARIABLE bad_wrapper_err TIMEOUT 20)
if(NOT bad_wrapper STREQUAL "64" OR EXISTS "${work}/bad-profile")
  fail_case("AOT wrapper silently changed invalid profile: ${bad_wrapper}")
endif()

if(failures GREATER 0)
  message(FATAL_ERROR "sealed scalar AOT: ${failures} failures")
endif()
message(STATUS "sealed scalar AOT: compiler negative and linked guest checks passed")
