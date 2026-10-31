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

if(BUILD_TESTING)
    add_test(NAME inverted_typed_hir_and_native_owner
        COMMAND oreslang-inverted-typed-plan-test)
    set_tests_properties(inverted_typed_hir_and_native_owner PROPERTIES TIMEOUT 30)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    find_program(ORE_LLVM_IR_CLANG NAMES clang clang-23 clang-22 clang-21 clang-20 clang-19 clang-18 clang-17)
    if(NOT ORE_LLVM_IR_CLANG)
        message(FATAL_ERROR "The source-to-native smoke needs a Clang IR linker")
    endif()
    add_test(NAME inverted_i64_standalone_source_to_native
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_LIST_DIR}/../native_inverted_i64_guest.py"
            --compiler "$<TARGET_FILE:oreslang-inverted-i64-standalone-driver>"
            --clang "${ORE_LLVM_IR_CLANG}"
            --fixtures "${CMAKE_CURRENT_LIST_DIR}/../fixtures/inverted-i64")
    set_tests_properties(inverted_i64_standalone_source_to_native PROPERTIES TIMEOUT 180)
endif()
