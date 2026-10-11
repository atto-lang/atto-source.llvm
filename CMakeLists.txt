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
    oreslang_contract_test(shared_http_counter_kernel_ir http-shared-counter-kernel.ores --ir "call i64 @counter_value")
    oreslang_contract_test(common_guest_http_handler_ir http-guest-counter-handler.ores --ir "call i64 @counter_response")
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

    # Strict i64 fields: immutable const/val, mutable let, synchronous method writes.
    oreslang_contract_test(actor_source_field_layout actor-owned-fields.ores --ir "oreslang_actor_Counter_read")
    oreslang_contract_test(actor_source_mutable_field actor-owned-fields.ores --ir "ores_actor_i64_heap_field_write")
    oreslang_contract_test(reject_actor_immutable_write actor-immutable-write.ores --diagnostic "cannot assign immutable or unknown actor field")
    oreslang_contract_test(reject_actor_unknown_write actor-unknown-write.ores --diagnostic "cannot assign immutable or unknown actor field")
    oreslang_contract_test(reject_actor unsupported-actor.ores --diagnostic "actor requires at least one scalar field")
    oreslang_contract_test(reject_actor_duplicate_field actor-duplicate-field.ores --diagnostic "duplicate actor field")
    oreslang_contract_test(reject_actor_bad_field_type actor-unsupported-field.ores --diagnostic "expected 'int'")
    oreslang_contract_test(reject_actor_unknown_field actor-unknown-field.ores --diagnostic "unknown parameter")
    oreslang_contract_test(reject_actor_shadow actor-field-shadow.ores --diagnostic "actor parameter shadows field")
    oreslang_contract_test(reject_actor_user_symbol_collision actor-abi-func-collision.ores --diagnostic "conflicting generated actor symbol")
    oreslang_contract_test(reject_actor_actor_symbol_collision actor-abi-actor-collision.ores --diagnostic "conflicting generated actor symbol")
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
        oreslang_contract_test(native_exec_shared_http_counter_kernel http-shared-counter-kernel.ores --run 43 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_exec_common_guest_http_handler http-guest-counter-handler.ores --run 43 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_exec_arithmetic return-arithmetic.ores --run 28 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_exec_negative negative-literal.ores --run 42 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_exec_owned_lexical owned-lexical-main.ores --run 42 --clang "${ORE_CLANG}")
        oreslang_contract_test(native_exec_owned_region owned-native-region-main.ores --run 42 --clang "${ORE_CLANG}" --native-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeOwnedI64Region.c")
        oreslang_contract_test(native_exec_owned_region_nested owned-native-region-call.ores --run 42 --clang "${ORE_CLANG}" --native-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeOwnedI64Region.c")
        oreslang_contract_test(native_exec_actor_fields actor-owned-fields.ores --run 0 --clang "${ORE_CLANG}"
            --native-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeOwnedI64Region.c"
            --actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"
            --native-harness "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_actor_source_harness.c")


    else()
        message(WARNING "Clang unavailable: native executable tests are disabled")
    endif()

    # Real TCP traffic, not a stubbed HTTP handler. The trusted C transport
    # invokes Oreslang actor methods compiled by oreslang-llvmc from source.
    if(ORE_CLANG AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_test(NAME native_http_llvm_compiled_actor_loopback
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/http_actor_server_e2e.py"
                --compiler "$<TARGET_FILE:oreslang-llvmc>"
                --fixture "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/actor-owned-fields.ores"
                --clang "${ORE_CLANG}"
                --actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"
                --arena-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/ActorArena.c"
                --native-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeOwnedI64Region.c"
                --harness "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_actor_http_server.c"
                --include "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
        set_tests_properties(native_http_llvm_compiled_actor_loopback
            PROPERTIES TIMEOUT 45)
    endif()

    # Two native host placement profiles, five real LLVM-compiled actors each.
    # "private" profile uses per-actor OS processes, not source isoactor lowering.
    if(ORE_CLANG AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_test(NAME native_http_llvm_five_actor_placement_matrix
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/http_actor_matrix_e2e.py"
                --compiler "$<TARGET_FILE:oreslang-llvmc>"
                --fixture "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/actor-owned-fields.ores"
                --kernel-fixture "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/http-guest-counter-handler.ores"
                --clang "${ORE_CLANG}"
                --actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"
                --arena-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/ActorArena.c"
                --native-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeOwnedI64Region.c"
                --mailbox-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeActorMailbox.c"
                --scheduler-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeActorScheduler.c"
                --harness "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_actor_http_matrix_server.c"
                --include "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
        set_tests_properties(native_http_llvm_five_actor_placement_matrix
            PROPERTIES TIMEOUT 420)
    endif()

    find_package(Threads REQUIRED)
    # Bounded actor FIFO admission, independent of physical carrier/thread ID.
    add_executable(oreslang-native-actor-mailbox-test
        tests/native_actor_mailbox_test.c
        src/main/cpp/oreslang/runtime/NativeActorMailbox.c)
    target_include_directories(oreslang-native-actor-mailbox-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-actor-mailbox-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-actor-mailbox-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_actor_fifo_mailbox_admission
        COMMAND oreslang-native-actor-mailbox-test)
    set_tests_properties(native_actor_fifo_mailbox_admission PROPERTIES TIMEOUT 20)

    # Nonblocking admission: fixed-size per-actor message rings, one turn per
    # actor, and preallocated native worker pool. HTTP ingress never parks.
    add_executable(oreslang-native-actor-ready-queue-test
        tests/native_actor_scheduler_test.c
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c)
    target_include_directories(oreslang-native-actor-ready-queue-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-actor-ready-queue-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-actor-ready-queue-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_actor_nonblocking_ready_queue
        COMMAND oreslang-native-actor-ready-queue-test)
    set_tests_properties(native_actor_nonblocking_ready_queue PROPERTIES TIMEOUT 30)

    # Reserved supervisor CONTROL lane and lifecycle Future wakeup.
    add_executable(oreslang-native-supervisor-control-test
        tests/native_supervisor_control_test.c
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c
        src/main/cpp/oreslang/runtime/NativeActorFuture.c)
    target_include_directories(oreslang-native-supervisor-control-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-supervisor-control-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-supervisor-control-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_supervisor_reserved_control_future
        COMMAND oreslang-native-supervisor-control-test)
    set_tests_properties(native_supervisor_reserved_control_future PROPERTIES TIMEOUT 30)

    # Persistent actor-owned heap contract, distinct from per-call owned
    # scalar regions. Uses C11 atomics and cross-carrier thread migration.
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

    # Host-only deterministic actor arena: persistent slab + per-turn scratch.
    add_executable(oreslang-native-actor-arena-test
        tests/native_actor_arena_test.c
        src/main/cpp/oreslang/runtime/ActorArena.c)
    target_include_directories(oreslang-native-actor-arena-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-actor-arena-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-actor-arena-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_actor_arena_persistent_scratch_quiescence
        COMMAND oreslang-native-actor-arena-test)
    set_tests_properties(native_actor_arena_persistent_scratch_quiescence
        PROPERTIES TIMEOUT 20)

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
