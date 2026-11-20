# Ordinary-vs-sealed contrast tests; not compiler-implementation mutation.
# Exact-head stepful CI is required before any test result is claimed.
cmake_minimum_required(VERSION 3.20)
# Never remove a guessed workspace when BUILD/ROOT is absent or aliased.
# The test's work directory is deleted recursively below: validate before IO.
if(NOT DEFINED BUILD OR NOT IS_DIRECTORY "${BUILD}" OR
   NOT DEFINED ROOT OR NOT IS_DIRECTORY "${ROOT}")
  message(FATAL_ERROR "sealed contrasts: existing BUILD and ROOT directories required")
endif()
file(REAL_PATH "${BUILD}" build_real)
file(REAL_PATH "${ROOT}" root_real)
if(NOT "${BUILD}" STREQUAL "${build_real}" OR
   "${build_real}" STREQUAL "/" OR
   "${build_real}" STREQUAL "${root_real}")
  message(FATAL_ERROR "sealed contrasts: canonical separate build directory required")
endif()
set(work "${build_real}/sealed-aot-profile-mutations")
if(IS_SYMLINK "${work}")
  message(FATAL_ERROR "sealed contrasts: symlinked test workspace denied")
endif()
if(NOT EXISTS "${build_real}/oreslang-llvmc")
  message(FATAL_ERROR "sealed contrasts: native compiler must be built")
endif()
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}")
set(cases 0)
set(failures 0)

function(fail_case reason)
  message(SEND_ERROR "${reason}")
  math(EXPR n "${failures}+1")
  set(failures "${n}" PARENT_SCOPE)
endfunction()

function(profile_contrast name source needle)
  set(dir "${work}/${name}")
  file(MAKE_DIRECTORY "${dir}")
  file(WRITE "${dir}/main.atto" "${source}")
  execute_process(COMMAND "${BUILD}/oreslang-llvmc" --target=host
    "${dir}/main.atto" WORKING_DIRECTORY "${dir}"
    RESULT_VARIABLE baseline OUTPUT_VARIABLE baseline_ir
    ERROR_VARIABLE baseline_err TIMEOUT 30)
  string(FIND "${baseline_ir}" "; ModuleID" module_mark)
  string(FIND "${baseline_ir}" "define " native_function)
  if(NOT baseline STREQUAL "0" OR module_mark EQUAL -1 OR native_function EQUAL -1)
    fail_case("${name}: ordinary LLVM frontend did not emit function IR: ${baseline} ${baseline_err}")
  else()
    execute_process(COMMAND "${BUILD}/oreslang-llvmc"
      --aot-profile=sealed-class-fields-v1 --target=host
      "${dir}/main.atto" WORKING_DIRECTORY "${dir}"
      RESULT_VARIABLE rejected OUTPUT_VARIABLE output_ir
      ERROR_VARIABLE stderr TIMEOUT 30)
    string(FIND "${stderr}" "${needle}" found)
    file(GLOB emitted "${dir}/*.ll" "${dir}/*.bc" "${dir}/*.o"
      "${dir}/*.obj" "${dir}/*.s" "${dir}/*.exe")
    if(rejected STREQUAL "0" OR NOT rejected MATCHES "^[0-9]+$"
       OR NOT output_ir STREQUAL "" OR found EQUAL -1 OR emitted)
      fail_case("${name}: sealed admission did not reject/no IR: ${rejected} ${stderr}")
    else()
      message(STATUS "PASS ordinary-vs-sealed: ${name}")
    endif()
  endif()
  math(EXPR n "${cases}+1")
  set(cases "${n}" PARENT_SCOPE)
  set(failures "${failures}" PARENT_SCOPE)
endfunction()

function(fixture_contrast name fixture needle)
  file(READ "${ROOT}/tests/fixtures/${fixture}" code)
  profile_contrast("${name}" "${code}" "${needle}")
  set(cases "${cases}" PARENT_SCOPE)
  set(failures "${failures}" PARENT_SCOPE)
endfunction()

function(mutate_class name from into needle)
  file(READ "${ROOT}/tests/fixtures/class-public-field-read-native.ores" code)
  string(FIND "${code}" "${from}" at)
  if(at EQUAL -1)
    fail_case("${name}: original source span missing")
    set(failures "${failures}" PARENT_SCOPE)
    return()
  endif()
  string(REPLACE "${from}" "${into}" mutant "${code}")
  if(mutant STREQUAL code)
    fail_case("${name}: source mutation was a no-op")
  else()
    profile_contrast("${name}" "${mutant}" "${needle}")
    set(cases "${cases}" PARENT_SCOPE)
  endif()
  set(failures "${failures}" PARENT_SCOPE)
endfunction()

# Eight previously supported normal-LLVM native-source fixtures.
fixture_contrast(mutable_three_fields
  "class-multifield-independent-write.ores"
  "sealed class fields require immutable literal scalar defaults")
fixture_contrast(shared_class_borrow
  "class-multifield-shared-read.ores"
  "sealed class fields require immutable literal scalar defaults")
fixture_contrast(parameterized_constructor
  "class-scalar-constructor-arithmetic-native.ores"
  "sealed class-field AOT rejects methods or parameterized constructors")
fixture_contrast(mutating_receiver
  "class-multifield-mutator.ores"
  "sealed class fields require immutable literal scalar defaults")
fixture_contrast(mutable_owned_field
  "class-owned-field-native.ores"
  "sealed class fields require immutable literal scalar defaults")
fixture_contrast(take_in_guard
  "class-field-take-branch-native.ores"
  "sealed class fields require immutable literal scalar defaults")
fixture_contrast(actor_handler
  "source-actor-bare-method.ores"
  "sealed scalar AOT rejects non-scalar declarations")
fixture_contrast(actor_ready_continuation
  "source-actor-await-ready.ores"
  "sealed scalar AOT rejects non-scalar declarations")

# Three adversarial source mutations of a known-good immutable class program.
mutate_class(mutable_class_owner
  "let Public value = new Public();"
  "let mut Public value = new Public();"
  "sealed class-field AOT requires a fresh stack-only class local")
mutate_class(mutable_referent
  "pub final int token: 42;"
  "pub final mut int token: 42;"
  "sealed class fields require immutable literal scalar defaults")
mutate_class(mutable_class_slot
  "pub final int token: 42;"
  "pub let int token = 42;"
  "sealed class fields require immutable literal scalar defaults")

# A helper's IO effect may not disappear just because main never calls it.
file(READ "${ROOT}/tests/fixtures/class-public-field-read-native.ores" extra)
string(APPEND extra
  "\nfnc dormant(): void {\n  stdio.stdout.println(42);\n  return;\n}\n")
profile_contrast(unreachable_effectful_helper "${extra}"
  "sealed AOT requires fully modeled statically admitted effects and calls")

if(NOT cases EQUAL 12)
  message(FATAL_ERROR "expected exactly 12 profile contrasts, got ${cases}")
endif()
if(failures GREATER 0)
  message(FATAL_ERROR "sealed profile contrasts failed: ${failures}")
endif()
message(STATUS "12 native compiler profile-contrast tests passed")
