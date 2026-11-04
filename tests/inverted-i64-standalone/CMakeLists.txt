cmake_minimum_required(VERSION 3.18)
project(oreslang_inverted_i64_source_adapter_smoke LANGUAGES CXX)
include(CTest)

# Standalone dependency-free C++ adapter check, NOT full oreslang-llvmc.
add_executable(oreslang-inverted-i64-standalone-driver
    ../inverted_i64_standalone_driver.cpp)
target_include_directories(oreslang-inverted-i64-standalone-driver PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../src/main/cpp")
target_compile_features(oreslang-inverted-i64-standalone-driver PRIVATE cxx_std_20)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(oreslang-inverted-i64-standalone-driver PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()

# Compiler-owned typed linear HIR and checked native row-handle runtime,
# compiled directly without LLVM development headers or runtime libraries.
find_package(Threads REQUIRED)
add_executable(oreslang-inverted-typed-plan-test
    ../native_inverted_typed_plan_test.cpp)
target_include_directories(oreslang-inverted-typed-plan-test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../src/main/cpp")
target_compile_features(oreslang-inverted-typed-plan-test PRIVATE cxx_std_20)
target_link_libraries(oreslang-inverted-typed-plan-test PRIVATE Threads::Threads)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(oreslang-inverted-typed-plan-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
endif()

add_executable(oreslang-inverted-mir-ir-guest
    ../native_inverted_mir_llvm_guest.cpp)
target_include_directories(oreslang-inverted-mir-ir-guest PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../src/main/cpp")
set_target_properties(oreslang-inverted-mir-ir-guest PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(oreslang-inverted-mir-ir-guest PRIVATE
    -Wall -Wextra -Wpedantic -Werror)

add_executable(oreslang-inverted-schema-mir-preflight-test
    ../native_inverted_schema_mir_preflight_test.cpp)
target_include_directories(oreslang-inverted-schema-mir-preflight-test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../src/main/cpp")
set_target_properties(oreslang-inverted-schema-mir-preflight-test PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(oreslang-inverted-schema-mir-preflight-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
endif()

add_executable(oreslang-inverted-production-mir-test
    ../native_inverted_production_mir_test.cpp)
target_include_directories(oreslang-inverted-production-mir-test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../src/main/cpp")
set_target_properties(oreslang-inverted-production-mir-test PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(oreslang-inverted-production-mir-test PRIVATE
    -Wall -Wextra -Wpedantic -Werror -UNDEBUG)

add_executable(oreslang-inverted-mir-compiler17-test
    ../native_inverted_mir_compiler17_test.cpp)
target_include_directories(oreslang-inverted-mir-compiler17-test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../src/main/cpp")
set_target_properties(oreslang-inverted-mir-compiler17-test PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(oreslang-inverted-mir-compiler17-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
endif()

add_executable(oreslang-inverted-typed-mir-test
    ../native_inverted_typed_mir_test.cpp)
target_include_directories(oreslang-inverted-typed-mir-test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../src/main/cpp")
target_compile_features(oreslang-inverted-typed-mir-test PRIVATE cxx_std_20)
target_link_libraries(oreslang-inverted-typed-mir-test PRIVATE Threads::Threads)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(oreslang-inverted-typed-mir-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
endif()

if(BUILD_TESTING)
    add_test(NAME inverted_production_source_mir_cpp17
        COMMAND oreslang-inverted-production-mir-test)
    set_tests_properties(inverted_production_source_mir_cpp17 PROPERTIES TIMEOUT 30)
    add_test(NAME inverted_production_schema_mir_preflight
        COMMAND oreslang-inverted-schema-mir-preflight-test)
    set_tests_properties(inverted_production_schema_mir_preflight PROPERTIES TIMEOUT 30)
    add_test(NAME inverted_mir_compiler_cpp17
        COMMAND oreslang-inverted-mir-compiler17-test)
    set_tests_properties(inverted_mir_compiler_cpp17 PROPERTIES TIMEOUT 30)
    add_test(NAME inverted_typed_mir_native_columns
        COMMAND oreslang-inverted-typed-mir-test)
    set_tests_properties(inverted_typed_mir_native_columns PROPERTIES TIMEOUT 30)
    add_test(NAME inverted_typed_hir_and_native_owner
        COMMAND oreslang-inverted-typed-plan-test)
    set_tests_properties(inverted_typed_hir_and_native_owner PROPERTIES TIMEOUT 30)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    find_program(ORE_LLVM_IR_CLANG NAMES clang clang-23 clang-22 clang-21 clang-20 clang-19 clang-18 clang-17)
    if(NOT ORE_LLVM_IR_CLANG)
        message(FATAL_ERROR "The source-to-native smoke needs a Clang IR linker")
    endif()
    add_test(NAME inverted_verified_mir_ir_native_guest
        COMMAND bash "${CMAKE_CURRENT_LIST_DIR}/../native_inverted_mir_ir_guest.sh"
            "$<TARGET_FILE:oreslang-inverted-mir-ir-guest>"
            "${ORE_LLVM_IR_CLANG}")
    set_tests_properties(inverted_verified_mir_ir_native_guest PROPERTIES TIMEOUT 120)
    # Synthetic source-free smoke exercises the *test gate*, not AttoLang
    # compiler correctness: catch false numeric exit-132 and wrong diagnostics.
    add_test(NAME inverted_mir_ir_gate_source_free_selftest
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_LIST_DIR}/../native_inverted_mir_gate_selftest.py"
            --shell "${CMAKE_CURRENT_LIST_DIR}/../native_inverted_mir_ir_guest.sh"
            --clang "${ORE_LLVM_IR_CLANG}")
    set_tests_properties(inverted_mir_ir_gate_source_free_selftest PROPERTIES TIMEOUT 180)
    add_test(NAME inverted_i64_standalone_source_to_native
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_LIST_DIR}/../native_inverted_i64_guest.py"
            --compiler "$<TARGET_FILE:oreslang-inverted-i64-standalone-driver>"
            --clang "${ORE_LLVM_IR_CLANG}"
            --fixtures "${CMAKE_CURRENT_LIST_DIR}/../fixtures/inverted-i64")
    set_tests_properties(inverted_i64_standalone_source_to_native PROPERTIES TIMEOUT 180)
endif()
