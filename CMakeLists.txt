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

# Host-runtime actor slab heap (Offset32 + Handle32); no LLVM dependency.
# See docs/ACTOR_SLAB_HEAP.md. Not compiled .atto allocation evidence.
include(CTest)
if(BUILD_TESTING)
  add_executable(oreslang-actor-slab-heap-test tests/actor_slab_heap_test.cpp)
  target_include_directories(oreslang-actor-slab-heap-test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
  target_compile_options(oreslang-actor-slab-heap-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME native_actor_slab_heap_contract COMMAND oreslang-actor-slab-heap-test)
  set_tests_properties(native_actor_slab_heap_contract PROPERTIES TIMEOUT 120)
  add_executable(oreslang-actor-slab-heap-bench tests/actor_slab_heap_bench.cpp)
  target_include_directories(oreslang-actor-slab-heap-bench PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
  target_compile_options(oreslang-actor-slab-heap-bench PRIVATE -O2 -Wall -Wextra -Wpedantic -Werror)
endif()

# Cross-family composition is not source-supported on native LLVM yet.
# Keep unsupported syntax fail-closed until each case has real guest execution.
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME elm_composition_unsupported_source_admission
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/elm_composition_admission.py"
            "$<TARGET_FILE:oreslang-llvmc>")
    set_tests_properties(elm_composition_unsupported_source_admission
        PROPERTIES TIMEOUT 90)
endif()
