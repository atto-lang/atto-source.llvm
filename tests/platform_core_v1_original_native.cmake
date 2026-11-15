cmake_minimum_required(VERSION 3.20)
# Exact ORIGINAL Oreslang platform-core-v1 fixtures: compiler -> LLVM IR ->
# Clang native O0/O2 -> fixed C oracle using real i64 __oreslang_guest_main.
# Test never rewrites the Oreslang source. Only the compiled OS wrapper main
# symbol is renamed with objcopy to link the independent full-i64 oracle.
# This is NOT a cross-backend parity certificate until executed on pinned heads.
if(NOT DEFINED MODE OR NOT (MODE STREQUAL "validate" OR MODE STREQUAL "native"))
  message(FATAL_ERROR "platform-core-v1 MODE must be validate or native")
endif()
if(NOT DEFINED SOURCE_DIR OR NOT EXISTS "${SOURCE_DIR}/CMakeLists.txt" OR
   NOT EXISTS "${SOURCE_DIR}/contracts/platform-core-v1/manifest.json")
  message(FATAL_ERROR "platform-core-v1 requires an exact LLVM source checkout")
endif()
set(ROOT "${SOURCE_DIR}/contracts/platform-core-v1")
set(POSITIVE
 "arith-precedence.ores" "signed-minimum.ores" "max-minus-five.ores"
 "two-negative-factors.ores" "nested-unary.ores" "negative-product.ores"
 "eager-checked-int.ores")
set(NEGATIVE
 "reject-unknown-symbol.ores" "reject-string-as-int.ores"
 "reject-overflow.ores" "reject-xor-operator.ores"
 "reject-no-value-return.ores")
set(PINS
 "14f0a6fde3c55ead28f361da6c0e40279344137a"
 "450ebee951733d3f0b567413e70724155d26ce13"
 "ea1932dd800fdcf3ef00a01549ce6565d7691ade"
 "d83e07867159bb3f5b1027a0689f7eeefa84ac88"
 "c6aa564c5bd4ce38c5e5fcc1e96cec8cea58c6d3"
 "ba664537993dd3bb808f0d3453bd01f5435cc098"
 "b95fd94f36da98a8972f665eda126b46159de0f9"
 "fb17d2b5a662d435c367c682abbd5af7807f1e48"
 "494a5d02d86c7d8f6222c25e1ca33eb7f92c1fba"
 "eed3113842125b1d2a295a11e3cc01a5c1e3da00"
 "e7f5e47a28219f1f43cb1e18c7f50d45c089a6c4"
 "8cce1a3e24bec592834089d228f1047d61f510f0")
list(LENGTH POSITIVE good_count)
list(LENGTH NEGATIVE bad_count)
list(LENGTH PINS pin_count)
if(NOT good_count EQUAL 7 OR NOT bad_count EQUAL 5 OR NOT pin_count EQUAL 12)
  message(FATAL_ERROR "platform-core-v1 fixed corpus count drift")
endif()
find_program(GIT_EXECUTABLE git REQUIRED)
set(MANIFEST "${ROOT}/manifest.json")
if(IS_SYMLINK "${ROOT}" OR IS_SYMLINK "${MANIFEST}")
  message(FATAL_ERROR "symlinked platform-core manifest is forbidden")
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" hash-object --no-filters "${MANIFEST}"
 RESULT_VARIABLE git_rc OUTPUT_VARIABLE git_blob OUTPUT_STRIP_TRAILING_WHITESPACE
 TIMEOUT 5 ERROR_VARIABLE git_error)
if(NOT git_rc STREQUAL "0" OR
   NOT git_blob STREQUAL "8d06e0c7dff46253868954c595d2698ed5356e5f")
  message(FATAL_ERROR "platform-core-v1 Java/LLVM/Wasm manifest Git blob mismatch")
endif()
file(READ "${MANIFEST}" data LIMIT 65536)
string(JSON schema ERROR_VARIABLE json_err GET "${data}" schema)
if(json_err OR NOT schema STREQUAL "oreslang.platform-core.v1")
  message(FATAL_ERROR "platform-core-v1 manifest schema mismatch")
endif()
string(JSON candidates ERROR_VARIABLE json_err LENGTH "${data}" positive)
if(json_err OR NOT candidates EQUAL 7)
  message(FATAL_ERROR "platform-core-v1 positive manifest count mismatch")
endif()
string(JSON rejected ERROR_VARIABLE json_err LENGTH "${data}" negative)
if(json_err OR NOT rejected EQUAL 5)
  message(FATAL_ERROR "platform-core-v1 negative manifest count mismatch")
endif()
set(ALL "${POSITIVE};${NEGATIVE}")
foreach(index RANGE 0 11)
  list(GET ALL ${index} case_name)
  list(GET PINS ${index} expected_git_blob)
  set(source "${ROOT}/${case_name}")
  if(NOT EXISTS "${source}" OR IS_DIRECTORY "${source}" OR IS_SYMLINK "${source}")
    message(FATAL_ERROR "missing, symlinked or non-regular canonical fixture ${case_name}")
  endif()
  execute_process(COMMAND "${GIT_EXECUTABLE}" hash-object --no-filters "${source}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE observed_blob
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_VARIABLE err TIMEOUT 5)
  if(NOT rc STREQUAL "0" OR NOT observed_blob STREQUAL expected_git_blob)
    message(FATAL_ERROR "platform-core-v1 fixture Git blob differs from Java/Wasm ${case_name}")
  endif()
  if(index LESS 7)
    string(JSON declared ERROR_VARIABLE err GET "${data}" positive ${index} file)
    string(JSON expected ERROR_VARIABLE err_value GET "${data}" positive ${index} expected_stdout)
    if(err OR err_value OR NOT declared STREQUAL case_name OR
       NOT expected MATCHES "^-?[0-9]+\n$")
      message(FATAL_ERROR "noncanonical positive manifest case ${case_name}")
    endif()
  else()
    math(EXPR negative_index "${index} - 7")
    string(JSON declared ERROR_VARIABLE err GET "${data}" negative ${negative_index} file)
    string(JSON rejected_exit ERROR_VARIABLE err_exit GET "${data}" negative ${negative_index} expected_exit)
    if(err OR err_exit OR NOT declared STREQUAL case_name OR NOT rejected_exit STREQUAL "nonzero")
      message(FATAL_ERROR "noncanonical negative manifest case ${case_name}")
    endif()
  endif()
endforeach()
message(STATUS "PLATFORM_CORE_V1_SOURCE_BLOBS_VALIDATED=12 NOT_GUEST_EXECUTED=TRUE")
if(MODE STREQUAL "validate")
  return()
endif()
foreach(required IN ITEMS COMPILER CLANG OBJCOPY SHIM WORKDIR EXPECTED_SOURCE_SHA COMPILER_SHA256)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "platform-core-v1 native mode missing required ${required}")
  endif()
endforeach()
if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" OR
   NOT EXISTS "${COMPILER}" OR NOT EXISTS "${CLANG}" OR
   NOT EXISTS "${OBJCOPY}" OR NOT EXISTS "${SHIM}")
  message(FATAL_ERROR "platform-core-v1 native mode requires real Linux LLVM/Clang/objcopy/observer")
endif()
if(IS_SYMLINK "${WORKDIR}" OR "${WORKDIR}" STREQUAL "/" OR "${WORKDIR}" STREQUAL "${SOURCE_DIR}")
  message(FATAL_ERROR "unsafe native guest output directory")
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" rev-parse HEAD
 OUTPUT_VARIABLE head OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE head_rc TIMEOUT 5)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" rev-parse "HEAD^{tree}"
 OUTPUT_VARIABLE tree OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE tree_rc TIMEOUT 5)
if(NOT head_rc STREQUAL "0" OR NOT tree_rc STREQUAL "0" OR
   NOT head STREQUAL EXPECTED_SOURCE_SHA)
  message(FATAL_ERROR "platform-core-v1 source exact-head Git pin mismatch")
endif()
file(SHA256 "${COMPILER}" compiler_actual)
if(NOT compiler_actual STREQUAL COMPILER_SHA256)
  message(FATAL_ERROR "platform-core-v1 reviewed compiler SHA-256 mismatch")
endif()
file(MAKE_DIRECTORY "${WORKDIR}")
foreach(opt IN ITEMS O0 O2)
  execute_process(COMMAND "${CLANG}" "-${opt}" -std=c11 -Wall -Wextra -Werror
    -c "${SHIM}" -o "${WORKDIR}/observer.${opt}.o"
    RESULT_VARIABLE rc ERROR_VARIABLE err TIMEOUT 20)
  if(NOT rc STREQUAL "0")
    message(FATAL_ERROR "failed to compile fixed native observer: ${err}")
  endif()
endforeach()
set(executed 0)
foreach(index RANGE 0 11)
  list(GET ALL ${index} name)
  set(stem "${WORKDIR}/platform-core-${index}")
  set(src "${ROOT}/${name}")
  set(ir "${stem}.ll")
  file(REMOVE "${ir}")
  execute_process(COMMAND "${COMPILER}" "${src}"
    RESULT_VARIABLE compiled OUTPUT_FILE "${ir}" ERROR_VARIABLE compiler_stderr TIMEOUT 20)
  if(index GREATER 6 AND NOT index EQUAL 9)
    # Reject unknown symbol, implicit string conversion, removed ^ and no-return
    # as compiler diagnostics. OUTPUT_FILE can create an empty IR file even on
    # failure; only zero-length IR is acceptable and no signal/crash qualifies.
    if(compiled STREQUAL "0" OR NOT compiled MATCHES "^[0-9]+$" OR
       compiler_stderr STREQUAL "")
      message(FATAL_ERROR "compiler did not issue a normal negative diagnostic: ${name}: ${compiled}")
    endif()
    if(EXISTS "${ir}")
      file(SIZE "${ir}" rejected_ir_size)
      if(rejected_ir_size GREATER 0)
        message(FATAL_ERROR "negative compiler published nonempty LLVM IR: ${name}")
      endif()
    endif()
    continue()
  endif()
  if(NOT compiled STREQUAL "0" OR NOT EXISTS "${ir}")
    message(FATAL_ERROR "native LLVM compiler rejected expected compiled fixture ${name}: ${compiler_stderr}")
  endif()
  file(SIZE "${ir}" ir_size)
  if(ir_size LESS 16 OR ir_size GREATER 131072)
    message(FATAL_ERROR "invalid bounded LLVM IR for ${name}")
  endif()
  file(READ "${ir}" ir_text LIMIT 131072)
  if(NOT ir_text MATCHES "@__oreslang_guest_main" OR NOT ir_text MATCHES "@main")
    message(FATAL_ERROR "LLVM compiler did not emit original source guest ABI for ${name}")
  endif()
  if(index EQUAL 9 AND (NOT ir_text MATCHES "llvm[.]trap" OR
                        NOT ir_text MATCHES "with[.]overflow"))
    message(FATAL_ERROR "native checked overflow guard not found in LLVM IR")
  endif()
  foreach(opt IN ITEMS O0 O2)
    set(obj "${stem}.${opt}.o")
    set(native "${stem}.${opt}.native")
    execute_process(COMMAND "${CLANG}" "-${opt}" -Wno-override-module -x ir -c
      "${ir}" -o "${obj}" RESULT_VARIABLE rc ERROR_VARIABLE err TIMEOUT 20)
    if(NOT rc STREQUAL "0")
      message(FATAL_ERROR "LLVM native object failed ${name}/${opt}: ${err}")
    endif()
    execute_process(COMMAND "${OBJCOPY}" --redefine-sym main=platform_core_os_wrapper
      "${obj}" RESULT_VARIABLE rc ERROR_VARIABLE err TIMEOUT 10)
    if(NOT rc STREQUAL "0")
      message(FATAL_ERROR "cannot isolate native OS main wrapper: ${err}")
    endif()
    execute_process(COMMAND "${CLANG}" "-${opt}" "${WORKDIR}/observer.${opt}.o"
      "${obj}" -o "${native}" RESULT_VARIABLE rc ERROR_VARIABLE err TIMEOUT 20)
    if(NOT rc STREQUAL "0")
      message(FATAL_ERROR "failed to link LLVM guest + fixed oracle ${name}: ${err}")
    endif()
    if(index EQUAL 9)
      # SIGILL on Linux x86-64 conventionally becomes exit 132 through Bash.
      # This is a provisional trap ABI, not a typed Error or supervisor notice.
      execute_process(COMMAND bash -c
        "ulimit -c 0; \"$1\" >/dev/null 2>&1; code=$?; test \"$code\" -eq 132"
        bash "${native}"
        RESULT_VARIABLE result TIMEOUT 5 ERROR_VARIABLE native_stderr)
      if(NOT result STREQUAL "0")
        message(FATAL_ERROR "overflow did not trap SIGILL ${name}/${opt}: ${result}: ${native_stderr}")
      endif()
    else()
      string(JSON expected ERROR_VARIABLE err GET "${data}" positive ${index} expected_stdout)
      string(STRIP "${expected}" expected)
      execute_process(COMMAND "${native}" "${expected}" RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr TIMEOUT 5)
      if(NOT result STREQUAL "42" OR NOT stdout STREQUAL "" OR NOT stderr STREQUAL "")
        message(FATAL_ERROR "native full-i64 canonical result differs for ${name}/${opt}: ${result}")
      endif()
    endif()
    math(EXPR executed "${executed} + 1")
  endforeach()
endforeach()
if(NOT executed EQUAL 16)
  message(FATAL_ERROR "incomplete native platform-core-v1 execution count")
endif()
message(STATUS "PLATFORM_CORE_V1_LLVM_NATIVE_GUEST_EXECUTED=16; SOURCE_HEAD=${head}; TREE=${tree}")
