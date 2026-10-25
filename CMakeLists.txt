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

include(CTest)
if(BUILD_TESTING)
    # Compiler-level trait projection contract: closed-world HasFuture dispatch
    # for native ActorRef readiness and its one-shot Future<void> token.
    # Independently runnable without LLVM libraries or a guest scheduler.
    add_executable(oreslang-native-hasfuture-contract-test
        tests/native_has_future_contract_test.cpp)
    target_include_directories(oreslang-native-hasfuture-contract-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-native-hasfuture-contract-test PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror -UNDEBUG>)
    add_test(NAME native_hasfuture_trait_projection_admission
        COMMAND oreslang-native-hasfuture-contract-test)
    set_tests_properties(native_hasfuture_trait_projection_admission PROPERTIES TIMEOUT 15)

    # Compiler-only Send/Sync parity model. LLVM guest lowering remains gated.
    add_executable(oreslang-native-send-sync-contract-test
        tests/native_send_sync_contract_test.cpp)
    target_include_directories(oreslang-native-send-sync-contract-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-native-send-sync-contract-test PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror -UNDEBUG>)
    add_test(NAME native_send_sync_marker_contract
        COMMAND oreslang-native-send-sync-contract-test)
    set_tests_properties(native_send_sync_marker_contract PROPERTIES TIMEOUT 15)
    # Host-only tagged Symbol property identity / static layout parity.
    # This is NOT compiled guest Symbol source, a Unicode NFC decoder,
    # actor admission or property-key codegen.
    add_executable(oreslang-native-symbol-property-layout-v1-test
        tests/symbol_property_layout_v1_test.cpp)
    target_include_directories(oreslang-native-symbol-property-layout-v1-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-native-symbol-property-layout-v1-test PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>)
    add_test(NAME native_symbol_property_layout_host_only_v1
        COMMAND oreslang-native-symbol-property-layout-v1-test)
    set_tests_properties(native_symbol_property_layout_host_only_v1
        PROPERTIES TIMEOUT 15)

    find_package(Threads REQUIRED)
    add_executable(oreslang-native-string-region-test
        tests/native_string_region_test.c src/main/cpp/oreslang/runtime/NativeStringRegion.c)
    target_include_directories(oreslang-native-string-region-test PRIVATE src/main/cpp)
    set_target_properties(oreslang-native-string-region-test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES)
    target_compile_options(oreslang-native-string-region-test PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(oreslang-native-string-region-test PRIVATE Threads::Threads)
    add_test(NAME native_string_region_lifetime_and_budgets COMMAND oreslang-native-string-region-test)
    set_tests_properties(native_string_region_lifetime_and_budgets PROPERTIES TIMEOUT 20)
    find_program(ORES_CARGO_EXECUTABLE NAMES cargo REQUIRED)
    set(ORES_RUST_TEST_HARNESS_MANIFEST
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/rust-harness/Cargo.toml")
    set(ORES_RUST_TEST_HARNESS_TARGET_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/rust-test-harness-target")
    set(ORES_RUST_TEST_HARNESS_BIN
        "${ORES_RUST_TEST_HARNESS_TARGET_DIR}/release/oreslang-llvm-test-harness${CMAKE_EXECUTABLE_SUFFIX}")
    add_custom_target(oreslang-rust-test-harness ALL
        COMMAND "${ORES_CARGO_EXECUTABLE}" build
            --manifest-path "${ORES_RUST_TEST_HARNESS_MANIFEST}"
            --release --locked
            --target-dir "${ORES_RUST_TEST_HARNESS_TARGET_DIR}"
        BYPRODUCTS "${ORES_RUST_TEST_HARNESS_BIN}"
        COMMENT "Building Rust-only LLVM test harness"
        VERBATIM)
    # Host-only generational reply slab proof; not source-level actor RPC.
    add_executable(oreslang-native-generational-reply-test
        tests/native_generational_reply_slots_test.cpp)
    target_include_directories(oreslang-native-generational-reply-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-generational-reply-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-generational-reply-test PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror -UNDEBUG>)
    add_test(NAME native_actor_supervisor_generational_reply_host_only
        COMMAND oreslang-native-generational-reply-test)
    set_tests_properties(native_actor_supervisor_generational_reply_host_only
        PROPERTIES TIMEOUT 30)
    # Host-only instrumented-trace analyzer (not full guest instrumentation).
    add_executable(oreslang-concurrency-race-analyzer-test
        tests/concurrency_race_analyzer_test.cpp)
    target_include_directories(oreslang-concurrency-race-analyzer-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-concurrency-race-analyzer-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-concurrency-race-analyzer-test PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror -UNDEBUG>)
    add_test(NAME native_concurrency_vector_clock_host_only
        COMMAND oreslang-concurrency-race-analyzer-test)
    set_tests_properties(native_concurrency_vector_clock_host_only
        PROPERTIES TIMEOUT 20)

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

    # Experimental native compiler AST safety graph; not a full security proof.
    add_executable(oreslang-native-static-safety-test
        tests/native_static_safety_test.cpp)
    target_include_directories(oreslang-native-static-safety-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-native-static-safety-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME native_static_safety_ast_contract
        COMMAND oreslang-native-static-safety-test)
    set_tests_properties(native_static_safety_ast_contract PROPERTIES TIMEOUT 15)

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

    add_executable(oreslang-native-http-exchange-test tests/native_http_exchange_test.cpp)
    target_include_directories(oreslang-native-http-exchange-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-http-exchange-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-http-exchange-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_http_exchange_generation_ownership COMMAND oreslang-native-http-exchange-test)
    set_tests_properties(native_http_exchange_generation_ownership PROPERTIES TIMEOUT 20)

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

    # OS-independent allocator-backed AllocJson text/tree substrate. This does
    # not expose guest-source Json/Map/record syntax until the native frontend
    # has checked string and structural value lowering.
    add_executable(oreslang-native-alloc-json-test
        tests/native_alloc_json_test.cpp)
    target_include_directories(oreslang-native-alloc-json-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-native-alloc-json-test PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror -UNDEBUG>)
    add_test(NAME native_alloc_json_strict_text_tree_substrate
        COMMAND oreslang-native-alloc-json-test)
    set_tests_properties(native_alloc_json_strict_text_tree_substrate
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
    add_executable(oreslang-native-int-array-transfer-test
        tests/native_int_array_transfer_test.c
        src/main/cpp/oreslang/runtime/NativeIntArrayRegion.c)
    target_include_directories(oreslang-native-int-array-transfer-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_definitions(oreslang-native-int-array-transfer-test PRIVATE ORESLANG_NATIVE_TEST_ALLOC_FAILURE=1)
    target_compile_options(oreslang-native-int-array-transfer-test PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Werror -Wpedantic>)
    add_test(NAME native_int_array_cross_region_transfer COMMAND oreslang-native-int-array-transfer-test)
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
    add_test(NAME native_realtime_profile_cli
        COMMAND "${ORES_RUST_TEST_HARNESS_BIN}"
            realtime-profile "$<TARGET_FILE:oreslang-llvmc>")
    set_tests_properties(native_realtime_profile_cli PROPERTIES TIMEOUT 60)

    # Executable source admission gate: function values are not nominal classes.
    add_test(NAME native_callable_nominal_boundary
        COMMAND "${ORES_RUST_TEST_HARNESS_BIN}"
            callable-nominal "$<TARGET_FILE:oreslang-llvmc>")
    set_tests_properties(native_callable_nominal_boundary PROPERTIES TIMEOUT 35)
    # Constrained compiler-owned alias/trait/interface type declarations:
    # exact source syntax, no runtime value, and fail-closed unsupported types.
    add_test(NAME native_type_only_declaration_boundaries
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_type_only_declarations.py"
            --compiler "$<TARGET_FILE:oreslang-llvmc>")
    set_tests_properties(native_type_only_declaration_boundaries PROPERTIES TIMEOUT 90)
    add_test(NAME native_type_constraint_descriptor_v1
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_type_constraint_descriptors.py"
            --compiler "$<TARGET_FILE:oreslang-llvmc>")
    set_tests_properties(native_type_constraint_descriptor_v1 PROPERTIES TIMEOUT 90)
    # Broader refinement boundary: bounded int/i64 min/max descriptors are
    # supported, but unchecked runtime use and unimplemented refinement forms
    # must fail before emitting LLVM IR.
    add_test(NAME native_refinement_admission_failclosed
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/refinement_admission_gate.py"
            --compiler "$<TARGET_FILE:oreslang-llvmc>")
    set_tests_properties(native_refinement_admission_failclosed PROPERTIES TIMEOUT 90)
    add_test(NAME native_nominal_type_binder_boundary
        COMMAND "${ORES_RUST_TEST_HARNESS_BIN}"
            nominal-type-binders "$<TARGET_FILE:oreslang-llvmc>")
    set_tests_properties(native_nominal_type_binder_boundary PROPERTIES TIMEOUT 35)
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
    # Exact source alias-extension proof: compiler-owned static receiver dispatch,
    # LLVM IR emission, Clang link and native process execution.
    add_test(NAME native_alias_extension_method_aot
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_alias_extension_methods.py"
            --compiler "$<TARGET_FILE:oreslang-llvmc>"
            --clang "${ORE_SOURCE_I64_CLANG}")
    set_tests_properties(native_alias_extension_method_aot PROPERTIES TIMEOUT 120)
    # Exact compiler -> LLVM -> Clang -> OS conformance from the shared TSV.
    add_test(NAME native_shared_type_declaration_conformance
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/shared_type_declarations_conformance.py"
            --compiler "$<TARGET_FILE:oreslang-llvmc>"
            --clang "${ORE_SOURCE_I64_CLANG}")
    set_tests_properties(native_shared_type_declaration_conformance PROPERTIES TIMEOUT 120)
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
            COMMAND "${ORES_RUST_TEST_HARNESS_BIN}" tagged-message-wire
                "$<TARGET_FILE:oreslang-tagged-message-wire-v2-test>"
                "${CMAKE_CURRENT_SOURCE_DIR}/contracts/v1/design/tagged_message_vectors.json")
        set_tests_properties(native_tagged_enum_message_java_wire_parity PROPERTIES TIMEOUT 30)
    else()
        message(STATUS "Native MessagePack v2 parity test requires OpenSSL Crypto; not enabled")
    endif()
    find_program(ORE_CLANG NAMES clang clang-19 clang-18 clang-17 clang-16 clang-15 clang-14)
    # Executable Oreslang method fixtures, using Rust-only tooling.
    if(ORE_CLANG)
        add_test(NAME native_inline_self_receiver_arity_aot
            COMMAND "${ORES_RUST_TEST_HARNESS_BIN}"
                native-inline-self-arity
                "$<TARGET_FILE:oreslang-llvmc>"
                "${ORE_CLANG}")
        set_tests_properties(native_inline_self_receiver_arity_aot PROPERTIES TIMEOUT 90)
    endif()
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

    if(APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
        foreach(mode IN ITEMS release sanitizer)
            set(native_http_extra)
            if(mode STREQUAL "sanitizer")
                set(native_http_extra --sanitize)
            endif()
            add_test(NAME native_http_compiled_actor_${mode}
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_http_build_and_test.py"
                    --compiler "$<TARGET_FILE:oreslang-llvmc>" --clang "${ORE_RT_AOT_CLANG}"
                    --cxx "${ORE_RT_AOT_CXX}" --root "${CMAKE_CURRENT_SOURCE_DIR}" ${native_http_extra})
            set_tests_properties(native_http_compiled_actor_${mode} PROPERTIES TIMEOUT 180)
        endforeach()
    endif()

    foreach(mode IN ITEMS release sanitizer)
        set(native_method_extra)
        if(mode STREQUAL "sanitizer")
            set(native_method_extra --sanitize)
        endif()
        add_test(NAME native_source_class_methods_${mode}
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_class_methods.py"
                --compiler "$<TARGET_FILE:oreslang-llvmc>" --clang "${ORE_CLANG}" ${native_method_extra})
        set_tests_properties(native_source_class_methods_${mode} PROPERTIES TIMEOUT 120)
    endforeach()

    foreach(mode IN ITEMS release sanitizer)
        set(native_bool_extra)
        if(mode STREQUAL "sanitizer")
            set(native_bool_extra --sanitize)
        endif()
        add_test(NAME native_source_bool_${mode}
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_bool_types.py"
                --compiler "$<TARGET_FILE:oreslang-llvmc>" --clang "${ORE_CLANG}" ${native_bool_extra})
        set_tests_properties(native_source_bool_${mode} PROPERTIES TIMEOUT 120)
    endforeach()

    add_test(NAME native_shared_borrow_contract
        COMMAND "${CMAKE_CURRENT_BINARY_DIR}/oreslang-native-borrow-alias-matrix"
            --compiler "$<TARGET_FILE:oreslang-llvmc>" --clang "${ORE_CLANG}"
            --corpus "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/borrow-contract.tsv")
    set_tests_properties(native_shared_borrow_contract PROPERTIES TIMEOUT 120)

    add_test(NAME native_source_module_graph_execution
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_module_imports.py"
            --compiler "$<TARGET_FILE:oreslang-llvmc>" --clang "${ORE_RT_AOT_CLANG}")
    set_tests_properties(native_source_module_graph_execution PROPERTIES TIMEOUT 120)

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
    target_link_libraries(oreslang-dop-kernel-test PRIVATE Threads::Threads)
    add_test(NAME dop_typed_cpu_opencl_ownership COMMAND oreslang-dop-kernel-test)
    add_executable(oreslang-dop-gpu-admission-test tests/dop_gpu_admission_test.cpp)
    target_include_directories(oreslang-dop-gpu-admission-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-dop-gpu-admission-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-dop-gpu-admission-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME dop_gpu_exact_layout_fence_admission COMMAND oreslang-dop-gpu-admission-test)
    # Host-only parity for the source-visible std/compute descriptor contract.
    # These values are metadata only; they carry no device/buffer/fence authority
    # and do not imply source-module linking or physical GPU execution.
    add_executable(oreslang-portable-compute-descriptor-test
        tests/portable_compute_descriptor_test.cpp)
    target_include_directories(oreslang-portable-compute-descriptor-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-portable-compute-descriptor-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_portable_compute_descriptor_contract
        COMMAND oreslang-portable-compute-descriptor-test)
    add_executable(oreslang-portable-compute-authority-test
        tests/portable_compute_authority_test.cpp)
    target_include_directories(oreslang-portable-compute-authority-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-portable-compute-authority-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(oreslang-portable-compute-authority-test PRIVATE Threads::Threads)
    add_test(NAME native_portable_compute_authority_contract
        COMMAND oreslang-portable-compute-authority-test)
    add_executable(oreslang-portable-compute-kernel-g2-test
        tests/portable_compute_kernel_g2_test.cpp)
    target_include_directories(oreslang-portable-compute-kernel-g2-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-portable-compute-kernel-g2-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_portable_compute_kernel_g2_contract
        COMMAND oreslang-portable-compute-kernel-g2-test)
    add_executable(oreslang-portable-compute-opencl-g2-test
        tests/portable_compute_opencl_g2_test.cpp)
    target_include_directories(oreslang-portable-compute-opencl-g2-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-portable-compute-opencl-g2-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_portable_compute_opencl_g2_contract
        COMMAND oreslang-portable-compute-opencl-g2-test)
    add_executable(oreslang-opencl-provider-g3-test
        tests/opencl_provider_g3_test.cpp)
    target_include_directories(oreslang-opencl-provider-g3-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-opencl-provider-g3-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(oreslang-opencl-provider-g3-test PRIVATE Threads::Threads)
    add_test(NAME native_opencl_provider_g3_state_machine
        COMMAND oreslang-opencl-provider-g3-test)
    set_tests_properties(native_opencl_provider_g3_state_machine PROPERTIES TIMEOUT 30)

    add_executable(oreslang-opencl-system-gpu-probe
        tests/opencl_system_gpu_probe.cpp)
    target_include_directories(oreslang-opencl-system-gpu-probe PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-opencl-system-gpu-probe PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(oreslang-opencl-system-gpu-probe PRIVATE
        Threads::Threads ${CMAKE_DL_LIBS})
    add_test(NAME native_opencl_real_gpu_probe_conditional
        COMMAND oreslang-opencl-system-gpu-probe)
    set_tests_properties(native_opencl_real_gpu_probe_conditional PROPERTIES TIMEOUT 30)
    add_executable(oreslang-dop-read-loan-thread-test tests/dop_hostbuffer_concurrency_test.cpp)
    target_include_directories(oreslang-dop-read-loan-thread-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-dop-read-loan-thread-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(oreslang-dop-read-loan-thread-test PRIVATE Threads::Threads)
    add_test(NAME dop_hostbuffer_multithread_read_loan_quiescence
        COMMAND oreslang-dop-read-loan-thread-test)
    set_tests_properties(dop_hostbuffer_multithread_read_loan_quiescence PROPERTIES TIMEOUT 25)
    # Pure compile-time ECS component/chunk dependency planning. No guest
    # multi-threaded lowering or actor-state loan splitting is implied.
    add_executable(oreslang-dop-access-planner-test tests/dop_system_access_planner_test.cpp)
    target_include_directories(oreslang-dop-access-planner-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-dop-access-planner-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME dop_static_access_batch_conflicts
        COMMAND oreslang-dop-access-planner-test)
    # Backend-independent ownership auto traits plus logical DOP place
    # conflicts. This is a semantic gate, not source syntax/lowering proof.
    add_executable(oreslang-ownership-auto-traits-dop-test
        tests/ownership_auto_traits_dop_test.cpp)
    target_include_directories(oreslang-ownership-auto-traits-dop-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-ownership-auto-traits-dop-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_ownership_auto_traits_and_logical_dop
        COMMAND oreslang-ownership-auto-traits-dop-test)
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
    oreslang_contract_test(native_exit_reject_unsupported_explicit_exit exit-unsupported-process-exit.ores
        --diagnostic "oreslang-llvmc:")
    oreslang_contract_test(native_exit_reject_unsupported_async_root exit-unsupported-async-main.ores
        --diagnostic "native actor/async declaration requires")
    oreslang_contract_test(native_exit_void_main_ir exit-void-main.ores
        --ir "define i32 @main()")
    if(ORE_CLANG)
        oreslang_contract_test(native_exit_void_fallthrough exit-void-main.ores
            --run 0 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_exit_module_main module-main.ores
            --run 7 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_exit_max_valid exit-max-main.ores
            --run 255 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_exit_overflow_fails_closed exit-invalid-positive-main.ores
            --run 1 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_exit_negative_fails_closed exit-invalid-negative-main.ores
            --run 1 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_exit_zero exit-zero-main.ores
            --run 0 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_exit_one exit-one-main.ores
            --run 1 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_exit_wide_64bit_overflow_fails_closed exit-wide-overflow-main.ores
            --run 1 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_exit_void_runs_defer exit-void-deferred-cleanup-main.ores
            --run 0 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_exit_no_main_is_not_linkable exit-no-main.ores
            --no-main-link --clang "${ORE_CLANG}")
    endif()
    add_test(NAME native_explicit_wasm32_target_ir
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm32_target_ir_test.py"
            "$<TARGET_FILE:oreslang-llvmc>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/constant-main.ores")
    oreslang_contract_test(module_main module-main.ores --ir "demo.main")
    # Compile-time i64 contract subset: actually type-check a module before
    # emitting verified IR, then link/run a native main. Cross-unit imports and
    # String contracts must reject without emitting any LLVM IR.
    oreslang_contract_test(native_i64_module_contract_valid native-contract-i64-valid.ores
        --ir "call i64 @Math.add")
    oreslang_contract_test(native_i64_module_contract_missing native-contract-i64-missing.ores
        --diagnostic "missing public native module contract method")
    oreslang_contract_test(native_i64_module_contract_private native-contract-i64-private.ores
        --diagnostic "missing public native module contract method")
    oreslang_contract_test(native_i64_module_contract_arity native-contract-i64-arity.ores
        --diagnostic "native module contract parameter arity mismatch")
    oreslang_contract_test(native_i64_module_contract_borrow native-contract-i64-borrow.ores
        --diagnostic "native module contract ownership mode mismatch")
    oreslang_contract_test(native_i64_module_contract_void native-contract-i64-void.ores
        --diagnostic "native module contract requires int return")
    oreslang_contract_test(native_i64_module_contract_duplicate_parameter native-contract-i64-duplicate-param.ores
        --diagnostic "duplicate native module contract parameter")
    oreslang_contract_test(native_i64_module_contract_split_arrow native-contract-i64-split-arrow.ores
        --diagnostic "contract signatures require contiguous '=>'")
    oreslang_contract_test(native_i64_module_contract_unknown native-contract-i64-unknown.ores
        --diagnostic "unknown native module contract")
    oreslang_contract_test(native_i64_module_contract_duplicate native-contract-i64-duplicate.ores
        --diagnostic "duplicate native module contract method")
    oreslang_contract_test(native_contract_unlinked_import native-contract-i64-import.ores
        --diagnostic "native contract imports require a validated cross-unit linker")
    oreslang_contract_test(native_contract_string_unimplemented native-contract-reject-string.ores
        --diagnostic "expected 'int'")

    oreslang_contract_test(call_scoped_borrow_parameter_refresh unsupported-borrow-param.ores
        --ir "call i64 @inspect" --run 42 --clang "${ORE_CLANG}")
    oreslang_contract_test(call_and_forward_ref forward-call-main.ores --ir "call i64 @add")
    add_test(NAME native_named_call_lexical_ir_order
        COMMAND "${ORES_RUST_TEST_HARNESS_BIN}" named-call-order
          "$<TARGET_FILE:oreslang-llvmc>"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/named-call-source-order.ores")
    set_tests_properties(native_named_call_lexical_ir_order PROPERTIES TIMEOUT 30)
    if(ORE_CLANG)
        oreslang_contract_test(native_named_call_reordered_guest named-call-reordered-main.ores
            --run 43 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_named_call_mixed_guest named-call-mixed-main.ores
            --run 43 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_named_call_mut_borrow_binding named-call-mut-borrow-native.ores
            --run 7 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_named_call_overload_arity_guest named-call-arity-overload-main.ores
            --run 43 --clang "${ORE_CLANG}")
    endif()
    # Same exact public Oreslang handler fixture as Java/Graal; execute native code.
    if(ORE_CLANG)
        oreslang_contract_test(shared_java_llvm_http_handler http-guest-counter-handler.ores --ir "call i64 @counter_response" --run 43 --clang "${ORE_CLANG}")
        add_test(NAME shared_java_llvm_128_call_matrix
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/shared_guest_handler_matrix.py"
                --compiler "$<TARGET_FILE:oreslang-llvmc>"
                --clang "${ORE_CLANG}"
                --fixture "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/http-guest-counter-handler.ores")
    endif()
    # Bounded synchronous defer: LIFO calls on taken/fallthrough return CFG edges.
    if(ORE_CLANG)
        oreslang_contract_test(native_defer_lifo_guarded_execution native-defer-lifo-cfg.ores
            --ir "call void @multiply_ten" --run 111 --clang "${ORE_CLANG}")
    endif()
    if(ORE_CLANG)
        oreslang_contract_test(native_defer_multiguard_cfg_exit_execution native-defer-multiguard-cfg.ores
            --ir "call void @multiply_two" --run 15 --clang "${ORE_CLANG}")
    endif()
    oreslang_contract_test(reject_native_defer_noncall native-defer-noncall-reject.ores
        --diagnostic "native defer requires a direct function call")
    oreslang_contract_test(reject_native_defer_moved_return native-defer-moved-after-return.ores
        --diagnostic "borrow of explicitly moved int")
    # Native return snapshot is immutable under subsequent deferred field cleanup.
    if(ORE_CLANG)
        oreslang_contract_test(native_defer_field_return_snapshot native-defer-field-return.ores
            --ir "return.guard.cond.i1" --run 222 --clang "${ORE_CLANG}")
    endif()
    oreslang_contract_test(reject_native_defer_field_shared_borrow
        native-defer-field-shared-borrow-reject.ores
        --diagnostic "cannot mutate through shared class borrow")
    oreslang_contract_test(reject_native_defer_field_moved_return
        native-defer-field-moved-after-return-reject.ores
        --diagnostic "not definitely initialized")
    oreslang_contract_test(reject_native_deferred_return_rebinding
        native-defer-return-rebinding-reject.ores
        --diagnostic "native defer requires a direct function call or class field assignment")
    oreslang_contract_test(reject_native_finally_override
        native-defer-finally-unsupported.ores
        --diagnostic "cannot escape finally")
    if(ORE_CLANG)
        oreslang_contract_test(native_try_finally_return_snapshot
            native-try-finally-return-snapshot.ores
            --run 246 --clang "${ORE_CLANG}")
    endif()
    # First executable native typed-i64 throw/catch CFG; Java still reserves
    # source throw, so this is NOT cross-backend guest-source parity.
    if(ORE_CLANG)
        oreslang_contract_test(native_caught_throw_unwind_lifo
            native-caught-throw-unwind-lifo.ores
            --ir "throw.cleanup" --run 47 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_caught_throw_payload
            native-caught-throw-payload.ores
            --ir "throw.catch" --run 43 --clang "${ORE_CLANG}")
    endif()
    oreslang_contract_test(reject_caught_throw_nested_call
        native-caught-throw-nested-call-reject.ores
        --diagnostic "native caught throw requires nonthrowing int payload")
    oreslang_contract_test(reject_caught_throw_unsafe_defer
        native-caught-throw-defer-call-reject.ores
        --diagnostic "native caught throw defer requires named class field write")
    oreslang_contract_test(reject_caught_throw_effect_signature
        native-caught-throw-qualified-effects-reject.ores
        --diagnostic "native caught throw effect-qualified function unsupported")
    oreslang_contract_test(reject_caught_throw_finally_call
        native-caught-throw-finally-call-reject.ores
        --diagnostic "native caught throw finally requires nonthrowing field write")
    # Native ABI-backed reads are not pure scalar operations.
    oreslang_contract_test(reject_caught_throw_array_size_operand
        native-caught-throw-array-size-reject.ores
        --diagnostic "native caught throw scalar read requires stack i64")
    oreslang_contract_test(reject_caught_throw_heap_owned_operand
        native-caught-throw-heap-owned-reject.ores --experimental-owned-int
        --diagnostic "native caught throw scalar read requires stack i64")
    oreslang_contract_test(reject_caught_throw_defer_array_size
        native-caught-throw-defer-array-size-reject.ores
        --diagnostic "native caught throw scalar read requires stack i64")
    oreslang_contract_test(reject_caught_throw_catch_array_size
        native-caught-throw-catch-array-size-reject.ores
        --diagnostic "native caught throw scalar read requires stack i64")
    oreslang_contract_test(reject_caught_throw_finally_array_size
        native-caught-throw-finally-array-size-reject.ores
        --diagnostic "native caught throw scalar read requires stack i64")
    oreslang_contract_test(reject_caught_throw_wasm32
        native-caught-throw-payload.ores --target wasm32
        --diagnostic "wasm32 native caught-throw guest unsupported")
    oreslang_contract_test(reject_finally_return_override
        native-finally-return-reject.ores --diagnostic "cannot escape finally")
    oreslang_contract_test(reject_finally_break_override
        native-finally-break-reject.ores --diagnostic "cannot escape finally")
    oreslang_contract_test(reject_finally_continue_override
        native-finally-continue-reject.ores --diagnostic "cannot escape finally")
    oreslang_contract_test(reject_try_nonempty_catch
        native-try-nonempty-catch-reject.ores
        --diagnostic "native nonthrowing catch requires only an integer literal return")
    oreslang_contract_test(reject_try_effectful_body
        native-try-effectful-body-reject.ores
        --diagnostic "native terminal try supports only return guards and final return")
    oreslang_contract_test(reject_try_throw_capable_return
        native-try-throw-capable-return-reject.ores
        --diagnostic "native protected try return requires nonthrowing scalar/field read")
    oreslang_contract_test(reject_unadmitted_trap_expression
        unsupported-trap-expression.ores
        --diagnostic "native trap expression unsupported: typed throw effects and cleanup lowering required")
    oreslang_contract_test(reject_nested_unadmitted_trap_expression
        unsupported-trap-nested-call.ores
        --diagnostic "native trap expression unsupported: typed throw effects and cleanup lowering required")
    oreslang_contract_test(reject_binary_operand_trap_expression
        unsupported-trap-binary-operand.ores
        --diagnostic "native trap expression unsupported: typed throw effects and cleanup lowering required")
    # Native source if/else CFG: separate branch states must join safely.
    # Real Clang execution, ASan/UBSan and no-partial-IR rejection matrix.
    if(ORE_CLANG)
        add_test(NAME native_source_conditional_cfg
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/conditional_cfg_native_matrix.py"
                --compiler "$<TARGET_FILE:oreslang-llvmc>"
                --clang "${ORE_CLANG}")
        add_test(NAME native_source_conditional_cfg_asan_ubsan
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/conditional_cfg_native_matrix.py"
                --compiler "$<TARGET_FILE:oreslang-llvmc>"
                --clang "${ORE_CLANG}" --sanitize)
        set_tests_properties(native_source_conditional_cfg
            native_source_conditional_cfg_asan_ubsan PROPERTIES TIMEOUT 180)
    endif()
    # Canonical value-producing conditional expression. Conditions are
    # first-class bool expressions on this stacked branch; exactly one value
    # arm executes and affine state must agree at the PHI join.
    if(ORE_CLANG)
        add_test(NAME native_source_ternary_expression
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/ternary_native_matrix.py"
                --compiler "$<TARGET_FILE:oreslang-llvmc>"
                --clang "${ORE_CLANG}")
        add_test(NAME native_source_ternary_expression_asan_ubsan
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/ternary_native_matrix.py"
                --compiler "$<TARGET_FILE:oreslang-llvmc>"
                --clang "${ORE_CLANG}" --sanitize)
        set_tests_properties(native_source_ternary_expression
            native_source_ternary_expression_asan_ubsan PROPERTIES TIMEOUT 180)
    endif()
    # First-class native bool scalars. Source typing distinguishes int/bool
    # while the initial same-module ABI canonicalizes bool to i64 0/1.
    if(ORE_CLANG)
        add_test(NAME native_source_bool_scalars
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_bool_matrix.py"
                --compiler "$<TARGET_FILE:oreslang-llvmc>"
                --clang "${ORE_CLANG}")
        add_test(NAME native_source_bool_scalars_asan_ubsan
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_bool_matrix.py"
                --compiler "$<TARGET_FILE:oreslang-llvmc>"
                --clang "${ORE_CLANG}" --sanitize)
        set_tests_properties(native_source_bool_scalars
            native_source_bool_scalars_asan_ubsan PROPERTIES TIMEOUT 180)
    endif()
    # Synchronous branch-scoped i64 lifetimes cannot escape the owning arm.
    if(ORE_CLANG)
        add_test(NAME native_branch_local_scope_source_execution
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/branch_local_cfg_matrix.py"
                --compiler "$<TARGET_FILE:oreslang-llvmc>" --clang "${ORE_CLANG}")
        add_test(NAME native_branch_local_scope_asan_ubsan
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/branch_local_cfg_matrix.py"
                --compiler "$<TARGET_FILE:oreslang-llvmc>"
                --clang "${ORE_CLANG}" --sanitize)
        set_tests_properties(native_branch_local_scope_source_execution
            native_branch_local_scope_asan_ubsan PROPERTIES TIMEOUT 180)
    endif()
    oreslang_contract_test(arithmetic_precedence return-arithmetic.ores --ir "ret i64 28")
    # Proven-nonescaping integer-field classes are stack-only and emit no
    # malloc, tracing collector roots, or native heap calls.
    oreslang_contract_test(owned_stack_class owned-stack-class.ores --ir "alloca %Box")
    oreslang_contract_test(reject_owned_stack_escape owned-stack-escape.ores --diagnostic "class is non-Copy")
    oreslang_contract_test(reject_owned_stack_unknown_field owned-stack-bad-field.ores --diagnostic "unknown class field")
    oreslang_contract_test(reject_owned_stack_duplicate_field owned-stack-double-field.ores --diagnostic "duplicate class field")
    oreslang_contract_test(negative_literal negative-literal.ores --ir "ret i64 42")
    oreslang_contract_test(nested_comments nested-comments.ores --ir "ret i64 42")
    oreslang_contract_test(native_owned_lexical_lifetime owned-lexical-main.ores --no-native-abi --ir "llvm.lifetime.end")
    oreslang_contract_test(native_owned_identifier owned-identifier-main.ores --no-native-abi --ir "call i64 @owned")
    # Port the unique generated stress corpus from old stacked #72 without its
    # outdated frontend/backend implementation. This compiles + runs native IR.
    if(ORE_CLANG)
        add_test(NAME native_owned_scalar_cfg_generated_stress
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/owned_syntax_stress.py"
                --compiler "$<TARGET_FILE:oreslang-llvmc>"
                --clang "${ORE_CLANG}"
                --native-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeOwnedI64Region.c")
        set_tests_properties(native_owned_scalar_cfg_generated_stress PROPERTIES TIMEOUT 240)
    endif()
    # Class moves are compiled and exercised through real native guests.
    find_program(ORE_MOVE_RUSTC rustc)
    if(ORE_CLANG AND ORE_MOVE_RUSTC)
        add_custom_command(
            OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/oreslang-class-move-native-matrix"
            COMMAND "${ORE_MOVE_RUSTC}" --edition=2021 -O -D warnings
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/class_move_native_matrix.rs"
                -o "${CMAKE_CURRENT_BINARY_DIR}/oreslang-class-move-native-matrix"
            DEPENDS tests/class_move_native_matrix.rs VERBATIM)
        add_custom_target(oreslang-class-move-native-matrix-build ALL
            DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/oreslang-class-move-native-matrix")
        add_test(NAME native_class_move_source_execution
            COMMAND "${CMAKE_CURRENT_BINARY_DIR}/oreslang-class-move-native-matrix"
                --compiler "$<TARGET_FILE:oreslang-llvmc>" --clang "${ORE_CLANG}")
        add_test(NAME native_class_move_source_asan_ubsan
            COMMAND "${CMAKE_CURRENT_BINARY_DIR}/oreslang-class-move-native-matrix"
                --compiler "$<TARGET_FILE:oreslang-llvmc>" --clang "${ORE_CLANG}" --sanitize)
        add_test(NAME native_class_move_optimized_rust_assertions
            COMMAND "${CMAKE_CURRENT_BINARY_DIR}/oreslang-class-move-native-matrix" --self-test)
        set_tests_properties(native_class_move_source_execution
            native_class_move_source_asan_ubsan
            native_class_move_optimized_rust_assertions PROPERTIES TIMEOUT 180)
    else()
        foreach(move_gate IN ITEMS native_class_move_source_execution
                native_class_move_source_asan_ubsan native_class_move_optimized_rust_assertions)
            add_test(NAME ${move_gate} COMMAND "${CMAKE_COMMAND}" -E false)
        endforeach()
    endif()
    oreslang_contract_test(reject_owned_int_without_opt_in owned-native-region-main.ores --diagnostic "experimental owned int requires --experimental-owned-int")
    oreslang_contract_test(owned_native_heap_lowering owned-native-region-main.ores --experimental-owned-int --ir "call void @oreslang_owned_i64_region_drop")
    oreslang_contract_test(owned_native_region_nested owned-native-region-call.ores --experimental-owned-int --ir "call void @oreslang_owned_i64_region_close")
    oreslang_contract_test(reject_unsupported_native_heap_type owned-native-region-unsupported.ores --experimental-owned-int --diagnostic "native scalar local requires int or bool")
    oreslang_contract_test(reject_local_uninitialized owned-uninitialized.ores --diagnostic "unknown parameter")
    oreslang_contract_test(reject_local_duplicate owned-duplicate.ores --diagnostic "duplicate owned local")
    oreslang_contract_test(reject_local_forward_reference owned-use-before-declaration.ores --diagnostic "unknown parameter")
    oreslang_contract_test(reject_local_unsupported_object owned-unsupported-heap.ores --diagnostic "unknown record class")
    oreslang_contract_test(reject_local_moved_before_read owned-local-use-after-take.ores --diagnostic "use of explicitly moved int")
    oreslang_contract_test(native_owned_local_shared_borrow owned-local-shared-borrow.ores --ir "llvm.lifetime.end")
    oreslang_contract_test(native_owned_local_mutable_borrow owned-local-mutable-borrow.ores --ir "llvm.lifetime.end")
    oreslang_contract_test(reject_actor unsupported-actor.ores --diagnostic "actor requires at least one scalar field")
    # Optional instance method marker: fnc, method, and no marker all lower
    # to the same native supported on_message(int) handler.
    oreslang_contract_test(native_actor_explicit_method source-actor-method-keyword.ores
        --ir "oreslang_source_actor_spawn")
    oreslang_contract_test(native_contextual_method_identifier contextual-method-identifier.ores
        --ir "ret i64")
    oreslang_contract_test(native_actor_bare_method source-actor-bare-method.ores
        --ir "oreslang_source_actor_spawn")
    oreslang_contract_test(native_actor_suspending_method_gate source-actor-suspend-method-reject.ores
        --diagnostic "native suspends actor method requires continuation lowering")
    oreslang_contract_test(native_actor_final_group_field_gate source-actor-final-group-field-reject.ores
        --diagnostic "native final actor fields require typed field initialization and ownership lowering")
    # Host-only ActorGroup substrate: source actor spawn/await fail closed.
    oreslang_contract_test(reject_native_actor_group_spawn unsupported-actor-group-spawn.ores --diagnostic "expected int expression")
    oreslang_contract_test(reject_native_actor_group_await_spawn unsupported-actor-group-await-spawn.ores --diagnostic "native async/channel/select boundary unsupported")
    # LLVM source ActorGroups remain unsupported until typed handle/permit lowering.
    # No native IR may be emitted on these negative compiler fixtures.
    oreslang_contract_test(reject_guest_actor_group_create source-actor-group-create-unsupported.ores --diagnostic "native ActorGroup source lowering unsupported")
    oreslang_contract_test(reject_guest_actor_group_join source-actor-group-join-unsupported.ores --diagnostic "native ActorGroup source lowering unsupported")
    oreslang_contract_test(reject_guest_actor_group_shadow source-actor-group-declaration-unsupported.ores --diagnostic "native ActorGroup source lowering unsupported")

    # ActorRef<T> is a source-checked non-Copy capability; Future<T> remains
    # explicitly blocked until compiler-generated suspension is lowered.
    oreslang_contract_test(source_actor_typed_ref_admit source-actor-typed-ref.ores
        --ir "oreslang_source_actor_spawn")
    oreslang_contract_test(source_actor_typed_ref_mismatch source-actor-typed-ref-mismatch.ores
        --diagnostic "ActorRef<T> type mismatch")
    oreslang_contract_test(source_actor_typed_ref_nonspawn source-actor-typed-ref-nonspawn.ores
        --diagnostic "ActorRef<T> requires spawn initializer")
    oreslang_contract_test(source_actor_await_ready_continuation_ir source-actor-await-ready.ores
        --ir "ready_continuation")
    oreslang_contract_test(source_actor_await_ready_trace_runtime_ir source-actor-await-ready.ores
        --ir "oreslang_source_actor_await_ready")
    # ActorRef implements HasFuture: awaiting its capability directly
    # delegates to the same get_ready() root CONTROL continuation.
    oreslang_contract_test(source_actor_hasfuture_direct_await_ir
        source-actor-await-worker.ores --ir "ready_continuation")
    oreslang_contract_test(source_actor_hasfuture_direct_await_runtime_ir
        source-actor-await-worker.ores --ir "oreslang_source_actor_await_ready")
    oreslang_contract_test(reject_source_actor_hasfuture_unknown_receiver
        source-actor-await-unknown.ores
        --diagnostic "await get_ready requires a live direct ActorRef receiver")
    oreslang_contract_test(reject_source_actor_hasfuture_get_done
        source-actor-await-get-done.ores
        --diagnostic "expected 'get_ready'")
    oreslang_contract_test(reject_source_actor_hasfuture_double_await
        source-actor-await-worker-twice.ores
        --diagnostic "native source supports only one await get_ready continuation")
    # HasFuture must be a checked live actor capability, not merely an
    # identifier that happens to appear in an await expression.
    oreslang_contract_test(reject_source_actor_hasfuture_scalar_operand
        source-actor-await-non-hasfuture-scalar.ores
        --diagnostic "await get_ready requires a live direct ActorRef receiver")
    oreslang_contract_test(reject_source_actor_hasfuture_stopped_actor
        source-actor-await-after-stop.ores
        --diagnostic "await get_ready must precede post-await actor effects")
    oreslang_contract_test(reject_source_actor_hasfuture_skips_linear_future
        source-actor-future-void-await-worker-instead.ores
        --diagnostic "native Future<void> await requires its declared readiness token")
    oreslang_contract_test(reject_source_actor_hasfuture_reuses_future
        source-actor-future-void-await-again.ores
        --diagnostic "native source supports only one await get_ready continuation")
    # First actual native source-level typed Future: Future<void> from a
    # single ActorRef.get_ready(). It uses the existing root CONTROL
    # continuation, not synchronous polling or a generic Future<T> ABI.
    oreslang_contract_test(source_actor_typed_ready_future_continuation_ir
        source-actor-future-void-ready.ores --ir "ready_continuation")
    oreslang_contract_test(source_actor_typed_ready_future_runtime_ir
        source-actor-future-void-ready.ores --ir "oreslang_source_actor_await_ready")
    oreslang_contract_test(reject_source_actor_unused_ready_future
        source-actor-future-void-unused.ores
        --diagnostic "Future<void> readiness token must be awaited exactly once")
    oreslang_contract_test(reject_source_actor_wrong_ready_future
        source-actor-future-void-wrong-name.ores
        --diagnostic "native Future<void> await requires its declared readiness token")
    oreslang_contract_test(reject_source_actor_double_ready_future
        source-actor-future-void-double-await.ores
        --diagnostic "native source supports only one await get_ready continuation")
    oreslang_contract_test(reject_source_actor_unspawned_ready_future
        source-actor-future-void-unspawned.ores
        --diagnostic "Future<void> requires previously spawned ActorRef.get_ready")
    oreslang_contract_test(reject_source_actor_ready_future_shadow
        source-actor-future-void-shadow.ores
        --diagnostic "Future<void> readiness token shadows a live owner")
    oreslang_contract_test(reject_source_actor_duplicate_ready_future
        source-actor-future-void-duplicate.ores
        --diagnostic "native source supports only one Future<void> readiness token")
    oreslang_contract_test(reject_source_actor_ready_future_wrong_producer
        source-actor-future-void-wrong-producer.ores
        --diagnostic "expected 'get_ready'")
    oreslang_contract_test(reject_source_actor_ready_future_as_message
        source-actor-future-void-send.ores
        --diagnostic "unknown parameter")
    oreslang_contract_test(reject_source_actor_await_suspended_local
        source-actor-spawn-unavailable-await.ores
        --diagnostic "await get_ready requires one source actor without suspended locals")
    oreslang_contract_test(reject_source_actor_await_callable_result
        source-actor-await-ready-impure-result.ores
        --diagnostic "await get_ready requires side-effect-free scalar return")
    oreslang_contract_test(source_future_typed_await_boundary source-future-typed-unimplemented.ores
        --diagnostic "Future<T> requires native continuation lowering")
    # These tests reject nested type declarations BEFORE member-call lowering.
    # They prove fail-closed type admission, not flatten/unwrap_all execution.
    # Operation-level conformance remains blocked on LLVM generic representation.
    oreslang_contract_test(source_nested_option_type_admission_fail_closed
        source-nested-option-flatten-unimplemented.ores --diagnostic "native scalar local requires int or bool")
    oreslang_contract_test(source_nested_future_type_admission_fail_closed
        source-nested-future-flatten-unimplemented.ores --diagnostic "expected '>'")


    # Entry<K,V> is a compiler-owned nominal product in Java/GraalVM.
    # Native lowering is intentionally denied until representation, ownership,
    # projection and transport parity land together.
    oreslang_contract_test(reject_native_entry_shadow_class
        entry-shadow-class-reject.ores --diagnostic "builtin Entry is compiler-owned")
    oreslang_contract_test(reject_native_entry_shadow_actor
        entry-shadow-actor-reject.ores --diagnostic "builtin Entry is compiler-owned")
    oreslang_contract_test(reject_native_entry_parameter_until_parity
        entry-parameter-unimplemented.ores --diagnostic "native Entry<K,V> requires nominal product lowering and ownership parity")
    oreslang_contract_test(reject_native_entry_return_until_parity
        entry-return-unimplemented.ores --diagnostic "native Entry<K,V> requires nominal product lowering and ownership parity")
    oreslang_contract_test(reject_native_entry_local_until_parity
        entry-local-unimplemented.ores --diagnostic "native Entry<K,V> requires nominal product lowering and ownership parity")

    # Real source-compiled Oreslang actors, not host-only C callback tests.
    # The synchronous stop barrier is allowed only in trusted top-level main.
    # One zero-local await ActorRef.get_ready() is admitted through a traced
    # root CONTROL continuation; generic Future<T>/select remain fail-closed.
    add_executable(oreslang-source-actor-await-trace-runtime-test
        tests/source_actor_await_trace_runtime_test.c
        src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c
        src/main/cpp/oreslang/runtime/NativeOresVM.c
        src/main/cpp/oreslang/runtime/NativeActorFuture.c
        src/main/cpp/oreslang/runtime/SourceActorBootstrap.c
        src/main/cpp/oreslang/runtime/NativeExecutionContextStore.c
        src/main/cpp/oreslang/runtime/NativeContinuationStore.c
        src/main/cpp/oreslang/runtime/NativeAwaitTraceBridge.c
        src/main/cpp/oreslang/runtime/NativeLogicalTrace.c)
    target_include_directories(oreslang-source-actor-await-trace-runtime-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-source-actor-await-trace-runtime-test PRIVATE
        Threads::Threads)
    target_compile_options(oreslang-source-actor-await-trace-runtime-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME source_actor_await_trace_runtime
        COMMAND oreslang-source-actor-await-trace-runtime-test)
    set_tests_properties(source_actor_await_trace_runtime PROPERTIES TIMEOUT 30)

    if(ORE_CLANG)
        add_test(NAME source_actor_compiled_native_lifecycle
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/source_actor_e2e.py"
                --compiler "$<TARGET_FILE:oreslang-llvmc>"
                --clang "${ORE_CLANG}"
                --root "${CMAKE_CURRENT_SOURCE_DIR}")
        set_tests_properties(source_actor_compiled_native_lifecycle PROPERTIES TIMEOUT 180)
        add_test(NAME source_actor_compiled_native_asan_ubsan
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/source_actor_e2e.py"
                --compiler "$<TARGET_FILE:oreslang-llvmc>"
                --clang "${ORE_CLANG}"
                --root "${CMAKE_CURRENT_SOURCE_DIR}" --sanitize)
        set_tests_properties(source_actor_compiled_native_asan_ubsan PROPERTIES TIMEOUT 180)
    endif()

    # Run both direct await and typed Future<void> source through real
    # Clang-linked actor runtime, including independent release/sanitizer
    # physical lifecycle receipts. No new Python test drivers.
    find_program(ORE_READY_FUTURE_RUSTC NAMES rustc)
    if(ORE_CLANG AND ORE_READY_FUTURE_RUSTC)
        add_custom_command(
            OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/oreslang-ready-future-void-e2e"
            COMMAND "${ORE_READY_FUTURE_RUSTC}" --edition=2021 -O -D warnings
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_future_void_source_e2e.rs"
                -o "${CMAKE_CURRENT_BINARY_DIR}/oreslang-ready-future-void-e2e"
            DEPENDS tests/native_future_void_source_e2e.rs VERBATIM)
        add_custom_target(oreslang-ready-future-void-e2e-build ALL
            DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/oreslang-ready-future-void-e2e")
        add_test(NAME native_ready_future_void_guest_release
            COMMAND "${CMAKE_CURRENT_BINARY_DIR}/oreslang-ready-future-void-e2e"
                --compiler "$<TARGET_FILE:oreslang-llvmc>"
                --clang "${ORE_CLANG}"
                --root "${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME native_ready_future_void_guest_asan_ubsan
            COMMAND "${CMAKE_CURRENT_BINARY_DIR}/oreslang-ready-future-void-e2e"
                --compiler "$<TARGET_FILE:oreslang-llvmc>"
                --clang "${ORE_CLANG}"
                --root "${CMAKE_CURRENT_SOURCE_DIR}" --sanitize)
        set_tests_properties(native_ready_future_void_guest_release
            native_ready_future_void_guest_asan_ubsan PROPERTIES TIMEOUT 150)
    endif()

    # Trusted host resource reclamation: close in reverse acquisition order only
    # after active turns, outstanding loans and native callbacks quiesce.
    add_executable(oreslang-actor-resource-registry-test
        tests/actor_resource_registry_test.cpp)
    target_include_directories(oreslang-actor-resource-registry-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-actor-resource-registry-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(oreslang-actor-resource-registry-test PRIVATE Threads::Threads)
    add_test(NAME native_actor_resource_registry_quiescence
        COMMAND oreslang-actor-resource-registry-test)
    set_tests_properties(native_actor_resource_registry_quiescence PROPERTIES TIMEOUT 15)

    add_executable(oreslang-actor-resource-registry-identity-test
        tests/actor_resource_registry_identity_test.cpp
        tests/actor_resource_registry_identity_peer.cpp)
    target_include_directories(oreslang-actor-resource-registry-identity-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_features(oreslang-actor-resource-registry-identity-test PRIVATE cxx_std_17)
    target_compile_options(oreslang-actor-resource-registry-identity-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(oreslang-actor-resource-registry-identity-test PRIVATE Threads::Threads)
    add_test(NAME native_actor_resource_registry_identity
        COMMAND oreslang-actor-resource-registry-identity-test)
    set_tests_properties(native_actor_resource_registry_identity PROPERTIES TIMEOUT 30)

    # Counter injection exists only in this isolated single-TU test binary.
    add_executable(oreslang-actor-resource-registry-exhaustion-test
        tests/actor_resource_registry_exhaustion_test.cpp)
    target_include_directories(oreslang-actor-resource-registry-exhaustion-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_features(oreslang-actor-resource-registry-exhaustion-test PRIVATE cxx_std_17)
    target_compile_definitions(oreslang-actor-resource-registry-exhaustion-test PRIVATE
        ORESLANG_NATIVE_TEST_REGISTRY_DOMAIN_EXHAUSTION=1)
    target_compile_options(oreslang-actor-resource-registry-exhaustion-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(oreslang-actor-resource-registry-exhaustion-test PRIVATE Threads::Threads)
    add_test(NAME native_actor_resource_registry_domain_exhaustion
        COMMAND oreslang-actor-resource-registry-exhaustion-test)
    set_tests_properties(native_actor_resource_registry_domain_exhaustion PROPERTIES TIMEOUT 30)

    add_executable(oreslang-actor-group-lifecycle-test tests/actor_group_lifecycle_test.cpp)
    target_include_directories(oreslang-actor-group-lifecycle-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-actor-group-lifecycle-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(oreslang-actor-group-lifecycle-test PRIVATE Threads::Threads)
    add_test(NAME native_actor_group_managed_worker_readiness COMMAND oreslang-actor-group-lifecycle-test)
    set_tests_properties(native_actor_group_managed_worker_readiness PROPERTIES TIMEOUT 15)
    # Cooperation membership is many-to-many, distinct from one lifecycle owner.
    add_executable(oreslang-native-actor-membership-test tests/native_actor_membership_test.cpp)
    target_include_directories(oreslang-native-actor-membership-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-native-actor-membership-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(oreslang-native-actor-membership-test PRIVATE Threads::Threads)
    add_test(NAME native_actor_multi_group_membership COMMAND oreslang-native-actor-membership-test)
    set_tests_properties(native_actor_multi_group_membership PROPERTIES TIMEOUT 20)
    # Host-only CONTROL timer queue: no guest callback, no source async lowering.
    # Expiry tickets are held until exactly-once claim/discard, including
    # accounting for polled-but-not-yet-dispatched timers.
    add_executable(oreslang-native-timer-queue-test tests/native_timer_queue_test.cpp)
    target_include_directories(oreslang-native-timer-queue-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-timer-queue-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-timer-queue-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_monotonic_timer_admission_and_cancel_race
        COMMAND oreslang-native-timer-queue-test)
    set_tests_properties(native_monotonic_timer_admission_and_cancel_race
        PROPERTIES TIMEOUT 20)

    # Pinned native Future<void> lifecycle frame: host callbacks only record
    # completion; resumption is admitted on a parent actor's CONTROL lane.
    # Guest-source await remains compiler-rejected pending typed MIR lowering.
    add_executable(oreslang-actor-lifecycle-await-test tests/actor_lifecycle_await_test.cpp)
    target_include_directories(oreslang-actor-lifecycle-await-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-actor-lifecycle-await-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(oreslang-actor-lifecycle-await-test PRIVATE Threads::Threads)
    add_test(NAME native_pinned_actor_future_control_continuation
        COMMAND oreslang-actor-lifecycle-await-test)
    set_tests_properties(native_pinned_actor_future_control_continuation PROPERTIES TIMEOUT 15)
    add_executable(oreslang-bounded-actor-port-test tests/bounded_actor_port_test.cpp)
    target_include_directories(oreslang-bounded-actor-port-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-bounded-actor-port-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_actor_port_bounded_owned_transport COMMAND oreslang-bounded-actor-port-test)
    # Bounded-port mailbox ledger, physical ownership and supervisor CONTROL
    # transport tests; host primitives only, not guest compiler parity.
    target_link_libraries(oreslang-bounded-actor-port-test PRIVATE Threads::Threads)
    set_tests_properties(native_actor_port_bounded_owned_transport PROPERTIES TIMEOUT 30)

    add_executable(oreslang-native-mailbox-parallel-terminal-test
        tests/native_mailbox_parallel_terminal_test.cpp)
    target_include_directories(oreslang-native-mailbox-parallel-terminal-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-native-mailbox-parallel-terminal-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(oreslang-native-mailbox-parallel-terminal-test PRIVATE Threads::Threads)
    add_test(NAME native_mailbox_parallel_terminal_races
        COMMAND oreslang-native-mailbox-parallel-terminal-test)
    set_tests_properties(native_mailbox_parallel_terminal_races PROPERTIES TIMEOUT 60)

    add_executable(oreslang-native-actor-port-alloc-fault-test
        tests/native_actor_port_allocation_failure_test.cpp)
    target_include_directories(oreslang-native-actor-port-alloc-fault-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-native-actor-port-alloc-fault-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_actor_port_ownership_allocator_rollback
        COMMAND oreslang-native-actor-port-alloc-fault-test)
    set_tests_properties(native_actor_port_ownership_allocator_rollback PROPERTIES TIMEOUT 15)

    add_executable(oreslang-priority-actor-inbox-test tests/priority_actor_inbox_test.cpp)
    target_include_directories(oreslang-priority-actor-inbox-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-priority-actor-inbox-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(oreslang-priority-actor-inbox-test PRIVATE Threads::Threads)
    add_test(NAME native_priority_user_inbox_control_isolation
        COMMAND oreslang-priority-actor-inbox-test)
    set_tests_properties(native_priority_user_inbox_control_isolation PROPERTIES TIMEOUT 30)

    # Incarnation ticket finalization is a CAS winner-takes-all race.
    # Register this target exactly once; stale #894 duplicated it.
    add_executable(oreslang-actor-resource-loan-terminal-race-test
        tests/actor_resource_loan_terminal_race_test.cpp)
    target_include_directories(oreslang-actor-resource-loan-terminal-race-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-actor-resource-loan-terminal-race-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(oreslang-actor-resource-loan-terminal-race-test PRIVATE Threads::Threads)
    add_test(NAME native_actor_resource_loan_terminal_race
        COMMAND oreslang-actor-resource-loan-terminal-race-test)
    set_tests_properties(native_actor_resource_loan_terminal_race PROPERTIES TIMEOUT 60)

    add_executable(oreslang-actor-mailbox-completion-model-test
        tests/actor_mailbox_completion_model_test.cpp)
    target_include_directories(oreslang-actor-mailbox-completion-model-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-actor-mailbox-completion-model-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(oreslang-actor-mailbox-completion-model-test PRIVATE Threads::Threads)
    add_test(NAME native_actor_mailbox_completion_model
        COMMAND oreslang-actor-mailbox-completion-model-test)
    set_tests_properties(native_actor_mailbox_completion_model PROPERTIES TIMEOUT 60)
    add_executable(oreslang-native-ndarray-view-admission-test tests/native_ndarray_view_admission_test.cpp)
    target_include_directories(oreslang-native-ndarray-view-admission-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-native-ndarray-view-admission-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_ndarray_write_view_reference COMMAND oreslang-native-ndarray-view-admission-test)
    # Prove control-plane stop requests honor existing loan-gated native heaps.
    add_executable(oreslang-actor-group-heap-quiescence-test
        tests/actor_group_heap_quiescence_test.cpp
        src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c)
    target_include_directories(oreslang-actor-group-heap-quiescence-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-actor-group-heap-quiescence-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(oreslang-actor-group-heap-quiescence-test PRIVATE Threads::Threads)
    add_test(NAME native_actor_group_heap_loan_quiescence
        COMMAND oreslang-actor-group-heap-quiescence-test)
    set_tests_properties(native_actor_group_heap_loan_quiescence PROPERTIES TIMEOUT 15)
    # Bind the actual native actor heap, resource registry and ActorGroup
    # physical-done acknowledgment into one quiescence-checked host lifecycle.
    add_executable(oreslang-native-actor-retirement-barrier-test
        tests/native_actor_retirement_barrier_test.cpp
        src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c)
    target_include_directories(oreslang-native-actor-retirement-barrier-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-native-actor-retirement-barrier-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(oreslang-native-actor-retirement-barrier-test PRIVATE Threads::Threads)
    target_compile_definitions(oreslang-native-actor-retirement-barrier-test PRIVATE
        ORESLANG_NATIVE_SOURCE_CLEANUP_TEST_PEER=1)
    add_test(NAME native_actor_done_resource_quiescence
        COMMAND oreslang-native-actor-retirement-barrier-test)
    set_tests_properties(native_actor_done_resource_quiescence PROPERTIES TIMEOUT 15)
    # Optional source-unwind receipt: cancellation does not imply physical
    # teardown, and no native allocation can be reclaimed without a receipt.
    add_executable(oreslang-native-source-cleanup-gate-test
        tests/native_source_cleanup_gate_test.cpp)
    target_include_directories(oreslang-native-source-cleanup-gate-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-native-source-cleanup-gate-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(oreslang-native-source-cleanup-gate-test PRIVATE Threads::Threads)
    target_compile_definitions(oreslang-native-source-cleanup-gate-test PRIVATE
        ORESLANG_NATIVE_SOURCE_CLEANUP_TEST_PEER=1)
    add_test(NAME native_source_cleanup_deferred_physical_receipt
        COMMAND oreslang-native-source-cleanup-gate-test)
    set_tests_properties(native_source_cleanup_deferred_physical_receipt PROPERTIES TIMEOUT 30)
    # Validate cleanup admission against a live, claimed continuation-store
    # generation and exact actor incarnation instead of a caller's raw ticket.
    add_executable(oreslang-native-source-cleanup-binding-test
        tests/native_source_cleanup_binding_test.cpp
        src/main/cpp/oreslang/runtime/NativeExecutionContextStore.c
        src/main/cpp/oreslang/runtime/NativeContinuationStore.c)
    target_include_directories(oreslang-native-source-cleanup-binding-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-native-source-cleanup-binding-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(oreslang-native-source-cleanup-binding-test PRIVATE Threads::Threads)
    add_test(NAME native_source_cleanup_continuation_binding
        COMMAND oreslang-native-source-cleanup-binding-test)
    set_tests_properties(native_source_cleanup_continuation_binding PROPERTIES TIMEOUT 30)
    oreslang_contract_test(reject_gpu_kernel unsupported-gpu-kernel.ores --diagnostic "native gpu/quantum boundary unsupported")
    oreslang_contract_test(reject_quantum_kernel unsupported-quantum-kernel.ores --diagnostic "native gpu/quantum boundary unsupported")
    oreslang_contract_test(reject_gpu_expression unsupported-gpu-expression.ores --diagnostic "native gpu/quantum boundary unsupported")
    oreslang_contract_test(reject_cpu_expression unsupported-cpu-expression.ores --diagnostic "native cpu execution block boundary unsupported")
    add_executable(oreslang-data-layout-advisor-test tests/native_data_layout_advisor_test.cpp)
    target_include_directories(oreslang-data-layout-advisor-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_features(oreslang-data-layout-advisor-test PRIVATE cxx_std_17)
    target_compile_options(oreslang-data-layout-advisor-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_data_layout_advisor COMMAND oreslang-data-layout-advisor-test)
    add_executable(oreslang-dop-layout-test tests/dop_layout_test.cpp)
    target_include_directories(oreslang-dop-layout-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-dop-layout-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME dop_layout_checked_geometry COMMAND oreslang-dop-layout-test)

    # Heterogeneous u32/f64/bool/ref32 geometry; 4 distinct AoS/SoA/AoSoA/SoAoS
    # plans. Native host ABI tests only; source-compiled guest parity is separate.
    add_executable(oreslang-dop-heterogeneous-layout-test
        tests/dop_heterogeneous_layout_test.cpp)
    target_include_directories(oreslang-dop-heterogeneous-layout-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_features(oreslang-dop-heterogeneous-layout-test PRIVATE cxx_std_17)
    target_compile_options(oreslang-dop-heterogeneous-layout-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME dop_heterogeneous_four_layout_checked_geometry
        COMMAND oreslang-dop-heterogeneous-layout-test)
    set_tests_properties(dop_heterogeneous_four_layout_checked_geometry
        PROPERTIES TIMEOUT 20)

    # Host-only physical AoS/SoA/AoSoA storage and checked opaque row identity.
    # No guest Ref<T> lowering or actor execution authority is implied.
    add_executable(oreslang-dop-checked-region-test tests/dop_checked_region_test.cpp)
    target_include_directories(oreslang-dop-checked-region-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_features(oreslang-dop-checked-region-test PRIVATE cxx_std_17)
    target_compile_options(oreslang-dop-checked-region-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME native_dop_checked_region_addressing
        COMMAND oreslang-dop-checked-region-test)
    set_tests_properties(native_dop_checked_region_addressing PROPERTIES TIMEOUT 20)

    add_executable(oreslang-dop-inverted-columns-test tests/dop_inverted_columns_test.cpp)
    target_include_directories(oreslang-dop-inverted-columns-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_features(oreslang-dop-inverted-columns-test PRIVATE cxx_std_20)
    target_compile_options(oreslang-dop-inverted-columns-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(oreslang-dop-inverted-columns-test PRIVATE Threads::Threads)
    add_test(NAME dop_inverted_columns_safe_handles COMMAND oreslang-dop-inverted-columns-test)
    # An opt-in *native host* sanitizer build for the same row/column/borrow
    # differential stress tests. This does not certify guest source lowering.
    option(ORESLANG_DOP_ASAN_UBSAN
           "Build additional SoA C++ regression executable under ASan+UBSan" OFF)
    if(ORESLANG_DOP_ASAN_UBSAN)
        if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            message(FATAL_ERROR "SoA ASan/UBSan requires GNU or Clang")
        endif()
        add_executable(oreslang-dop-inverted-columns-sanitized-test
                       tests/dop_inverted_columns_test.cpp)
        target_include_directories(oreslang-dop-inverted-columns-sanitized-test PRIVATE
                                   "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
        target_compile_features(oreslang-dop-inverted-columns-sanitized-test PRIVATE cxx_std_20)
        target_compile_options(oreslang-dop-inverted-columns-sanitized-test PRIVATE
                               -O1 -g -Wall -Wextra -Wpedantic -Werror
                               -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(oreslang-dop-inverted-columns-sanitized-test PRIVATE
                            -fsanitize=address,undefined)
        target_link_libraries(oreslang-dop-inverted-columns-sanitized-test PRIVATE Threads::Threads)
        add_test(NAME dop_inverted_columns_asan_ubsan
                 COMMAND oreslang-dop-inverted-columns-sanitized-test)
        set_tests_properties(dop_inverted_columns_asan_ubsan PROPERTIES
                             TIMEOUT 120
                             ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1:halt_on_error=1;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
    endif()
    add_test(NAME dop_inverted_type_admission
        COMMAND ${CMAKE_COMMAND} -E env "CXX=${CMAKE_CXX_COMPILER}"
                bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/dop_inverted_negative_admission.sh")

    add_executable(oreslang-managed-ref-test tests/managed_ref_test.cpp)
    target_include_directories(oreslang-managed-ref-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-managed-ref-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_managed_reference_width_and_dod_layout
        COMMAND oreslang-managed-ref-test)
    # Nonescaping int loans are supported at checked call boundaries only;
    # a reference still cannot be returned or stored, or cross suspension.
    oreslang_contract_test(reject_rt_borrow unsupported-rt-borrow.ores --diagnostic "borrowed reference may only be passed")
    oreslang_contract_test(native_borrowed_parameter unsupported-borrowed-parameter.ores --ir "load i64")
    oreslang_contract_test(reject_rt_borrow_mut unsupported-rt-borrow-mut.ores --diagnostic "borrowed reference may only be passed")
    oreslang_contract_test(reject_duplicate_mutable_borrow unsupported-double-mut-borrow.ores --diagnostic "duplicate mutable borrow qualifier")
    oreslang_contract_test(reject_rt_ref unsupported-rt-ref.ores --diagnostic "pointer-style rt operation is not valid Oreslang source")
    oreslang_contract_test(reject_rt_deref unsupported-rt-deref.ores --diagnostic "pointer-style rt operation is not valid Oreslang source")
    oreslang_contract_test(reject_rt_unref unsupported-rt-unref.ores --diagnostic "pointer-style rt operation is not valid Oreslang source")
    oreslang_contract_test(reject_rt_raw unsupported-rt-raw.ores --diagnostic "pointer-style rt operation is not valid Oreslang source")
    oreslang_contract_test(reject_pointer_address_of unsupported-address-of.ores --diagnostic "unsupported Oreslang token")
    oreslang_contract_test(reject_pointer_address_of_mut unsupported-address-of-mut.ores --diagnostic "unsupported Oreslang token")
    oreslang_contract_test(reject_pointer_unary_deref unsupported-unary-deref.ores --diagnostic "expected int expression")
    oreslang_contract_test(reject_pointer_type_star unsupported-pointer-type.ores --diagnostic "expected non-keyword identifier")
    oreslang_contract_test(reject_rt_take unsupported-rt-take.ores --diagnostic "unknown parameter")
    oreslang_contract_test(reject_rt_copy unsupported-rt-copy.ores --diagnostic "unknown parameter")
    oreslang_contract_test(reject_rt_share unsupported-rt-share.ores --diagnostic "unknown parameter")
    oreslang_contract_test(reject_unqualified_borrow_call unsupported-borrow-function.ores --diagnostic "expected int expression")
    oreslang_contract_test(reject_suspends unsupported-suspends.ores --diagnostic "native suspends declaration requires")
    oreslang_contract_test(reject_critical unsupported-critical.ores --diagnostic "native critical declaration requires")
    oreslang_contract_test(reject_hyphenated_critical_read unsupported-hyphenated-critical-read.ores --diagnostic "native reader/writer critical declaration requires")
    oreslang_contract_test(reject_hyphenated_critical_read_after_fnc unsupported-hyphenated-critical-read-after-fnc.ores --diagnostic "native reader/writer critical declaration requires")
    oreslang_contract_test(reject_hyphenated_critical_write unsupported-hyphenated-critical-write.ores --diagnostic "native reader/writer critical declaration requires")
    oreslang_contract_test(reject_hyphenated_critical_write_after_fnc unsupported-hyphenated-critical-write-after-fnc.ores --diagnostic "native reader/writer critical declaration requires")
    oreslang_contract_test(reject_hyphenated_actor_critical_read_method unsupported-hyphenated-critical-read-actor-method.ores --diagnostic "native reader/writer critical declaration requires")
    oreslang_contract_test(reject_hyphenated_actor_critical_write_after_fnc unsupported-hyphenated-critical-write-actor-after-fnc.ores --diagnostic "native reader/writer critical declaration requires")
    oreslang_contract_test(reject_hyphenated_actor_critical_read_after_method unsupported-hyphenated-critical-read-actor-after-method.ores --diagnostic "native reader/writer critical declaration requires")
    oreslang_contract_test(reject_hyphenated_actor_critical_read_after_fnc unsupported-hyphenated-critical-read-actor-after-fnc.ores --diagnostic "native reader/writer critical declaration requires")
    oreslang_contract_test(reject_hyphenated_actor_critical_write_before_method unsupported-hyphenated-critical-write-actor-method.ores --diagnostic "native reader/writer critical declaration requires")
    # Whitespace/suffix near-misses must not be interpreted as new RW modes.
    oreslang_contract_test(reject_hyphenated_critical_spaced_as_invalid_name unsupported-hyphenated-critical-spaced.ores --diagnostic "expected non-keyword identifier")
    oreslang_contract_test(reject_hyphenated_critical_suffix_as_invalid_name unsupported-hyphenated-critical-suffix.ores --diagnostic "expected non-keyword identifier")
    oreslang_contract_test(reject_hyphenated_nb_select unsupported-hyphenated-nb-select.ores --diagnostic "native async/channel/select boundary unsupported")
    oreslang_contract_test(reject_hyphenated_transform_pipe unsupported-hyphenated-transform-pipe.ores --diagnostic "native transform-pipe actor requires")
    oreslang_contract_test(hyphenated_transform_minus_pipe hyphenated-transform-minus-pipe.ores)
    oreslang_contract_test(reject_suspend_critical unsupported-suspend-critical.ores --diagnostic "native suspends declaration requires")
    # Native parity substrate for the Graal per-incarnation i64/bool
    # StateAccess lifetime. LLVM guest-source lowering is a separate gate.
    add_executable(oreslang-native-scalar-state-test
        tests/native_scalar_state_test.cpp
        src/main/cpp/oreslang/runtime/ActorArena.c)
    target_include_directories(oreslang-native-scalar-state-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    find_package(Threads REQUIRED)
    target_link_libraries(oreslang-native-scalar-state-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-scalar-state-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_actor_scalar_state_critical_lifetimes
        COMMAND oreslang-native-scalar-state-test)
    # Bounded independent supervisor CONTROL transport (runtime substrate only).
    add_executable(oreslang-supervisor-hierarchy-test tests/supervisor_hierarchy_test.cpp)
    target_include_directories(oreslang-supervisor-hierarchy-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-supervisor-hierarchy-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(oreslang-supervisor-hierarchy-test PRIVATE Threads::Threads)
    add_test(NAME native_supervisor_hierarchy_control COMMAND oreslang-supervisor-hierarchy-test)
    add_executable(oreslang-supervisor-control-turn-test tests/supervisor_control_turn_test.cpp)
    target_include_directories(oreslang-supervisor-control-turn-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-supervisor-control-turn-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(oreslang-supervisor-control-turn-test PRIVATE Threads::Threads)
    add_test(NAME native_supervisor_priority_control COMMAND oreslang-supervisor-control-turn-test)
    add_executable(oreslang-effect-unwind-test tests/effect_unwind_test.cpp)
    target_include_directories(oreslang-effect-unwind-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-effect-unwind-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_typed_exception_unwind COMMAND oreslang-effect-unwind-test)
    # Primitive-only native Assert ABI substrate; imported source execution
    # and compiler-emitted trap remain separately gated.
    add_executable(oreslang-native-assert-scalar-abi-test
        tests/native_assert_scalar_abi_test.cpp)
    target_include_directories(oreslang-native-assert-scalar-abi-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-native-assert-scalar-abi-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_assert_scalar_abi_contract
        COMMAND oreslang-native-assert-scalar-abi-test)
    # Actor-local uncaught routing never admits PANIC into guest callbacks.
    add_executable(oreslang-actor-local-failure-test tests/actor_local_failure_policy_test.cpp)
    target_include_directories(oreslang-actor-local-failure-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-actor-local-failure-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_actor_local_unhandled_panic_bypass COMMAND oreslang-actor-local-failure-test)
    # Authenticated data-only HTTP/SSR response framing, NOT socket I/O.
    add_executable(oreslang-http-response-guard-test tests/http_response_frame_guard_test.cpp)
    target_include_directories(oreslang-http-response-guard-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-http-response-guard-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_http_response_supervisor_guard COMMAND oreslang-http-response-guard-test)
    # Typed host-runtime bridge; native guest actor compiler lowering is still gated.
    add_executable(oreslang-effect-supervision-bridge-test tests/effect_supervision_bridge_test.cpp)
    target_include_directories(oreslang-effect-supervision-bridge-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-effect-supervision-bridge-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(oreslang-effect-supervision-bridge-test PRIVATE Threads::Threads)
    add_test(NAME native_effect_unwind_supervisor_control_bridge
        COMMAND oreslang-effect-supervision-bridge-test)
    # Actual host-runtime path: native cleanup -> CONTROL publication ->
    # supervisor admission, with promoted Panic and original typed origin.
    add_executable(oreslang-effect-supervisor-e2e-test
        tests/effect_supervisor_end_to_end_test.cpp)
    target_include_directories(oreslang-effect-supervisor-e2e-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-effect-supervisor-e2e-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-effect-supervisor-e2e-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_effect_unwind_supervisor_end_to_end
        COMMAND oreslang-effect-supervisor-e2e-test)
    add_executable(oreslang-supervisor-failure-lane-test tests/supervisor_failure_lane_test.cpp)
    target_include_directories(oreslang-supervisor-failure-lane-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-supervisor-failure-lane-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_supervisor_failure_lane COMMAND oreslang-supervisor-failure-lane-test)
    oreslang_contract_test(reject_throw_effect unsupported-throw-effect.ores --diagnostic "native exceptional-control boundary unsupported")
    oreslang_contract_test(reject_raise_effect unsupported-raise-effect.ores --diagnostic "native exceptional-control boundary unsupported")
    oreslang_contract_test(reject_panic_effect unsupported-panic-effect.ores --diagnostic "native exceptional-control boundary unsupported")
    oreslang_contract_test(reject_recover_effect unsupported-recover-effect.ores --diagnostic "native exceptional-control boundary unsupported")

    add_executable(oreslang-critical-gate-test tests/critical_gate_test.cpp)
    target_include_directories(oreslang-critical-gate-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    find_package(Threads REQUIRED)
    target_link_libraries(oreslang-supervisor-failure-lane-test PRIVATE Threads::Threads)
    target_link_libraries(oreslang-critical-gate-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-critical-gate-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_logical_critical_gate COMMAND oreslang-critical-gate-test)
    add_executable(oreslang-function-effects-test tests/function_effects_test.cpp)
    target_include_directories(oreslang-function-effects-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-function-effects-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_function_effect_bitset_contract COMMAND oreslang-function-effects-test)
    add_executable(oreslang-critical-state-effects-test tests/critical_handle_state_contract_test.cpp)
    target_include_directories(oreslang-critical-state-effects-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-critical-state-effects-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_critical_handle_state_using_contract COMMAND oreslang-critical-state-effects-test)
    add_executable(oreslang-critical-state-loan-bridge-test tests/critical_state_loan_bridge_test.cpp)
    target_include_directories(oreslang-critical-state-loan-bridge-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-critical-state-loan-bridge-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_critical_state_loan_runtime_bridge COMMAND oreslang-critical-state-loan-bridge-test)
    add_executable(oreslang-owned-scalar-state-test tests/actor_owned_scalar_state_test.cpp)
    target_include_directories(oreslang-owned-scalar-state-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-owned-scalar-state-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_actor_owned_typed_scalar_state COMMAND oreslang-owned-scalar-state-test)
    # Bounded synchronous state-loan guard; not source-level suspend lowering.
    add_executable(oreslang-actor-state-scope-test tests/actor_state_scope_test.c)
    target_compile_options(oreslang-actor-state-scope-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_actor_state_scope_nested_loan_fencing
        COMMAND oreslang-actor-state-scope-test)
    add_executable(oreslang-logical-mutex-test tests/logical_mutex_test.cpp)
    target_include_directories(oreslang-logical-mutex-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-logical-mutex-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-logical-mutex-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_logical_mutex_fifo_and_cancellation COMMAND oreslang-logical-mutex-test)
    # Host policy only; guest exceptional effects remain rejected.
    add_executable(oreslang-native-exception-policy-test tests/native_exception_policy_test.cpp)
    target_include_directories(oreslang-native-exception-policy-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-native-exception-policy-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_exception_failure_precedence_and_diagnostics
             COMMAND oreslang-native-exception-policy-test)
    # Task borrow/capture regressions: source Task execution remains
    # fail-closed, while the host-only admission primitive proves exact
    # boundary/epoch/domain identity and Send gating independently.
    find_program(ORE_RUSTC NAMES rustc REQUIRED)
    set(ORE_NATIVE_TASK_BORROW_TEST
        "${CMAKE_CURRENT_BINARY_DIR}/native_task_borrow_rejection")
    add_custom_command(
        OUTPUT "${ORE_NATIVE_TASK_BORROW_TEST}"
        COMMAND "${ORE_RUSTC}" --edition=2021 -D warnings
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_task_borrow_rejection.rs"
            -o "${ORE_NATIVE_TASK_BORROW_TEST}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_task_borrow_rejection.rs"
        VERBATIM)
    add_custom_target(oreslang-native-task-borrow-rejection-rust ALL
        DEPENDS "${ORE_NATIVE_TASK_BORROW_TEST}")
    add_test(NAME native_task_borrow_rejection_no_ir
        COMMAND "${ORE_NATIVE_TASK_BORROW_TEST}"
            "$<TARGET_FILE:oreslang-llvmc>")
    set_tests_properties(native_task_borrow_rejection_no_ir PROPERTIES TIMEOUT 60)

    add_executable(oreslang-native-task-capture-admission-test
        tests/native_task_capture_admission_test.cpp)
    target_include_directories(oreslang-native-task-capture-admission-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-native-task-capture-admission-test PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror -UNDEBUG>)
    add_test(NAME native_task_capture_admission_host_only
        COMMAND oreslang-native-task-capture-admission-test)
    set_tests_properties(native_task_capture_admission_host_only
        PROPERTIES TIMEOUT 20)

    add_test(NAME native_reserved_java_keywords
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_reserved_java_keywords.py"
            "$<TARGET_FILE:oreslang-llvmc>")
    oreslang_contract_test(reject_static_select unsupported-static-select.ores --diagnostic "native async/channel/select boundary unsupported")
    oreslang_contract_test(reject_dynamic_select_plan unsupported-dynamic-select-plan.ores --diagnostic "native async/channel/select boundary unsupported")
    # Until typed descriptor ownership and atomic selection land, source
    # SelectPlan factories and persistent case writes must fail closed.
    oreslang_contract_test(reject_select_plan_factory unsupported-select-plan-constructor.ores --diagnostic "native SelectPlan/SelectCase ownership unsupported")
    oreslang_contract_test(reject_reusable_case_write unsupported-select-case-write.ores --diagnostic "native SelectPlan/SelectCase ownership unsupported")
    oreslang_contract_test(reject_undefined undefined-name.ores --diagnostic "unknown parameter")

    # Native i64 is a Copy value: rt copy/rt share are scalar value copies,
    # while rt take invalidates named scalar binding until function exit.
    oreslang_contract_test(native_rt_copy_scalar native-rt-copy-scalar.ores --ir "ret i64 42")
    oreslang_contract_test(native_rt_take_scalar native-rt-take-scalar.ores --ir "ret i64 42")
    oreslang_contract_test(native_rt_share_scalar native-rt-share-scalar.ores --ir "ret i64 42")
    oreslang_contract_test(reject_take_then_reuse native-reject-take-then-reuse.ores --diagnostic "use of explicitly moved int")
    oreslang_contract_test(reject_take_call_arg_overlap native-reject-take-call-args.ores --diagnostic "use of explicitly moved int")
    oreslang_contract_test(reject_double_take native-reject-double-take.ores --diagnostic "use of explicitly moved int")
    oreslang_contract_test(native_loan_read_ir native-borrow-read.ores --ir "call i64 @inspect")
    oreslang_contract_test(native_loan_exclusive_ir native-borrow-exclusive.ores --ir "call i64 @inspect_mut")
    oreslang_contract_test(native_reborrow_shared_ir native-reborrow-shared.ores --ir "call i64 @inspect")
    oreslang_contract_test(native_reborrow_mut_ir native-reborrow-mut.ores --ir "call i64 @inspect_mut")
    oreslang_contract_test(native_reborrow_mut_to_shared_ir native-reborrow-mut-shared.ores --ir "call i64 @inspect")
    add_test(NAME native_ownership_async_boundary_matrix
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_ownership_boundary_matrix.py"
            "$<TARGET_FILE:oreslang-llvmc>")
    # LLVM has no native I/O binding registry or continuation lowering yet.
    # Both spellings must fail closed with no IR until implementation lands.
    oreslang_contract_test(reject_unsupported_native_io_blocking unsupported-native-io-blocking.ores --diagnostic "unknown function or wrong call arity")
    oreslang_contract_test(reject_unsupported_native_io_bare unsupported-native-io-bare.ores --diagnostic "unknown function or wrong call arity")
    oreslang_contract_test(reject_unverified_target_header unsupported-target-header.ores --diagnostic "native target admission unsupported")
    # Neither naming style is permitted without a typed I/O registry, async
    # transport and continuation lowering. No IR may be emitted on failure.
    add_test(NAME native_async_only_io_boundary_matrix
        COMMAND "${ORES_RUST_TEST_HARNESS_BIN}" native-async-io-boundary
            "$<TARGET_FILE:oreslang-llvmc>")
    # General structural shapes are not a scalar-i64 ABI: prove negative
    # admission with a functioning scalar-positive compiler control.
    # Rust-only test executable, built with ordinary CMake ALL builds.
    # If rustc is absent, the CTest deliberately fails rather than claiming coverage.
    find_program(ORE_STRUCTURAL_RUSTC rustc)
    if(ORE_STRUCTURAL_RUSTC)
        add_custom_command(
            OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/oreslang-native-structural-gate"
            COMMAND "${ORE_STRUCTURAL_RUSTC}" --edition=2021
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_structural_shape_failclosed.rs"
                -o "${CMAKE_CURRENT_BINARY_DIR}/oreslang-native-structural-gate"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_structural_shape_failclosed.rs"
            VERBATIM)
        add_custom_target(oreslang-native-structural-gate-build ALL
            DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/oreslang-native-structural-gate")
        add_test(NAME native_structural_shape_failclosed
            COMMAND "${CMAKE_CURRENT_BINARY_DIR}/oreslang-native-structural-gate"
                "$<TARGET_FILE:oreslang-llvmc>")
    else()
        add_test(NAME native_structural_shape_failclosed
            COMMAND "${CMAKE_COMMAND}" -E false)
    endif()
    set_tests_properties(native_structural_shape_failclosed PROPERTIES TIMEOUT 180)
    # Bounded exhaustive shared/exclusive loan and alias partitions. Keep both
    # registrations even without rustc/clang: unavailable tooling must fail.
    if(ORE_STRUCTURAL_RUSTC AND ORE_CLANG)
        add_custom_command(
            OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/oreslang-native-borrow-alias-matrix"
            COMMAND "${ORE_STRUCTURAL_RUSTC}" --edition=2021 -D warnings
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_borrow_alias_matrix.rs"
                -o "${CMAKE_CURRENT_BINARY_DIR}/oreslang-native-borrow-alias-matrix"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_borrow_alias_matrix.rs"
            VERBATIM)
        add_custom_target(oreslang-native-borrow-alias-matrix-build ALL
            DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/oreslang-native-borrow-alias-matrix")
    endif()
    foreach(mode IN ITEMS release sanitizer)
        if(ORE_STRUCTURAL_RUSTC AND ORE_CLANG)
            set(alias_matrix_extra)
            if(mode STREQUAL "sanitizer")
                set(alias_matrix_extra --sanitize)
            endif()
            add_test(NAME native_borrow_alias_matrix_${mode}
                COMMAND "${CMAKE_CURRENT_BINARY_DIR}/oreslang-native-borrow-alias-matrix"
                    --compiler "$<TARGET_FILE:oreslang-llvmc>" --clang "${ORE_CLANG}"
                    ${alias_matrix_extra})
        else()
            add_test(NAME native_borrow_alias_matrix_${mode}
                COMMAND "${CMAKE_COMMAND}" -E false)
        endif()
        set_tests_properties(native_borrow_alias_matrix_${mode} PROPERTIES TIMEOUT 180)
    endforeach()
    if(ORE_STRUCTURAL_RUSTC AND ORE_CLANG)
        add_custom_command(
            OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/oreslang-native-class-string-parity"
            COMMAND "${ORE_STRUCTURAL_RUSTC}" --edition=2021 -D warnings
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_class_string_parity.rs"
                -o "${CMAKE_CURRENT_BINARY_DIR}/oreslang-native-class-string-parity"
            DEPENDS tests/native_class_string_parity.rs
                tests/fixtures/native-class-string/main.ores tests/fixtures/native-class-string/main-1.ores
                tests/fixtures/native-class-string/string-array.ores
                tests/fixtures/native-class-string/path-segment.ores
                tests/fixtures/native-class-string/record-array.ores
                tests/fixtures/native-class-string/owned-path-pattern.ores
                tests/fixtures/native-class-string/record-return.ores
                tests/fixtures/native-class-string/nested-owned-capture.ores
                tests/fixtures/native-class-string/routing-option-capture.ores
                tests/fixtures/native-class-string/routing-path-validation.ores
                tests/fixtures/native-class-string/typed-array-iteration.ores
                tests/fixtures/native-class-string/owned-aggregate-return.ores
            VERBATIM)
        add_custom_target(oreslang-native-class-string-parity-build ALL
            DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/oreslang-native-class-string-parity")
    endif()
    foreach(mode IN ITEMS release sanitizer)
        set(string_parity_extra)
        if(mode STREQUAL "sanitizer")
            set(string_parity_extra --sanitize)
        endif()
        if(ORE_STRUCTURAL_RUSTC AND ORE_CLANG)
            add_test(NAME native_class_string_parity_${mode}
                COMMAND "${CMAKE_CURRENT_BINARY_DIR}/oreslang-native-class-string-parity"
                    --compiler "$<TARGET_FILE:oreslang-llvmc>" --clang "${ORE_CLANG}"
                    --runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeStringRegion.c"
                    --aot-driver "${CMAKE_CURRENT_SOURCE_DIR}/scripts/compile-aot.sh"
                    ${string_parity_extra})
        else()
            add_test(NAME native_class_string_parity_${mode} COMMAND "${CMAKE_COMMAND}" -E false)
        endif()
        set_tests_properties(native_class_string_parity_${mode} PROPERTIES TIMEOUT 180)
    endforeach()
    # Unary + now lowers as an int-only identity under the existing typed
    # scalar checker. Verify guest compilation, real AOT execution, and
    # fail-closed type/range/recursion rejection without Python tooling.
    if(ORE_STRUCTURAL_RUSTC AND ORE_CLANG)
        add_custom_command(
            OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/oreslang-native-unary-plus-parity"
            COMMAND "${ORE_STRUCTURAL_RUSTC}" --edition=2021 -D warnings
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_unary_plus_source_parity.rs"
                -o "${CMAKE_CURRENT_BINARY_DIR}/oreslang-native-unary-plus-parity"
            DEPENDS tests/native_unary_plus_source_parity.rs
            VERBATIM)
        add_custom_target(oreslang-native-unary-plus-parity-build ALL
            DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/oreslang-native-unary-plus-parity")
        add_test(NAME native_unary_plus_source_aot
            COMMAND "${CMAKE_CURRENT_BINARY_DIR}/oreslang-native-unary-plus-parity"
                "$<TARGET_FILE:oreslang-llvmc>" "${ORE_CLANG}")
    else()
        add_test(NAME native_unary_plus_source_aot COMMAND "${CMAKE_COMMAND}" -E false)
    endif()
    set_tests_properties(native_unary_plus_source_aot PROPERTIES TIMEOUT 180)
    add_test(NAME native_strict_nonblocking_io_source_guard
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/strict_nonblocking_io_source_guard.py")
    oreslang_contract_test(pure_user_blocking_suffix pure-user-blocking-suffix.ores --ir "call i64 @pure_math_blocking")
    # The terminal-state gate is a standalone native safety primitive, not
    # an async I/O reactor or source-level await implementation.
    add_executable(oreslang-async-io-operation-test tests/async_io_operation_test.cpp)
    target_include_directories(oreslang-async-io-operation-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    find_package(Threads REQUIRED)
    target_link_libraries(oreslang-async-io-operation-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-async-io-operation-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_async_io_completion_generation_races COMMAND oreslang-async-io-operation-test)
    # Linux-only: nonblocking TCP syscalls + epoll readiness, not std/net lowering.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(oreslang-nonblocking-tcp-linux-test tests/nonblocking_tcp_linux_test.cpp)
        target_include_directories(oreslang-nonblocking-tcp-linux-test PRIVATE src/main/cpp)
        target_compile_options(oreslang-nonblocking-tcp-linux-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
        add_test(NAME native_linux_nonblocking_tcp_readiness COMMAND oreslang-nonblocking-tcp-linux-test)
        set_tests_properties(native_linux_nonblocking_tcp_readiness PROPERTIES TIMEOUT 20)
        # Host-only, bounded network request buffer/loan contract. The test
        # executes actual nonblocking TCP handoff on Linux; it does not prove
        # Oreslang guest actor lowering or HTTP parser parity.
        add_executable(oreslang-request-slot-table-test tests/request_slot_table_test.cpp)
        target_include_directories(oreslang-request-slot-table-test PRIVATE src/main/cpp)
        target_link_libraries(oreslang-request-slot-table-test PRIVATE Threads::Threads)
        target_compile_options(oreslang-request-slot-table-test PRIVATE
            -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
        add_test(NAME native_network_request_slot_borrow_host_only
                 COMMAND oreslang-request-slot-table-test)
        set_tests_properties(native_network_request_slot_borrow_host_only
            PROPERTIES TIMEOUT 30)
        # Host-only, strict-subset HTTP/1 head views over transport read leases.
        add_executable(oreslang-http-request-head-view-test tests/http_request_head_view_test.cpp)
        target_include_directories(oreslang-http-request-head-view-test PRIVATE src/main/cpp)
        target_link_libraries(oreslang-http-request-head-view-test PRIVATE Threads::Threads)
        target_compile_options(oreslang-http-request-head-view-test PRIVATE
            -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
        add_test(NAME native_http_request_head_borrow_noalloc_host_only
                 COMMAND oreslang-http-request-head-view-test)
        set_tests_properties(native_http_request_head_borrow_noalloc_host_only
            PROPERTIES TIMEOUT 30)
        # Incarnation-bound host authority and teardown revocation proof.
        # No guest actor compiler or source mailbox integration is implied.
        add_executable(oreslang-actor-bound-request-table-test
            tests/actor_bound_request_table_test.cpp)
        target_include_directories(oreslang-actor-bound-request-table-test PRIVATE src/main/cpp)
        target_link_libraries(oreslang-actor-bound-request-table-test PRIVATE Threads::Threads)
        target_compile_options(oreslang-actor-bound-request-table-test PRIVATE
            -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
        add_test(NAME native_actor_request_incarnation_revocation_host_only
            COMMAND oreslang-actor-bound-request-table-test)
        set_tests_properties(native_actor_request_incarnation_revocation_host_only
            PROPERTIES TIMEOUT 30)
    endif()
    set_tests_properties(native_async_io_completion_generation_races PROPERTIES TIMEOUT 20)
    # Pre-source-lowering safety gate: prevents stale continuation wakeups,
    # premature actor-heap retirement, duplicate resumes and live-loan drops.
    add_executable(oreslang-pinned-continuation-gate-test tests/pinned_continuation_gate_test.cpp)
    target_include_directories(oreslang-pinned-continuation-gate-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-pinned-continuation-gate-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-pinned-continuation-gate-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_pinned_continuation_loan_cancel_quiescence
             COMMAND oreslang-pinned-continuation-gate-test)
    set_tests_properties(native_pinned_continuation_loan_cancel_quiescence PROPERTIES TIMEOUT 30)
    # Real actor-owned fixed-field loans must survive logical suspension
    # without permitting mutation, cancellation-time drop, or cross-actor alias.
    if(UNIX)
        add_executable(oreslang-pinned-actor-heap-suspend-test
            tests/pinned_actor_heap_suspend_test.cpp
            src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c)
        target_include_directories(oreslang-pinned-actor-heap-suspend-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
        target_compile_options(oreslang-pinned-actor-heap-suspend-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
        target_link_libraries(oreslang-pinned-actor-heap-suspend-test PRIVATE Threads::Threads)
        add_test(NAME native_pinned_actor_heap_suspend_cancel_quiescence
                 COMMAND oreslang-pinned-actor-heap-suspend-test)
        set_tests_properties(native_pinned_actor_heap_suspend_cancel_quiescence PROPERTIES TIMEOUT 30)
    endif()
    # Tuple / Entry are nominal aggregate values in Java/Graal. Until the
    # LLVM frontend provides a checked native product ABI, fail closed before
    # emitting IR for *all* arities, including () and (x,).
    oreslang_contract_test(native_tuple_explicit_literal_failclosed
        native-tuple-explicit-reject.ores --diagnostic "native tuple literal unsupported")
    oreslang_contract_test(native_tuple_compact_literal_failclosed
        native-tuple-compact-reject.ores --diagnostic "native tuple literal unsupported")
    oreslang_contract_test(native_tuple_empty_explicit_failclosed
        native-tuple-empty-explicit-reject.ores --diagnostic "native tuple literal unsupported")
    oreslang_contract_test(native_tuple_empty_compact_failclosed
        native-tuple-empty-compact-reject.ores --diagnostic "native tuple literal unsupported")
    oreslang_contract_test(native_tuple_singleton_explicit_failclosed
        native-tuple-singleton-explicit-reject.ores --diagnostic "native tuple literal unsupported")
    oreslang_contract_test(native_tuple_singleton_compact_failclosed
        native-tuple-singleton-compact-reject.ores --diagnostic "native tuple literal unsupported")
    oreslang_contract_test(native_entry_value_failclosed
        native-entry-reject.ores --diagnostic "native Entry<K,V> unsupported")
    oreslang_contract_test(native_entry_generic_product_failclosed
        native-entry-generic-reject.ores --diagnostic "native Entry<K,V> unsupported")
    oreslang_contract_test(native_user_tuple_call_is_ordinary
        native-user-tuple-call.ores --ir "call i64 @tuple")
    oreslang_contract_test(native_local_entry_identifier_is_ordinary
        native-local-entry-name.ores --ir "define i64 @__oreslang_guest_main")
    if(ORE_CLANG)
        oreslang_contract_test(native_user_tuple_call_executes
            native-user-tuple-call.ores --run 42 --clang "${ORE_CLANG}")
        set_tests_properties(native_user_tuple_call_executes PROPERTIES TIMEOUT 60)
        oreslang_contract_test(native_local_entry_identifier_executes
            native-local-entry-name.ores --run 42 --clang "${ORE_CLANG}")
        set_tests_properties(native_local_entry_identifier_executes PROPERTIES TIMEOUT 60)
    endif()

    oreslang_contract_test(reject_wrong_arity wrong-arity.ores --diagnostic "wrong call arity")
    oreslang_contract_test(native_named_call_unknown_label named-call-unknown-reject.ores
        --diagnostic "unknown named argument")
    oreslang_contract_test(native_named_call_duplicate_slot named-call-duplicate-reject.ores
        --diagnostic "duplicate binding of parameter")
    oreslang_contract_test(native_named_call_positional_after_named named-call-positional-after-named-reject.ores
        --diagnostic "positional argument cannot follow named argument")
    oreslang_contract_test(native_named_call_mut_borrow_conflict named-call-mut-borrow-after-loan-reject.ores
        --diagnostic "field read conflicts with active mutable borrow")
    oreslang_contract_test(reject_duplicates duplicate-function.ores
        --diagnostic "already has arity 0")
    oreslang_contract_test(reject_same_arity_different_names native-name-arity-duplicate.ores
        --diagnostic "already has arity 1")
    # Audit canonical name+arity selection through the ownership checker,
    # not merely LLVM symbol generation. No type/mode scoring or loan escapes.
    oreslang_contract_test(native_name_arity_conflicting_loans native-name-arity-conflicting-loans.ores
        --diagnostic "conflicting active native borrow")
    oreslang_contract_test(native_name_arity_nested_loan_conflict native-name-arity-nested-loan-conflict.ores
        --diagnostic "conflicting active native borrow")
    oreslang_contract_test(native_name_arity_mode_mismatch native-name-arity-borrow-mode-mismatch.ores
        --diagnostic "borrow mode does not match borrowed parameter")
    oreslang_contract_test(native_name_arity_borrow_mode_duplicate native-name-arity-duplicate-borrow-modes.ores
        --diagnostic "already has arity 1")
    oreslang_contract_test(native_name_arity_main_overload_reject native-name-arity-overloaded-main.ores
        --diagnostic "main cannot be overloaded")
    oreslang_contract_test(native_name_arity_gap_reject native-name-arity-gap-reject.ores
        --diagnostic "wrong call arity")
    if(ORE_CLANG)
        oreslang_contract_test(native_compiled_name_arity_overloads
            native-name-arity-overloads.ores
            --ir "pick\$arity0" --run 24 --clang "${ORE_CLANG}")
        set_tests_properties(native_compiled_name_arity_overloads PROPERTIES TIMEOUT 60)
        oreslang_contract_test(native_compiled_name_arity_borrow_abi
            native-name-arity-borrow-execute.ores
            --ir "read\$arity1" --run 41 --clang "${ORE_CLANG}")
        set_tests_properties(native_compiled_name_arity_borrow_abi PROPERTIES TIMEOUT 60)
    endif()
    oreslang_contract_test(reject_duplicate_parameters duplicate-parameter.ores --diagnostic "duplicate parameter")
    oreslang_contract_test(reject_oversized_integer overflow-integer.ores --diagnostic "overflows signed 64-bit")
    oreslang_contract_test(reject_unclosed_comment unclosed-comment.ores --diagnostic "unterminated block comment")

    # Primitive field projections use synchronous whole-object loan
    # admission until per-field provenance can be proven by both checker and
    # LLVM lowering. Rejections must emit zero IR.
    oreslang_contract_test(native_projection_alias_conflict
        class-projection-overlap-reject.ores --diagnostic "conflicting active native borrow")
    oreslang_contract_test(native_projection_immutable_exclusive
        class-projection-immutable-mut-reject.ores --diagnostic "cannot mutably borrow immutable owner")
    oreslang_contract_test(native_projection_moved_field
        class-projection-moved-field-reject.ores --diagnostic "cannot borrow a moved class field")
    oreslang_contract_test(native_projection_private_field
        class-projection-private-reject.ores --diagnostic "private class field")
    oreslang_contract_test(native_projection_type_mismatch
        class-projection-type-mismatch-reject.ores --diagnostic "borrowed argument type mismatch")
    oreslang_contract_test(native_projection_whole_owner_conflict
        class-projection-whole-object-conflict-reject.ores --diagnostic "conflicting active native borrow")

    # Source-level bounded class ownership and per-call shared/exclusive loans.
    # All class records are stack allocated; unverified class shapes and
    # by-value object parameters remain compile-time errors with no IR.
    # Primitive i64 field takes/reinitializations admit checked partial-state
    # ownership. Nontrivial drop/region transfer remains compile-time rejected.
    add_test(NAME native_field_and_region_move_admission
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_class_field_move_admission.py"
            --compiler "$<TARGET_FILE:oreslang-llvmc>")
    set_tests_properties(native_field_and_region_move_admission PROPERTIES TIMEOUT 90)
    add_test(NAME native_colon_class_field_admission
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_colon_class_fields.py"
            --compiler "$<TARGET_FILE:oreslang-llvmc>")
    set_tests_properties(native_colon_class_field_admission PROPERTIES TIMEOUT 90)
    if(ORE_CLANG)
        add_test(NAME native_colon_class_field_execution
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_colon_class_fields.py"
                --compiler "$<TARGET_FILE:oreslang-llvmc>"
                --clang "${ORE_CLANG}")
        set_tests_properties(native_colon_class_field_execution PROPERTIES TIMEOUT 120)
    endif()
    # Executable transfer + exact-once conditional owned-region drop flags.
    # Wasm has no native region ABI; unsafe transfers must reject with no IR.
    if(ORE_CLANG)
        add_test(NAME native_owned_region_conditional_drop_matrix
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/owned_region_conditional_drop_matrix.py"
                --compiler "$<TARGET_FILE:oreslang-llvmc>"
                --clang "${ORE_CLANG}"
                --runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeOwnedI64Region.c"
                --include "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
        set_tests_properties(native_owned_region_conditional_drop_matrix PROPERTIES TIMEOUT 240)
        add_test(NAME native_owned_region_conditional_drop_sanitized
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/owned_region_conditional_drop_matrix.py"
                --compiler "$<TARGET_FILE:oreslang-llvmc>"
                --clang "${ORE_CLANG}"
                --runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeOwnedI64Region.c"
                --include "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp" --sanitize)
        set_tests_properties(native_owned_region_conditional_drop_sanitized PROPERTIES TIMEOUT 240)
        add_test(NAME native_single_cleanup_epilogue
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_single_cleanup_epilogue_matrix.py"
                --compiler "$<TARGET_FILE:oreslang-llvmc>"
                --clang "${ORE_CLANG}"
                --runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeOwnedI64Region.c"
                --include "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
        add_test(NAME native_single_cleanup_epilogue_sanitized
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_single_cleanup_epilogue_matrix.py"
                --compiler "$<TARGET_FILE:oreslang-llvmc>"
                --clang "${ORE_CLANG}"
                --runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeOwnedI64Region.c"
                --include "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp" --sanitize)
        set_tests_properties(native_single_cleanup_epilogue
            native_single_cleanup_epilogue_sanitized PROPERTIES TIMEOUT 240)
    endif()
    oreslang_contract_test(class_reject_byvalue_mutation class-mutation-byvalue-reject.ores --diagnostic "ownership violation")
    # This is the *exact same Git blob* as the Java/Graal conformance
    # source (contracts/v1/semantics/fixtures/ownership.ores), not a
    # purpose-built LLVM-only approximation. A generic parse failure
    # does NOT count as an ownership diagnostic.
    add_test(NAME class_exact_shared_ownership_diagnostic
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract.py"
            --compiler "$<TARGET_FILE:oreslang-llvmc>"
            --fixture "${CMAKE_CURRENT_SOURCE_DIR}/contracts/v1/semantics/fixtures/ownership.ores"
            --diagnostic "ownership violation")

    oreslang_contract_test(class_reject_shared_mutation class-shared-mutation-reject.ores --diagnostic "ownership violation")
    oreslang_contract_test(class_reject_overlapping_borrows class-double-borrow-reject.ores --diagnostic "conflicting active native borrow")
    oreslang_contract_test(class_reject_unknown_field class-unknown-field-reject.ores --diagnostic "unknown class field")
    oreslang_contract_test(class_reject_noncopy_value class-noncopy-return-reject.ores --diagnostic "ownership violation")
    oreslang_contract_test(class_reject_suspend_boundary class-async-borrow-reject.ores --diagnostic "native async/channel/select boundary unsupported")
    oreslang_contract_test(class_admit_ordered_legacy_two_field_layout class-unsupported-layout-reject.ores)
    oreslang_contract_test(class_reject_duplicate_field class-multifield-duplicate-field-reject.ores --diagnostic "duplicate class field")
    oreslang_contract_test(class_reject_nonint_nested_field class-multifield-nonint-field-reject.ores --diagnostic "class field initializer type")
    oreslang_contract_test(class_reject_shared_write_secondary class-multifield-shared-write-reject.ores --diagnostic "ownership violation")
    oreslang_contract_test(class_reject_overlap_across_fields class-multifield-overlap-reject.ores --diagnostic "conflicting active native borrow")
    oreslang_contract_test(class_reject_immutable_exclusive_loan class-immutable-owner-mut-borrow-reject.ores --diagnostic "cannot mutably borrow immutable owner")
    oreslang_contract_test(class_reject_immutable_owner_field_write class-immutable-owner-field-write-reject.ores --diagnostic "class mutation requires let mut owner")
    oreslang_contract_test(class_reject_unknown_secondary_field class-multifield-unknown-field-reject.ores --diagnostic "unknown class field")
    oreslang_contract_test(class_reject_byvalue_multifield_move class-multifield-byvalue-reject.ores --diagnostic "ownership violation")
    oreslang_contract_test(class_reject_taken_field_reuse class-field-use-after-take-reject.ores --diagnostic "ownership violation")
    oreslang_contract_test(class_reject_field_take_on_borrowed_owner class-field-borrowed-take-reject.ores --diagnostic "ownership violation")
    oreslang_contract_test(class_reject_whole_borrow_of_partially_moved_object class-field-borrow-partial-reject.ores --diagnostic "ownership violation")
    oreslang_contract_test(class_reject_actor_send_after_move source-actor-send-after-take-reject.ores --diagnostic "use of explicitly moved int")
    oreslang_contract_test(class_reject_actor_borrowed_payload source-actor-send-borrow-reject.ores --diagnostic "borrowed reference may only be passed")
    oreslang_contract_test(cfg_reject_impure_move_condition cfg-guard-impure-take-reject.ores --diagnostic "use of explicitly moved int")
    oreslang_contract_test(cfg_execute_call_condition cfg-guard-call.ores --ir "call i64 @observe" --run 42 --clang "${ORE_CLANG}")
    oreslang_contract_test(cfg_reject_multifield_class_return cfg-multifield-branch-noncopy-reject.ores --diagnostic "ownership violation")
    oreslang_contract_test(cfg_execute_multifield_call_in_condition cfg-multifield-guard-call.ores --ir "call i64 @observe" --run 42 --clang "${ORE_CLANG}")
    oreslang_contract_test(cfg_reject_missing_fallthrough cfg-guard-missing-final-reject.ores --diagnostic "expected 'return'")
    oreslang_contract_test(cfg_reject_missing_fi cfg-guard-missing-fi-reject.ores --diagnostic "expected 'fi'")
    oreslang_contract_test(cfg_reject_void_guard cfg-guard-void-reject.ores --diagnostic "guarded early return requires a scalar result")




    # Native deterministic ownership is a runtime primitive, not yet source
    # Oreslang object/borrow lowering. Compile and execute it in every LLVM CI.
    find_package(Threads REQUIRED)
    add_executable(oreslang-owned-heap-test tests/owned_heap_test.cpp)
    target_include_directories(oreslang-owned-heap-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-owned-heap-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-owned-heap-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_owned_heap_deterministic_drop
             COMMAND oreslang-owned-heap-test)
    set_tests_properties(native_owned_heap_deterministic_drop PROPERTIES TIMEOUT 30)

    add_executable(oreslang-epoch-arena-test tests/epoch_arena_test.cpp)
    target_include_directories(oreslang-epoch-arena-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-epoch-arena-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_actor_epoch_deterministic_drop
             COMMAND oreslang-epoch-arena-test)
    set_tests_properties(native_actor_epoch_deterministic_drop PROPERTIES TIMEOUT 20)

    oreslang_contract_test(reject_native_private_array_mutation class-private-array-mutation-reject.ores --diagnostic "private class field")
    oreslang_contract_test(reject_native_private_filled_commit class-private-filled-array-commit-reject.ores --diagnostic "private class field")

    if(ORE_CLANG)
        oreslang_contract_test(native_public_class_field_read class-public-field-read-native.ores --run 42 --clang "${ORE_CLANG}" --no-native-abi)
        oreslang_contract_test(class_exec_mutable_borrow class-mutable-borrow-native.ores --run 42 --clang "${ORE_CLANG}")

        oreslang_contract_test(native_projection_shared_clang
            class-projection-shared-native.ores --run 42 --clang "${ORE_CLANG}" --no-native-abi)
        oreslang_contract_test(native_projection_exclusive_clang
            class-projection-exclusive-native.ores --run 42 --clang "${ORE_CLANG}" --no-native-abi)
        oreslang_contract_test(native_projection_shared_reborrow_clang
            class-projection-shared-reborrow-native.ores --run 42 --clang "${ORE_CLANG}" --no-native-abi)
        oreslang_contract_test(native_projection_two_shared_clang
            class-projection-two-shared-native.ores --run 42 --clang "${ORE_CLANG}" --no-native-abi)

        add_test(NAME native_projection_borrow_compiler_to_clang_matrix
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_projected_field_borrow_matrix.py"
                --compiler "$<TARGET_FILE:oreslang-llvmc>"
                --clang "${ORE_CLANG}"
                --fixtures "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
        set_tests_properties(native_projection_borrow_compiler_to_clang_matrix
            PROPERTIES TIMEOUT 120)

        oreslang_contract_test(class_exec_shared_read class-shared-read-native.ores --run 42 --clang "${ORE_CLANG}")
        oreslang_contract_test(class_exec_mut_to_shared_reborrow class-reborrow-native.ores --run 42 --clang "${ORE_CLANG}")
        oreslang_contract_test(class_exec_owned_stack_field class-owned-field-native.ores --run 42 --clang "${ORE_CLANG}")
        oreslang_contract_test(class_exec_field_take_reinitialization class-field-take-reinit-native.ores --run 42 --clang "${ORE_CLANG}" --no-native-abi)
        oreslang_contract_test(class_exec_cfg_branch_field_take class-field-take-branch-native.ores --run 42 --clang "${ORE_CLANG}" --no-native-abi)
        oreslang_contract_test(class_exec_two_field_mutator class-multifield-mutator.ores --run 42 --clang "${ORE_CLANG}" --no-native-abi)
        oreslang_contract_test(cfg_exec_mutated_multifield_early_true cfg-multifield-mutator-early-true.ores --run 42 --clang "${ORE_CLANG}" --no-native-abi)
        oreslang_contract_test(cfg_exec_mutated_multifield_early_false cfg-multifield-mutator-early-false.ores --run 42 --clang "${ORE_CLANG}" --no-native-abi)
        oreslang_contract_test(class_exec_three_field_mutator class-multifield-independent-write.ores --run 42 --clang "${ORE_CLANG}" --no-native-abi)
        # Exact same .ores source is exercised by GraalVM ParityScalarEcsGuestTest.
        # This runs actual emitted LLVM IR through Clang; NOT four-layout lowering.
        oreslang_contract_test(parity_source_scalar_ecs_guest
            ecs-scalar-guest-baseline.ores --run 42 --clang "${ORE_CLANG}" --no-native-abi)
        oreslang_contract_test(cfg_exec_multifield_guard_taken cfg-multifield-guard-true.ores --run 42 --clang "${ORE_CLANG}" --no-native-abi --ir "guard.return")
        oreslang_contract_test(cfg_exec_multifield_guard_fallthrough cfg-multifield-guard-false.ores --run 42 --clang "${ORE_CLANG}" --no-native-abi --ir "guard.next")
        oreslang_contract_test(cfg_exec_multifield_guard_chain cfg-multifield-guard-chain.ores --run 42 --clang "${ORE_CLANG}" --no-native-abi)
        oreslang_contract_test(cfg_exec_scalar_branch_move cfg-scalar-branch-move.ores --run 42 --clang "${ORE_CLANG}" --no-native-abi)
        oreslang_contract_test(cfg_exec_experimental_owned_early_drop cfg-heap-owned-early-drop.ores --experimental-owned-int --run 42 --clang "${ORE_CLANG}" --native-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeOwnedI64Region.c" --ir "guard.return")
        add_test(NAME cfg_guarded_drop_all_native_exit_edges
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/guarded_cfg_drop_matrix.py"
                --compiler "$<TARGET_FILE:oreslang-llvmc>"
                --clang "${ORE_CLANG}"
                --native-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeOwnedI64Region.c")
        set_tests_properties(cfg_guarded_drop_all_native_exit_edges PROPERTIES TIMEOUT 240)
        oreslang_contract_test(class_exec_two_field_shared_read class-multifield-shared-read.ores --run 42 --clang "${ORE_CLANG}" --no-native-abi)
        add_test(NAME class_native_1_to_16_field_layout_matrix
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/class_multifield_layout_matrix.py"
                --compiler "$<TARGET_FILE:oreslang-llvmc>"
                --clang "${ORE_CLANG}")
        set_tests_properties(class_native_1_to_16_field_layout_matrix PROPERTIES TIMEOUT 240)
        # Free functions must never project private class state, whether
        # by read, write, take or a nested Array<int> member operation.
        oreslang_contract_test(reject_native_private_field_read
            class-private-read-reject.ores --diagnostic "private class field")
        oreslang_contract_test(reject_native_private_field_write
            class-private-write-reject.ores --diagnostic "private class field")
        oreslang_contract_test(reject_native_private_field_take
            class-private-take-reject.ores --diagnostic "private class field")
        oreslang_contract_test(reject_native_private_array_size
            class-private-array-size-reject.ores --diagnostic "private class field")
        oreslang_contract_test(reject_native_final_field_write
            class-final-write-reject.ores --diagnostic "cannot reassign final class field")
        oreslang_contract_test(reject_native_final_field_take
            class-final-take-reject.ores --diagnostic "cannot take final class field")
        oreslang_contract_test(native_class_final_read class-final-read-native.ores --run 42 --clang "${ORE_CLANG}" --no-native-abi)
        oreslang_contract_test(native_class_colon_write class-colon-rebindable-native.ores --run 42 --clang "${ORE_CLANG}" --no-native-abi)
        oreslang_contract_test(native_class_val_final_mut_read class-val-read-native.ores --run 42 --clang "${ORE_CLANG}" --no-native-abi)
        oreslang_contract_test(reject_native_const_alias_write class-final-alias-write-reject.ores --diagnostic "cannot reassign final class field")
        oreslang_contract_test(reject_native_final_uninitialized class-final-uninitialized-reject.ores --diagnostic "native class constructor argument arity mismatch")
        oreslang_contract_test(reject_native_val_alias_write class-val-alias-write-reject.ores --diagnostic "cannot reassign final class field")
        oreslang_contract_test(reject_native_final_mut_write class-final-mut-write-reject.ores --diagnostic "cannot reassign final class field")
        oreslang_contract_test(reject_native_const_alias_take class-const-take-reject.ores --diagnostic "cannot take final class field")
        # Source-identical Java/GraalVM constructor parity and fail-closed
        # malformed/effectful/owned-input negative controls.
        oreslang_contract_test(native_scalar_constructor_arithmetic
            class-scalar-constructor-arithmetic-native.ores --run 40 --clang "${ORE_CLANG}" --no-native-abi)
        oreslang_contract_test(native_scalar_constructor_simple
            class-constructor-init-unsupported.ores --run 42 --clang "${ORE_CLANG}" --no-native-abi)
        oreslang_contract_test(native_scalar_constructor_public
            class-public-constructor-init-unsupported.ores --run 42 --clang "${ORE_CLANG}" --no-native-abi)
        oreslang_contract_test(reject_scalar_constructor_arity
            class-scalar-constructor-arity-reject.ores --diagnostic "native class constructor argument arity mismatch")
        oreslang_contract_test(reject_scalar_constructor_effect_call
            class-scalar-constructor-call-reject.ores --diagnostic "native class field initializer requires pure scalar")
        oreslang_contract_test(reject_scalar_constructor_unknown
            class-scalar-constructor-unknown-reject.ores --diagnostic "unknown parameter 'other'")
        oreslang_contract_test(reject_scalar_constructor_duplicate
            class-scalar-constructor-duplicate-reject.ores --diagnostic "duplicate native constructor parameter")
        oreslang_contract_test(reject_scalar_constructor_owned_array
            class-scalar-constructor-owned-reject.ores --diagnostic "native owned Array<int> constructor requires checked failure cleanup")
        oreslang_contract_test(reject_scalar_constructor_effect_arg
            class-scalar-constructor-args-effect-reject.ores --diagnostic "native scalar constructor argument requires pure integer expression")
        oreslang_contract_test(reject_scalar_constructor_owned_int_arg
            class-scalar-constructor-owned-arg-reject.ores --experimental-owned-int --diagnostic "native scalar constructor arguments require plain stack scalar values")
        oreslang_contract_test(reject_scalar_constructor_effect_body
            class-scalar-constructor-effectful-body-reject.ores --diagnostic "native scalar constructor body must be empty or contain only")
        oreslang_contract_test(native_exec_constant constant-main.ores --run 42 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_exec_forward_call forward-call-main.ores --run 42 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_exec_arithmetic return-arithmetic.ores --run 28 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_exec_owned_stack_class owned-stack-class.ores --run 13 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_exec_negative negative-literal.ores --run 42 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_exec_lexical_local owned-lexical-main.ores --run 42 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_exec_owned_identifier owned-identifier-main.ores --no-native-abi --run 42 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_exec_owned_region owned-native-region-main.ores --experimental-owned-int --run 42 --clang "${ORE_CLANG}" --native-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeOwnedI64Region.c")
        oreslang_contract_test(native_exec_owned_region_nested owned-native-region-call.ores --experimental-owned-int --run 42 --clang "${ORE_CLANG}" --native-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeOwnedI64Region.c")
        oreslang_contract_test(native_exec_lexical_shared_loan owned-local-shared-borrow.ores --run 42 --clang "${ORE_CLANG}")
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
    # Host-only native access evidence, not a language-wide race-proof.
    add_executable(oreslang-actor-heap-access-trace-test
        tests/actor_heap_access_trace_test.c
        src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c)
    set_target_properties(oreslang-actor-heap-access-trace-test PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_include_directories(oreslang-actor-heap-access-trace-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-actor-heap-access-trace-test PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror -UNDEBUG>)
    add_test(NAME native_actor_i64_access_evidence_host_only
        COMMAND oreslang-actor-heap-access-trace-test)
    set_tests_properties(native_actor_i64_access_evidence_host_only
        PROPERTIES TIMEOUT 20)

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
        src/main/cpp/oreslang/runtime/NativeActorFuture.c
        src/main/cpp/oreslang/runtime/NativeOresVM.c
        src/main/cpp/oreslang/runtime/NativeActorFuture.c
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
        src/main/cpp/oreslang/runtime/NativeActorFuture.c
        src/main/cpp/oreslang/runtime/NativeOresVM.c
        src/main/cpp/oreslang/runtime/NativeActorFuture.c
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
        src/main/cpp/oreslang/runtime/NativeActorFuture.c
        src/main/cpp/oreslang/runtime/NativeOresVM.c
        src/main/cpp/oreslang/runtime/NativeActorFuture.c
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
        src/main/cpp/oreslang/runtime/NativeActorFuture.c
        src/main/cpp/oreslang/runtime/NativeOresVM.c
        src/main/cpp/oreslang/runtime/NativeActorFuture.c
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
        src/main/cpp/oreslang/runtime/NativeActorFuture.c
        src/main/cpp/oreslang/runtime/NativeOresVM.c
        src/main/cpp/oreslang/runtime/NativeActorFuture.c
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

    # Characterize a distinct-second-child CONTROL overflow while DATA is
    # saturated. Does NOT by itself prove durable failure retry at the owner.
    add_executable(oreslang-native-supervisor-control-overflow-test
        tests/native_supervisor_control_overflow_adversarial_test.c
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c)
    target_include_directories(oreslang-native-supervisor-control-overflow-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-supervisor-control-overflow-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-supervisor-control-overflow-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_supervisor_control_overflow_characterization
        COMMAND oreslang-native-supervisor-control-overflow-test)
    set_tests_properties(native_supervisor_control_overflow_characterization PROPERTIES TIMEOUT 20)

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
    # Scheduler-owned producer continuation wake escrow: independent from
    # supervisor CONTROL and USER mail capacity. Native host substrate only.
    add_executable(oreslang-native-continuation-wake-escrow-test
        tests/native_continuation_wake_escrow_test.c
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c)
    target_include_directories(oreslang-native-continuation-wake-escrow-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-native-continuation-wake-escrow-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(oreslang-native-continuation-wake-escrow-test PRIVATE
        Threads::Threads)
    add_test(NAME native_continuation_wake_escrow_stop_cancel_aba
        COMMAND oreslang-native-continuation-wake-escrow-test)
    set_tests_properties(native_continuation_wake_escrow_stop_cancel_aba
        PROPERTIES TIMEOUT 45)

    # Reservation must fail cleanly if pthread_create cannot allocate a
    # future wake carrier. The macro is absent from production builds.
    add_executable(oreslang-native-wake-carrier-failure-test
        tests/native_continuation_wake_carrier_failure_test.c
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c)
    target_include_directories(oreslang-native-wake-carrier-failure-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_definitions(oreslang-native-wake-carrier-failure-test PRIVATE
        ORESLANG_NATIVE_TEST_PTHREAD_CREATE_FAILURE=1
        ORESLANG_NATIVE_TEST_EXECUTION_EXHAUSTION=1)
    target_compile_options(oreslang-native-wake-carrier-failure-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(oreslang-native-wake-carrier-failure-test PRIVATE
        Threads::Threads)
    add_test(NAME native_continuation_wake_carrier_failure_fence
        COMMAND oreslang-native-wake-carrier-failure-test)
    set_tests_properties(native_continuation_wake_carrier_failure_fence
        PROPERTIES TIMEOUT 30)

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

    # SCHED_FIFO is a bootstrap-time qualification step. Unprivileged hosts may
    # reject the carrier creation, but no actor work may be admitted first.
    add_executable(oreslang-native-realtime-policy-test
        tests/native_actor_realtime_policy_test.c
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c)
    target_include_directories(oreslang-native-realtime-policy-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-realtime-policy-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-realtime-policy-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_actor_realtime_policy_fail_closed
        COMMAND oreslang-native-realtime-policy-test)
    set_tests_properties(native_actor_realtime_policy_fail_closed PROPERTIES TIMEOUT 30)

    add_executable(oreslang-linux-realtime-environment-test
        tests/linux_realtime_environment_test.c
        src/main/cpp/oreslang/runtime/LinuxRealtimeEnvironment.c)
    target_include_directories(oreslang-linux-realtime-environment-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-linux-realtime-environment-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME linux_realtime_memory_lock_fail_closed
        COMMAND oreslang-linux-realtime-environment-test)
    set_tests_properties(linux_realtime_memory_lock_fail_closed PROPERTIES TIMEOUT 20)

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

    # Logical execution ids are reserved at admission. The final UINT64_MAX id
    # may execute exactly once; additional work must fail closed before dispatch.
    add_executable(oreslang-native-execution-context-exhaustion-test
        tests/native_execution_context_exhaustion_test.c
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c)
    target_include_directories(oreslang-native-execution-context-exhaustion-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-execution-context-exhaustion-test PRIVATE Threads::Threads)
    target_compile_definitions(oreslang-native-execution-context-exhaustion-test PRIVATE
        ORESLANG_NATIVE_TEST_EXECUTION_EXHAUSTION=1)
    target_compile_options(oreslang-native-execution-context-exhaustion-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_execution_context_exhaustion_admission
        COMMAND oreslang-native-execution-context-exhaustion-test)
    set_tests_properties(native_execution_context_exhaustion_admission PROPERTIES TIMEOUT 30)

    # Generation-checked AoSoA logical execution/continuation stores. The
    # fixture suspends, destroys the worker C stack, resumes on fresh pthreads,
    # and reconstructs execution + continuation + actor/supervisor ancestry.
    add_executable(oreslang-native-execution-continuation-store-test
        tests/native_execution_continuation_store_test.c
        src/main/cpp/oreslang/runtime/NativeExecutionContextStore.c
        src/main/cpp/oreslang/runtime/NativeContinuationStore.c
        src/main/cpp/oreslang/runtime/NativeAwaitTraceBridge.c
        src/main/cpp/oreslang/runtime/NativeLogicalTrace.c)
    target_include_directories(oreslang-native-execution-continuation-store-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-execution-continuation-store-test PRIVATE
        Threads::Threads)
    target_compile_options(oreslang-native-execution-continuation-store-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_execution_continuation_aosoa_trace
        COMMAND oreslang-native-execution-continuation-store-test)
    set_tests_properties(native_execution_continuation_aosoa_trace
        PROPERTIES TIMEOUT 30)

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

if(BUILD_TESTING AND ORE_CLANG)
    add_test(NAME refreshed_i64_module_contract_execution
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/module_contract_refresh.py"
        "$<TARGET_FILE:oreslang-llvmc>" "${ORE_CLANG}" "${CMAKE_CURRENT_SOURCE_DIR}")
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
        COMMAND "${ORES_RUST_TEST_HARNESS_BIN}" actor-persistent-fields
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

if(BUILD_TESTING AND ORE_CLANG)
    add_test(NAME native_aot_driver
        COMMAND "${ORES_RUST_TEST_HARNESS_BIN}" aot-driver
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

# HOST-ONLY token bucket arithmetic and concurrent reservation prototype.
# Does NOT enforce guest sends or prove native UNTRUSTED isolation.
if(BUILD_TESTING)
    add_executable(oreslang-trusted-outbound-token-bucket-host-test
        tests/trusted_outbound_token_bucket_test.cpp)
    target_include_directories(oreslang-trusted-outbound-token-bucket-host-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-trusted-outbound-token-bucket-host-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-trusted-outbound-token-bucket-host-test PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>)
    add_test(NAME trusted_outbound_token_bucket_host_only
        COMMAND oreslang-trusted-outbound-token-bucket-host-test)
    set_tests_properties(trusted_outbound_token_bucket_host_only PROPERTIES TIMEOUT 15)
endif()

# HOST-ONLY UNTRUSTED HTTP/TCP budget helper test. No guest network admission
# path is wired here and no in-process native security claim is made.
if(BUILD_TESTING)
    add_executable(oreslang-untrusted-network-quota-host-test
        tests/trusted_untrusted_network_quota_test.cpp)
    target_include_directories(oreslang-untrusted-network-quota-host-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-untrusted-network-quota-host-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-untrusted-network-quota-host-test PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>)
    add_test(NAME untrusted_network_quota_host_only
        COMMAND oreslang-untrusted-network-quota-host-test)
    set_tests_properties(untrusted_network_quota_host_only PROPERTIES TIMEOUT 20)
endif()

# Hierarchical host-only HTTP/TCP quota: child, supervisor, tenant.
# This does not admit arbitrary native UNTRUSTED guest source.
if(BUILD_TESTING)
    add_executable(oreslang-trusted-hierarchical-network-gate-host-test
        tests/trusted_hierarchical_network_gate_test.cpp)
    target_include_directories(oreslang-trusted-hierarchical-network-gate-host-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-trusted-hierarchical-network-gate-host-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-trusted-hierarchical-network-gate-host-test PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>)
    add_test(NAME trusted_hierarchical_network_gate_host_only
        COMMAND oreslang-trusted-hierarchical-network-gate-host-test)
    set_tests_properties(trusted_hierarchical_network_gate_host_only PROPERTIES TIMEOUT 20)
endif()


# Standalone native IPC receipt reference; not live cross-process actor transport.
if(BUILD_TESTING)
    add_executable(oreslang-native-ipc-receipt-ledger-test
        tests/native_ipc_receipt_ledger_test.cpp)
    target_include_directories(oreslang-native-ipc-receipt-ledger-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    find_package(Threads REQUIRED)
    target_link_libraries(oreslang-native-ipc-receipt-ledger-test PRIVATE Threads::Threads)
    target_compile_features(oreslang-native-ipc-receipt-ledger-test PRIVATE cxx_std_20)
    target_compile_options(oreslang-native-ipc-receipt-ledger-test PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>)
    add_test(NAME native_ipc_receipt_correlation_host_reference
        COMMAND oreslang-native-ipc-receipt-ledger-test)
    set_tests_properties(native_ipc_receipt_correlation_host_reference
        PROPERTIES TIMEOUT 10)
endif()


# Advisory v0 only: compiler-backed literal-main facts, no actor/termination claims.
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME llvm_static_facts_v0_negative
        COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/static_facts_v0.py" --self-test)
    add_test(NAME llvm_static_facts_v0_real_compiler
        COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/static_facts_v0.py"
        --compiler "$<TARGET_FILE:oreslang-llvmc>"
        --source "${CMAKE_CURRENT_SOURCE_DIR}/contracts/v1/fixtures/literal-42.ores")
    set_tests_properties(llvm_static_facts_v0_negative llvm_static_facts_v0_real_compiler
        PROPERTIES TIMEOUT 30)
endif()

# Source-compiled JVM/native declaration parity slice. This deliberately tests
# native scalar aliases only; generic and trait/interface forms MUST fail closed.
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    find_program(ORE_FORWARD_TYPE_CLANG
        NAMES clang-18 clang-19 clang clang-17 clang-16 clang-15 clang-14 REQUIRED)
    add_test(NAME native_declare_type_scalar_source_parity
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_declare_type_parity.py"
            --compiler "$<TARGET_FILE:oreslang-llvmc>"
            --clang "${ORE_FORWARD_TYPE_CLANG}")
    set_tests_properties(native_declare_type_scalar_source_parity PROPERTIES TIMEOUT 120)
endif()

# Host-only native proxy slab parity with Java ActorProxyRequestSlab.
# The borrowed arena span requires caller-owned actor-turn fencing; this is
# not guest Oreslang lowering or a live HTTP/CONNECT transport.
if(BUILD_TESTING)
    add_executable(oreslang-native-proxy-request-slab-test
        tests/native_proxy_request_slab_test.cpp
        src/main/cpp/oreslang/runtime/ActorArena.c)
    target_include_directories(oreslang-native-proxy-request-slab-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_features(oreslang-native-proxy-request-slab-test PRIVATE cxx_std_20)
    target_compile_options(oreslang-native-proxy-request-slab-test PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>)
    add_test(NAME native_proxy_request_slab_host_only
        COMMAND oreslang-native-proxy-request-slab-test)
    set_tests_properties(native_proxy_request_slab_host_only
        PROPERTIES TIMEOUT 20)
endif()

# Full native actor scheduler proof: exact-generation ownership and paired
# actor/VM quota, with backpressure, receipt reuse and foreign-turn rejection.
# C scheduler source must be compiled as C; the actor test uses C++20.
if(BUILD_TESTING)
    add_executable(oreslang-native-actor-proxy-slab-owner-test
        tests/native_actor_proxy_request_slab_owner_test.cpp
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c
        src/main/cpp/oreslang/runtime/ActorArena.c)
    target_include_directories(oreslang-native-actor-proxy-slab-owner-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-actor-proxy-slab-owner-test PRIVATE Threads::Threads)
    target_compile_features(oreslang-native-actor-proxy-slab-owner-test PRIVATE cxx_std_20)
    target_compile_options(oreslang-native-actor-proxy-slab-owner-test PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror -UNDEBUG>)
    add_test(NAME native_actor_proxy_slab_generation_and_quota
        COMMAND oreslang-native-actor-proxy-slab-owner-test)
    set_tests_properties(native_actor_proxy_slab_generation_and_quota
        PROPERTIES TIMEOUT 30)
endif()
# Host-only fixed-size, lock-free physical turn credits. This does NOT enable
# concurrent source actors or relax the existing NativeExecutionAuthority.
if(BUILD_TESTING)
    find_package(Threads REQUIRED)
    add_executable(oreslang-native-actor-turn-credits-test
        tests/native_actor_turn_credits_test.cpp)
    target_include_directories(oreslang-native-actor-turn-credits-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-actor-turn-credits-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-actor-turn-credits-test PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>)
    add_test(NAME native_actor_physical_turn_credit_host_only
        COMMAND oreslang-native-actor-turn-credits-test)
    set_tests_properties(native_actor_physical_turn_credit_host_only
        PROPERTIES TIMEOUT 25)
endif()

# Dedicated Linux epoll reactor thread, bounded completion ring and generation
# fencing. Host-only substrate; no compiler source-level await linkage yet.
if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Threads REQUIRED)
    add_executable(oreslang-native-epoll-reactor-test tests/native_epoll_reactor_test.cpp)
    target_include_directories(oreslang-native-epoll-reactor-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-epoll-reactor-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-epoll-reactor-test PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>)
    add_test(NAME native_epoll_reactor_generation_completion
        COMMAND oreslang-native-epoll-reactor-test)
    set_tests_properties(native_epoll_reactor_generation_completion PROPERTIES TIMEOUT 20)

    # Separate translation unit interposes eventfd only for controlled
    # constructor failure-injection, checking both errno and FD reclamation.
    add_executable(oreslang-native-epoll-reactor-bootstrap-failure-test
        tests/native_epoll_reactor_bootstrap_failure_test.cpp)
    target_include_directories(oreslang-native-epoll-reactor-bootstrap-failure-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-epoll-reactor-bootstrap-failure-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-epoll-reactor-bootstrap-failure-test PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>)
    add_test(NAME native_epoll_reactor_eventfd_bootstrap_failclosed
        COMMAND oreslang-native-epoll-reactor-bootstrap-failure-test)
    set_tests_properties(native_epoll_reactor_eventfd_bootstrap_failclosed PROPERTIES TIMEOUT 20)

    add_executable(oreslang-native-epoll-reactor-manager-test
        tests/native_epoll_reactor_manager_test.cpp)
    target_include_directories(oreslang-native-epoll-reactor-manager-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-epoll-reactor-manager-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-epoll-reactor-manager-test PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>)
    add_test(NAME native_epoll_reactor_manager_rt_preallocated
        COMMAND oreslang-native-epoll-reactor-manager-test)
    set_tests_properties(native_epoll_reactor_manager_rt_preallocated PROPERTIES TIMEOUT 20)
endif()

# Native RT profile has no pthread creation after VM preparation. Existing
# throughput open remains metadata-only until first runnable admission.
if(BUILD_TESTING)
    find_package(Threads REQUIRED)
    add_executable(oreslang-native-vm-realtime-prestart-test
        tests/native_vm_realtime_prestart_test.c
        src/main/cpp/oreslang/runtime/NativeOresVM.c
        src/main/cpp/oreslang/runtime/NativeActorFuture.c
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c)
    target_include_directories(oreslang-native-vm-realtime-prestart-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-vm-realtime-prestart-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-vm-realtime-prestart-test PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>)
    add_test(NAME native_vm_realtime_preallocated_startup
        COMMAND oreslang-native-vm-realtime-prestart-test)
    set_tests_properties(native_vm_realtime_preallocated_startup PROPERTIES TIMEOUT 25)
endif()

# Narrow C ABI for the C++ Linux reactor domain. Validates C compiler + C++
# link, typed generation tickets, readiness/completion, and RT startup mode.
if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Threads REQUIRED)
    add_executable(oreslang-native-epoll-reactor-abi-test
        tests/native_epoll_reactor_abi_test.c
        src/main/cpp/oreslang/runtime/NativeEpollReactorABI.cpp)
    target_include_directories(oreslang-native-epoll-reactor-abi-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    set_target_properties(oreslang-native-epoll-reactor-abi-test PROPERTIES LINKER_LANGUAGE CXX)
    target_link_libraries(oreslang-native-epoll-reactor-abi-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-epoll-reactor-abi-test PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic -Werror>
        $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Wpedantic -Werror>)
    add_test(NAME native_epoll_reactor_c_abi
        COMMAND oreslang-native-epoll-reactor-abi-test)
    set_tests_properties(native_epoll_reactor_c_abi PROPERTIES TIMEOUT 25)
endif()

# Host VM ↔ epoll integration. C-only actor/scheduler paths remain linkable
# without NIO C++ objects; only this opt-in Linux target links the bridge.
if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Threads REQUIRED)
    add_executable(oreslang-native-vm-nio-lifecycle-test
        tests/native_vm_nio_lifecycle_test.c
        src/main/cpp/oreslang/runtime/NativeOresVM.c
        src/main/cpp/oreslang/runtime/NativeActorFuture.c
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c
        src/main/cpp/oreslang/runtime/NativeOresVMNio.cpp
        src/main/cpp/oreslang/runtime/NativeEpollReactorABI.cpp)
    target_include_directories(oreslang-native-vm-nio-lifecycle-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    set_target_properties(oreslang-native-vm-nio-lifecycle-test PROPERTIES
        LINKER_LANGUAGE CXX)
    target_compile_features(oreslang-native-vm-nio-lifecycle-test PRIVATE c_std_11 cxx_std_17)
    target_link_libraries(oreslang-native-vm-nio-lifecycle-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-vm-nio-lifecycle-test PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic -Werror>
        $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Wpedantic -Werror>)
    add_test(NAME native_vm_nio_lifecycle_and_rt_prestart
        COMMAND oreslang-native-vm-nio-lifecycle-test)
    set_tests_properties(native_vm_nio_lifecycle_and_rt_prestart
        PROPERTIES TIMEOUT 30)
endif()

# This compiles the REAL native frontend parser and ownership checker,
# independent of LLVM IR emission. LOW is executable; MEDIUM/HIGH fail closed
# before codegen until safe per-turn actor frames and state loans exist.
if(BUILD_TESTING)
    add_executable(oreslang-native-actor-max-threads-admission-test
        tests/actor_max_threads_source_admission_test.cpp
        src/main/cpp/oreslang/frontend/Parser.cpp
        src/main/cpp/oreslang/frontend/MethodResolver.cpp
        src/main/cpp/oreslang/types/NativeScalarTypes.cpp
        src/main/cpp/oreslang/types/OwnershipChecker.cpp)
    target_include_directories(oreslang-native-actor-max-threads-admission-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_features(oreslang-native-actor-max-threads-admission-test PRIVATE cxx_std_17)
    target_compile_options(oreslang-native-actor-max-threads-admission-test PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>)
    add_test(NAME native_actor_max_threads_source_admission
        COMMAND oreslang-native-actor-max-threads-admission-test)
    set_tests_properties(native_actor_max_threads_source_admission
        PROPERTIES TIMEOUT 20)
endif()

# Dependency-free, executable import/source admission boundary. This test
# compiles the production SourceInput.hpp with the host C++ compiler.
if(BUILD_TESTING AND UNIX)
    add_executable(oreslang-native-source-input-admission-test
        tests/native_source_input_admission.cpp)
    target_include_directories(oreslang-native-source-input-admission-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_features(oreslang-native-source-input-admission-test PRIVATE cxx_std_17)
    target_compile_options(oreslang-native-source-input-admission-test PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>)
    add_test(NAME native_source_input_admission
        COMMAND oreslang-native-source-input-admission-test)
    set_tests_properties(native_source_input_admission PROPERTIES TIMEOUT 10)
endif()

if(BUILD_TESTING)
    add_executable(oreslang-native-record-region-test
        tests/native_record_region_test.c
        src/main/cpp/oreslang/runtime/NativeRecordRegion.c)
    target_include_directories(oreslang-native-record-region-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_definitions(oreslang-native-record-region-test PRIVATE ORESLANG_RECORD_TEST_ALLOC_FAILURE=1)
    target_compile_features(oreslang-native-record-region-test PRIVATE c_std_11)
    target_compile_options(oreslang-native-record-region-test PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>)
    target_link_libraries(oreslang-native-record-region-test PRIVATE Threads::Threads)
    add_test(NAME native_record_region_generation_quota_cleanup COMMAND oreslang-native-record-region-test)
    set_tests_properties(native_record_region_generation_quota_cleanup PROPERTIES TIMEOUT 30)
endif()

# Stage-2 prerequisite: readiness-gated Future continuation ownership.
# This host race test is not source-level async, channels or select parity.
if(BUILD_TESTING)
    find_package(Threads REQUIRED)
    add_executable(oreslang-native-continuation-ready-resume-test
        tests/native_continuation_ready_resume_test.c
        src/main/cpp/oreslang/runtime/NativeExecutionContextStore.c
        src/main/cpp/oreslang/runtime/NativeContinuationStore.c
        src/main/cpp/oreslang/runtime/NativeAwaitTraceBridge.c)
    set_target_properties(oreslang-native-continuation-ready-resume-test PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_include_directories(oreslang-native-continuation-ready-resume-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-continuation-ready-resume-test PRIVATE
        Threads::Threads)
    target_compile_options(oreslang-native-continuation-ready-resume-test PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror -UNDEBUG>)
    add_test(NAME native_continuation_ready_cancel_resume_race
        COMMAND oreslang-native-continuation-ready-resume-test)
    set_tests_properties(native_continuation_ready_cancel_resume_race PROPERTIES TIMEOUT 45)
endif()

# Source-free continuation wake failure-boundary oracle. This verifies only
# model invariants; production continuation-store tests remain separate.
if(BUILD_TESTING)
    add_executable(oreslang-continuation-wake-transaction-oracle
        tests/continuation_wake_transaction_oracle.c)
    set_target_properties(oreslang-continuation-wake-transaction-oracle PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_options(oreslang-continuation-wake-transaction-oracle PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror -UNDEBUG>)
    add_test(NAME continuation_wake_transaction_model
        COMMAND oreslang-continuation-wake-transaction-oracle)
    set_tests_properties(continuation_wake_transaction_model PROPERTIES TIMEOUT 10)
endif()

# Source-actor lifecycle prerequisite for real ActorRef.get_done lowering:
# stop request must not wait for physical termination; join_done owns the
# host-side terminal ACK and heap-reclamation barrier.
if(BUILD_TESTING)
    find_package(Threads REQUIRED)

    add_executable(oreslang-native-actor-sealed-stop-test
        tests/native_actor_sealed_stop_test.c
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c)
    target_include_directories(oreslang-native-actor-sealed-stop-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-actor-sealed-stop-test PRIVATE
        Threads::Threads)
    target_compile_features(oreslang-native-actor-sealed-stop-test PRIVATE c_std_11)
    target_compile_options(oreslang-native-actor-sealed-stop-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME native_actor_sealed_stop_abandons_queued_data
        COMMAND oreslang-native-actor-sealed-stop-test)
    set_tests_properties(native_actor_sealed_stop_abandons_queued_data
        PROPERTIES TIMEOUT 30)

    add_executable(oreslang-native-actor-sealed-stop-race-test
        tests/native_actor_sealed_stop_race_test.c
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c)
    target_include_directories(oreslang-native-actor-sealed-stop-race-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-actor-sealed-stop-race-test PRIVATE
        Threads::Threads)
    target_compile_features(oreslang-native-actor-sealed-stop-race-test PRIVATE c_std_11)
    target_compile_options(oreslang-native-actor-sealed-stop-race-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME native_actor_sealed_stop_send_race
        COMMAND oreslang-native-actor-sealed-stop-race-test)
    set_tests_properties(native_actor_sealed_stop_send_race
        PROPERTIES TIMEOUT 60)

    add_executable(oreslang-source-actor-stop-join-split-test
        tests/source_actor_stop_join_split_test.c
        src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c
        src/main/cpp/oreslang/runtime/NativeOresVM.c
        src/main/cpp/oreslang/runtime/NativeActorFuture.c
        src/main/cpp/oreslang/runtime/SourceActorBootstrap.c
        src/main/cpp/oreslang/runtime/NativeExecutionContextStore.c
        src/main/cpp/oreslang/runtime/NativeContinuationStore.c
        src/main/cpp/oreslang/runtime/NativeAwaitTraceBridge.c
        src/main/cpp/oreslang/runtime/NativeLogicalTrace.c)
    target_include_directories(oreslang-source-actor-stop-join-split-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-source-actor-stop-join-split-test PRIVATE
        Threads::Threads)
    target_compile_features(oreslang-source-actor-stop-join-split-test PRIVATE c_std_11)
    target_compile_options(oreslang-source-actor-stop-join-split-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME source_actor_stop_request_join_split
        COMMAND oreslang-source-actor-stop-join-split-test)
    set_tests_properties(source_actor_stop_request_join_split PROPERTIES TIMEOUT 30)

    add_executable(oreslang-source-actor-await-done-trace-test
        tests/source_actor_await_done_trace_runtime_test.c
        src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c
        src/main/cpp/oreslang/runtime/NativeOresVM.c
        src/main/cpp/oreslang/runtime/NativeActorFuture.c
        src/main/cpp/oreslang/runtime/SourceActorBootstrap.c
        src/main/cpp/oreslang/runtime/NativeExecutionContextStore.c
        src/main/cpp/oreslang/runtime/NativeContinuationStore.c
        src/main/cpp/oreslang/runtime/NativeAwaitTraceBridge.c
        src/main/cpp/oreslang/runtime/NativeLogicalTrace.c)
    target_include_directories(oreslang-source-actor-await-done-trace-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-source-actor-await-done-trace-test PRIVATE
        Threads::Threads)
    target_compile_features(oreslang-source-actor-await-done-trace-test PRIVATE c_std_11)
    target_compile_definitions(oreslang-source-actor-await-done-trace-test PRIVATE
        ORESLANG_SOURCE_ACTOR_TESTING=1)
    target_compile_options(oreslang-source-actor-await-done-trace-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME source_actor_await_done_trace_runtime
        COMMAND oreslang-source-actor-await-done-trace-test)
    set_tests_properties(source_actor_await_done_trace_runtime PROPERTIES TIMEOUT 30)


    add_executable(oreslang-source-actor-stop-before-ready-test
        tests/source_actor_stop_before_ready_test.c
        src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c
        src/main/cpp/oreslang/runtime/NativeOresVM.c
        src/main/cpp/oreslang/runtime/NativeActorFuture.c
        src/main/cpp/oreslang/runtime/SourceActorBootstrap.c
        src/main/cpp/oreslang/runtime/NativeExecutionContextStore.c
        src/main/cpp/oreslang/runtime/NativeContinuationStore.c
        src/main/cpp/oreslang/runtime/NativeAwaitTraceBridge.c
        src/main/cpp/oreslang/runtime/NativeLogicalTrace.c)
    target_include_directories(oreslang-source-actor-stop-before-ready-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-source-actor-stop-before-ready-test PRIVATE
        Threads::Threads)
    target_compile_features(oreslang-source-actor-stop-before-ready-test PRIVATE c_std_11)
    target_compile_definitions(oreslang-source-actor-stop-before-ready-test PRIVATE
        ORESLANG_SOURCE_ACTOR_TESTING=1)
    target_compile_options(oreslang-source-actor-stop-before-ready-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME source_actor_stop_before_ready
        COMMAND oreslang-source-actor-stop-before-ready-test)
    set_tests_properties(source_actor_stop_before_ready PROPERTIES TIMEOUT 30)

    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        add_executable(oreslang-source-actor-stop-before-ready-sanitizer-test
            tests/source_actor_stop_before_ready_test.c
            src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c
            src/main/cpp/oreslang/runtime/NativeActorScheduler.c
            src/main/cpp/oreslang/runtime/NativeOresVM.c
            src/main/cpp/oreslang/runtime/NativeActorFuture.c
            src/main/cpp/oreslang/runtime/SourceActorBootstrap.c
            src/main/cpp/oreslang/runtime/NativeExecutionContextStore.c
            src/main/cpp/oreslang/runtime/NativeContinuationStore.c
            src/main/cpp/oreslang/runtime/NativeAwaitTraceBridge.c
            src/main/cpp/oreslang/runtime/NativeLogicalTrace.c)
        target_include_directories(
            oreslang-source-actor-stop-before-ready-sanitizer-test PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
        target_link_libraries(
            oreslang-source-actor-stop-before-ready-sanitizer-test PRIVATE
            Threads::Threads)
        target_compile_features(
            oreslang-source-actor-stop-before-ready-sanitizer-test PRIVATE
            c_std_11)
        target_compile_definitions(
            oreslang-source-actor-stop-before-ready-sanitizer-test PRIVATE
            ORESLANG_SOURCE_ACTOR_TESTING=1)
        target_compile_options(
            oreslang-source-actor-stop-before-ready-sanitizer-test PRIVATE
            -Wall -Wextra -Wpedantic -Werror -UNDEBUG
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(
            oreslang-source-actor-stop-before-ready-sanitizer-test PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        add_test(NAME source_actor_stop_before_ready_asan_ubsan
            COMMAND oreslang-source-actor-stop-before-ready-sanitizer-test)
        set_tests_properties(source_actor_stop_before_ready_asan_ubsan
            PROPERTIES TIMEOUT 60)

        add_executable(oreslang-source-actor-stop-before-ready-tsan-test
            tests/source_actor_stop_before_ready_test.c
            src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c
            src/main/cpp/oreslang/runtime/NativeActorScheduler.c
            src/main/cpp/oreslang/runtime/NativeOresVM.c
            src/main/cpp/oreslang/runtime/NativeActorFuture.c
            src/main/cpp/oreslang/runtime/SourceActorBootstrap.c
            src/main/cpp/oreslang/runtime/NativeExecutionContextStore.c
            src/main/cpp/oreslang/runtime/NativeContinuationStore.c
            src/main/cpp/oreslang/runtime/NativeAwaitTraceBridge.c
            src/main/cpp/oreslang/runtime/NativeLogicalTrace.c)
        target_include_directories(
            oreslang-source-actor-stop-before-ready-tsan-test PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
        target_link_libraries(
            oreslang-source-actor-stop-before-ready-tsan-test PRIVATE
            Threads::Threads)
        target_compile_features(
            oreslang-source-actor-stop-before-ready-tsan-test PRIVATE
            c_std_11)
        target_compile_definitions(
            oreslang-source-actor-stop-before-ready-tsan-test PRIVATE
            ORESLANG_SOURCE_ACTOR_TESTING=1)
        target_compile_options(
            oreslang-source-actor-stop-before-ready-tsan-test PRIVATE
            -Wall -Wextra -Wpedantic -Werror -UNDEBUG
            -fsanitize=thread -fno-omit-frame-pointer)
        target_link_options(
            oreslang-source-actor-stop-before-ready-tsan-test PRIVATE
            -fsanitize=thread -fno-omit-frame-pointer)
        add_test(NAME source_actor_stop_before_ready_tsan
            COMMAND oreslang-source-actor-stop-before-ready-tsan-test)
        set_tests_properties(source_actor_stop_before_ready_tsan
            PROPERTIES TIMEOUT 60)
    endif()
endif()

# Receiver-elided source overload selector; this C++ unit is intentionally
# independent of LLVM libraries and can be compiled on minimal hosts.
if(BUILD_TESTING)
    add_executable(oreslang-native-callable-selector-test
        tests/native_callable_selector_test.cpp)
    target_include_directories(oreslang-native-callable-selector-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_features(oreslang-native-callable-selector-test PRIVATE cxx_std_17)
    target_compile_options(oreslang-native-callable-selector-test PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror -UNDEBUG>)
    add_test(NAME native_callable_selector_source_arity
        COMMAND oreslang-native-callable-selector-test)
    set_tests_properties(native_callable_selector_source_arity PROPERTIES TIMEOUT 10)
endif()

# Host-only structured Task cancellation authority. This mirrors the Java
# stage-1 runtime primitive only: it is NOT source-level task lowering,
# scheduler continuation integration, or proof that get_done is quiescent.
if(BUILD_TESTING)
    find_package(Threads REQUIRED)
    add_executable(oreslang-native-task-cancellation-test
        tests/native_task_cancellation_test.cpp)
    target_include_directories(oreslang-native-task-cancellation-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_features(oreslang-native-task-cancellation-test PRIVATE cxx_std_17)
    target_link_libraries(oreslang-native-task-cancellation-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-task-cancellation-test PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror -UNDEBUG>)
    add_test(NAME native_task_cancellation_tree_host_only
        COMMAND oreslang-native-task-cancellation-test)
    set_tests_properties(native_task_cancellation_tree_host_only
        PROPERTIES TIMEOUT 30)
endif()

# Task authority <-> generation-checked continuation integration, with
# producer-ready/cancel and scheduler-claim/cancel adversarial races.
# Tests a real shared host admission primitive, not arbitrary guest Task syntax.
if(BUILD_TESTING)
    find_package(Threads REQUIRED)
    add_executable(oreslang-native-task-continuation-gate-test
        tests/native_task_continuation_gate_test.cpp
        src/main/cpp/oreslang/runtime/NativeExecutionContextStore.c
        src/main/cpp/oreslang/runtime/NativeContinuationStore.c
        src/main/cpp/oreslang/runtime/NativeAwaitTraceBridge.c)
    target_include_directories(oreslang-native-task-continuation-gate-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_features(oreslang-native-task-continuation-gate-test PRIVATE cxx_std_17)
    target_link_libraries(oreslang-native-task-continuation-gate-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-task-continuation-gate-test PRIVATE
        "$<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Werror;-UNDEBUG>"
        "$<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Werror;-UNDEBUG>")
    add_test(NAME native_task_continuation_cancel_ready_races
        COMMAND oreslang-native-task-continuation-gate-test)
    set_tests_properties(native_task_continuation_cancel_ready_races
        PROPERTIES TIMEOUT 60)
endif()

# #170: independently check atomic nested actor adoption, host-only.
if(BUILD_TESTING)
    add_executable(oreslang-native-actor-child-adoption-test tests/native_actor_child_adoption_test.cpp)
    target_include_directories(oreslang-native-actor-child-adoption-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_features(oreslang-native-actor-child-adoption-test PRIVATE cxx_std_17)
    target_compile_options(oreslang-native-actor-child-adoption-test PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>)
    add_test(NAME native_actor_child_adoption COMMAND oreslang-native-actor-child-adoption-test)
endif()
