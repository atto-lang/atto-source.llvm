cmake_minimum_required(VERSION 3.20)
project(oreslang_llvm VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

    find_package(Python3 COMPONENTS Interpreter REQUIRED)
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
    function(oreslang_contract_test name fixture)
        add_test(NAME ${name}
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract.py"
                --compiler "$<TARGET_FILE:oreslang-llvmc>"
                --fixture "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/${fixture}"
                ${ARGN})
    endfunction()

    # Unsupported guest source suspension requires heap-owned continuation
    # lowering. Never emit IR for partial/unsafe loop and cooperate forms.
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
        --diagnostic "reserved native entrypoint symbol collision")
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
    oreslang_contract_test(call_and_forward_ref forward-call-main.ores --ir "call i64 @add")
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
            --ir "return.guard.eq" --run 222 --clang "${ORE_CLANG}")
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
    if(ORE_CLANG)
        add_test(NAME native_class_move_source_execution
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/class_move_native_matrix.py"
                --compiler "$<TARGET_FILE:oreslang-llvmc>" --clang "${ORE_CLANG}")
        add_test(NAME native_class_move_source_asan_ubsan
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/class_move_native_matrix.py"
                --compiler "$<TARGET_FILE:oreslang-llvmc>" --clang "${ORE_CLANG}" --sanitize)
        set_tests_properties(native_class_move_source_execution
            native_class_move_source_asan_ubsan PROPERTIES TIMEOUT 180)
    endif()
    oreslang_contract_test(reject_owned_int_without_opt_in owned-native-region-main.ores --diagnostic "experimental owned int requires --experimental-owned-int")
    oreslang_contract_test(owned_native_heap_lowering owned-native-region-main.ores --experimental-owned-int --ir "call void @oreslang_owned_i64_region_drop")
    oreslang_contract_test(owned_native_region_nested owned-native-region-call.ores --experimental-owned-int --ir "call void @oreslang_owned_i64_region_close")
    oreslang_contract_test(reject_unsupported_native_heap_type owned-native-region-unsupported.ores --experimental-owned-int --diagnostic "expected 'int'")
    oreslang_contract_test(reject_local_uninitialized owned-uninitialized.ores --diagnostic "unknown parameter")
    oreslang_contract_test(reject_local_duplicate owned-duplicate.ores --diagnostic "duplicate owned local")
    oreslang_contract_test(reject_local_forward_reference owned-use-before-declaration.ores --diagnostic "unknown parameter")
    oreslang_contract_test(reject_local_unsupported_object owned-unsupported-heap.ores --diagnostic "expected int expression")
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
    oreslang_contract_test(source_future_typed_await_boundary source-future-typed-unimplemented.ores
        --diagnostic "Future<T> requires native continuation lowering")

    # Real source-compiled Oreslang actors, not host-only C callback tests.
    # The synchronous stop barrier is allowed only in trusted top-level main.
    # Source await/select/Futures still fail closed pending pinned-frame lowering.
    if(ORE_CLANG)
        add_test(NAME source_actor_compiled_native_lifecycle
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/source_actor_e2e.py"
                --compiler "$<TARGET_FILE:oreslang-llvmc>"
                --clang "${ORE_CLANG}"
                --root "${CMAKE_CURRENT_SOURCE_DIR}")
        set_tests_properties(source_actor_compiled_native_lifecycle PROPERTIES TIMEOUT 90)
        add_test(NAME source_actor_compiled_native_asan_ubsan
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/source_actor_e2e.py"
                --compiler "$<TARGET_FILE:oreslang-llvmc>"
                --clang "${ORE_CLANG}"
                --root "${CMAKE_CURRENT_SOURCE_DIR}" --sanitize)
        set_tests_properties(source_actor_compiled_native_asan_ubsan PROPERTIES TIMEOUT 90)
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
    add_test(NAME native_actor_done_resource_quiescence
        COMMAND oreslang-native-actor-retirement-barrier-test)
    set_tests_properties(native_actor_done_resource_quiescence PROPERTIES TIMEOUT 15)
    oreslang_contract_test(reject_gpu_kernel unsupported-gpu-kernel.ores --diagnostic "native gpu/quantum boundary unsupported")
    oreslang_contract_test(reject_quantum_kernel unsupported-quantum-kernel.ores --diagnostic "native gpu/quantum boundary unsupported")
    oreslang_contract_test(reject_gpu_expression unsupported-gpu-expression.ores --diagnostic "native gpu/quantum boundary unsupported")
    add_executable(oreslang-dop-layout-test tests/dop_layout_test.cpp)
    target_include_directories(oreslang-dop-layout-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-dop-layout-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME dop_layout_checked_geometry COMMAND oreslang-dop-layout-test)
    # Nonescaping int loans are supported at checked call boundaries only;
    # a reference still cannot be returned or stored, or cross suspension.
    oreslang_contract_test(reject_rt_borrow unsupported-rt-borrow.ores --diagnostic "borrowed reference may only be passed")
    oreslang_contract_test(native_borrowed_parameter unsupported-borrowed-parameter.ores --ir "load i64")
    oreslang_contract_test(reject_rt_borrow_mut unsupported-rt-borrow-mut.ores --diagnostic "borrowed reference may only be passed")
    oreslang_contract_test(reject_rt_take unsupported-rt-take.ores --diagnostic "unknown parameter")
    oreslang_contract_test(reject_rt_copy unsupported-rt-copy.ores --diagnostic "unknown parameter")
    oreslang_contract_test(reject_rt_share unsupported-rt-share.ores --diagnostic "unknown parameter")
    oreslang_contract_test(reject_unqualified_borrow_call unsupported-borrow-function.ores --diagnostic "expected int expression")
    oreslang_contract_test(reject_suspends unsupported-suspends.ores --diagnostic "native suspends declaration requires")
    oreslang_contract_test(reject_critical unsupported-critical.ores --diagnostic "native critical declaration requires")
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
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_async_io_boundary_matrix.py"
            "$<TARGET_FILE:oreslang-llvmc>")
    oreslang_contract_test(pure_user_blocking_suffix pure-user-blocking-suffix.ores --ir "call i64 @pure_math_blocking")
    # The terminal-state gate is a standalone native safety primitive, not
    # an async I/O reactor or source-level await implementation.
    add_executable(oreslang-async-io-operation-test tests/async_io_operation_test.cpp)
    target_include_directories(oreslang-async-io-operation-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    find_package(Threads REQUIRED)
    target_link_libraries(oreslang-async-io-operation-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-async-io-operation-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_async_io_completion_generation_races COMMAND oreslang-async-io-operation-test)
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
    oreslang_contract_test(reject_wrong_arity wrong-arity.ores --diagnostic "wrong call arity")
    oreslang_contract_test(reject_duplicates duplicate-function.ores --diagnostic "duplicate function")
    oreslang_contract_test(reject_duplicate_parameters duplicate-parameter.ores --diagnostic "duplicate parameter")
    oreslang_contract_test(reject_oversized_integer overflow-integer.ores --diagnostic "overflows signed 64-bit")
    oreslang_contract_test(reject_unclosed_comment unclosed-comment.ores --diagnostic "unterminated block comment")
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
    oreslang_contract_test(class_reject_nonint_nested_field class-multifield-nonint-field-reject.ores --diagnostic "expected 'int'")
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
    oreslang_contract_test(cfg_reject_impure_move_condition cfg-guard-impure-take-reject.ores --diagnostic "native guarded condition must be pure")
    oreslang_contract_test(cfg_reject_impure_call_condition cfg-guard-impure-call-reject.ores --diagnostic "native guarded condition must be pure")
    oreslang_contract_test(cfg_reject_multifield_class_return cfg-multifield-branch-noncopy-reject.ores --diagnostic "ownership violation")
    oreslang_contract_test(cfg_reject_multifield_call_in_condition cfg-multifield-guard-call-reject.ores --diagnostic "native guarded condition must be pure")
    oreslang_contract_test(cfg_reject_missing_fallthrough cfg-guard-missing-final-reject.ores --diagnostic "expected 'return'")
    oreslang_contract_test(cfg_reject_missing_fi cfg-guard-missing-fi-reject.ores --diagnostic "expected 'fi'")
    oreslang_contract_test(cfg_reject_void_guard cfg-guard-void-reject.ores --diagnostic "guarded early return requires int result")




    if(ORE_CLANG)
        oreslang_contract_test(class_exec_mutable_borrow class-mutable-borrow-native.ores --run 42 --clang "${ORE_CLANG}")
        oreslang_contract_test(class_exec_shared_read class-shared-read-native.ores --run 42 --clang "${ORE_CLANG}")
        oreslang_contract_test(class_exec_mut_to_shared_reborrow class-reborrow-native.ores --run 42 --clang "${ORE_CLANG}")
        oreslang_contract_test(class_exec_owned_stack_field class-owned-field-native.ores --run 42 --clang "${ORE_CLANG}")
        oreslang_contract_test(class_exec_field_take_reinitialization class-field-take-reinit-native.ores --run 42 --clang "${ORE_CLANG}" --no-native-abi)
        oreslang_contract_test(class_exec_cfg_branch_field_take class-field-take-branch-native.ores --run 42 --clang "${ORE_CLANG}" --no-native-abi)
        oreslang_contract_test(class_exec_two_field_mutator class-multifield-mutator.ores --run 42 --clang "${ORE_CLANG}" --no-native-abi)
        oreslang_contract_test(cfg_exec_mutated_multifield_early_true cfg-multifield-mutator-early-true.ores --run 42 --clang "${ORE_CLANG}" --no-native-abi)
        oreslang_contract_test(cfg_exec_mutated_multifield_early_false cfg-multifield-mutator-early-false.ores --run 42 --clang "${ORE_CLANG}" --no-native-abi)
        oreslang_contract_test(class_exec_three_field_mutator class-multifield-independent-write.ores --run 42 --clang "${ORE_CLANG}" --no-native-abi)
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
        oreslang_contract_test(native_exec_constant constant-main.ores --run 42 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_exec_forward_call forward-call-main.ores --run 42 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_exec_arithmetic return-arithmetic.ores --run 28 --clang "${ORE_CLANG}")
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
        tests/native_actor_arena_test.c
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
        tests/actor_owned_heap_lifecycle_test.c
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
endif()
