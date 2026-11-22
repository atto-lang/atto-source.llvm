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
    # Host-executed actor-owned logical guest frame storage (not LLVM guest
    # codegen and not independent physical C/OS stacks).
    find_package(Threads REQUIRED)
    add_executable(oreslang-native-actor-guest-frame-stack-test
        tests/native_actor_guest_frame_stack_test.cpp
        src/main/cpp/oreslang/runtime/MultiActorCarrier.cpp)
    target_include_directories(oreslang-native-actor-guest-frame-stack-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_features(oreslang-native-actor-guest-frame-stack-test
        PRIVATE cxx_std_17)
    target_link_libraries(oreslang-native-actor-guest-frame-stack-test
        PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-actor-guest-frame-stack-test PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Werror -UNDEBUG>)
    add_test(NAME native_actor_guest_frame_stack_host_only
        COMMAND oreslang-native-actor-guest-frame-stack-test)
    set_tests_properties(native_actor_guest_frame_stack_host_only
        PROPERTIES TIMEOUT 15)
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
