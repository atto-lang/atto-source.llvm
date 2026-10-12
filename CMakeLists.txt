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
    # Standalone LLVM frontend admission proof. The parser is linked directly
    # so nominal declarations, monotonic tags and fail-closed actor usage can
    # be tested without generating IR for unsupported tagged operations.
    add_executable(oreslang-nominal-message-parser-test
        tests/native_nominal_message_parser_test.cpp
        src/main/cpp/oreslang/frontend/Parser.cpp)
    target_include_directories(oreslang-nominal-message-parser-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    add_test(NAME source_nominal_enum_pattern_parser_gate
        COMMAND oreslang-nominal-message-parser-test)
    # Independent native implementation of the exact nested-enum MessagePack
    # v2 golden vector. This checks compiler metadata and wire compatibility,
    # NOT compiled LLVM nested guest actor execution.
    add_executable(oreslang-nested-enum-v2-wire-test
        tests/native_nested_enum_v2_wire_test.cpp
        src/main/cpp/oreslang/frontend/Parser.cpp)
    target_include_directories(oreslang-nested-enum-v2-wire-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp"
        "${LLVM_INCLUDE_DIRS}")
    target_link_libraries(oreslang-nested-enum-v2-wire-test PRIVATE ${ORE_LLVM_LIBS})
    add_test(NAME nested_enum_v2_native_golden_vector
        COMMAND oreslang-nested-enum-v2-wire-test)
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
    # Restricted compiler-generated ActorRef lifecycle: trusted top-level
    # main may spawn one or more typed actors and must stop each exactly once.
    # Source await, actor methods, and cross-boundary ref escape remain rejected.
    # Closed actor inbox declarations: module-local and top-level contracts.
    oreslang_contract_test(actor_receive_exhaustive_bool_ir message-receive-bool-match.ores
        --ir "match.true")
    oreslang_contract_test(actor_receive_bool_conditional_ir message-receive-bool-match.ores
        --ir "match.false")
    oreslang_contract_test(reject_receive_bool_nonexhaustive message-receive-bool-nonexhaustive.ores
        --diagnostic "nonexhaustive receive boolean match")
    oreslang_contract_test(reject_receive_bool_duplicate_arm message-receive-bool-duplicate.ores
        --diagnostic "duplicate receive boolean match arm")
    oreslang_contract_test(reject_receive_bool_wrong_subject message-receive-bool-wrong-subject.ores
        --diagnostic "receive match must inspect Message parameter")
    oreslang_contract_test(reject_receive_bool_wrong_contract message-receive-int-bool-match.ores
        --diagnostic "receive match true/false requires bool-only contract")
    oreslang_contract_test(reject_unnarrowed_message_payload message-receive-bool-unnarrowed-payload.ores
        --diagnostic "match arm Message payload binding is not narrowed")
    oreslang_contract_test(actor_message_shape_declaration message-pattern-shapes.ores
        --ir "oreslang_actor_Foo_receive")
    oreslang_contract_test(actor_module_typed_receive_symbols message-pattern-module.ores
        --ir "oreslang_actor_Bar_Foo_receive")
    oreslang_contract_test(actor_module_typed_main_bridge message-pattern-module.ores
        --ir "Bar.main")
    oreslang_contract_test(actor_message_native_int_handler message-pattern-native-int.ores
        --ir "oreslang_actor_Worker_receive")
    oreslang_contract_test(reject_unknown_actor_message_match message-pattern-unknown-contract.ores
        --diagnostic "unknown actor message match contract")
    oreslang_contract_test(reject_typed_actor_missing_receiver message-pattern-no-receiver.ores
        --diagnostic "typed actor requires exactly one receive(Message m)")
    oreslang_contract_test(reject_mismatched_bool_message message-pattern-bad-bool-send.ores
        --diagnostic "actor send expects bool message")
    oreslang_contract_test(reject_mismatched_int_message message-pattern-bad-int-send.ores
        --diagnostic "actor send expects int message")
    oreslang_contract_test(reject_structural_message_without_abi message-pattern-union-send.ores
        --diagnostic "typed structural/union message transport requires tagged envelope lowering")
    oreslang_contract_test(reject_duplicate_actor_match_alternative message-pattern-duplicate-shape.ores
        --diagnostic "duplicate message shape in match")
    oreslang_contract_test(reject_receive_payload_without_match_lowering message-pattern-inspect-no-match.ores
        --diagnostic "receive Message payload inspection requires match lowering")
    oreslang_contract_test(reject_direct_receive_method_call message-pattern-direct-receive.ores
        --diagnostic "expected 'send'")

    oreslang_contract_test(source_actor_spawn_ir source-actor-spawn-stop.ores
        --ir "oreslang_source_actor_spawn")
    oreslang_contract_test(source_actor_typed_ref_ir source-actor-typed-ref.ores
        --ir "oreslang_source_actor_spawn")
    oreslang_contract_test(source_actor_await_ready_continuation_ir source-actor-await-ready.ores
        --ir "main.ready_continuation")
    oreslang_contract_test(source_actor_await_future_dispatch_ir source-actor-await-ready.ores
        --ir "oreslang_source_actor_await_ready")
    oreslang_contract_test(reject_await_ready_wrong_receiver source-actor-await-ready-wrong-ref.ores
        --diagnostic "await get_ready requires one source actor without suspended locals")
    oreslang_contract_test(reject_source_actor_typed_mismatch source-actor-typed-ref-mismatch.ores
        --diagnostic "ActorRef<T> type mismatch")
    oreslang_contract_test(reject_source_actor_typed_nonspawn source-actor-typed-ref-nonspawn.ores
        --diagnostic "ActorRef<T> requires spawn initializer")
    oreslang_contract_test(reject_future_type_without_await source-future-typed-unimplemented.ores
        --diagnostic "Future<T> requires native continuation lowering")
    oreslang_contract_test(source_actor_stop_ir source-actor-spawn-stop.ores
        --ir "oreslang_source_actor_stop")
    oreslang_contract_test(reject_source_actor_missing_stop source-actor-spawn-missing-stop.ores
        --diagnostic "actor reference must be stopped before return")
    oreslang_contract_test(reject_source_actor_unknown source-actor-spawn-unknown.ores
        --diagnostic "unknown actor type")
    oreslang_contract_test(reject_source_actor_double_stop source-actor-spawn-double-stop.ores
        --diagnostic "invalid or repeated actor stop")
    oreslang_contract_test(reject_source_actor_done_await_unimplemented source-actor-spawn-unavailable-await.ores
        --diagnostic "expected 'get_ready'")
    oreslang_contract_test(reject_source_actor_await_spawn source-actor-await-spawn.ores
        --diagnostic "await requires native compiler continuation lowering")
    oreslang_contract_test(reject_source_actor_shadow source-actor-ref-local-shadow.ores
        --diagnostic "scalar local shadows actor reference")
    oreslang_contract_test(reject_source_actor_escape source-actor-spawn-escape.ores
        --diagnostic "unknown parameter")
    oreslang_contract_test(reject_source_actor_pipeline_forward_unlowered
        source-actor-pipeline-forward-unavailable.ores
        --diagnostic "self.forward requires native pipeline routing lowering")
    oreslang_contract_test(reject_source_actor_pipeline_backward_unlowered
        source-actor-pipeline-backward-unavailable.ores
        --diagnostic "self.backward requires native pipeline routing lowering")
    oreslang_contract_test(reject_pipeline_actor_without_declared_route
        source-actor-pipeline-unbound.ores
        --diagnostic "pipeline actor routing requires declared pipeline topology")
    oreslang_contract_test(reject_pipeline_actor_wrong_route_direction
        source-actor-pipeline-wrong-direction.ores
        --diagnostic "pipeline route direction is not authorized for actor role")
    oreslang_contract_test(native_ir_three_actor_bidirectional_pipeline
        source-actor-pipeline-bidirectional.ores
        --ir "oreslang_source_actor_pipeline_emit_i64")
    oreslang_contract_test(reject_spread_nonexhaustive_receive
        source-tagged-int-spread-nonexhaustive.ores
        --diagnostic "nonexhaustive tagged receive match")
    if(ORE_CLANG)
        oreslang_contract_test(native_exec_three_actor_bidirectional_pipeline
            source-actor-pipeline-bidirectional.ores
            --run 43 --clang "${ORE_CLANG}"
            --runs 20
            --actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"
            --scheduler-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeActorScheduler.c"
            --source-actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/SourceActorBootstrap.c"
            --stderr "SOURCE_PIPELINE_ROUNDTRIP_PASS actors=3 typed=1 forward=2 backward=2 result=19")
    endif()
    oreslang_contract_test(source_actor_send_ir source-actor-send.ores
        --ir "oreslang_source_actor_send")
    oreslang_contract_test(source_actor_five_workers_ir source-actor-five-workers.ores
        --ir "oreslang_source_actor_spawn")
    oreslang_contract_test(reject_source_actor_send_no_handler source-actor-send-missing-handler.ores
        --diagnostic "actor send requires pub on_message(int)")
    oreslang_contract_test(reject_source_actor_send_bad_receiver source-actor-send-unknown-ref.ores
        --diagnostic "unknown actor message receiver")
    oreslang_contract_test(reject_source_actor_ref_as_payload source-actor-send-ref-as-payload.ores
        --diagnostic "unknown parameter")

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
        # First source-compiled nominal enum actor end-to-end test:
        # SHA-pinned Java-compatible schemas, closed pattern ordinal dispatch,
        # checked DATA sends and actor-local state effects.
        oreslang_contract_test(source_tagged_nominal_actor_ir source-tagged-payloadless-dispatch.ores
            --ir "oreslang_source_actor_spawn_tagged_variants")
        oreslang_contract_test(source_tagged_nominal_actor_java_schema source-tagged-payloadless-dispatch.ores
            --ir "oreslang.enum.e5a943e4263cf4ff7a59b28b63ffbac5a6a3279952ec16ca95f26193831c33bf")
        oreslang_contract_test(native_tagged_nominal_actor source-tagged-payloadless-dispatch.ores
            --run 43 --clang "${ORE_CLANG}" --runs 20
            --actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"
            --scheduler-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeActorScheduler.c"
            --source-actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/SourceActorBootstrap.c"
            --stderr "processed=3 last=14")
        # Int-payload tagged messages share identical wire shape across
        # distinct enum cases, yet dispatch to distinct typed receive arms.
        oreslang_contract_test(source_tagged_int_payload_ir source-tagged-int-payload-dispatch.ores
            --ir "oreslang_source_actor_tagged_i64")
        oreslang_contract_test(source_tagged_int_spread_ir source-tagged-int-spread-dispatch.ores
            --ir "oreslang_source_actor_tagged_i64")
        oreslang_contract_test(native_tagged_int_payload source-tagged-int-payload-dispatch.ores
            --run 43 --clang "${ORE_CLANG}" --runs 20
            --actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"
            --scheduler-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeActorScheduler.c"
            --source-actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/SourceActorBootstrap.c"
            --stderr "processed=3 last=23")
        oreslang_contract_test(native_tagged_int_spread_actor source-tagged-int-spread-dispatch.ores
            --run 43 --clang "${ORE_CLANG}" --runs 20
            --actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"
            --scheduler-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeActorScheduler.c"
            --source-actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/SourceActorBootstrap.c"
            --stderr "processed=3 last=23")
        oreslang_contract_test(native_asan_tagged_int_payload source-tagged-int-payload-dispatch.ores
            --run 43 --clang "${ORE_CLANG}" --sanitize --runs 5
            --actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"
            --scheduler-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeActorScheduler.c"
            --source-actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/SourceActorBootstrap.c"
            --stderr "processed=3 last=23")
        oreslang_contract_test(native_asan_tagged_nominal_actor source-tagged-payloadless-dispatch.ores
            --run 43 --clang "${ORE_CLANG}" --sanitize --runs 5
            --actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"
            --scheduler-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeActorScheduler.c"
            --source-actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/SourceActorBootstrap.c"
            --stderr "processed=3 last=14")
        oreslang_contract_test(native_actor_bool_match_dispatch message-receive-bool-match.ores
            --run 43 --clang "${ORE_CLANG}" --runs 30
            --actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"
            --scheduler-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeActorScheduler.c"
            --source-actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/SourceActorBootstrap.c"
            --stderr "processed=2 last=3")
        oreslang_contract_test(native_asan_actor_bool_match_dispatch message-receive-bool-match.ores
            --run 43 --clang "${ORE_CLANG}" --sanitize --runs 10
            --actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"
            --scheduler-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeActorScheduler.c"
            --source-actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/SourceActorBootstrap.c"
            --stderr "processed=2 last=3")
        oreslang_contract_test(native_actor_typed_bool_module message-pattern-module.ores
            --run 43 --clang "${ORE_CLANG}" --runs 20
            --actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"
            --scheduler-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeActorScheduler.c"
            --source-actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/SourceActorBootstrap.c"
            --stderr "processed=2 last=2")
        oreslang_contract_test(native_asan_actor_typed_bool_module message-pattern-module.ores
            --run 43 --clang "${ORE_CLANG}" --sanitize --runs 5
            --actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"
            --scheduler-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeActorScheduler.c"
            --source-actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/SourceActorBootstrap.c"
            --stderr "processed=2 last=2")
        oreslang_contract_test(native_actor_typed_int_receive message-pattern-native-int.ores
            --run 43 --clang "${ORE_CLANG}" --runs 20
            --actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"
            --scheduler-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeActorScheduler.c"
            --source-actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/SourceActorBootstrap.c"
            --stderr "processed=2 last=2")
        oreslang_contract_test(native_source_await_ready source-actor-await-ready.ores
            --run 43 --clang "${ORE_CLANG}" --runs 100
            --actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"
            --scheduler-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeActorScheduler.c"
            --source-actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/SourceActorBootstrap.c"
            --stderr "SOURCE_AWAIT_READY_CONTINUATION_PASS root_lane=1 real_future=1"
            --stderr-count 1)
        oreslang_contract_test(native_asan_source_await_ready source-actor-await-ready.ores
            --run 43 --clang "${ORE_CLANG}" --sanitize --runs 10
            --actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"
            --scheduler-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeActorScheduler.c"
            --source-actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/SourceActorBootstrap.c"
            --stderr "SOURCE_AWAIT_READY_CONTINUATION_PASS root_lane=1 real_future=1"
            --stderr-count 1)
        oreslang_contract_test(native_exec_source_actor_typed_ref source-actor-typed-ref.ores
            --run 43 --clang "${ORE_CLANG}"
            --actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"
            --scheduler-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeActorScheduler.c"
            --source-actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/SourceActorBootstrap.c"
            --stderr "processed=2 last=3")
        oreslang_contract_test(native_exec_source_actor_spawn source-actor-spawn-stop.ores
            --run 43 --clang "${ORE_CLANG}" --runs 40
            --actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"
            --scheduler-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeActorScheduler.c"
            --source-actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/SourceActorBootstrap.c"
            --stderr "SOURCE_ACTOR_SPAWN_STOP_VERIFIED")
        oreslang_contract_test(native_asan_source_actor_spawn source-actor-spawn-stop.ores
            --run 43 --clang "${ORE_CLANG}" --sanitize --runs 10
            --actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"
            --scheduler-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeActorScheduler.c"
            --source-actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/SourceActorBootstrap.c"
            --stderr "SOURCE_ACTOR_SPAWN_STOP_VERIFIED")
        oreslang_contract_test(native_exec_source_actor_send source-actor-send.ores
            --run 43 --clang "${ORE_CLANG}"
            --actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"
            --scheduler-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeActorScheduler.c"
            --source-actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/SourceActorBootstrap.c"
            --stderr "processed=2 last=3")
        oreslang_contract_test(native_asan_source_actor_send source-actor-send.ores
            --run 43 --clang "${ORE_CLANG}" --sanitize
            --actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"
            --scheduler-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeActorScheduler.c"
            --source-actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/SourceActorBootstrap.c"
            --stderr "processed=2 last=3")
        oreslang_contract_test(native_exec_source_actor_five_workers source-actor-five-workers.ores
            --run 43 --clang "${ORE_CLANG}"
            --actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"
            --scheduler-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeActorScheduler.c"
            --source-actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/SourceActorBootstrap.c"
            --stderr "processed=2 last=3" --stderr-count 5)
        oreslang_contract_test(native_asan_source_actor_five_workers source-actor-five-workers.ores
            --run 43 --clang "${ORE_CLANG}" --sanitize
            --actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"
            --scheduler-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeActorScheduler.c"
            --source-actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/SourceActorBootstrap.c"
            --stderr "processed=2 last=3" --stderr-count 5)
        oreslang_contract_test(native_exec_source_actorgroup source-actor-five-workers.ores
            --run 43 --clang "${ORE_CLANG}"
            --actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"
            --scheduler-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeActorScheduler.c"
            --source-actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/SourceActorBootstrap.c"
            --stderr "SOURCE_ACTOR_GROUP_VERIFIED active=0 slots=5 peak=5 carriers=4"
            --stderr-count 1)
        oreslang_contract_test(native_asan_source_actorgroup source-actor-five-workers.ores
            --run 43 --clang "${ORE_CLANG}" --sanitize
            --actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"
            --scheduler-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeActorScheduler.c"
            --source-actor-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/SourceActorBootstrap.c"
            --stderr "SOURCE_ACTOR_GROUP_VERIFIED active=0 slots=5 peak=5 carriers=4"
            --stderr-count 1)
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

    # Typed child terminal failure to reserved parent CONTROL slot.
    add_executable(oreslang-native-supervisor-panic-test
        tests/native_supervisor_panic_test.c
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c)
    target_include_directories(oreslang-native-supervisor-panic-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-supervisor-panic-test PRIVATE Threads::Threads)
    target_compile_options(oreslang-native-supervisor-panic-test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME native_supervisor_typed_panic_event
        COMMAND oreslang-native-supervisor-panic-test)
    set_tests_properties(native_supervisor_typed_panic_event PROPERTIES TIMEOUT 30)

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
    # Canonical actor transport; exact Java MessagePack v2 golden vectors.
    add_executable(oreslang-native-messagepack-v2-test
        tests/native_messagepack_actor_v2_test.c
        src/main/cpp/oreslang/runtime/MessagePackActorV2.c
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c)
    target_include_directories(oreslang-native-messagepack-v2-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-native-messagepack-v2-test PRIVATE Threads::Threads)
    add_test(NAME native_messagepack_v2_owned_actor_dispatch
        COMMAND oreslang-native-messagepack-v2-test)

    # Compiler-facing source actor ABI: scalar/wire callbacks share one
    # exclusive NativeActorScheduler lane and common actor-owned field heap.
    add_executable(oreslang-source-messagepack-v2-test
        tests/native_source_actor_msgpack_v2_test.c
        src/main/cpp/oreslang/runtime/SourceActorBootstrap.c
        src/main/cpp/oreslang/runtime/MessagePackActorV2.c
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c
        src/main/cpp/oreslang/runtime/NativeActorFuture.c
        src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c)
    target_include_directories(oreslang-source-messagepack-v2-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-source-messagepack-v2-test PRIVATE Threads::Threads)
    add_test(NAME native_source_actor_messagepack_v2_scheduler
        COMMAND oreslang-source-messagepack-v2-test)

    add_executable(oreslang-source-variants-v2-test
        tests/native_source_actor_variants_v2_test.c
        src/main/cpp/oreslang/runtime/SourceActorBootstrap.c
        src/main/cpp/oreslang/runtime/MessagePackActorV2.c
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c
        src/main/cpp/oreslang/runtime/NativeActorFuture.c
        src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c)
    target_include_directories(oreslang-source-variants-v2-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-source-variants-v2-test PRIVATE Threads::Threads)
    add_test(NAME native_source_actor_closed_variant_mailbox
        COMMAND oreslang-source-variants-v2-test)

    add_executable(oreslang-source-nominal-guard-v2-test
        tests/native_source_actor_nominal_guard_v2_test.c
        src/main/cpp/oreslang/runtime/SourceActorBootstrap.c
        src/main/cpp/oreslang/runtime/MessagePackActorV2.c
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c
        src/main/cpp/oreslang/runtime/NativeActorFuture.c
        src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c)
    target_include_directories(oreslang-source-nominal-guard-v2-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-source-nominal-guard-v2-test PRIVATE Threads::Threads)
    add_test(NAME native_source_actor_pinned_nominal_enum_identity
        COMMAND oreslang-source-nominal-guard-v2-test)

    add_executable(oreslang-source-scalar-compat-test
        tests/native_source_actor_scalar_compat_test.c
        src/main/cpp/oreslang/runtime/SourceActorBootstrap.c
        src/main/cpp/oreslang/runtime/MessagePackActorV2.c
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c
        src/main/cpp/oreslang/runtime/NativeActorFuture.c
        src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c)
    target_include_directories(oreslang-source-scalar-compat-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-source-scalar-compat-test PRIVATE Threads::Threads)
    add_test(NAME native_scalar_source_actor_backwards_compatibility
        COMMAND oreslang-source-scalar-compat-test)

    add_executable(oreslang-tagged-messagepack-v2-test
        tests/native_tagged_message_v2_conformance_test.c
        src/main/cpp/oreslang/runtime/MessagePackActorV2.c
        src/main/cpp/oreslang/runtime/NativeActorScheduler.c)
    target_include_directories(oreslang-tagged-messagepack-v2-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
    target_link_libraries(oreslang-tagged-messagepack-v2-test PRIVATE Threads::Threads)
    add_test(NAME native_tagged_enum_messagepack_v2_java_parity
        COMMAND oreslang-tagged-messagepack-v2-test)

endif()
