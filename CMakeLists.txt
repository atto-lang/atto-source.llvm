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
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    find_program(ORE_CLANG NAMES clang clang-19 clang-18 clang-17 clang-16 clang-15 clang-14)
    function(oreslang_contract_test name fixture)
        add_test(NAME ${name}
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract.py"
                --compiler "$<TARGET_FILE:oreslang-llvmc>"
                --fixture "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/${fixture}"
                ${ARGN})
    endfunction()

    oreslang_contract_test(constant_main constant-main.ores --ir "ret i64 42")
    oreslang_contract_test(module_main module-main.ores --ir "demo.main")
    oreslang_contract_test(call_and_forward_ref forward-call-main.ores --ir "call i64 @add")
    oreslang_contract_test(arithmetic_precedence return-arithmetic.ores --ir "ret i64 28")
    oreslang_contract_test(negative_literal negative-literal.ores --ir "ret i64 42")
    oreslang_contract_test(nested_comments nested-comments.ores --ir "ret i64 42")
    oreslang_contract_test(owned_lexical_lifetime owned-lexical-main.ores --ir "llvm.lifetime.end")
    oreslang_contract_test(owned_native_heap_lowering owned-native-region-main.ores --ir "call void @oreslang_owned_i64_region_drop")
    oreslang_contract_test(owned_native_region_nested owned-native-region-call.ores --ir "call void @oreslang_owned_i64_region_close")
    oreslang_contract_test(reject_unsupported_native_heap_type owned-native-region-unsupported.ores --diagnostic "expected 'int'")

    oreslang_contract_test(reject_uninitialized_owned_local owned-uninitialized.ores --diagnostic "unknown parameter")
    oreslang_contract_test(reject_duplicate_owned_local owned-duplicate.ores --diagnostic "duplicate owned local")
    oreslang_contract_test(reject_forward_owned_local owned-use-before-declaration.ores --diagnostic "unknown parameter")
    oreslang_contract_test(reject_unsupported_owned_heap owned-unsupported-heap.ores --diagnostic "expected int expression")

    oreslang_contract_test(source_actor_field_ir actor-fields-counter.ores --ir "ores_actor_i64_fields_codegen_set")
    oreslang_contract_test(source_actor_spawn_await_ir actor-spawn-await-source.ores --ir "ores_actor_host_poll_done")
    oreslang_contract_test(source_actor_await_spawn_ir actor-await-spawn-source.ores --ir "ores_actor_host_poll_ready")
    oreslang_contract_test(reject_spawn_foreign_actor actor-spawn-wrong-type.ores --diagnostic "source spawn actor must match enclosing actor declaration")
    oreslang_contract_test(reject_spawn_without_done actor-spawn-missing-done.ores --diagnostic "expected 'await'")
    oreslang_contract_test(reject_actor_script_method_collision actor-script-method-collision.ores
        --diagnostic "reserved actor lifecycle method")
    oreslang_contract_test(reject_actor_field_unknown actor-fields-unknown.ores --diagnostic "unknown or forbidden actor field")
    oreslang_contract_test(reject_actor_field_duplicate actor-fields-duplicate.ores --diagnostic "duplicate actor field")
    oreslang_contract_test(reject_actor_field_escape actor-fields-escape.ores --diagnostic "expected int expression")
    oreslang_contract_test(reject_actor_readonly_field actor-fields-immutable.ores --diagnostic "cannot assign immutable actor field")
    oreslang_contract_test(reject_actor_lifecycle_collision actor-fields-method-collision.ores --diagnostic "reserved actor lifecycle method")
    oreslang_contract_test(reject_actor_without_fields unsupported-actor.ores --diagnostic "actor requires at least one int field")
    # Ownership remains Java-only. Reject all source ownership forms before
    # LLVM IR emission until a lifetime/provenance-aware typechecker exists.
    oreslang_contract_test(reject_rt_borrow unsupported-rt-borrow.ores --diagnostic "expected int expression")
    oreslang_contract_test(reject_borrowed_param unsupported-borrowed-parameter.ores --diagnostic "expected 'int'")
    oreslang_contract_test(reject_rt_borrow_mut unsupported-rt-borrow-mut.ores --diagnostic "expected int expression")
    oreslang_contract_test(reject_rt_take unsupported-rt-take.ores --diagnostic "expected int expression")
    oreslang_contract_test(reject_rt_copy unsupported-rt-copy.ores --diagnostic "expected int expression")
    oreslang_contract_test(reject_rt_share unsupported-rt-share.ores --diagnostic "expected int expression")
    oreslang_contract_test(reject_unqualified_borrow_call unsupported-borrow-function.ores --diagnostic "unknown function or wrong call arity")
    oreslang_contract_test(reject_static_select unsupported-static-select.ores --diagnostic "expected 'return'")
    oreslang_contract_test(reject_dynamic_select_plan unsupported-dynamic-select-plan.ores --diagnostic "expected int expression")
    oreslang_contract_test(reject_undefined undefined-name.ores --diagnostic "unknown parameter")
    # LLVM has no native I/O binding registry or continuation lowering yet.
    # Both spellings must fail closed with no IR until implementation lands.
    oreslang_contract_test(reject_unsupported_native_io_blocking unsupported-native-io-blocking.ores --diagnostic "unknown function or wrong call arity")
    oreslang_contract_test(reject_unsupported_native_io_bare unsupported-native-io-bare.ores --diagnostic "unknown function or wrong call arity")
    oreslang_contract_test(reject_wrong_arity wrong-arity.ores --diagnostic "wrong call arity")
    oreslang_contract_test(reject_duplicates duplicate-function.ores --diagnostic "duplicate function")
    oreslang_contract_test(reject_duplicate_parameters duplicate-parameter.ores --diagnostic "duplicate parameter")
    oreslang_contract_test(reject_oversized_integer overflow-integer.ores --diagnostic "overflows signed 64-bit")
    oreslang_contract_test(reject_unclosed_comment unclosed-comment.ores --diagnostic "unterminated block comment")

    if(ORE_CLANG)
        oreslang_contract_test(native_exec_constant constant-main.ores --run 42 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_exec_forward_call forward-call-main.ores --run 42 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_exec_arithmetic return-arithmetic.ores --run 28 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_exec_negative negative-literal.ores --run 42 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_exec_owned_lexical owned-lexical-main.ores --run 42 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_exec_owned_region owned-native-region-main.ores --run 42 --clang "${ORE_CLANG}" --native-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeOwnedI64Region.c")
        oreslang_contract_test(native_exec_owned_region_nested owned-native-region-call.ores --run 42 --clang "${ORE_CLANG}" --native-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeOwnedI64Region.c")
        oreslang_contract_test(native_exec_persistent_actor_fields actor-fields-counter.ores
            --run 0 --clang "${ORE_CLANG}"
            --native-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"
            --native-harness "${CMAKE_CURRENT_SOURCE_DIR}/tests/actor_fields_generated_host.c")
        oreslang_contract_test(native_exec_source_spawn_await actor-spawn-await-source.ores
            --run 32 --clang "${ORE_CLANG}"
            --native-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"
            --native-harness-cxx "${CMAKE_CURRENT_SOURCE_DIR}/tests/actor_source_spawn_await_host.cpp")
        oreslang_contract_test(native_exec_source_await_spawn actor-await-spawn-source.ores
            --run 32 --clang "${ORE_CLANG}"
            --native-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"
            --native-harness-cxx "${CMAKE_CURRENT_SOURCE_DIR}/tests/actor_source_spawn_await_host.cpp")
        oreslang_contract_test(native_exec_source_await_handler_panic actor-spawn-await-source.ores
            --run 79 --clang "${ORE_CLANG}"
            --native-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"
            --native-harness-cxx "${CMAKE_CURRENT_SOURCE_DIR}/tests/actor_source_spawn_await_host.cpp")
        set_tests_properties(native_exec_source_await_handler_panic PROPERTIES
            ENVIRONMENT "ORES_ACTOR_TEST_HANDLER_PANIC=1")
        # Real compiler-generated Counter code reused by three independent
        # ActorGroup-owned heaps with callback-resumed parent CONTROL.
        oreslang_contract_test(native_exec_multi_actor_scheduled_group actor-fields-counter.ores
            --run 0 --clang "${ORE_CLANG}"
            --native-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"
            --native-harness-cxx "${CMAKE_CURRENT_SOURCE_DIR}/tests/actor_scheduled_group_generated_host.cpp")
        oreslang_contract_test(native_exec_actor_group_owned_fields actor-fields-counter.ores
            --run 0 --clang "${ORE_CLANG}"
            --native-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"
            --native-harness-cxx "${CMAKE_CURRENT_SOURCE_DIR}/tests/actor_group_owned_heap_generated_host.cpp")


    else()
        message(WARNING "Clang unavailable: native executable tests are disabled")
    endif()

    # Persistent actor-owned heap contract, distinct from per-call owned
    # scalar regions. Uses C11 atomics and cross-carrier thread migration.
    find_package(Threads REQUIRED)
    # Bounded logical actor / parent supervisor lanes: no carrier blocked
    # on mailbox admission; CONTROL remains available during data saturation.
    add_executable(oreslang-actor-exclusive-scheduler-test
        tests/actor_exclusive_scheduler_test.cpp)
    target_include_directories(oreslang-actor-exclusive-scheduler-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-actor-exclusive-scheduler-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-actor-exclusive-scheduler-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_actor_exclusive_scheduler_control_and_backpressure
        COMMAND oreslang-actor-exclusive-scheduler-test)
    set_tests_properties(native_actor_exclusive_scheduler_control_and_backpressure PROPERTIES TIMEOUT 20)

    add_executable(oreslang-actor-owned-heap-test
        tests/actor_owned_heap_lifecycle_test.c
        src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c)
    target_include_directories(oreslang-actor-owned-heap-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-actor-owned-heap-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-actor-owned-heap-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_actor_owned_heap_quiescence
        COMMAND oreslang-actor-owned-heap-test)
    set_tests_properties(native_actor_owned_heap_quiescence PROPERTIES TIMEOUT 20)

    add_executable(oreslang-owned-native-region-test
        tests/native_owned_region_test.c
        src/main/cpp/oreslang/runtime/NativeOwnedI64Region.c)
    target_include_directories(oreslang-owned-native-region-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-owned-native-region-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_owned_region_provenance_and_drop_contract
        COMMAND oreslang-owned-native-region-test)

    add_executable(oreslang-deterministic-graph-arena-test tests/deterministic_graph_arena_test.cpp)
    target_include_directories(oreslang-deterministic-graph-arena-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_compile_options(oreslang-deterministic-graph-arena-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_deterministic_graph_arena COMMAND oreslang-deterministic-graph-arena-test)

    # Linux-specific actor-neutral wake/park primitive. This does NOT
    # implement the actor scheduler or certify lock-free mailbox delivery.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_package(Threads REQUIRED)
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
endif()
