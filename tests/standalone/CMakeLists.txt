cmake_minimum_required(VERSION 3.20)
project(oreslang_sealed_state_admission_source_free LANGUAGES CXX)

# This source-free entry point intentionally does not configure the root
# project: find_package(LLVM CONFIG REQUIRED) there is an unrelated native
# compiler gate and must not block header-only safety checks.
include(CTest)

if(BUILD_TESTING)
    add_executable(oreslang-sealed-state-admission-source-free-test
        ../sealed_state_admission_source_free_test.cpp)
    target_compile_features(oreslang-sealed-state-admission-source-free-test
        PRIVATE cxx_std_17)
    set_target_properties(oreslang-sealed-state-admission-source-free-test
        PROPERTIES CXX_EXTENSIONS OFF)
    target_include_directories(oreslang-sealed-state-admission-source-free-test
        PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src/main/cpp")
    target_compile_options(oreslang-sealed-state-admission-source-free-test PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror -UNDEBUG>)
    add_test(NAME native_sealed_state_admission_source_free
        COMMAND oreslang-sealed-state-admission-source-free-test)
    set_tests_properties(native_sealed_state_admission_source_free
        PROPERTIES TIMEOUT 30)
endif()
