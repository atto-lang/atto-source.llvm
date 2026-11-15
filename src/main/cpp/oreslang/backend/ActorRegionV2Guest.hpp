#pragma once

#include "oreslang/backend/ActorRegionV1Guest.hpp"
#include "oreslang/backend/OwnershipDataflowV1.hpp"
#include "oreslang/backend/OwnershipSourceLoweringV1.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace oreslang {

// Shared cleanup plan for lexical ActorRegionV1 owners. This is deliberately
// separate from LLVM emission: every supported normal CFG exit consumes the
// same reverse-order plan exactly once. Future exit kinds (throw, suspend,
// cancel, unwind) must be explicitly added to the plan's verified exit set;
// they cannot silently inherit normal cleanup.
struct ActorRegionCleanupPlanV1 {
    std::vector<std::string> owners;
    std::vector<std::size_t> cleanup_order;

    void add_owner(const std::string& name) {
        if (name.empty() || owners.size() >= 8 ||
            std::find(owners.begin(), owners.end(), name) != owners.end())
            throw std::runtime_error("actor-region cleanup plan: invalid/duplicate owner");
        owners.push_back(name);
    }
    void seal_normal_exits(unsigned supported_normal_exits) {
        if (owners.empty() || supported_normal_exits == 0 ||
            supported_normal_exits > 2)
            throw std::runtime_error("actor-region cleanup plan: unsupported exit graph");
        cleanup_order.clear();
        for (unsigned i = static_cast<unsigned>(owners.size()); i > 0; --i)
            cleanup_order.push_back(i - 1);
    }
    void verify_normal_exits_with_dataflow(unsigned supported_normal_exits) {
        if (owners.empty() || supported_normal_exits != 2)
            throw std::runtime_error("actor-region cleanup plan: unsupported exit graph");
        OwnershipV1Graph graph;
        graph.owners = owners;
        graph.blocks.resize(3);
        for (std::size_t i = 0; i < owners.size(); ++i)
            graph.blocks[0].operations.push_back(
                {OwnershipV1OpKind::Create, i});
        graph.blocks[0].successors = {1, 2};
        graph.blocks[1].operations.push_back(
            {OwnershipV1OpKind::ExitNormal, 0});
        graph.blocks[2].operations.push_back(
            {OwnershipV1OpKind::ExitNormal, 0});
        const auto analysis = OwnershipDataflowV1::analyze(graph);
        if (analysis.normal_exits.size() != supported_normal_exits)
            throw std::runtime_error("actor-region cleanup plan: CFG exit proof mismatch");
        const auto verified_order = analysis.normal_exits.front().cleanup_reverse_order;
        for (const auto& exit : analysis.normal_exits)
            if (exit.cleanup_reverse_order != verified_order)
                throw std::runtime_error("actor-region cleanup plan: cleanup differs by exit");
        if (verified_order != cleanup_order)
            throw std::runtime_error("actor-region cleanup plan: legacy order disagrees with dataflow");
        // The verified analysis result is authoritative for LLVM emission.
        cleanup_order = verified_order;
    }
};

// ActorRegionV2 is an opt-in AOT code-generation *slice* over the unchanged
// ActorRegionV1 native ABI. It admits two lexical owners in ONE authenticated
// root actor region and one pure guarded early return. Both successor blocks
// converge on ONE compiler-generated drop/retire block; this is the first
// path-sensitive exactly-once destructor proof, not arbitrary CFG lowering.
// No source actor scheduling, exceptions, await, FFI or dynamic dispatch.
inline std::string emit_actor_region_v2_guest_ir(const Program& program) {
    const auto reject = [](const char* why) -> void {
        throw std::runtime_error(
            std::string("actor-region-v2 guest AOT rejects ") + why);
    };
    if (!program.imports.empty() || !program.actors.empty() ||
        !program.generators.empty() || !program.cooperative_loops.empty() ||
        !program.sum_types.empty() || !program.type_only_declarations.empty() ||
        !program.type_constraint_descriptors.empty() ||
        !program.contracts.empty() || !program.conforms.empty() ||
        program.classes.size() != 1 || program.functions.size() != 1)
        reject("non-closed source graph or unmodeled declarations");

    const ClassDecl& cls = program.classes.front();
    if (cls.explicit_constructor || !cls.constructor_parameters.empty() ||
        !cls.constructor_parameter_types.empty() || !cls.methods.empty() ||
        cls.fields.empty() || cls.fields.size() > 16)
        reject("methods, constructors or unsupported class layout");
    for (const ClassField& field : cls.fields) {
        const Expression* initial = field.initializer_expression.get();
        if (!field.public_member || !field.final_field ||
            field.mutable_referent || field.scalar_type != ScalarType::I64 ||
            field.int_array || !field.class_name.empty() ||
            field.constructor_argument || !field.sum_type.empty() ||
            !initial || initial->kind != Expression::Kind::Integer ||
            initial->left || initial->right || initial->condition ||
            initial->when_true || initial->when_false ||
            initial->condition_lhs || initial->condition_rhs ||
            !initial->arguments.empty() ||
            !initial->argument_names.empty() ||
            !initial->type_arguments.empty() || initial->type_arguments_present)
            reject("effectful, mutable or nonliteral class field");
    }

    const Function& fn = program.functions.front();
    if (fn.name != "main" || !fn.parameters.empty() ||
        fn.locals.empty() || fn.locals.size() > 8 || fn.early_returns.size() != 1 ||
        !fn.statements.empty() || !fn.actor_spawns.empty() ||
        !fn.actor_actions.empty() || !fn.actor_stops.empty() ||
        !fn.actor_sends.empty() || !fn.actor_field_writes.empty() ||
        !fn.source_tasks.empty() || !fn.future_pipelines.empty() ||
        fn.scalar_while || fn.caught_throw_value ||
        !fn.caught_throw_defers.empty() || fn.async_source_i64 ||
        !fn.ready_await_actor.empty() || !fn.done_await_actor.empty() ||
        fn.returns_void || fn.return_type != ScalarType::I64 ||
        !fn.return_class.empty() || !fn.return_sum.empty() ||
        !fn.generic_parameters.empty() || !fn.result)
        reject("nonlexical owner lifetime or unmodeled control flow");

    // A checked AST is not permission for implicit effect promotion or
    // hidden linker-owned function entrypoints. The actor-region subset is
    // strictly synchronous, effect-free and source-defined.
    if (fn.has_declared_effects || fn.native_internal ||
        !fn.method_owner.empty() || !fn.c_export_u32_symbol.empty() ||
        !fn.pending_return_type_alias.empty() ||
        !fn.ready_future_name.empty() || !fn.ready_future_actor.empty() ||
        !fn.catch_binding.empty()) {
        reject("hidden effects, native entrypoint or unmodeled scope");
    }

    ActorRegionCleanupPlanV1 cleanup_plan;
    for (const LocalBinding& owner : fn.locals) {
        if (!owner.new_class || owner.class_name != cls.name ||
            owner.name.empty() || owner.record_view || owner.move_class ||
            owner.heap_owned || owner.int_array || owner.owner_mutable ||
            owner.field_mutable || owner.assignable ||
            !owner.constructor_arguments.empty() || owner.initializer ||
            !owner.task_await_name.empty() || !owner.future_await_name.empty())
            reject("non-fresh, escaping or mutable owner");
        cleanup_plan.add_owner(owner.name);
    }
    cleanup_plan.seal_normal_exits(2);
    // Analyze the actual resolver/typechecker-owned Program AST, rather than
    // only a synthetic list of owners. The source adapter maps its lexical
    // allocations, field reads, guard and both return expressions to CFG ops.
    const auto source_ownership =
        OwnershipSourceLoweringV1::lower_guarded_owned_class_function(
            program, fn, cls);
    if (source_ownership.normal_exits.size() != 2)
        reject("source ownership CFG did not prove both return paths");
    const auto& source_cleanup =
        source_ownership.normal_exits.front().cleanup_reverse_order;
    for (const auto& exit : source_ownership.normal_exits)
        if (exit.cleanup_reverse_order != source_cleanup)
            reject("source ownership CFG has path-dependent unmerged cleanup");
    cleanup_plan.verify_normal_exits_with_dataflow(2);
    if (cleanup_plan.cleanup_order != source_cleanup)
        reject("source and backend cleanup plans disagree");
    cleanup_plan.cleanup_order = source_cleanup;
    // The source checker currently produces these metadata fields in a
    // limited class grammar. Fail closed if a future grammar adds ownership
    // transfers, sum payloads or unvalidated aliases alongside 'new'.
    for (const LocalBinding& owner : fn.locals) {
        if (!owner.move_source.empty() || !owner.sum_type.empty() ||
            !owner.pending_type_alias.empty() ||
            !owner.constructor_arguments.empty())
            reject("unmodeled owner transfer or hidden constructor");
    }
    // add_owner already rejects duplicate owner identities.
    // A four-byte compact offset cannot authorize field access by itself.
    // Resolve every source place to an ORIGINAL trusted owner sidecar before
    // generating LLVM IR; no forged, inherited or dynamically named places.
    struct FieldAccess { unsigned owner, field; };
    const auto checked_field = [&](const Expression* expression) -> FieldAccess {
        if (!expression || expression->kind != Expression::Kind::Field ||
            !expression->left ||
            expression->left->kind != Expression::Kind::Variable ||
            expression->left->left || expression->left->right ||
            expression->right || expression->condition ||
            expression->when_true || expression->when_false ||
            !expression->arguments.empty() || expression->name.empty())
            reject("non-direct scalar class field access");
        unsigned owner_index = static_cast<unsigned>(fn.locals.size());
        for (unsigned i = 0; i < fn.locals.size(); ++i) {
            if (fn.locals[i].name == expression->left->name)
                owner_index = i;
        }
        if (owner_index == fn.locals.size()) reject("field uses unowned receiver");
        unsigned field_index = static_cast<unsigned>(cls.fields.size());
        for (unsigned i = 0; i < cls.fields.size(); ++i) {
            if (cls.fields[i].name == expression->name)
                field_index = i;
        }
        if (field_index == cls.fields.size())
            reject("unknown or inaccessible field");
        return {owner_index, field_index};
    };

    const GuardedReturn& guard = fn.early_returns.front();
    if (!guard.condition || guard.condition->kind != Expression::Kind::Equal ||
        !guard.condition->left || !guard.condition->right ||
        guard.condition->right->kind != Expression::Kind::Integer ||
        guard.condition->right->left || guard.condition->right->right)
        reject("guard is not a pure equality with an integer literal");
    // The admitted equality itself may only contain the two operands.
    // Reject any additional AST fields if future parsers extend the shape.
    if (guard.condition->condition || guard.condition->when_true ||
        guard.condition->when_false || guard.condition->condition_lhs ||
        guard.condition->condition_rhs || !guard.condition->arguments.empty() ||
        guard.condition->type_arguments_present ||
        !guard.condition->type_arguments.empty() ||
        !guard.condition->argument_names.empty() ||
        guard.condition->right->condition ||
        guard.condition->right->when_true ||
        guard.condition->right->when_false ||
        !guard.condition->right->arguments.empty()) {
        reject("unmodeled guard expressions or side effects");
    }
    const FieldAccess guard_field = checked_field(guard.condition->left.get());
    const FieldAccess early_value = checked_field(guard.result.get());
    const FieldAccess normal_value = checked_field(fn.result.get());
    // This first guest uses the OS process exit code as its observable
    // result. POSIX exposes at most 8 bits of that code. Do not silently
    // truncate a 64-bit AttoLang int and misreport source semantics; the
    // future full-AOT launcher must return a true i64 guest result via a
    // versioned typed ABI rather than using process exit status.
    for (const FieldAccess returned : {early_value, normal_value}) {
        const auto value = cls.fields[returned.field].initial;
        if (value < 0 || value > 255)
            reject("return field outside exact host exit-status range");
    }

    const std::int64_t compare_literal = guard.condition->right->integer;

    llvm::LLVMContext context;
    llvm::Module module("atto.actor-region-v2.guarded-owners", context);
    llvm::IRBuilder<> builder(context);
    auto* i8 = llvm::Type::getInt8Ty(context);
    auto* i32 = llvm::Type::getInt32Ty(context);
    auto* i64 = llvm::Type::getInt64Ty(context);
    // LLVM 18 removed Type::getInt8PtrTy; use the opaque address-space-0 pointer.
    auto* ptr = llvm::PointerType::get(context, 0);
    auto* void_type = llvm::Type::getVoidTy(context);
    const auto native = [&](const char* name, llvm::Type* output,
                            std::vector<llvm::Type*> args) {
        return module.getOrInsertFunction(
            name, llvm::FunctionType::get(output, args, false));
    };
    const auto open = native("ores_actor_region_v1_open", ptr,
        {i64, i64, i32, i32, i32});
    const auto enter = native("ores_actor_region_v1_compiler_enter", void_type,
        {ptr, i64, i64, ptr});
    const auto alloc = native("ores_actor_region_v1_compiler_alloc", void_type,
        {ptr, ptr, i32, ptr});
    const auto write = native("ores_actor_region_v1_compiler_write_i64", void_type,
        {ptr, ptr, ptr, i32, i64});
    const auto read = native("ores_actor_region_v1_compiler_read_i64", i64,
        {ptr, ptr, ptr, i32});
    const auto drop = native("ores_actor_region_v1_compiler_drop", void_type,
        {ptr, ptr, ptr});
    const auto leave = native("ores_actor_region_v1_compiler_leave", void_type,
        {ptr, ptr});
    const auto retire = native("ores_actor_region_v1_retire", void_type, {ptr});
    const auto destroy = native("ores_actor_region_v1_try_destroy", i32, {ptr});
    const auto abort_fn = native("abort", void_type, {});

    auto* entry = llvm::Function::Create(
        llvm::FunctionType::get(i32, false),
        llvm::GlobalValue::ExternalLinkage, "main", module);
    auto* start = llvm::BasicBlock::Create(context, "entry", entry);
    builder.SetInsertPoint(start);

    // The same trusted native turn owns BOTH class allocations. The
    // compiler never stores generation/actor authority inside guest fields.
    const std::uint32_t slot_bytes =
        static_cast<std::uint32_t>(cls.fields.size() * sizeof(std::int64_t));
    auto* turn_storage = builder.CreateAlloca(i8, builder.getInt32(16),
                                             "turn.sidecar");
    turn_storage->setAlignment(llvm::Align(8));
    std::vector<llvm::Value*> refs;
    refs.reserve(cleanup_plan.owners.size());
    for (const auto& name : cleanup_plan.owners) {
        auto* storage = builder.CreateAlloca(i8, builder.getInt32(32),
                                             name + ".sidecar");
        storage->setAlignment(llvm::Align(8));
        refs.push_back(storage);
    }
    auto* region = builder.CreateCall(open, {builder.getInt64(1),
        builder.getInt64(1), builder.getInt32(
            static_cast<std::uint32_t>(cleanup_plan.owners.size())),
        builder.getInt32(slot_bytes), builder.getInt32(64)}, "actor.region");
    builder.CreateCall(enter, {region, builder.getInt64(1),
                               builder.getInt64(1), turn_storage});
    for (unsigned i = 0; i < cleanup_plan.owners.size(); ++i) {
        builder.CreateCall(alloc, {region, turn_storage,
                                   builder.getInt32(slot_bytes), refs[i]});
        for (unsigned field = 0; field < cls.fields.size(); ++field) {
            builder.CreateCall(write, {region, turn_storage, refs[i],
                builder.getInt32(field * sizeof(std::int64_t)),
                llvm::ConstantInt::get(
                    i64, static_cast<std::uint64_t>(cls.fields[field].initial))});
        }
    }
    const auto read_field = [&](FieldAccess field) -> llvm::Value* {
        return builder.CreateCall(read, {region, turn_storage,
            refs[field.owner],
            builder.getInt32(field.field * sizeof(std::int64_t))},
            "owner.field.read");
    };
    auto* guard_value = read_field(guard_field);
    auto* matched = builder.CreateICmpEQ(guard_value, llvm::ConstantInt::get(
        i64, static_cast<std::uint64_t>(compare_literal)));
    auto* early = llvm::BasicBlock::Create(context, "guard.return", entry);
    auto* normal = llvm::BasicBlock::Create(context, "normal.return", entry);
    auto* cleanup = llvm::BasicBlock::Create(context, "shared.owner.cleanup", entry);
    builder.CreateCondBr(matched, early, normal);

    builder.SetInsertPoint(early);
    llvm::Value* early_result = read_field(early_value);
    builder.CreateBr(cleanup);

    builder.SetInsertPoint(normal);
    llvm::Value* normal_result = read_field(normal_value);
    builder.CreateBr(cleanup);

    builder.SetInsertPoint(cleanup);
    auto* result = builder.CreatePHI(i64, 2, "return.snapshot");
    result->addIncoming(early_result, early);
    result->addIncoming(normal_result, normal);
    // Both return paths converge here: drop in reverse lexical ownership
    // order, exactly once each, AFTER the scalar return value is snapshotted.
    // This does not cover throw/suspend/cancel; those are rejected above.
    for (const std::size_t owner_index : cleanup_plan.cleanup_order)
        builder.CreateCall(drop, {region, turn_storage, refs[owner_index]});
    builder.CreateCall(leave, {region, turn_storage});
    builder.CreateCall(retire, {region});
    auto* freed = builder.CreateCall(destroy, {region}, "region.reclaimed");
    auto* released = builder.CreateICmpEQ(freed, builder.getInt32(1));
    auto* completed = llvm::BasicBlock::Create(context, "cleanup.complete", entry);
    auto* failed = llvm::BasicBlock::Create(context, "cleanup.failed", entry);
    builder.CreateCondBr(released, completed, failed);
    builder.SetInsertPoint(failed);
    builder.CreateCall(abort_fn);
    builder.CreateUnreachable();
    builder.SetInsertPoint(completed);
    builder.CreateRet(builder.CreateTrunc(result, i32));

    std::string errors;
    llvm::raw_string_ostream error_stream(errors);
    if (llvm::verifyModule(module, &error_stream))
        throw std::runtime_error("actor-region-v2 generated invalid LLVM IR");
    std::string text;
    llvm::raw_string_ostream output(text);
    module.print(output, nullptr);
    output.flush();
    return text;
}

} // namespace oreslang
