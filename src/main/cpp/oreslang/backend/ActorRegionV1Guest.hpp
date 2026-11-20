#pragma once

#include "oreslang/frontend/Parser.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Alignment.h"
#include "llvm/Support/raw_ostream.h"

namespace oreslang {

// FIRST COMPILER-BACKED GUEST SLICE (not general class/actor lowering):
// Parse/resolve/ownership validate with the existing native frontend before
// entering this opt-in backend. Admit exactly one stack-scoped class owner,
// 1..16 immutable literal int fields and a direct field-return from main.
// The emitted native guest itself calls ActorRegionV1's checked allocator,
// field accessors, drop, and quiescent retire functions. No interpreter/JIT.
// No actor scheduler, exception, suspension or multi-actor claim is implied.
// Preserve all prior LLVM lowering for unselected builds.
inline std::string emit_actor_region_v1_guest_ir(const Program& program) {
    const auto reject = [](const char* why) -> void {
        throw std::runtime_error(std::string("actor-region-v1 guest AOT rejects ") + why);
    };
    if (!program.imports.empty() || !program.actors.empty() ||
        !program.generators.empty() || !program.cooperative_loops.empty() ||
        !program.sum_types.empty() || !program.type_only_declarations.empty() ||
        !program.contracts.empty() || !program.conforms.empty() ||
        program.classes.size() != 1 || program.functions.size() != 1)
        reject("non-closed declarations or additional roots");

    const ClassDecl& cls = program.classes.front();
    if (cls.explicit_constructor || !cls.constructor_parameters.empty() ||
        !cls.methods.empty() || cls.fields.empty() || cls.fields.size() > 16)
        reject("constructors, methods or unsupported class layout");
    for (const auto& field : cls.fields) {
        if (field.scalar_type != ScalarType::I64 || !field.public_member ||
            !field.final_field || field.mutable_referent ||
            field.int_array || !field.class_name.empty() ||
            field.constructor_argument || !field.sum_type.empty() ||
            !field.initializer_expression ||
            field.initializer_expression->kind != Expression::Kind::Integer)
            reject("nonliteral, nonpublic or mutable class field");
        // The first version accepts a single integer leaf, not an expression
        // graph which could hide calls, effects, ownership, or runtime work.
        const Expression& expr = *field.initializer_expression;
        if (expr.left || expr.right || expr.condition || expr.when_true ||
            expr.when_false || !expr.arguments.empty())
            reject("unmodeled class initializer expression");
    }

    const Function& main_fn = program.functions.front();
    if (main_fn.name != "main" || !main_fn.parameters.empty() ||
        main_fn.locals.size() != 1 || !main_fn.statements.empty() ||
        !main_fn.early_returns.empty() || !main_fn.actor_spawns.empty() ||
        !main_fn.actor_actions.empty() || !main_fn.actor_stops.empty() ||
        !main_fn.actor_sends.empty() || !main_fn.actor_field_writes.empty() ||
        !main_fn.source_tasks.empty() || !main_fn.future_pipelines.empty() ||
        main_fn.scalar_while || main_fn.caught_throw_value ||
        !main_fn.caught_throw_defers.empty() ||
        main_fn.async_source_i64 || main_fn.returns_void ||
        main_fn.return_type != ScalarType::I64 || !main_fn.return_class.empty() ||
        !main_fn.return_sum.empty() || !main_fn.generic_parameters.empty())
        reject("non-synchronous main/0 or nonlexical control flow");

    const LocalBinding& owner = main_fn.locals.front();
    if (!owner.new_class || owner.class_name != cls.name ||
        owner.record_view || owner.move_class || owner.heap_owned ||
        owner.int_array || owner.owner_mutable || owner.field_mutable ||
        owner.assignable || !owner.constructor_arguments.empty() ||
        owner.initializer || owner.name.empty())
        reject("non-fresh, mutable or escaping class owner");
    if (!main_fn.result ||
        main_fn.result->kind != Expression::Kind::Field ||
        !main_fn.result->left ||
        main_fn.result->left->kind != Expression::Kind::Variable ||
        main_fn.result->left->name != owner.name ||
        main_fn.result->right || main_fn.result->condition ||
        !main_fn.result->arguments.empty())
        reject("return outside the original class owner's scalar field");

    const auto field_it = std::find_if(cls.fields.begin(), cls.fields.end(),
        [&](const ClassField& field) { return field.name == main_fn.result->name; });
    if (field_it == cls.fields.end())
        reject("unknown return field");
    const std::uint32_t result_index =
        static_cast<std::uint32_t>(field_it - cls.fields.begin());
    const std::uint32_t byte_length =
        static_cast<std::uint32_t>(cls.fields.size() * sizeof(std::int64_t));

    llvm::LLVMContext context;
    llvm::Module module("atto.actor-region-v1.guest", context);
    llvm::IRBuilder<> builder(context);
    auto* i8 = llvm::Type::getInt8Ty(context);
    auto* i32 = llvm::Type::getInt32Ty(context);
    auto* i64 = llvm::Type::getInt64Ty(context);
    // LLVM 18 removed Type::getInt8PtrTy; use the opaque address-space-0 pointer.
    auto* ptr = llvm::PointerType::get(context, 0);
    auto* void_type = llvm::Type::getVoidTy(context);
    const auto native = [&](const char* symbol, llvm::Type* output,
                            std::vector<llvm::Type*> arguments) {
        return module.getOrInsertFunction(symbol,
            llvm::FunctionType::get(output, arguments, false));
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
    auto* block = llvm::BasicBlock::Create(context, "entry", entry);
    builder.SetInsertPoint(block);

    // The trusted native compiler owns and pins these sidecars on the native
    // stack. The guest's 32-bit offset NEVER carries a forged capability.
    // Checked C shims check the exact ABI structure sizes on their side.
    auto* turn_storage = builder.CreateAlloca(i8, builder.getInt32(16),
                                              "region.turn.sidecar");
    turn_storage->setAlignment(llvm::Align(8));
    auto* ref_storage = builder.CreateAlloca(i8, builder.getInt32(32),
                                             "region.ref.sidecar");
    ref_storage->setAlignment(llvm::Align(8));

    auto* region = builder.CreateCall(open, {builder.getInt64(1),
         builder.getInt64(1), builder.getInt32(1),
         builder.getInt32(byte_length), builder.getInt32(64)}, "actor.region");
    builder.CreateCall(enter, {region, builder.getInt64(1),
                               builder.getInt64(1), turn_storage});
    builder.CreateCall(alloc, {region, turn_storage,
                               builder.getInt32(byte_length), ref_storage});
    for (std::uint32_t n = 0; n < cls.fields.size(); ++n) {
        const auto value = cls.fields[n].initial;
        builder.CreateCall(write, {region, turn_storage, ref_storage,
            builder.getInt32(n * sizeof(std::int64_t)),
            llvm::ConstantInt::get(i64, static_cast<std::uint64_t>(value))});
    }
    auto* result = builder.CreateCall(read, {region, turn_storage, ref_storage,
         builder.getInt32(result_index * sizeof(std::int64_t))}, "guest.field");
    // Exactly one destructor is emitted before turn release on the sole
    // normal return. Early return, throw and await are rejected above until
    // path-sensitive drop insertion and supervisor quiescence are proven.
    builder.CreateCall(drop, {region, turn_storage, ref_storage});
    builder.CreateCall(leave, {region, turn_storage});
    builder.CreateCall(retire, {region});
    auto* freed = builder.CreateCall(destroy, {region}, "region.reclaimed");
    auto* ok = builder.CreateICmpEQ(freed, builder.getInt32(1));
    auto* done = llvm::BasicBlock::Create(context, "owned.cleaned", entry);
    auto* failed = llvm::BasicBlock::Create(context, "cleanup.failed", entry);
    builder.CreateCondBr(ok, done, failed);
    builder.SetInsertPoint(failed);
    builder.CreateCall(abort_fn);
    builder.CreateUnreachable();
    builder.SetInsertPoint(done);
    builder.CreateRet(builder.CreateTrunc(result, i32));

    std::string verification;
    llvm::raw_string_ostream error(verification);
    if (llvm::verifyModule(module, &error))
        throw std::runtime_error("actor-region-v1 guest AOT generated invalid LLVM IR");
    std::string ir;
    llvm::raw_string_ostream output(ir);
    module.print(output, nullptr);
    output.flush();
    return ir;
}

} // namespace oreslang
