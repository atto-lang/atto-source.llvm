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

# Prototype freestanding actor kernel: host harness exercises the same C11
# implementation that compiles to ARM without libc or pthread dependencies.
# This does NOT yet prove source-defined actor execution under QEMU.
if(BUILD_TESTING)
    add_executable(oreslang-freestanding-actor-kernel-test
        tests/freestanding_actor_kernel_test.c
        src/main/cpp/oreslang/runtime/FreestandingActorKernel.c)
    target_include_directories(oreslang-freestanding-actor-kernel-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    if(MSVC)
        target_compile_options(oreslang-freestanding-actor-kernel-test PRIVATE /UNDEBUG)
    else()
        target_compile_options(oreslang-freestanding-actor-kernel-test PRIVATE
            -UNDEBUG -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME freestanding_actor_kernel_bounded_host
        COMMAND oreslang-freestanding-actor-kernel-test)
    set_tests_properties(freestanding_actor_kernel_bounded_host PROPERTIES TIMEOUT 15)
    # Independent bounded-mailbox reference model. Deterministic 100k-step
    # spawn/send/stop/snapshot/reset/turn matrix with stale-handle coverage.
    add_executable(oreslang-freestanding-actor-kernel-model-test
        tests/freestanding_actor_kernel_model_test.c
        src/main/cpp/oreslang/runtime/FreestandingActorKernel.c)
    target_include_directories(oreslang-freestanding-actor-kernel-model-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    if(MSVC)
        target_compile_options(oreslang-freestanding-actor-kernel-model-test PRIVATE /UNDEBUG)
    else()
        target_compile_options(oreslang-freestanding-actor-kernel-model-test PRIVATE
            -UNDEBUG -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME freestanding_actor_kernel_model_100k
        COMMAND oreslang-freestanding-actor-kernel-model-test)
    set_tests_properties(freestanding_actor_kernel_model_100k PROPERTIES TIMEOUT 30)
endif()

# Standalone MCU firmware with C actor handlers, not Oreslang source actors.
# Only register the positive + negative QEMU gate when all tools exist.
# CI targeting freestanding Cortex-M MUST set ORE_REQUIRE_CORTEXM_QEMU=ON,
# so missing emulator/toolchain cannot silently reduce test coverage.
option(ORE_REQUIRE_CORTEXM_QEMU "Require Cortex-M3/QEMU actor runtime validation" OFF)
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    find_program(ORE_CORTEXM_ARM_CLANG NAMES clang-18 clang)
    find_program(ORE_CORTEXM_READELF NAMES readelf llvm-readelf)
    find_program(ORE_CORTEXM_QEMU NAMES qemu-system-arm)
    if(ORE_REQUIRE_CORTEXM_QEMU AND
            (NOT ORE_CORTEXM_ARM_CLANG OR NOT ORE_CORTEXM_READELF OR NOT ORE_CORTEXM_QEMU))
        message(FATAL_ERROR "Cortex-M actor QEMU test requires clang, readelf and qemu-system-arm")
    endif()
    if(ORE_CORTEXM_ARM_CLANG AND ORE_CORTEXM_READELF AND ORE_CORTEXM_QEMU)
        add_test(NAME freestanding_actor_kernel_cortexm_qemu_positive_negative
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/freestanding_actor_kernel_qemu.py"
                --clang "${ORE_CORTEXM_ARM_CLANG}"
                --readelf "${ORE_CORTEXM_READELF}"
                --qemu "${ORE_CORTEXM_QEMU}")
        set_tests_properties(freestanding_actor_kernel_cortexm_qemu_positive_negative
            PROPERTIES TIMEOUT 120)
    endif()
endif()
