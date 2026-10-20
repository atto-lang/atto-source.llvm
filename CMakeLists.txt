Warning: truncated output (original token count: 37499)
Total output lines: 2176

cmake_minimum_required(VERSION 3.20)
project(oreslang_llvm VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Managed Oreslang references are a language/runtime storage policy, separate
# from the native target ABI. Keep compact actor-local references as the
# default on 64-bit hosts while preserving an explicit full-width mode.
set(ORES_MANAGED_REF_MODE "compressed32" CACHE STRING
    "Default native managed reference representation: compressed32 or native64")
set_property(CACHE ORES_MANAGED_REF_MODE PROPERTY STRINGS compressed32 native64)
if(ORES_MANAGED_REF_MODE STREQUAL "native64")
    add_compile_definitions(ORES_MANAGED_REF_DEFAULT_NATIVE64=1)
elseif(NOT ORES_MANAGED_REF_MODE STREQUAL "compressed32")
    message(FATAL_ERROR
        "ORES_MANAGED_REF_MODE must be compressed32 or native64; got '${ORES_MANAGED_REF_MODE}'")
endif()
message(STATUS "Oreslang managed reference mode: ${ORES_MANAGED_REF_MODE}")

find_package(LLVM CONFIG REQUIRED)
if(LLVM_VERSION_MAJOR LESS 14)
    message(FATAL_ERROR "Oreslang requires LLVM 14 or newer; found ${LLVM_PACKAGE_VERSION}")
endif()
message(STATUS "Building Oreslang LLVM against LLVM ${LLVM_PACKAGE_VERSION}")

add_executable(oreslang-llvmc
    src/main/cpp/oreslang/main.cpp
    src/main/cpp/oreslang/frontend/Parser.cpp
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

include(CTest)
if(BUILD_TESTING)
    # Strict hard-RT substrate: caller-backed, sealed/prefaulted, single-carrier
    # arena with no system allocator or lock path after initialization.
    add_executable(oreslang-realtime-actor-arena-test
        tests/realtime_actor_arena_test.cpp
        src/main/cpp/oreslang/runtime/RealtimeActorArena.c)
    target_include_directories(oreslang-realtime-actor-arena-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    set_target_properties(oreslang-realtime-actor-arena-test PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_options(oreslang-realtime-actor-arena-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_realtime_actor_arena_contract
        COMMAND oreslang-realtime-actor-arena-test)
    set_tests_properties(native_realtime_actor_arena_contract PROPERTIES TIMEOUT 15)

    # Compiler-owned transitive effect/call-graph hard-RT admission.
    add_executable(oreslang-realtime-analysis-test
        tests/realtime_analysis_test.cpp)
    target_include_directories(oreslang-realtime-analysis-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-realtime-analysis-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_realtime_transitive_effect_analysis
        COMMAND oreslang-realtime-analysis-test)
    set_tests_properties(native_realtime_transitive_effect_analysis PROPERTIES TIMEOUT 15)

    # Oreslang core synchronization, not Java/JVM locks. The C11 gate itself
    # is freestanding and nonparking; pthreads are used only in this test.
    find_package(Threads REQUIRED)
    add_executable(oreslang-native-core-sync-test tests/native_core_sync_test.c)
    set_target_properties(oreslang-native-core-sync-test PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_include_directories(oreslang-native-core-sync-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-core-sync-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-core-sync-test PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>)
    # Keep assert() live in Release/NDEBUG: these are security admission checks.
    target_compile_options(oreslang-native-core-sync-test PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-UNDEBUG>)
    add_test(NAME native_core_atomic_mutex_rw_admission
        COMMAND oreslang-native-core-sync-test)
    set_tests_properties(native_core_atomic_mutex_rw_admission
        PROPERTIES TIMEOUT 45)
    # Oreslang stdlib scheduler adapter: bounded nonparking logical tickets,
    # FIFO mutex, reader cohorts, physical-quiescence fences and cancellation.
    add_executable(oreslang-native-std-sync-test tests/native_std_sync_admission_test.c)
    set_target_properties(oreslang-native-std-sync-test PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_include_directories(oreslang-native-std-sync-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-std-sync-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-std-sync-test PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror -UNDEBUG>)
    add_test(NAME native_stdlib_mutex_rw_queue_and_quiescence
        COMMAND oreslang-native-std-sync-test)
    set_tests_properties(native_stdlib_mutex_rw_queue_and_quiescence
        PROPERTIES TIMEOUT 60)

    add_executable(oreslang-native-actor-state-rw-test
        tests/native_actor_state_rw_admission_test.c)
    set_target_properties(oreslang-native-actor-state-rw-test PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_include_directories(oreslang-native-actor-state-rw-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-actor-state-rw-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-actor-state-rw-test PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror -UNDEBUG>)
    add_test(NAME native_actor_state_rw_critical_using_admission
        COMMAND oreslang-native-actor-state-rw-test)
    set_tests_properties(native_actor_state_rw_critical_using_admission
        PROPERTIES TIMEOUT 60)

    # Native host-only CoreLog diagnostics. This test does not require guest
    # source/OTel modules or grant the compiled program a logging capability.
    find_package(Threads REQUIRED)
    add_executable(oreslang-native-core-log-test tests/native_core_log_test.cpp)
    target_include_directories(oreslang-native-core-log-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-core-log-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-core-log-test PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>)
    add_test(NAME native_core_log_severity_capacity_and_concurrency
        COMMAND oreslang-native-core-log-test)
    set_tests_properties(native_core_log_severity_capacity_and_concurrency
        PROPERTIES TIMEOUT 20)
    # HOST-ONLY closed Result<void,ArrayError> nominal/tag and exhaustive
    # pattern proof. This is not guest source Result/match lowering.
    add_executable(oreslang-native-array-result-contract-test
        tests/native_array_result_contract_test.cpp)
    target_include_directories(oreslang-native-array-result-contract-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-native-array-result-contract-test PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>)
    add_test(NAME native_array_result_nominal_closed_proof_host_only
        COMMAND oreslang-native-array-result-contract-test)
    set_tests_properties(native_array_result_nominal_closed_proof_host_only
        PROPERTIES TIMEOUT 20)
    # Host-only fairness budget; not evidence of compiled Oreslang suspension.
    add_executable(oreslang-native-reduction-quantum-test
        tests/native_cooperation_quantum_test.cpp)
    target_include_directories(oreslang-native-reduction-quantum-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-native-reduction-quantum-test PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>)
    add_test(NAME native_reduction_quantum_watchdog_host_only
        COMMAND oreslang-native-reduction-quantum-test)
    set_tests_properties(native_reduction_quantum_watchdog_host_only
        PROPERTIES TIMEOUT 15)

    # Native Array<int> substrate, independent of source-level admission.
    # Invalid operations must abort even in Release/NDEBUG.
    add_executable(oreslang-native-int-array-region-test
        tests/native_int_array_region_test.c
        src/main/cpp/oreslang/runtime/NativeIntArrayRegion.c)
    target_include_directories(oreslang-native-int-array-region-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_definitions(oreslang-native-int-array-region-test PRIVATE
        ORESLANG_NATIVE_TEST_TOKEN_EXHAUSTION=1)
    target_compile_options(oreslang-native-int-array-region-test PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Werror -Wpedantic>)
    add_test(NAME native_int_array_region_security
        COMMAND oreslang-native-int-array-region-test)

    # Checked, recoverable internal allocation/resize + staged prepare/commit.
    # Failpoint injection only exists in this test executable; existing
    # compiler-generated abort ABI and production runtime remain unchanged.
    add_executable(oreslang-native-int-array-transaction-test
        tests/native_int_array_transaction_test.c
        src/main/cpp/oreslang/runtime/NativeIntArrayRegion.c)
    target_include_directories(oreslang-native-int-array-transaction-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_definitions(oreslang-native-int-array-transaction-test PRIVATE
        ORESLANG_NATIVE_TEST_ALLOC_FAILURE=1)
    target_compile_options(oreslang-native-int-array-transaction-test PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Werror -Wpedantic>)
    add_test(NAME native_int_array_transaction_recovery
        COMMAND oreslang-native-int-array-transaction-test)
    set_tests_properties(native_int_array_transaction_recovery PROPERTIES TIMEOUT 120)
    # Deterministic model-based differential stress on real Array<int> ABI.
    add_executable(oreslang-native-int-array-model-test
        tests/native_int_array_model_test.c
        src/main/cpp/oreslang/runtime/NativeIntArrayRegion.c)
    target_include_directories(oreslang-native-int-array-model-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-native-int-array-model-test PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Werror -Wpedantic>)
    add_test(NAME native_int_array_model_differential
        COMMAND oreslang-native-int-array-model-test)
    set_tests_properties(native_int_array_model_differential PROPERTIES TIMEOUT 90)

    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    # Parser/runtime do not yet enforce refinements. A valid compiler control
    # followed by unsupported refinement sources must emit no partial LLVM IR.
    add_test(NAME native_refinement_admission_failclosed
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/refinement_admission_gate.py"
            --compiler "$<TARGET_FILE:oreslang-llvmc>")
    set_tests_properties(native_refinement_admission_failclosed PROPERTIES TIMEOUT 180)
    add_test(NAME native_realtime_profile_cli
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_realtime_profile_cli.py"
            "$<TARGET_FILE:oreslang-llvmc>")
    set_tests_properties(native_realtime_profile_cli PROPERTIES TIMEOUT 60)

    # Executable source admission gate: function values are not nominal classes.
    add_test(NAME native_callable_nominal_boundary
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_callable_nominal_boundary.py"
            --compiler "$<TARGET_FILE:oreslang-llvmc>")
    set_tests_properties(native_callable_nominal_boundary PROPERTIES TIMEOUT 35)
    add_test(NAME native_scalar_generic_contract_harness_selftest
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_scalar_generic_contract.py")
    set_tests_properties(native_scalar_generic_contract_harness_selftest PROPERTIES TIMEOUT 25)
    add_test(NAME native_scalar_generic_contract_harness_python_O
        COMMAND "${Python3_EXECUTABLE}" -O
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_scalar_generic_contract.py")
    set_tests_properties(native_scalar_generic_contract_harness_python_O PROPERTIES TIMEOUT 25)
    add_test(NAME native_compiler_source_budget
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_source_budget.py"
            "$<TARGET_FILE:oreslang-llvmc>")
    set_tests_properties(native_compiler_source_budget PROPERTIES TIMEOUT 25)
    # End-to-end source -> LLVM IR -> Clang -> actual native OS process.
    # Four portable cases have lossless exit status; DO NOT claim seven.
    find_program(ORE_SOURCE_I64_CLANG NAMES clang-18 clang-19 clang clang-17 clang-16 clang-15 clang-14)
    if(NOT ORE_SOURCE_I64_CLANG)
        message(WARNING "Portable native i64 execution gate will fail: Clang not found")
        set(ORE_SOURCE_I64_CLANG "${CMAKE_CURRENT_BINARY_DIR}/missing-source-i64-clang")
    endif()
    add_test(NAME native_source_i64_os_exit_subset
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_source_i64_executable.py"
            --compiler "$<TARGET_FILE:oreslang-llvmc>"
            --clang "${ORE_SOURCE_I64_CLANG}")
    set_tests_properties(native_source_i64_os_exit_subset PROPERTIES TIMEOUT 120)
    # First executable-level tree-shaking proof: main/pub roots plus transitive
    # direct calls. Size is measured but not thresholded for this tiny ELF fixture.
    add_test(NAME native_tree_shaking_direct_calls
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_tree_shaking.py"
            --compiler "$<TARGET_FILE:oreslang-llvmc>"
            --clang "${ORE_SOURCE_I64_CLANG}")
    set_tests_properties(native_tree_shaking_direct_calls PROPERTIES TIMEOUT 120)
    # Self-test for a measurement-only compile benchmark. No performance
    # threshold is asserted in CI: runner timing is not reproducible.
    add_test(NAME native_compile_benchmark_harness_selftest
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compile_benchmark_harness.py")
    set_tests_properties(native_compile_benchmark_harness_selftest
        PROPERTIES TIMEOUT 30)
    find_program(ORE_NATIVE_ARRAY_CLANG NAMES clang-18 clang-19 clang clang-17 clang-16 clang-15 clang-14)
    if(NOT ORE_NATIVE_ARRAY_CLANG)
        message(WARNING "Native array LLVM ABI gate cannot pass without Clang")
        set(ORE_NATIVE_ARRAY_CLANG "${CMAKE_CURRENT_BINARY_DIR}/missing-clang")
    endif()
    add_test(NAME native_annotation_macro_aot_admission
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/annotation_macro_aot_admission_matrix.py"
            --compiler "$<TARGET_FILE:oreslang-llvmc>"
            --clang "${ORE_NATIVE_ARRAY_CLANG}")
    set_tests_properties(native_annotation_macro_aot_admission PROPERTIES TIMEOUT 180)
    add_test(NAME native_annotation_macro_aot_harness_selftest
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_annotation_macro_aot_harness.py")
    set_tests_properties(native_annotation_macro_aot_harness_selftest PROPERTIES TIMEOUT 30)
    add_test(NAME native_int_array_llvm_abi
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_int_array_llvm_abi.py"
          --clang "${ORE_NATIVE_ARRAY_CLANG}"
          --source-root "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(native_int_array_llvm_abi PROPERTIES TIMEOUT 120)

    # Actual Oreslang -> LLVM compiler-generated IR -> Clang linked with
    # the trusted region ABI. Separate from hand-authored native ABI fixture.
    add_test(NAME native_int_array_source_execution
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_int_array_source_execution.py"
            --compiler "$<TARGET_FILE:oreslang-llvmc>"
            --clang "${ORE_NATIVE_ARRAY_CLANG}"
            --source-root "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME native_int_array_source_asan_ubsan
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_int_array_source_execution.py"
            --compiler "$<TARGET_FILE:oreslang-llvmc>"
            --clang "${ORE_NATIVE_ARRAY_CLANG}"
            --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
            --sanitizers)
    set_tests_properties(native_int_array_source_execution
        native_int_array_source_asan_ubsan PROPERTIES TIMEOUT 180)

    # Verify compiler-generated checked array allocation/growth actually
    # destroys exactly the initialized owners on every injected fault.
    # Requires the Linux ELF --wrap linker protocol.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_test(NAME native_checked_array_source_failure_cleanup
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_checked_array_failure_source.py"
                --compiler "$<TARGET_FILE:oreslang-llvmc>"
                --clang "${ORE_NATIVE_ARRAY_CLANG}"
                --source-root "${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME native_checked_array_source_failure_asan_ubsan
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_checked_array_failure_source.py"
                --compiler "$<TARGET_FILE:oreslang-llvmc>"
                --clang "${ORE_NATIVE_ARRAY_CLANG}"
                --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
                --sanitizers)
        set_tests_properties(native_checked_array_source_failure_cleanup
            native_checked_array_source_failure_asan_ubsan PROPERTIES TIMEOUT 180)
    endif()

    # Real native guest class-owned Array<int> fields, independent of the
    # handcrafted ABI fixture and lexical-array-only source admission.
    add_test(NAME native_class_owned_array_source_execution
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_class_owned_array_source.py"
            --compiler "$<TARGET_FILE:oreslang-llvmc>"
            --clang "${ORE_NATIVE_ARRAY_CLANG}"
            --source-root "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME native_class_owned_array_source_asan_ubsan
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_class_owned_array_source.py"
            --compiler "$<TARGET_FILE:oreslang-llvmc>"
            --clang "${ORE_NATIVE_ARRAY_CLANG}"
            --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
            --sanitizers)
    set_tests_properties(native_class_owned_array_source_execution
        native_class_owned_array_source_asan_ubsan PROPERTIES TIMEOUT 180)
    add_executable(oreslang-owned-i64-region-security-test
        tests/native_owned_region_security_test.c
        src/main/cpp/oreslang/runtime/NativeOwnedI64Region.c)
    target_include_directories(oreslang-owned-i64-region-security-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_definitions(oreslang-owned-i64-region-security-test PRIVATE
        ORESLANG_NATIVE_TEST_TOKEN_EXHAUSTION=1)
    target_compile_options(oreslang-owned-i64-region-security-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_owned_region_slot_security
        COMMAND oreslang-owned-i64-region-security-test)
    set_tests_properties(native_owned_region_slot_security PROPERTIES TIMEOUT 25)
    # The global token allocator must not repeat handles under parallel
    # allocations in separate, single-owner regions. This does NOT admit
    # concurrent access to one region or remove close-quiescence requirements.
    find_package(Threads REQUIRED)
    add_executable(oreslang-owned-i64-token-concurrent-regions-test
        tests/native_owned_region_token_concurrency_test.c
        src/main/cpp/oreslang/runtime/NativeOwnedI64Region.c)
    target_include_directories(oreslang-owned-i64-token-concurrent-regions-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-owned-i64-token-concurrent-regions-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-owned-i64-token-concurrent-regions-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_owned_region_unique_parallel_tokens
        COMMAND oreslang-owned-i64-token-concurrent-regions-test)
    set_tests_properties(native_owned_region_unique_parallel_tokens PROPERTIES TIMEOUT 25)

    # Independent thread-confined regions may allocate concurrently. The
    # process-global handle allocator must never issue duplicate tokens.
    add_executable(oreslang-native-int-array-token-concurrent-regions-test
        tests/native_int_array_token_concurrency_test.c
        src/main/cpp/oreslang/runtime/NativeIntArrayRegion.c)
    target_include_directories(oreslang-native-int-array-token-concurrent-regions-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-int-array-token-concurrent-regions-test PRIVATE Threads::Threads)
    set_target_properties(oreslang-native-int-array-token-concurrent-regions-test PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON)
    target_compile_options(oreslang-native-int-array-token-concurrent-regions-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_int_array_unique_parallel_tokens
        COMMAND oreslang-native-int-array-token-concurrent-regions-test)
    set_tests_properties(native_int_array_unique_parallel_tokens PROPERTIES TIMEOUT 30)
    # Header-only native arena proof: independent actor byte regions without
    # tracing GC. This is not yet a compiled Oreslang actor object heap.
    add_executable(oreslang-actor-owned-heap-test tests/actor_owned_heap_test.cpp)
    target_include_directories(oreslang-actor-owned-heap-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-actor-owned-heap-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_actor_owned_heap_contract
        COMMAND oreslang-actor-owned-heap-test)

    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    # Exact queue snapshots are pinned to the merged upstream Git objects.
    # An unsupported feature is NOT a blanket allowance to ignore arbitrary
    # compiler failures. The harness also builds and runs supported scalar IR.
    find_program(ORE_QUEUE_CLANG NAMES clang-18 clang-19 clang clang-17 clang-16 clang-15 clang-14)
    # Never silently omit the admission test when Clang is absent: a reduced
    # test count must not look like proof of a successful compiler/runtime gate.
    # The test explicitly fails if its native execution control is unavailable.
    if(NOT ORE_QUEUE_CLANG)
        message(WARNING "Linked-queue CTest will fail: Clang is required for the native execution control")
        set(ORE_QUEUE_CLANG "${CMAKE_CURRENT_BINARY_DIR}/missing-clang-executable")
    endif()
    add_test(NAME native_linked_queue_exact_source_failclosed
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_linked_queue_fail_closed.py"
            --compiler "$<TARGET_FILE:oreslang-llvmc>"
            --fixture-root "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/linked-queue"
            --clang "${ORE_QUEUE_CLANG}")
    set_tests_properties(native_linked_queue_exact_source_failclosed
        PROPERTIES TIMEOUT 120)
    add_test(NAME native_linked_queue_harness_adversarial
        COMMAND "${Python3_EXECUTABLE}" -m unittest discover
            -s "${CMAKE_CURRENT_SOURCE_DIR}/tests"
            -p "test_native_linked_queue_fail_closed.py" -v)
    set_tests_properties(native_linked_queue_harness_adversarial
        PROPERTIES TIMEOUT 30)
    find_package(OpenSSL QUIET COMPONENTS Crypto)
    if(OpenSSL_FOUND)
        add_executable(oreslang-messagepack-wire-v2-test
            tests/messagepack_wire_v2_test.cpp)
        target_include_directories(oreslang-messagepack-wire-v2-test PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
        target_link_libraries(oreslang-messagepack-wire-v2-test PRIVATE
            OpenSSL::Crypto)
        target_compile_options(oreslang-messagepack-wire-v2-test PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
        add_test(NAME native_messagepack_wire_v2_java_parity
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/messagepack_wire_v2_matrix.py"
                --binary "$<TARGET_FILE:oreslang-messagepack-wire-v2-test>"
                --vectors "${CMAKE_CURRENT_SOURCE_DIR}/contracts/v1/design/messagepack_collection_vectors.json")
        set_tests_properties(native_messagepack_wire_v2_java_parity PROPERTIES TIMEOUT 30)
        add_executable(oreslang-tagged-message-wire-v2-test
            tests/tagged_message_wire_v2_test.cpp)
        target_include_directories(oreslang-tagged-message-wire-v2-test PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
        target_link_libraries(oreslang-tagged-message-wire-v2-test PRIVATE
            OpenSSL::Crypto)
        target_compile_options(oreslang-tagged-message-wire-v2-test PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
        add_test(NAME native_tagged_enum_message_java_wire_parity
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/tagged_message_wire_v2_matrix.py"
                --binary "$<TARGET_FILE:oreslang-tagged-message-wire-v2-test>"
                --vectors "${CMAKE_CURRENT_SOURCE_DIR}/contracts/v1/design/tagged_message_vectors.json")
        set_tests_properties(native_tagged_enum_message_java_wire_parity PROPERTIES TIMEOUT 30)
    else()
        message(STATUS "Native MessagePack v2 parity test requires OpenSSL Crypto; not enabled")
    endif()
    find_program(ORE_CLANG NAMES clang clang-19 clang-18 clang-17 clang-16 clang-15 clang-14)
    # Positive compiler-only contracts must assemble the emitted IR, not just
    # accept an expected substring after the textual ModuleID header.
    if(ORE_CLANG)
        add_test(NAME native_contract_positive_object_verifies
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract.py"
                --compiler "$<TARGET_FILE:oreslang-llvmc>"
                --fixture "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/constant-main.ores"
                --verify-object --clang "${ORE_CLANG}")
        set_tests_properties(native_contract_positive_object_verifies
            PROPERTIES TIMEOUT 90)
    else()
        message(FATAL_ERROR "Native source IR object verification requires Clang")
    endif()
    # This isolated AOT source-to-native frame test does not relax the nine
    # default compiler-negative RT admission contracts.
    if(ORE_CLANG)
        add_test(NAME native_rt_scalar_frame_aot_two_tasks
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_rt_scalar_step_aot.py"
                --compiler "$<TARGET_FILE:oreslang-llvmc>"
                --clang "${ORE_CLANG}"
                --fixture "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/native-rt-scalar-step-positive.ores"
                --harness "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_rt_scalar_step_harness.c")
        set_tests_properties(native_rt_scalar_frame_aot_two_tasks PROPERTIES TIMEOUT 120)
    else()
        message(WARNING "Experimental scalar-loop AOT gate requires clang")
    endif()
    find_program(ORE_NODE NAMES node nodejs)
    if(ORE_CLANG AND ORE_NODE)
        add_test(NAME wasm32_source_class_ownership_runtime
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm32_owned_classes_matrix.py"
                --compiler "$<TARGET_FILE:oreslang-llvmc>"
                --clang "${ORE_CLANG}"
                --node "${ORE_NODE}"
                --fixtures "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
        set_tests_properties(wasm32_source_class_ownership_runtime PROPERTIES TIMEOUT 240)
        add_test(NAME wasm32_compiler_generated_source_actor_state
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm32_source_actor_matrix.py"
                --compiler "$<TARGET_FILE:oreslang-llvmc>"
                --clang "${ORE_CLANG}"
                --node "${ORE_NODE}"
                --fixture "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/wasm32-source-actor-counter.ores")
        set_tests_properties(wasm32_compiler_generated_source_actor_state PROPERTIES TIMEOUT 120)
        # Actual .ores actor handler lowered to import-free Wasm, with a
        # monotonic lifecycle and one private state image per Wasm instance.
        add_test(NAME wasm32_source_actor_guest_lifecycle
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm32_source_actor_guest.py"
                --compiler "$<TARGET_FILE:oreslang-llvmc>"
                --clang "${ORE_CLANG}"
                --node "${ORE_NODE}"
                --fixtures "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
        set_tests_properties(wasm32_source_actor_guest_lifecycle PROPERTIES TIMEOUT 120)
    endif()
    # First real compiler-emitted AOT while continuation. The host owns the
    # frame and schedules finite reductions; actor/mailbox integration is not
    # claimed until generated actor continuations use the carrier ABI.
    find_program(ORE_RT_AOT_CLANG NAMES clang-18 clang-19 clang clang-17 clang-16 clang-15 clang-14)
    find_program(ORE_RT_AOT_CXX NAMES clang++-18 clang++-19 clang++ clang++-17 clang++-16 clang++-15 clang++-14)
    if(NOT ORE_RT_AOT_CLANG OR NOT ORE_RT_AOT_CXX)
        message(FATAL_ERROR "AOT scalar continuation requires Clang and Clang++ to run")
    endif()
    add_test(NAME native_source_scalar_while_compiler_aot_resume
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_aot_scalar_while.py"
            --compiler "$<TARGET_FILE:oreslang-llvmc>"
            --clang "${ORE_RT_AOT_CLANG}"
            --cxx "${ORE_RT_AOT_CXX}"
            --source-root "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(native_source_scalar_while_compiler_aot_resume
        PROPERTIES TIMEOUT 120)

    function(oreslang_contract_test name fixture)
        add_test(NAME ${name}
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract.py"
                --compiler "$<TARGET_FILE:oreslang-llvmc>"
                --fixture "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/${fixture}"
                ${ARGN})
    endfunction()

    # A Python -O/PYTHONOPTIMIZE run must never turn failed compiler
    # admission/IR checks into a green CTest. Uses only fake compilers;
    # no LLVM toolchain or private-source mirror is needed.
    add_test(NAME native_contract_harness_optimize_failclosed
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_contract_harness.py")
    add_test(NAME native_contract_harness_python_O
        COMMAND "${Python3_EXECUTABLE}" -O
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_contract_harness.py")
    add_test(NAME native_contract_harness_python_OO
        COMMAND "${Python3_EXECUTABLE}" -OO
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_contract_harness.py")
    set_tests_properties(
        native_contract_harness_optimize_failclosed
        native_contract_harness_python_O
        native_contract_harness_python_OO
        PROPERTIES TIMEOUT 240)

    # Validate the CTest inventory itself, including non-skippable admission,
    # optimized Python harness registration and native executable gates.
    add_test(NAME native_ctest_manifest_harness_adversarial
        COMMAND "${Python3_EXECUTABLE}" -m unittest discover
            -s "${CMAKE_CURRENT_SOURCE_DIR}/tests"
            -p "test_ctest_manifest_gate.py" -v)
    set_tests_properties(native_ctest_manifest_harness_adversarial
        PROPERTIES TIMEOUT 30)

    # Reject all unsupported source-level scheduler suspension until verified
    # heap-owned continuations exist. The harness rejects any partial LLVM IR.
    oreslang_contract_test(native_rt_while_loop_failclosed
        native-rt-while-reject.ores
        --diagnostic "native realtime source loop requires verified LLVM continuation lowering")
    oreslang_contract_test(native_rt_for_loop_failclosed
        native-rt-for-reject.ores
        --diagnostic "native realtime source loop requires verified LLVM continuation lowering")
    oreslang_contract_test(native_rt_unbounded_loop_failclosed
        native-rt-loop-reject.ores
        --diagnostic "native realtime source loop requires verified LLVM continuation lowering")
    oreslang_contract_test(native_rt_cooperate_failclosed
        native-rt-cooperate-reject.ores
        --diagnostic "native rt cooperate requires verified heap-owned continuation lowering")
    oreslang_contract_test(native_rt_cooperate_expression_failclosed
        native-rt-cooperate-expression-reject.ores
        --diagnostic "native rt cooperate requires verified heap-owned continuation lowering")
    oreslang_contract_test(native_rt_nested_while_failclosed
        native-rt-nested-while-reject.ores
        --diagnostic "native realtime source loop requires verified LLVM continuation lowering")
    oreslang_contract_test(native_rt_nested_for_failclosed
        native-rt-nested-for-reject.ores
        --diagnostic "native realtime source loop requires verified LLVM continuation lowering")
    oreslang_contract_test(native_rt_nested_loop_failclosed
        native-rt-nested-loop-reject.ores
        --diagnostic "native realtime source loop requires verified LLVM continuation lowering")
    oreslang_contract_test(native_rt_nested_cooperate_failclosed
        native-rt-nested-cooperate-reject.ores
        --diagnostic "native rt cooperate requires verified heap-owned continuation lowering")

    add_executable(oreslang-ownership-cfg-dataflow-test
        tests/ownership_cfg_dataflow_test.cpp)
    target_include_directories(oreslang-ownership-cfg-dataflow-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-ownership-cfg-dataflow-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_ownership_cfg_dataflow_fixed_point
        COMMAND oreslang-ownership-cfg-dataflow-test)
    # Field-wise native destruction verifier and concrete owner flags.
    # Separate from unsupported source-level nontrivial class drop glue.
    add_executable(oreslang-ownership-drop-plan-test
        tests/ownership_drop_plan_test.cpp)
    target_include_directories(oreslang-ownership-drop-plan-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-ownership-drop-plan-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_ownership_cfg_exact_once_field_destruction
        COMMAND oreslang-ownership-drop-plan-test)
    add_executable(oreslang-target-abi-test tests/target_abi_test.cpp)
    target_include_directories(oreslang-target-abi-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-target-abi-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME portable_target_abi_v1 COMMAND oreslang-target-abi-test
        "${CMAKE_CURRENT_SOURCE_DIR}/contracts/architecture/v1/targets.tsv")
    add_executable(oreslang-dop-kernel-test tests/dop_kernel_lowering_test.cpp)
    target_include_directories(oreslang-dop-kernel-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-dop-kernel-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME dop_typed_cpu_opencl_ownership COMMAND oreslang-dop-kernel-test)
    add_executable(oreslang-dop-read-loan-thread-test tests/dop_hostbuffer_concurrency_test.cpp)
    target_include_directories(oreslang-dop-read-loan-thread-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-dop-read-loan-thread-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(oreslang-dop-read-loan-thread-test PRIVATE Threads::Threads)
    add_test(NAME dop_hostbuffer_multithread_read_loan_quiescence
        COMMAND oreslang-dop-read-loan-thread-test)
    set_tests_properties(dop_hostbuffer_multithread_read_loan_quiescence PROPERTIES TIMEOUT 25)
    add_executable(oreslang-quantum-circuit-test tests/quantum_circuit_lowering_test.cpp)
    target_include_directories(oreslang-quantum-circuit-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-quantum-circuit-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME quantum_circuit_qasm3_simulator COMMAND oreslang-quantum-circuit-test)
    add_executable(oreslang-gpu-fence-test tests/gpu_buffer_fence_test.cpp)
    target_include_directories(oreslang-gpu-fence-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-gpu-fence-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(oreslang-gpu-fence-test PRIVATE Threads::Threads)
    add_test(NAME gpu_device_generation_quiescence_fences COMMAND oreslang-gpu-fence-test)

    # Java '/' produces a float even for int/int. Until native float lowering
    # exists, reject '/' and '%' before IR rather than silently emitting i64.
    oreslang_contract_test(native_float_division_unavailable numeric-division-float-reject.ores
        --diagnostic "native floating division unsupported")
    oreslang_contract_test(native_float_division_precedence_unavailable numeric-division-precedence-reject.ores
        --diagnostic "native floating division unsupported")
    oreslang_contract_test(native_integer_remainder_unavailable numeric-remainder-unsupported.ores
        --diagnostic "native integer remainder unsupported")
    oreslang_contract_test(constant_main constant-main.ores --ir "ret i64 42")
    oreslang_contract_test(native_using_typed_effects using-pure-constant.ores --ir "ret i64 42")
    oreslang_contract_test(native_using_unknown_effect using-unknown-effect.ores --diagnostic "unknown function effect")
    oreslang_contract_test(native_using_duplicate_effect using-duplicate-effect.ores --diagnostic "duplicate function effect")
    oreslang_contract_test(native_critical_actor_state_not_lowered critical-handle-state-using.ores --diagnostic "native actor state { ... } requires sealed handle_state lowering")
    oreslang_contract_test(native_requires_rejects_unverified_wcet requires-unverified.ores --diagnostic "native requires clause awaits verified")
    oreslang_contract_test(native_actor_state_sealed_until_lowering state-sealed-unsupported.ores --diagnostic "native actor state")

    # Native OS ABI is i32 even though Oreslang int is represented as i64.
    oreslang_contract_test(native_exit_reject_main_arguments exit-invalid-main-args.ores
        --diagnostic "native main() must have zero arguments")
    oreslang_contract_test(native_exit_reject_reserved_symbol exit-reserved-native-symbol.ores
        --diagnostic "reserved native runtime ABI symbol '__oreslang_guest_main'")
    oreslang_contract_test(native_exit_reject_unsupported_explicit_exit exit-unsuppo…17499 tokens truncated…--clang "${ORE_CLANG}")
        oreslang_contract_test(native_exec_lexical_mutable_loan owned-local-mutable-borrow.ores --run 42 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_exec_rt_copy_scalar native-rt-copy-scalar.ores --run 42 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_exec_rt_take_scalar native-rt-take-scalar.ores --run 42 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_exec_rt_share_scalar native-rt-share-scalar.ores --run 42 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_exec_loan_read native-borrow-read.ores --run 83 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_exec_loan_exclusive native-borrow-exclusive.ores --run 40 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_exec_reborrow_shared native-reborrow-shared.ores --run 81 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_exec_reborrow_mut native-reborrow-mut.ores --run 40 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_exec_reborrow_mut_to_shared native-reborrow-mut-shared.ores --run 41 --clang "${ORE_CLANG}")
    else()
        message(WARNING "Clang unavailable: native executable tests are disabled")
    endif()

    # Inert native POD allocator: deterministic ownership without tracing GC.
    # The test does not establish compiler-enforced object lifetime parity.
    # Bounded native region with provenance-checked deterministic scalar drops.
    add_executable(oreslang-native-owned-i64-region-test
        tests/native_owned_region_test.c
        src/main/cpp/oreslang/runtime/NativeOwnedI64Region.c)
    target_include_directories(oreslang-native-owned-i64-region-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-native-owned-i64-region-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_owned_i64_region_drop_and_provenance
        COMMAND oreslang-native-owned-i64-region-test)

    # Host-only deterministic actor arena; no source actor lowering implied.
    add_executable(oreslang-native-actor-arena-test
        tests/native_actor_arena_test.cpp
        src/main/cpp/oreslang/runtime/ActorArena.c)
    target_include_directories(oreslang-native-actor-arena-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-actor-arena-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-actor-arena-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_actor_arena_persistent_scratch
        COMMAND oreslang-native-actor-arena-test)
    set_tests_properties(native_actor_arena_persistent_scratch PROPERTIES TIMEOUT 25)

    # Persist actor-native data across message turns and carrier migration.
    # Retirement denies new admissions; physical free waits for active loans.
    find_package(Threads REQUIRED)
    add_executable(oreslang-actor-owned-i64-heap-test
        tests/actor_owned_heap_lifecycle_test.cpp
        src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c)
    target_include_directories(oreslang-actor-owned-i64-heap-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-actor-owned-i64-heap-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-actor-owned-i64-heap-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_actor_owned_i64_heap_loan_quiescence
        COMMAND oreslang-actor-owned-i64-heap-test)
    set_tests_properties(native_actor_owned_i64_heap_loan_quiescence PROPERTIES TIMEOUT 20)

    add_executable(oreslang-fixed-arena-test tests/fixed_arena_test.cpp)
    target_include_directories(oreslang-fixed-arena-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-fixed-arena-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_fixed_arena_contract COMMAND oreslang-fixed-arena-test)

    add_executable(oreslang-deterministic-graph-arena-test tests/deterministic_graph_arena_test.cpp)
    target_include_directories(oreslang-deterministic-graph-arena-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-deterministic-graph-arena-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_deterministic_graph_arena COMMAND oreslang-deterministic-graph-arena-test)
    # Independent actor-owned native heap tests are portable (no Linux syscalls).
    find_package(Threads REQUIRED)
    add_executable(oreslang-actor-heap-test tests/actor_heap_test.cpp)
    target_include_directories(oreslang-actor-heap-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-actor-heap-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-actor-heap-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_actor_heap_no_tracing_gc
             COMMAND oreslang-actor-heap-test)

    # Linux-specific actor-neutral wake/park primitive. This does NOT
    # implement the actor scheduler or certify lock-free mailbox delivery.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_package(Threads REQUIRED)
        # Real joined carrier is the sole issuer of retirement proofs.
        add_executable(oreslang-actor-graph-heap-test
            tests/actor_owned_graph_heap_test.cpp
            src/main/cpp/oreslang/runtime/HungryCarrier.cpp
            src/main/cpp/oreslang/runtime/ParkingSignal.cpp)
        target_include_directories(oreslang-actor-graph-heap-test PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
        target_link_libraries(oreslang-actor-graph-heap-test PRIVATE Threads::Threads)
        target_compile_options(oreslang-actor-graph-heap-test PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
        add_test(NAME native_actor_owned_cyclic_graph_heap
            COMMAND oreslang-actor-graph-heap-test)
        set_tests_properties(native_actor_owned_cyclic_graph_heap PROPERTIES TIMEOUT 15)

        add_executable(oreslang-parking-signal-test
            tests/parking_signal_test.cpp
            src/main/cpp/oreslang/runtime/ParkingSignal.cpp)
        target_include_directories(oreslang-parking-signal-test PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
        target_link_libraries(oreslang-parking-signal-test PRIVATE Threads::Threads)
        target_compile_options(oreslang-parking-signal-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
        add_test(NAME parking_signal_wake_cancel_races COMMAND oreslang-parking-signal-test)
        set_tests_properties(parking_signal_wake_cancel_races PROPERTIES TIMEOUT 25)

        add_executable(oreslang-hungry-carrier-test
            tests/hungry_carrier_test.cpp
            src/main/cpp/oreslang/runtime/HungryCarrier.cpp
            src/main/cpp/oreslang/runtime/ParkingSignal.cpp)
        target_include_directories(oreslang-hungry-carrier-test PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
        target_link_libraries(oreslang-hungry-carrier-test PRIVATE Threads::Threads)
        target_compile_options(oreslang-hungry-carrier-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
        add_test(NAME hungry_carrier_async_bounded_cancel
                 COMMAND oreslang-hungry-carrier-test)
        set_tests_properties(hungry_carrier_async_bounded_cancel PROPERTIES TIMEOUT 30)

        # Actual compiled Oreslang scalar frame is driven by a real one-actor
        # HungryCarrier OS thread. CPU cooperation is NOT IO or logical DONE.
        # The build compiler may be GCC, which cannot compile LLVM IR.
        find_program(ORE_CLANGXX NAMES clang++)
        if(ORE_CLANG AND ORE_CLANGXX)
            add_test(NAME native_rt_compiled_scalar_carrier_cooperate
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_rt_scalar_carrier_aot.py"
                    --compiler "$<TARGET_FILE:oreslang-llvmc>"
                    --clang "${ORE_CLANG}"
                    --clangxx "${ORE_CLANGXX}"
                    --fixture "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/native-rt-scalar-step-positive.ores"
                    --test "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_rt_scalar_carrier_aot_test.cpp"
                    --source-root "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
            set_tests_properties(native_rt_compiled_scalar_carrier_cooperate
                PROPERTIES TIMEOUT 120)
        endif()

        # Experimental multiple actors on ONE OS worker, using two
        # independently compiled scalar .ores continuations. Not production
        # source actor lowering and not a hard realtime guarantee.
        if(ORE_CLANG AND ORE_CLANGXX)
            add_test(NAME native_multi_actor_compiled_round_robin_fairness
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_multi_actor_carrier_aot.py"
                    --compiler "$<TARGET_FILE:oreslang-llvmc>"
                    --clangxx "${ORE_CLANGXX}"
                    --fast "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/native-rt-scalar-step-positive.ores"
                    --slow "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/native-rt-scalar-multi-actor-slow.ores"
                    --test "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_multi_actor_carrier_aot_test.cpp"
                    --source-root "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
            set_tests_properties(native_multi_actor_compiled_round_robin_fairness
                PROPERTIES TIMEOUT 150)
        endif()

        add_executable(oreslang-carrier-supervisor-test
            tests/native_carrier_supervisor_integration.cpp
            src/main/cpp/oreslang/runtime/HungryCarrier.cpp
            src/main/cpp/oreslang/runtime/ParkingSignal.cpp)
        target_include_directories(oreslang-carrier-supervisor-test PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
        target_link_libraries(oreslang-carrier-supervisor-test PRIVATE Threads::Threads)
        target_compile_options(oreslang-carrier-supervisor-test PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
        add_test(NAME native_carrier_urgent_supervisor_turn
                 COMMAND oreslang-carrier-supervisor-test)
        set_tests_properties(native_carrier_urgent_supervisor_turn PROPERTIES TIMEOUT 15)

        # Real native carrier admissions and handler observations are used to
        # produce an executable trace (not fabricated actor-model fixtures).
        add_executable(oreslang-actor-trace-test
            tests/actor_trace_integration.cpp
            src/main/cpp/oreslang/runtime/HungryCarrier.cpp
            src/main/cpp/oreslang/runtime/ParkingSignal.cpp)
        target_include_directories(oreslang-actor-trace-test PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
        target_link_libraries(oreslang-actor-trace-test PRIVATE Threads::Threads)
        target_compile_options(oreslang-actor-trace-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
        add_test(NAME native_actor_ingress_handler_trace
                 COMMAND oreslang-actor-trace-test)
        set_tests_properties(native_actor_ingress_handler_trace PROPERTIES TIMEOUT 15)

        # Actual dedicated carrier callback accesses its own native ownership
        # heap, while the broker and supervisor cannot discover that heap.
        add_executable(oreslang-native-actor-heap-test
            tests/native_actor_heap_integration_test.cpp
            src/main/cpp/oreslang/runtime/HungryCarrier.cpp
            src/main/cpp/oreslang/runtime/ParkingSignal.cpp)
        target_include_directories(oreslang-native-actor-heap-test PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
        target_link_libraries(oreslang-native-actor-heap-test PRIVATE Threads::Threads)
        target_compile_options(oreslang-native-actor-heap-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
        add_test(NAME native_actor_heap_isolation_and_drop
                 COMMAND oreslang-native-actor-heap-test)
        set_tests_properties(native_actor_heap_isolation_and_drop PROPERTIES TIMEOUT 30)
    endif()
    # Current-main reconciliation of the native actor substrate from draft
    # LLVM #118. Never replace the newer main ownership/continuation tests.
    # This admits host-level actor scheduling and reserved CONTROL only;
    # source-level spawn/await lowering remains a separate compiler gate.
    find_package(Threads REQUIRED)

    add_executable(oreslang-native-actor-mailbox-test
        tests/native_actor_mailbox_test.c
        src/main/cpp/oreslang/runtime/NativeActorMailbox.c)
    target_include_directories(oreslang-native-actor-mailbox-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-actor-mailbox-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-actor-mailbox-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_actor_fifo_mailbox_admission
        COMMAND oreslang-native-actor-mailbox-test)
    set_tests_properties(native_actor_fifo_mailbox_admission PROPERTIES TIMEOUT 20)

    # Zero-thread central CONTROL wake coordinator on the four-domain VM.
    add_executable(oreslang-native-vm-control-plane-test
        tests/native_vm_control_plane_test.c
        src/main/cpp/oreslang/runtime/NativeOresVM.c
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c)
    target_include_directories(oreslang-native-vm-control-plane-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-vm-control-plane-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-vm-control-plane-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_vm_master_control_wake_coalescing
        COMMAND oreslang-native-vm-control-plane-test)
    set_tests_properties(native_vm_master_control_wake_coalescing PROPERTIES TIMEOUT 25)

    # VM-level ownership: four independent scheduler domains, five carriers
    # by default; cross-domain ActorRefs cannot be admitted by another pool.
    add_executable(oreslang-native-ores-vm-test
        tests/native_ores_vm_test.c
        src/main/cpp/oreslang/runtime/NativeOresVM.c
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c)
    target_include_directories(oreslang-native-ores-vm-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-ores-vm-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-ores-vm-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_ores_vm_four_carrier_domains
        COMMAND oreslang-native-ores-vm-test)
    set_tests_properties(native_ores_vm_four_carrier_domains PROPERTIES TIMEOUT 30)
    # Native scheduler cache-locality hint: bounded same-carrier reuse is
    # preempted by CONTROL and yields to competing actor lanes.
    add_executable(oreslang-native-actor-carrier-locality-test
        tests/native_actor_carrier_locality_test.c
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c)
    target_include_directories(oreslang-native-actor-carrier-locality-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-actor-carrier-locality-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-actor-carrier-locality-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_actor_carrier_locality_control_priority
        COMMAND oreslang-native-actor-carrier-locality-test)
    set_tests_properties(native_actor_carrier_locality_control_priority
        PROPERTIES TIMEOUT 30)

    # Opt-in budget-aware sizing; legacy VM defaults remain unchanged.
    add_executable(oreslang-native-vm-cpu-budget-test
        tests/native_vm_cpu_budget_test.c
        src/main/cpp/oreslang/runtime/NativeOresVM.c
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c)
    target_include_directories(oreslang-native-vm-cpu-budget-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-vm-cpu-budget-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-vm-cpu-budget-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_vm_cgroup_cpu_budgeted_carrier_admission
        COMMAND oreslang-native-vm-cpu-budget-test)
    set_tests_properties(native_vm_cgroup_cpu_budgeted_carrier_admission
        PROPERTIES TIMEOUT 30)
    # One physical turn permit for all native scheduler domains; idle
    # carriers do not execute guest callbacks while another owns the permit.
    add_executable(oreslang-native-vm-turn-gate-test
        tests/native_vm_physical_turn_gate_test.c
        src/main/cpp/oreslang/runtime/NativeOresVM.c
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c)
    target_include_directories(oreslang-native-vm-turn-gate-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-vm-turn-gate-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-vm-turn-gate-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_vm_cross_domain_physical_turn_limit
        COMMAND oreslang-native-vm-turn-gate-test)
    set_tests_properties(native_vm_cross_domain_physical_turn_limit
        PROPERTIES TIMEOUT 30)

    # Deterministic, isolated C11 physical permit contention test;
    # no guest compiler lowering or NUMA policy implied.
    add_executable(oreslang-native-physical-turn-gate-stress-test
        tests/native_physical_turn_gate_stress_test.c)
    target_include_directories(oreslang-native-physical-turn-gate-stress-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-physical-turn-gate-stress-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-physical-turn-gate-stress-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_shared_actor_turn_gate_pthread_contention
        COMMAND oreslang-native-physical-turn-gate-stress-test)
    set_tests_properties(native_shared_actor_turn_gate_pthread_contention
        PROPERTIES TIMEOUT 30)

    # Local atomics/quotas stress does not require the guest compiler or VM.
    add_executable(oreslang-native-shard-budget-stress-test
        tests/native_actor_shard_budget_stress_test.cpp)
    target_include_directories(oreslang-native-shard-budget-stress-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-shard-budget-stress-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-shard-budget-stress-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_actor_and_vm_shard_budget_contention
        COMMAND oreslang-native-shard-budget-stress-test)
    set_tests_properties(native_actor_and_vm_shard_budget_contention
        PROPERTIES TIMEOUT 30)

    # Standalone exact header CTest; its mock scheduler certifies one
    # physical actor turn, while reservations use the real shard code.
    add_executable(oreslang-native-actor-shard-snapshot-test
        tests/native_actor_shard_snapshot_unit_test.cpp)
    target_include_directories(oreslang-native-actor-shard-snapshot-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-actor-shard-snapshot-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-actor-shard-snapshot-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_actor_shard_snapshot_quota_and_generation
        COMMAND oreslang-native-actor-shard-snapshot-test)
    set_tests_properties(native_actor_shard_snapshot_quota_and_generation
        PROPERTIES TIMEOUT 30)

    # Host-only trusted actor-owned AoS/SoA/AoSoA scalar region.
    # The actor scheduler certifies owner identity; no source-level lowering.
    add_executable(oreslang-native-actor-shard-layout-test
        tests/native_actor_shard_layout_test.cpp
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c)
    target_include_directories(oreslang-native-actor-shard-layout-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-actor-shard-layout-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-actor-shard-layout-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_actor_owned_aos_soa_aosoa_dense_kernel
        COMMAND oreslang-native-actor-shard-layout-test)
    set_tests_properties(native_actor_owned_aos_soa_aosoa_dense_kernel
        PROPERTIES TIMEOUT 30)
    # Manual performance benchmark; do not gate correctness on wall times.
    add_executable(oreslang-native-actor-shard-bench
        tests/native_actor_shard_layout_bench.cpp
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c)
    target_include_directories(oreslang-native-actor-shard-bench PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-actor-shard-bench PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-actor-shard-bench PRIVATE
        -Wall -Wextra -Wpedantic -Werror)

    # VM-owned exact parent binding -> quiescent typed CONTROL publication.
    # No guest supervisor compiler lowering is claimed by this host C test.
    add_executable(oreslang-native-vm-supervisor-test
        tests/native_vm_supervisor_owner_test.c
        src/main/cpp/oreslang/runtime/NativeOresVM.c
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c)
    target_include_directories(oreslang-native-vm-supervisor-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-vm-supervisor-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-vm-supervisor-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_vm_supervisor_owner_typed_quiescent_control
        COMMAND oreslang-native-vm-supervisor-test)
    set_tests_properties(native_vm_supervisor_owner_typed_quiescent_control
        PROPERTIES TIMEOUT 30)
    # Host/supervisor execution lease transitions: linearizable carrier claim,
    # stop/retirement quiescence, stale resume and generation exhaustion.
    add_executable(oreslang-native-execution-authority-test
        tests/native_execution_authority_test.c)
    target_include_directories(oreslang-native-execution-authority-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-execution-authority-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-execution-authority-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_actor_supervisor_execution_authority
        COMMAND oreslang-native-execution-authority-test)
    set_tests_properties(native_actor_supervisor_execution_authority PROPERTIES TIMEOUT 30)

    # Across all actor-owned entrypoints (DATA and supervisor CONTROL), no
    # actor can have two physical guest executors, even on separate workers.
    # A typed supervisor panic is actor-owned CONTROL work and must obey
    # the same exclusive carrier admission as normal DATA for that parent.
    add_executable(oreslang-native-actor-typed-control-exclusive-test
        tests/native_actor_typed_control_exclusive_test.c
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c)
    target_include_directories(oreslang-native-actor-typed-control-exclusive-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-actor-typed-control-exclusive-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-actor-typed-control-exclusive-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_actor_typed_panic_exclusive_carrier
        COMMAND oreslang-native-actor-typed-control-exclusive-test)
    set_tests_properties(native_actor_typed_panic_exclusive_carrier PROPERTIES TIMEOUT 20)

    add_executable(oreslang-native-actor-exclusive-control-test
        tests/native_actor_exclusive_control_test.c
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c)
    target_include_directories(oreslang-native-actor-exclusive-control-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-actor-exclusive-control-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-actor-exclusive-control-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_actor_exclusive_data_and_control
        COMMAND oreslang-native-actor-exclusive-control-test)
    set_tests_properties(native_actor_exclusive_data_and_control PROPERTIES TIMEOUT 20)

    add_executable(oreslang-native-actor-ready-queue-test
        tests/native_actor_scheduler_test.c
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c)
    target_include_directories(oreslang-native-actor-ready-queue-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-actor-ready-queue-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-actor-ready-queue-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_actor_nonblocking_ready_queue
        COMMAND oreslang-native-actor-ready-queue-test)
    set_tests_properties(native_actor_nonblocking_ready_queue PROPERTIES TIMEOUT 30)
    # Fault injection is compiled only into this test executable. A blocked
    # actor must not make a different actor's unserviceable work "ACCEPTED".
    add_executable(oreslang-native-carrier-create-failure-test
        tests/native_actor_carrier_create_failure_test.c
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c)
    target_include_directories(oreslang-native-carrier-create-failure-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_definitions(oreslang-native-carrier-create-failure-test PRIVATE
        ORESLANG_NATIVE_TEST_PTHREAD_CREATE_FAILURE=1)
    target_link_libraries(oreslang-native-carrier-create-failure-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-carrier-create-failure-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_actor_lazy_carrier_creation_failure_fence
        COMMAND oreslang-native-carrier-create-failure-test)
    set_tests_properties(native_actor_lazy_carrier_creation_failure_fence PROPERTIES TIMEOUT 30)

    # Send and stop contend on one linearizable actor admission authority.
    add_executable(oreslang-native-send-stop-race-test
        tests/native_actor_send_stop_race_test.c
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c)
    target_include_directories(oreslang-native-send-stop-race-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-send-stop-race-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-send-stop-race-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_actor_send_stop_linearizable_race
        COMMAND oreslang-native-send-stop-race-test)
    set_tests_properties(native_actor_send_stop_linearizable_race PROPERTIES TIMEOUT 30)

    # Integration of the *actual native C scheduler* with the actor-owned C++
    # graph heap under two distinct actor lanes. No source-language actor
    # lowering or guest-state-loan enforcement is claimed by this host test.
    add_executable(oreslang-native-actor-graph-scheduler-bridge-test
        tests/native_actor_scheduler_graph_bridge_test.cpp
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c)
    target_include_directories(oreslang-native-actor-graph-scheduler-bridge-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-actor-graph-scheduler-bridge-test PRIVATE
        Threads::Threads)
    target_compile_options(oreslang-native-actor-graph-scheduler-bridge-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_actor_scheduler_graph_heap_host_integration
        COMMAND oreslang-native-actor-graph-scheduler-bridge-test)
    set_tests_properties(native_actor_scheduler_graph_heap_host_integration
        PROPERTIES TIMEOUT 30)

    # ActorRef scheduler domains are capabilities: exhaustion must not wrap
    # and concurrent opens near UINT64_MAX must never issue duplicates.
    add_executable(oreslang-native-domain-exhaustion-test
        tests/native_actor_domain_exhaustion_test.c
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c)
    target_include_directories(oreslang-native-domain-exhaustion-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-domain-exhaustion-test PRIVATE Threads::Threads)
    target_compile_definitions(oreslang-native-domain-exhaustion-test PRIVATE
        ORESLANG_NATIVE_TEST_DOMAIN_EXHAUSTION=1)
    target_compile_options(oreslang-native-domain-exhaustion-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_actor_domain_exhaustion_no_reuse
        COMMAND oreslang-native-domain-exhaustion-test)
    set_tests_properties(native_actor_domain_exhaustion_no_reuse PROPERTIES TIMEOUT 30)

    # Stop one child without waiting for an unrelated actor's busy turn.
    add_executable(oreslang-native-child-wait-test
        tests/native_actor_group_wait_done_test.c
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c)
    target_include_directories(oreslang-native-child-wait-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-child-wait-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-child-wait-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_actorgroup_per_child_wait_done
        COMMAND oreslang-native-child-wait-test)
    set_tests_properties(native_actorgroup_per_child_wait_done PROPERTIES TIMEOUT 20)

    add_executable(oreslang-native-supervisor-control-test
        tests/native_supervisor_control_test.c
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c
        src/main/cpp/oreslang/runtime/NativeActorFuture.c)
    target_include_directories(oreslang-native-supervisor-control-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-supervisor-control-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-supervisor-control-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_supervisor_reserved_control_future
        COMMAND oreslang-native-supervisor-control-test)
    set_tests_properties(native_supervisor_reserved_control_future PROPERTIES TIMEOUT 30)

    add_executable(oreslang-native-supervisor-panic-test
        tests/native_supervisor_panic_test.c
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c)
    target_include_directories(oreslang-native-supervisor-panic-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-supervisor-panic-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-supervisor-panic-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_supervisor_typed_panic_event
        COMMAND oreslang-native-supervisor-panic-test)
    set_tests_properties(native_supervisor_typed_panic_event PROPERTIES TIMEOUT 30)
    # Optimistic CAS for logical continuations: state must not be overwritten
    # after stale, foreign, retired, or exhausted version evidence.
    add_executable(oreslang-native-versioned-actor-cell-test
        tests/versioned_actor_cell_test.cpp)
    target_include_directories(oreslang-native-versioned-actor-cell-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-versioned-actor-cell-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-versioned-actor-cell-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_actor_versioned_state_logical_races
        COMMAND oreslang-native-versioned-actor-cell-test)
    set_tests_properties(native_actor_versioned_state_logical_races
        PROPERTIES TIMEOUT 20)
    set_tests_properties(native_actor_state_scope_nested_loan_fencing PROPERTIES TIMEOUT 20)

    # Portable native ML staging boundaries only: the sink is volatile and
    # MPA preflight never authorizes a privileged effect.
    add_executable(oreslang-native-ml-admission-test
        tests/native_ml_admission_test.cpp)
    target_include_directories(oreslang-native-ml-admission-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-ml-admission-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-ml-admission-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_ml_volatile_fence_and_mpa_failclosed
        COMMAND oreslang-native-ml-admission-test)
    set_tests_properties(native_ml_volatile_fence_and_mpa_failclosed PROPERTIES TIMEOUT 20)

    # A real .ores -> LLVM IR -> Clang executable evidence slice, deliberately
    # labeled scalar-only; this is NOT a full actor/HPC/ML parity claim.
    if(ORE_CLANG)
        add_test(NAME native_hpc_ml_scalar_source_provenance
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_hpc_ml_source_evidence.py"
                --compiler "$<TARGET_FILE:oreslang-llvmc>"
                --clang "${ORE_CLANG}"
                --fixture "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/hpc-ml-scalar-evidence.ores"
                --samples 3 --warmups 1 --expected-exit 42)
        set_tests_properties(native_hpc_ml_scalar_source_provenance PROPERTIES TIMEOUT 60)
    endif()
endif()

if(BUILD_TESTING)
    add_executable(oreslang-refreshed-control-data-lane-test tests/native_actor_control_data_lane_test.cpp)
    target_include_directories(oreslang-refreshed-control-data-lane-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-refreshed-control-data-lane-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-refreshed-control-data-lane-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME refreshed_actor_control_data_lane COMMAND oreslang-refreshed-control-data-lane-test)
    set_tests_properties(refreshed_actor_control_data_lane PROPERTIES TIMEOUT 30)

    add_executable(oreslang-test-assertions-test tests/test_assertions_enabled.cpp)
    add_test(NAME native_test_assertions_remain_enabled
        COMMAND oreslang-test-assertions-test)

    # Release tests use assertions for ownership checks and sometimes for
    # operations such as pthread_create. NDEBUG must not erase those operations
    # or turn safety tests into false passes. Keep the compiler's Release flags.
    get_property(oreslang_directory_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(oreslang_test_target IN LISTS oreslang_directory_targets)
        get_target_property(oreslang_target_type ${oreslang_test_target} TYPE)
        if(oreslang_target_type STREQUAL "EXECUTABLE"
                AND NOT oreslang_test_target STREQUAL "oreslang-llvmc")
            if(MSVC)
                target_compile_options(${oreslang_test_target} PRIVATE /UNDEBUG)
            else()
                target_compile_options(${oreslang_test_target} PRIVATE -UNDEBUG)
            endif()
        endif()
    endforeach()
endif()

if(BUILD_TESTING AND ORE_CLANG)
    add_test(NAME refreshed_actor_persistent_fields
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/actor_persistent_fields_refresh.py"
        "$<TARGET_FILE:oreslang-llvmc>" "${ORE_CLANG}" "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

if(BUILD_TESTING)
    oreslang_contract_test(refreshed_actor_abi_actor_collision actor-abi-actor-collision.ores --diagnostic "duplicate actor/class/function symbol")
    oreslang_contract_test(refreshed_actor_abi_func_collision actor-abi-func-collision.ores --diagnostic "reserved native runtime ABI symbol")
    oreslang_contract_test(refreshed_actor_duplicate_field actor-duplicate-field.ores --diagnostic "duplicate actor field")
    oreslang_contract_test(refreshed_actor_field_shadow actor-field-shadow.ores --diagnostic "actor parameter shadows field")
    oreslang_contract_test(refreshed_actor_immutable_write actor-immutable-write.ores --diagnostic "cannot assign immutable or unknown actor field")
    oreslang_contract_test(refreshed_actor_unknown_field actor-unknown-field.ores --diagnostic "unknown parameter")
    oreslang_contract_test(refreshed_actor_unknown_write actor-unknown-write.ores --diagnostic "cannot assign immutable or unknown actor field")
    oreslang_contract_test(refreshed_actor_unsupported_field actor-unsupported-field.ores --diagnostic "expected 'int'")
endif()

if(BUILD_TESTING AND Python3_Interpreter_FOUND AND ORE_CLANG)
    add_test(NAME native_aot_driver
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_aot_driver.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/compile-aot.sh"
            "$<TARGET_FILE:oreslang-llvmc>" "${ORE_CLANG}")
    set_tests_properties(native_aot_driver PROPERTIES TIMEOUT 30)
endif()

# Compiler-side nominal enum signature policy (host-only). This does NOT
# admit same-arity guest overloads in native LLVM source or claim runtime parity.
if(BUILD_TESTING)
    add_executable(oreslang-nominal-enum-overload-policy-test
        tests/nominal_enum_overload_policy_test.cpp)
    target_include_directories(oreslang-nominal-enum-overload-policy-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-nominal-enum-overload-policy-test PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>)
    add_test(NAME native_nominal_enum_overload_static_policy
        COMMAND oreslang-nominal-enum-overload-policy-test)
    set_tests_properties(native_nominal_enum_overload_static_policy
        PROPERTIES TIMEOUT 10)
endif()

# Freestanding Cortex-M target: actual Oreslang source -> LLVM IR -> ARM object.
# This is not Zephyr/QEMU firmware evidence; the embedded platform owns that.
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME cortexm_freestanding_source_object
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/cortexm_freestanding_source_object.py"
            --compiler "$<TARGET_FILE:oreslang-llvmc>")
    set_tests_properties(cortexm_freestanding_source_object PROPERTIES TIMEOUT 90)
    add_test(NAME cortexm_source_actor_admission
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/cortexm_source_actor_admission.py"
            --compiler "$<TARGET_FILE:oreslang-llvmc>")
    set_tests_properties(cortexm_source_actor_admission PROPERTIES TIMEOUT 90)
    # Exhaustive bounded-queue pairs and shuffled actor lifecycle schedules.
    add_test(NAME cortexm_actor_admission_model_matrix
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/cortexm_actor_admission_stress.py"
            --compiler "$<TARGET_FILE:oreslang-llvmc>")
    set_tests_properties(cortexm_actor_admission_model_matrix PROPERTIES TIMEOUT 180)
endif()

# Admission gates for language surfaces that this backend does not lower yet.
if(BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME value_boundary_admission
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/value_boundaries.py"
      "$<TARGET_FILE:oreslang-llvmc>")
  set_tests_properties(value_boundary_admission PROPERTIES TIMEOUT 60)
endif()

