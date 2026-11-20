cmake_minimum_required(VERSION 3.20)
# Temporary *carrier-only* build description. Every compiler implementation
# translation unit and project header is the exact Git blob from source PR #1490.
# This is a minimal reconstruction of its oreslang-llvmc target, NOT a full-tree
# mirror, not the source repository's complete CMake build and not production.
project(atto_llvm_verified_source_subset LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LLVM CONFIG REQUIRED)
if(LLVM_VERSION_MAJOR LESS 18)
  message(FATAL_ERROR "LLVM18_OR_NEWER_REQUIRED_FOR_COMPILER_GUEST_TEST")
endif()
message(STATUS "Compiling exact-origin oreslang-llvmc sources against LLVM ${LLVM_PACKAGE_VERSION}")
add_executable(oreslang-llvmc
  src/main/cpp/oreslang/main.cpp
  src/main/cpp/oreslang/frontend/Parser.cpp
  src/main/cpp/oreslang/frontend/ModuleLoader.cpp
  src/main/cpp/oreslang/frontend/MethodResolver.cpp
  src/main/cpp/oreslang/types/NativeScalarTypes.cpp
  src/main/cpp/oreslang/types/OwnershipChecker.cpp
  src/main/cpp/oreslang/backend/LlvmCodegen.cpp
  src/main/cpp/oreslang/backend/ScalarLoopLowering.cpp
)
target_include_directories(oreslang-llvmc PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp"
  "${LLVM_INCLUDE_DIRS}"
)
add_definitions(${LLVM_DEFINITIONS})
llvm_map_components_to_libnames(ORE_LLVM_LIBS core support)
target_link_libraries(oreslang-llvmc PRIVATE ${ORE_LLVM_LIBS})

# Experimental projection from the SAME canonical parser/type/ownership source.
# This is not a separately authored language frontend.
add_executable(atto-checked-actor-ir
  ci/m5/export_checked_actor_ir.cpp
  src/main/cpp/oreslang/frontend/Parser.cpp
  src/main/cpp/oreslang/frontend/MethodResolver.cpp
  src/main/cpp/oreslang/types/NativeScalarTypes.cpp
  src/main/cpp/oreslang/types/OwnershipChecker.cpp
  src/main/cpp/oreslang/backend/ScalarLoopLowering.cpp
)
target_include_directories(atto-checked-actor-ir PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp"
  "${LLVM_INCLUDE_DIRS}"
)
target_link_libraries(atto-checked-actor-ir PRIVATE ${ORE_LLVM_LIBS})

# Canonical trap Result v2: source-compiler/LLVM/Clang execution and no-IR
# negatives. Do not treat compile-only or zero-step CI as guest proof.
include(CTest)
if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  find_program(ATTO_TRAP_CLANG NAMES clang-22 clang-21 clang-20 clang-19 clang-18 clang REQUIRED)
  set(ATTO_TRAP_FIXTURES "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/trap-result-v2")
  set(ATTO_RECORD_RUNTIME "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeRecordRegion.c")
  add_test(NAME atto_trap_01_success_O0
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract.py"
      --compiler "$<TARGET_FILE:oreslang-llvmc>" --clang "${ATTO_TRAP_CLANG}" --target host
      --fixture "${ATTO_TRAP_FIXTURES}/01-success.atto"
      --fixture-git-blob 7600b92b1365be731017c757bdffb85350fd5038 --run 42 --opt-level 0
      --native-runtime "${ATTO_RECORD_RUNTIME}")
  add_test(NAME atto_trap_01_success_O2
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract.py"
      --compiler "$<TARGET_FILE:oreslang-llvmc>" --clang "${ATTO_TRAP_CLANG}" --target host
      --fixture "${ATTO_TRAP_FIXTURES}/01-success.atto"
      --fixture-git-blob 7600b92b1365be731017c757bdffb85350fd5038 --run 42 --opt-level 2
      --native-runtime "${ATTO_RECORD_RUNTIME}")
  add_test(NAME atto_trap_02_existing_ok_O0
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract.py"
      --compiler "$<TARGET_FILE:oreslang-llvmc>" --clang "${ATTO_TRAP_CLANG}" --target host
      --fixture "${ATTO_TRAP_FIXTURES}/02-existing-ok.atto"
      --fixture-git-blob b1e7fb880b9a81b8d9c1f7209d256846a220a4f2 --run 42 --opt-level 0
      --native-runtime "${ATTO_RECORD_RUNTIME}")
  add_test(NAME atto_trap_02_existing_ok_O2
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract.py"
      --compiler "$<TARGET_FILE:oreslang-llvmc>" --clang "${ATTO_TRAP_CLANG}" --target host
      --fixture "${ATTO_TRAP_FIXTURES}/02-existing-ok.atto"
      --fixture-git-blob b1e7fb880b9a81b8d9c1f7209d256846a220a4f2 --run 42 --opt-level 2
      --native-runtime "${ATTO_RECORD_RUNTIME}")
  add_test(NAME atto_trap_03_existing_err_O0
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract.py"
      --compiler "$<TARGET_FILE:oreslang-llvmc>" --clang "${ATTO_TRAP_CLANG}" --target host
      --fixture "${ATTO_TRAP_FIXTURES}/03-existing-err.atto"
      --fixture-git-blob 7bbebd3c2b0621c37163f9120c62360f64688bcc --run 42 --opt-level 0
      --native-runtime "${ATTO_RECORD_RUNTIME}")
  add_test(NAME atto_trap_03_existing_err_O2
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract.py"
      --compiler "$<TARGET_FILE:oreslang-llvmc>" --clang "${ATTO_TRAP_CLANG}" --target host
      --fixture "${ATTO_TRAP_FIXTURES}/03-existing-err.atto"
      --fixture-git-blob 7bbebd3c2b0621c37163f9120c62360f64688bcc --run 42 --opt-level 2
      --native-runtime "${ATTO_RECORD_RUNTIME}")
  add_test(NAME atto_trap_04_nested_trap_O0
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract.py"
      --compiler "$<TARGET_FILE:oreslang-llvmc>" --clang "${ATTO_TRAP_CLANG}" --target host
      --fixture "${ATTO_TRAP_FIXTURES}/04-nested-trap.atto"
      --fixture-git-blob 0e4605f1d8939a9dfb542805cfd3b17ec782ddc2 --run 42 --opt-level 0
      --native-runtime "${ATTO_RECORD_RUNTIME}")
  add_test(NAME atto_trap_04_nested_trap_O2
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract.py"
      --compiler "$<TARGET_FILE:oreslang-llvmc>" --clang "${ATTO_TRAP_CLANG}" --target host
      --fixture "${ATTO_TRAP_FIXTURES}/04-nested-trap.atto"
      --fixture-git-blob 0e4605f1d8939a9dfb542805cfd3b17ec782ddc2 --run 42 --opt-level 2
      --native-runtime "${ATTO_RECORD_RUNTIME}")
  add_test(NAME atto_trap_05_bool_success_O0
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract.py"
      --compiler "$<TARGET_FILE:oreslang-llvmc>" --clang "${ATTO_TRAP_CLANG}" --target host
      --fixture "${ATTO_TRAP_FIXTURES}/05-bool-success.atto"
      --fixture-git-blob 2aa5028371a22900d2e9addaf40401a0c227c697 --run 42 --opt-level 0
      --native-runtime "${ATTO_RECORD_RUNTIME}")
  add_test(NAME atto_trap_05_bool_success_O2
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract.py"
      --compiler "$<TARGET_FILE:oreslang-llvmc>" --clang "${ATTO_TRAP_CLANG}" --target host
      --fixture "${ATTO_TRAP_FIXTURES}/05-bool-success.atto"
      --fixture-git-blob 2aa5028371a22900d2e9addaf40401a0c227c697 --run 42 --opt-level 2
      --native-runtime "${ATTO_RECORD_RUNTIME}")
  add_test(NAME atto_trap_11_reject_option_output
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract.py"
      --compiler "$<TARGET_FILE:oreslang-llvmc>" --target host
      --fixture "${ATTO_TRAP_FIXTURES}/11-reject-option-output.atto"
      --fixture-git-blob 0619a2815fc217161bc401bce2606414799d6000 --diagnostic "native trap produces Result<T,Error>, not Option or scalar")
  add_test(NAME atto_trap_12_reject_option_input
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract.py"
      --compiler "$<TARGET_FILE:oreslang-llvmc>" --target host
      --fixture "${ATTO_TRAP_FIXTURES}/12-reject-option-input.atto"
      --fixture-git-blob 23b5052b4f711248d1ecd2b78cc879e36fc914a7 --diagnostic "native trap does not implicitly convert Option into Result")
  add_test(NAME atto_trap_13_reject_forged_ok
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract.py"
      --compiler "$<TARGET_FILE:oreslang-llvmc>" --target host
      --fixture "${ATTO_TRAP_FIXTURES}/13-reject-forged-ok.atto"
      --fixture-git-blob 73a7efc99f77fcb5c9fbbd0b0e4e345fd27ac320 --diagnostic "native Result<T,Error> may only be constructed by checked trap")
  add_test(NAME atto_trap_14_reject_forged_err
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract.py"
      --compiler "$<TARGET_FILE:oreslang-llvmc>" --target host
      --fixture "${ATTO_TRAP_FIXTURES}/14-reject-forged-err.atto"
      --fixture-git-blob d0c670b62da40a8862dbe30d722147a97281eef0 --diagnostic "native Result<T,Error> may only be constructed by checked trap")
  add_test(NAME atto_trap_15_reject_wrong_result
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract.py"
      --compiler "$<TARGET_FILE:oreslang-llvmc>" --target host
      --fixture "${ATTO_TRAP_FIXTURES}/15-reject-wrong-result.atto"
      --fixture-git-blob a7be0dee34c62cf313ebcabeb9eb5754a60f1aae --diagnostic "native trap value type does not match Result<T,Error>")
  add_test(NAME atto_trap_16_reject_impure_call
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract.py"
      --compiler "$<TARGET_FILE:oreslang-llvmc>" --target host
      --fixture "${ATTO_TRAP_FIXTURES}/16-reject-impure-call.atto"
      --fixture-git-blob 1a111733be303bfacab3059fa578d991d50728f2 --diagnostic "native trap requires nonthrowing scalar or existing Result value")
  add_test(NAME atto_trap_17_reject_string_allocation
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract.py"
      --compiler "$<TARGET_FILE:oreslang-llvmc>" --target host
      --fixture "${ATTO_TRAP_FIXTURES}/17-reject-string-allocation.atto"
      --fixture-git-blob 9e2d1275106562bbe0f9bb1d80291819e6e879ca --diagnostic "native trap String allocation requires typed Error cleanup lowering")
  add_test(NAME atto_trap_terminal_overflow_O0_O2
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/trap_result_terminal_aot.py"
      --compiler "$<TARGET_FILE:oreslang-llvmc>" --clang "${ATTO_TRAP_CLANG}" --root "${CMAKE_CURRENT_SOURCE_DIR}"
      --fixture "${ATTO_TRAP_FIXTURES}/06-overflow-terminal.atto"
      --fixture-git-blob 3aeac8aefd880f91d16cf75e88a1d1c401382aaa)
endif()
