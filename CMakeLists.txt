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

# Host-only native actor hot-upgrade epoch/pin proof.
# Does NOT establish source actor migration, protected workers, PIC or ORC JIT.
if(BUILD_TESTING)
    find_package(Threads REQUIRED)
    add_executable(oreslang-actor-hot-upgrade-gate-test
        tests/actor_hot_upgrade_gate_test.cpp)
    target_include_directories(oreslang-actor-hot-upgrade-gate-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-actor-hot-upgrade-gate-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-actor-hot-upgrade-gate-test PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror -UNDEBUG>)
    add_test(NAME native_actor_hot_upgrade_epoch_gate_host_only
        COMMAND oreslang-actor-hot-upgrade-gate-test)
    set_tests_properties(native_actor_hot_upgrade_epoch_gate_host_only
        PROPERTIES TIMEOUT 20)
endif()

# Trusted POSIX fixture ONLY: two offline AOT-compiled PIC modules, an ABI
# incompatible module, and generation-pinned dynamic loader host regression.
# Not a production dynamic code loader, sandbox or cryptographic artifact gate.
if(BUILD_TESTING AND UNIX)
    find_package(Threads REQUIRED)
    add_library(oreslang-test-hot-image-old MODULE
        tests/fixtures/hot_aot_image_old.cpp)
    add_library(oreslang-test-hot-image-new MODULE
        tests/fixtures/hot_aot_image_new.cpp)
    add_library(oreslang-test-hot-image-incompatible MODULE
        tests/fixtures/hot_aot_image_incompatible.cpp)
    foreach(image_target IN ITEMS
            oreslang-test-hot-image-old
            oreslang-test-hot-image-new
            oreslang-test-hot-image-incompatible)
        target_compile_options(${image_target} PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>)
        set_target_properties(${image_target} PROPERTIES
            POSITION_INDEPENDENT_CODE ON)
    endforeach()
    add_executable(oreslang-test-hot-aot-pic-host
        tests/native_hot_aot_pic_loader_host_test.cpp)
    target_link_libraries(oreslang-test-hot-aot-pic-host PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads)
    target_compile_options(oreslang-test-hot-aot-pic-host PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror -UNDEBUG>)
    add_test(NAME native_hot_aot_pic_loader_host_only
        COMMAND oreslang-test-hot-aot-pic-host
            "$<TARGET_FILE:oreslang-test-hot-image-old>"
            "$<TARGET_FILE:oreslang-test-hot-image-new>"
            "$<TARGET_FILE:oreslang-test-hot-image-incompatible>")
    set_tests_properties(native_hot_aot_pic_loader_host_only
        PROPERTIES TIMEOUT 20)
endif()

# Independent, source-free canonical checkpoint wire oracle with normal and
# optimized Python semantics. This is NOT guest compiler state migration.
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME actor_checkpoint_v1_wire_oracle
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/actor_checkpoint_v1_wire.py")
    add_test(NAME actor_checkpoint_v1_wire_oracle_optimized
        COMMAND "${Python3_EXECUTABLE}" -O
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/actor_checkpoint_v1_wire.py")
    set_tests_properties(actor_checkpoint_v1_wire_oracle
        actor_checkpoint_v1_wire_oracle_optimized PROPERTIES TIMEOUT 20)
endif()

# Standalone checkpoint v1 host parity against the Java/Python golden.
# Native SHA-256 is an injected host provider; no crypto implementation or
# POSIX dependency is introduced into the freestanding actor/core substrate.
if(BUILD_TESTING)
    find_package(OpenSSL QUIET COMPONENTS Crypto)
    if(OpenSSL_FOUND)
        add_executable(oreslang-native-actor-checkpoint-v1-test
            tests/native_actor_checkpoint_v1_test.cpp)
        target_include_directories(oreslang-native-actor-checkpoint-v1-test PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
        target_link_libraries(oreslang-native-actor-checkpoint-v1-test PRIVATE
            OpenSSL::Crypto)
        target_compile_options(oreslang-native-actor-checkpoint-v1-test PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror -UNDEBUG>)
        add_test(NAME native_actor_checkpoint_v1_canonical_wire_host_only
            COMMAND oreslang-native-actor-checkpoint-v1-test)
        set_tests_properties(native_actor_checkpoint_v1_canonical_wire_host_only
            PROPERTIES TIMEOUT 25)
    else()
        message(STATUS "OpenSSL Crypto unavailable: skipping host checkpoint checksum parity test")
    endif()
endif()

# Trusted-host scalar hot-upgrade transaction proof only. This exercises
# real native generation leases, deterministic scalar checkpoint/restore and
# rollback, not compiled .ores actor code or sandboxed AOT executable loading.
if(BUILD_TESTING AND OpenSSL_FOUND)
    find_package(Threads REQUIRED)
    add_executable(oreslang-native-counter-upgrade-proof-test
        tests/native_counter_hot_upgrade_proof_test.cpp)
    target_include_directories(oreslang-native-counter-upgrade-proof-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-counter-upgrade-proof-test PRIVATE
        OpenSSL::Crypto Threads::Threads)
    target_compile_options(oreslang-native-counter-upgrade-proof-test PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror -UNDEBUG>)
    add_test(NAME native_counter_hot_upgrade_transaction_host_only
        COMMAND oreslang-native-counter-upgrade-proof-test)
    set_tests_properties(native_counter_hot_upgrade_transaction_host_only
        PROPERTIES TIMEOUT 20)
endif()

# Source-independent upgrade state machine regression (host-only).
if(BUILD_TESTING)
    add_test(NAME actor_hot_upgrade_model
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/actor_hot_upgrade_transaction_model.py")
    set_tests_properties(actor_hot_upgrade_model PROPERTIES TIMEOUT 25)
endif()

# Source-actor persistent-state wire oracle shared with Java evaluator v1.
# This remains a source-independent model, not guest actor execution.
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME source_actor_checkpoint_v1_wire_oracle
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/source_actor_checkpoint_v1_wire.py")
    add_test(NAME source_actor_checkpoint_v1_wire_oracle_optimized
        COMMAND "${Python3_EXECUTABLE}" -O
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/source_actor_checkpoint_v1_wire.py")
    set_tests_properties(source_actor_checkpoint_v1_wire_oracle
        source_actor_checkpoint_v1_wire_oracle_optimized PROPERTIES TIMEOUT 20)
endif()

# Native parity for Java evaluator's ORESAS01 scalar actor-state payload.
# This validates serialization only; executable-image provenance and actor
# routing remain separate production gates.
if(BUILD_TESTING AND OpenSSL_FOUND)
    add_executable(oreslang-native-source-actor-checkpoint-v1-test
        tests/native_source_actor_checkpoint_v1_test.cpp)
    target_include_directories(oreslang-native-source-actor-checkpoint-v1-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-source-actor-checkpoint-v1-test PRIVATE
        OpenSSL::Crypto)
    target_compile_options(oreslang-native-source-actor-checkpoint-v1-test PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror -UNDEBUG>)
    add_test(NAME native_source_actor_checkpoint_v1_java_parity_host_only
        COMMAND oreslang-native-source-actor-checkpoint-v1-test)
    set_tests_properties(native_source_actor_checkpoint_v1_java_parity_host_only
        PROPERTIES TIMEOUT 20)
endif()

if(BUILD_TESTING)
    add_test(NAME logical_actor_route_model
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/logical_actor_cross_context_route_model.py")
    add_test(NAME logical_actor_route_model_optimized
        COMMAND "${Python3_EXECUTABLE}" -O
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/logical_actor_cross_context_route_model.py")
    set_tests_properties(logical_actor_route_model logical_actor_route_model_optimized
        PROPERTIES TIMEOUT 30)
endif()

# Source-independent logical ActorRef route/upgrade linearization oracle shared
# with Java. Explicit checks remain active under python -O.
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME logical_actor_route_upgrade_model
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/logical_actor_route_upgrade_model.py")
    add_test(NAME logical_actor_route_upgrade_model_optimized
        COMMAND "${Python3_EXECUTABLE}" -O
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/logical_actor_route_upgrade_model.py")
    set_tests_properties(logical_actor_route_upgrade_model
        logical_actor_route_upgrade_model_optimized PROPERTIES TIMEOUT 20)
endif()
