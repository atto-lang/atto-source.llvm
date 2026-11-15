# Actual compiler-backed .atto -> LLVM IR -> Clang + ActorRegionV1 -> native
# guest executable. This is not a standalone proof of process-wide zero GC.
cmake_minimum_required(VERSION 3.20)
set(work "${BUILD}/actor-region-v1-compiled-guests")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}")
if(NOT DEFINED CLANG OR "${CLANG}" STREQUAL "" OR "${CLANG}" MATCHES "-NOTFOUND$")
  message(FATAL_ERROR "actor-region-v1 native guest tests require Clang")
endif()
if(NOT EXISTS "${BUILD}/oreslang-llvmc" OR
   NOT EXISTS "${BUILD}/oreslang-no-gc-ir-audit")
  message(FATAL_ERROR "actor-region-v1 compiler and real no-GC IR auditor must be built")
endif()
set(positives 0)
set(negatives 0)
set(failures 0)
function(fail_case why)
  message(SEND_ERROR "${why}")
  math(EXPR next "${failures}+1")
  set(failures "${next}" PARENT_SCOPE)
endfunction()
function(native_positive name code expected)
  set(dir "${work}/${name}")
  file(MAKE_DIRECTORY "${dir}")
  file(WRITE "${dir}/main.atto" "${code}")
  execute_process(
    COMMAND "${BUILD}/oreslang-llvmc" --actor-region-v1-guest
      --target=host "${dir}/main.atto"
    WORKING_DIRECTORY "${dir}"
    RESULT_VARIABLE frontend OUTPUT_VARIABLE ir ERROR_VARIABLE diagnostic
    TIMEOUT 35)
  string(FIND "${ir}" "define i32 @main()" entry)
  string(FIND "${ir}" "@ores_actor_region_v1_compiler_alloc" allocated)
  string(FIND "${ir}" "@ores_actor_region_v1_compiler_drop" dropped)
  string(FIND "${ir}" "@ores_actor_region_v1_compiler_read_i64" read)
  if(NOT frontend STREQUAL "0" OR entry EQUAL -1 OR
     allocated EQUAL -1 OR dropped EQUAL -1 OR read EQUAL -1)
    fail_case("${name}: missing compiler-generated guest allocation/read/drop: ${frontend} ${diagnostic}")
    set(failures "${failures}" PARENT_SCOPE)
    return()
  endif()
  file(WRITE "${dir}/program.ll" "${ir}")
  execute_process(COMMAND "${BUILD}/oreslang-no-gc-ir-audit"
    "${dir}/program.ll"
    RESULT_VARIABLE screened OUTPUT_VARIABLE audit_out
    ERROR_VARIABLE audit_err TIMEOUT 20)
  if(NOT screened STREQUAL "0")
    fail_case("${name}: IR tracing-GC audit refused output: ${audit_err}")
    set(failures "${failures}" PARENT_SCOPE)
    return()
  endif()
  foreach(opt IN ITEMS 0 2)
    set(bin "${dir}/guest-O${opt}")
    execute_process(
      COMMAND "${CLANG}" "-O${opt}" -x ir "${dir}/program.ll"
        -x c -std=c11
        "-I${ROOT}/src/main/cpp"
        "${ROOT}/src/main/cpp/oreslang/runtime/ActorRegionV1.c"
        "${ROOT}/src/main/cpp/oreslang/runtime/ActorArena.c"
        -o "${bin}"
      RESULT_VARIABLE linked OUTPUT_VARIABLE linked_out
      ERROR_VARIABLE linked_err TIMEOUT 90)
    if(NOT linked STREQUAL "0" OR NOT EXISTS "${bin}")
      fail_case("${name}: real native linkage O${opt} failed: ${linked} ${linked_err}")
    else()
      file(SHA256 "${bin}" binary_sha)
      execute_process(COMMAND "${bin}"
        RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr TIMEOUT 25)
      if(NOT result STREQUAL "${expected}" OR NOT stdout STREQUAL "" OR
         NOT stderr STREQUAL "")
        fail_case("${name}: compiled guest O${opt}: expected exit ${expected} with no output, got ${result}, stdout=[${stdout}], stderr=[${stderr}]")
      else()
        message(STATUS "PASS compiler-backed ActorRegionV1 native guest ${name} O${opt} exit=${result} sha256=${binary_sha}")
      endif()
    endif()
  endforeach()
  # Count both optimization modes once. Updating a parent-scope variable
  # inside foreach does not mutate this function's local variable.
  math(EXPR count "${positives}+2")
  set(positives "${count}" PARENT_SCOPE)
  set(failures "${failures}" PARENT_SCOPE)
endfunction()
function(native_negative name code)
  set(dir "${work}/reject-${name}")
  file(MAKE_DIRECTORY "${dir}")
  file(WRITE "${dir}/main.atto" "${code}")
  execute_process(
    COMMAND "${BUILD}/oreslang-llvmc" --actor-region-v1-guest
      "${dir}/main.atto"
    WORKING_DIRECTORY "${dir}"
    RESULT_VARIABLE status OUTPUT_VARIABLE ir ERROR_VARIABLE diagnostic
    TIMEOUT 25)
  file(GLOB artifact "${dir}/*.ll" "${dir}/*.bc" "${dir}/*.o"
       "${dir}/*.s" "${dir}/*.exe")
  if(status STREQUAL "0" OR NOT status MATCHES "^[0-9]+$" OR
     NOT ir STREQUAL "" OR artifact)
    fail_case("${name}: unsupported guest silently admitted/emitted IR: ${status} ${diagnostic}")
  else()
    message(STATUS "PASS fail-closed native guest rejection: ${name}")
  endif()
  math(EXPR count "${negatives}+1")
  set(negatives "${count}" PARENT_SCOPE)
  set(failures "${failures}" PARENT_SCOPE)
endfunction()

# Actual source parsed by the production frontend; not synthetic host IR.
file(READ "${ROOT}/tests/fixtures/class-public-field-read-native.ores" class_fixture)
native_positive(single_class "${class_fixture}" 42)
native_positive(two_fields
  "define class Dual as\n  pub final int first: 19;\n  pub final int second: 23;\nend\npub fnc main(): int {\n  let Dual value = new Dual();\n  return value.second;\n}\n" 23)
native_negative(no_class "pub fnc main(): int { return 42; }\n")
native_negative(unknown_callable "pub fnc main(): int { return missing(42); }\n")
native_negative(mutable_field
  "define class Box as\n  pub final mut int token: 42;\nend\npub fnc main(): int {\n  let Box b = new Box();\n  return b.token;\n}\n")
native_negative(mutable_owner
  "define class Box as\n  pub final int token: 42;\nend\npub fnc main(): int {\n  let mut Box b = new Box();\n  return b.token;\n}\n")
native_negative(extra_owner
  "define class Box as\n  pub final int token: 42;\nend\npub fnc main(): int {\n  let Box a = new Box();\n  let Box b = new Box();\n  return a.token;\n}\n")
native_negative(unreachable_function
  "define class Box as\n  pub final int token: 42;\nend\nfnc other(): int { return 1; }\npub fnc main(): int {\n  let Box b = new Box();\n  return b.token;\n}\n")
native_negative(constructor_arguments
  "define class Box as\n  pub final int token: value;\nend\npub fnc main(): int {\n  let Box b = new Box(42);\n  return b.token;\n}\n")
native_negative(class_method
  "define class Box as\n  pub final int token: 42;\n  pub answer(): int { return self.token; }\nend\npub fnc main(): int {\n  let Box b = new Box();\n  return b.token;\n}\n")
if(NOT positives EQUAL 4 OR NOT negatives EQUAL 8 OR failures GREATER 0)
  message(FATAL_ERROR "ActorRegionV1 compiled guest: expected 4 native executions and 8 rejections; saw ${positives}, ${negatives}, failures=${failures}")
endif()
message(STATUS "PASS ActorRegionV1 native guest AOT: two sources O0/O2 plus 8 unsupported-source refusals")
