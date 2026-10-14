#include "oreslang/backend/LlvmCodegen.hpp"
#include "oreslang/frontend/CallableSelector.hpp"
#include "oreslang/runtime/TargetAbi.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "llvm/Config/llvm-config.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

namespace oreslang {

// Guest identifiers exclude $, so native selector suffixes cannot alias source names.
namespace {

std::string field_key(const std::string& receiver, const std::string& field) {
    return receiver + "." + field;
}

// Owned i64 parameters receive stack slots so the only permitted loans can
// pass their addresses through the call ABI. Borrowed inputs already hold
// caller-owned slot pointers: passing them to nested borrowed parameters is
// a checked, call-scoped reborrow with no new allocation. Neither address
// can escape as a result:
// native functions return only i64, and the ownership pass restricts rt borrow
// to nonescaping call arguments.
struct RegionMoveLowering {
    llvm::Value* region = nullptr;
    llvm::FunctionCallee read;
    llvm::FunctionCallee drop;
    const std::unordered_map<std::string, llvm::Value*>* live_flags = nullptr;
    llvm::Value* array_region = nullptr;
    llvm::FunctionCallee array_add, array_get, array_set, array_length;
    const std::unordered_set<std::string>* array_names = nullptr;
    const std::unordered_set<std::string>* array_fields = nullptr;
    llvm::BasicBlock* resource_failure = nullptr;
    // Compiler-owned error tag survives every owner-drop and region close.
    // Values exactly match OresNativeIntArrayStatus; not a guest Result yet.
    llvm::Value* array_failure_status = nullptr;
    const std::unordered_map<std::string, llvm::Value*>* owned_failure_handles = nullptr;
};

// Only a lexical named array or a direct class-owned array field is legal.
// The type/ownership pass must already have certified this receiver.
llvm::Value* array_handle_address(const Expression& receiver,
                                   const std::unordered_map<std::string, llvm::Value*>& storage,
                                   const std::unordered_map<std::string, llvm::Value*>& fields,
                                   const RegionMoveLowering* moves) {
    if (!moves) throw std::runtime_error("native Array<int> operation lacks region");
    if (receiver.kind == Expression::Kind::Variable && moves->array_names &&
            moves->array_names->count(receiver.name))
        return storage.at(receiver.name);
    if (receiver.kind == Expression::Kind::Field && receiver.left &&
            receiver.left->kind == Expression::Kind::Variable && moves->array_fields) {
        const std::string key = field_key(receiver.left->name, receiver.name);
        if (moves->array_fields->count(key)) return fields.at(key);
    }
    throw std::runtime_error("native Array<int> receiver is not an owned place");
}

llvm::Value* lower(const Expression& expr,
                   llvm::IRBuilder<>& builder,
                   llvm::Type* i64,
                   const std::unordered_map<std::string, llvm::Value*>& storage,
                   const std::unordered_map<std::string, llvm::Value*>& field_storage,
                   const std::unordered_map<std::string, llvm::Function*>& functions,
                   const RegionMoveLowering* moves = nullptr) {
    switch (expr.kind) {
        case Expression::Kind::Integer:
            return llvm::ConstantInt::getSigned(i64, expr.integer);
        case Expression::Kind::Variable: {
            llvm::Value* stored = builder.CreateLoad(i64, storage.at(expr.name),
                expr.name + ".read");
            if (moves && moves->live_flags &&
                    moves->live_flags->count(expr.name))
                return builder.CreateCall(moves->read, {moves->region, stored},
                    expr.name + ".owned.value");
            return stored;
        }
        case Expression::Kind::Field:
            if (expr.name == "size" && expr.left &&
                    expr.left->kind == Expression::Kind::Field) {
                llvm::Value* handle = builder.CreateLoad(i64,
                    array_handle_address(*expr.left, storage, field_storage, moves),
                    "class.array.size.handle");
                return builder.CreateCall(moves->array_length,
                    {moves->array_region, handle}, "class.array.size");
            }
            if (moves && moves->array_names && expr.left &&
                    expr.left->kind == Expression::Kind::Variable &&
                    moves->array_names->count(expr.left->name)) {
                if (expr.name != "size")
                    throw std::runtime_error("native Array<int> supports only size field");
                llvm::Value* handle = builder.CreateLoad(i64,
                    storage.at(expr.left->name), expr.left->name + ".array.handle");
                return builder.CreateCall(moves->array_length,
                    {moves->array_region, handle}, expr.left->name + ".size");
            }
            return builder.CreateLoad(i64, field_storage.at(field_key(expr.left->name, expr.name)),
                                      expr.left->name + "." + expr.name + ".read");
        case Expression::Kind::ArrayCall: {
            if (!expr.left)
                throw std::runtime_error("native Array<int> call has no owned receiver");
            llvm::Value* handle = builder.CreateLoad(i64,
                array_handle_address(*expr.left, storage, field_storage, moves),
                "owned.array.call.handle");
            std::vector<llvm::Value*> args;
            for (const auto& arg : expr.arguments)
                args.push_back(lower(*arg, builder, i64, storage, field_storage, functions, moves));
            if (expr.name == "get" && args.size() == 1)
                return builder.CreateCall(moves->array_get,
                    {moves->array_region, handle, args[0]}, "array.get");
            if (expr.name == "add" && args.size() == 1) {
                llvm::Value* status = builder.CreateCall(moves->array_add,
                    {moves->array_region, handle, args[0]}, "array.add.status");
                llvm::BasicBlock* proceed = llvm::BasicBlock::Create(
                    builder.getContext(), "array.add.ok", builder.GetInsertBlock()->getParent());
                llvm::BasicBlock* failed = llvm::BasicBlock::Create(
                    builder.getContext(), "array.add.error", builder.GetInsertBlock()->getParent());
                builder.CreateCondBr(builder.CreateICmpEQ(status,
                    llvm::ConstantInt::get(status->getType(), 0), "array.add.success"),
                    proceed, failed);
                builder.SetInsertPoint(failed);
                builder.CreateStore(status, moves->array_failure_status);
                builder.CreateBr(moves->resource_failure);
                builder.SetInsertPoint(proceed);
                return llvm::ConstantInt::get(i64, 0);
            }
            if (expr.name == "set" && args.size() == 2)
                return builder.CreateCall(moves->array_set,
                    {moves->array_region, handle, args[0], args[1]});
            throw std::runtime_error("unsupported Array<int> native method");
        }
        case Expression::Kind::Borrow:
        case Expression::Kind::BorrowMut: {
            // Heap owner storage is a HANDLE, never a borrowable i64 address.
            // A shared call-scoped loan reads into a temporary scalar stack
            // slot that cannot escape this synchronous call. Mutable loans
            // require writeback and are conservatively rejected by checker.
            if (moves && moves->live_flags &&
                    moves->live_flags->count(expr.left->name)) {
                if (expr.kind == Expression::Kind::BorrowMut)
                    throw std::runtime_error("native owned-int mutable borrow needs handle-aware writeback");
                llvm::Value* handle = builder.CreateLoad(i64,
                    storage.at(expr.left->name), "borrow.owner.handle");
                llvm::Value* snapshot = builder.CreateCall(moves->read,
                    {moves->region, handle}, "borrow.owner.value");
                llvm::Value* temporary = builder.CreateAlloca(i64, nullptr,
                    "borrow.owner.snapshot");
                builder.CreateStore(snapshot, temporary);
                return temporary;
            }
            return storage.at(expr.left->name);
        }
        case Expression::Kind::Negate:
            return builder.CreateNeg(lower(*expr.left, builder, i64, storage, field_storage, functions, moves), "neg");
        case Expression::Kind::Copy:
        case Expression::Kind::Share:
            return lower(*expr.left, builder, i64, storage, field_storage, functions, moves);
        case Expression::Kind::Take: {
            // Native owned-int transfer reads the scalar before destroying
            // the old heap allocation. It then disables the exit-time drop.
            // Field takes of trivially destructible i64 stay simple loads.
            if (moves && moves->live_flags && expr.left &&
                    expr.left->kind == Expression::Kind::Variable) {
                const auto found = moves->live_flags->find(expr.left->name);
                if (found != moves->live_flags->end()) {
                    llvm::Value* value = lower(*expr.left, builder, i64,
                        storage, field_storage, functions, moves);
                    llvm::Value* handle = builder.CreateLoad(i64,
                        storage.at(expr.left->name), "take.owned.handle");
                    builder.CreateCall(moves->drop, {moves->region, handle});
                    builder.CreateStore(llvm::ConstantInt::get(i64, 0),
                        storage.at(expr.left->name));
                    builder.CreateStore(builder.getFalse(), found->second);
                    if (moves->owned_failure_handles) {
                        const auto shadow = moves->owned_failure_handles->find(expr.left->name);
                        if (shadow != moves->owned_failure_handles->end())
                            builder.CreateStore(llvm::ConstantInt::get(i64, 0), shadow->second);
                    }
                    return value;
                }
            }
            return lower(*expr.left, builder, i64, storage, field_storage, functions, moves);
        }
        case Expression::Kind::Add: {
            llvm::Value* left = lower(*expr.left, builder, i64, storage, field_storage, functions, moves);
            llvm::Value* right = lower(*expr.right, builder, i64, storage, field_storage, functions, moves);
            return builder.CreateAdd(left, right, "add");
        }
        case Expression::Kind::Subtract: {
            llvm::Value* left = lower(*expr.left, builder, i64, storage, field_storage, functions, moves);
            llvm::Value* right = lower(*expr.right, builder, i64, storage, field_storage, functions, moves);
            return builder.CreateSub(left, right, "sub");
        }
        case Expression::Kind::Multiply: {
            llvm::Value* left = lower(*expr.left, builder, i64, storage, field_storage, functions, moves);
            llvm::Value* right = lower(*expr.right, builder, i64, storage, field_storage, functions, moves);
            return builder.CreateMul(left, right, "mul");
        }
        case Expression::Kind::Boolean:
            // i1 values may only be lowered by verified typed bool returns,
            // never by the generic native i64 expression code generator.
            throw std::runtime_error("typed bool cannot be lowered as native i64");
        case Expression::Kind::ModuleCall:
            // Import AST has no native symbol until typed stdlib linking is
            // certified. Do not create a fake user/host function.
            throw std::runtime_error(
                "native Assert module call awaits typed source-owned linking");
        case Expression::Kind::Call: {
            llvm::Function* callee = functions.at(native_function_slot(expr.name, expr.arguments.size()));
            if (callee->getReturnType()->isIntegerTy(1))
                throw std::runtime_error("typed bool call requires bool-aware expression lowering");
            if (callee->arg_size() != expr.arguments.size()) {
                throw std::runtime_error("native named call argument count mismatch");
            }
            if (!expr.argument_names.empty()
                    && expr.argument_names.size() != expr.arguments.size()) {
                throw std::runtime_error("invalid native call label layout");
            }
            std::vector<llvm::Value*> ordered(expr.arguments.size(), nullptr);
            std::vector<bool> filled(expr.arguments.size(), false);
            std::size_t positional = 0;
            bool named_seen = false;
            for (std::size_t i = 0; i < expr.arguments.size(); ++i) {
                const std::string label = expr.argument_names.empty()
                        ? std::string{} : expr.argument_names[i];
                std::size_t target;
                if (!label.empty()) {
                    named_seen = true;
                    target = 0;
                    bool matched = false;
                    for (const auto& formal : callee->args()) {
                        if (formal.getName().str() == label) {
                            matched = true;
                            break;
                        }
                        ++target;
                    }
                    if (!matched) {
                        throw std::runtime_error("unknown native named argument '" + label + "'");
                    }
                } else {
                    if (named_seen) {
                        throw std::runtime_error("native positional argument after named argument");
                    }
                    target = positional++;
                }
                if (target >= ordered.size() || filled[target]) {
                    throw std::runtime_error("duplicate native named call binding");
                }
                filled[target] = true;
                // Emit instructions in source order, not in formal-slot order.
                ordered[target] = lower(*expr.arguments[i], builder, i64,
                                        storage, field_storage, functions, moves);
            }
            return builder.CreateCall(callee, ordered,
                callee->getReturnType()->isVoidTy() ? "" : "call");
        }
    }
    throw std::runtime_error("invalid expression kind while lowering LLVM IR");
}

} // namespace

std::string emit_verified_llvm_ir(const Program& source, bool wasm32_target) {
    // Fail before LLVM context, scalar-loop special lowering or exports.
    if (!source.imports.empty())
        throw std::runtime_error(
            "native module imports are unsupported until linked stdlib and typed assertion/trap lowering are implemented");
    // The parser rejects duplicate user type names, but this public emitter
    // also accepts constructed Program ASTs. LLVM silently renames duplicate
    // StructType and Function symbols, breaking source-to-ABI identity;
    // reject them before any native or scalar-loop fast path is selected.
    std::unordered_set<std::string> type_names;
    for (const auto& cls : source.classes) {
        if (cls.name.empty() || !type_names.insert(cls.name).second)
            throw std::runtime_error("duplicate or unnamed native class/actor declaration");
    }
    for (const auto& actor : source.actors) {
        if (actor.name.empty() || !type_names.insert(actor.name).second)
            throw std::runtime_error("duplicate or unnamed native class/actor declaration");
    }
    // Public AST entrypoint: validate reserved linker ABI names before the
    // scalar-loop fast path can return without emitting ordinary functions.
    // Do not permit LLVM's collision renaming to change guest symbol identity.
    for (const auto& fn : source.functions) {
        if (fn.name.rfind("oreslang_int_array_", 0) == 0)
            throw std::runtime_error("reserved native Array<int> ABI symbol");
        if (source.module.empty()) {
            const bool reserved =
                fn.name == "abort" ||
                fn.name == "__oreslang_guest_main" ||
                fn.name.rfind("oreslang_owned_i64_region_", 0) == 0 ||
                fn.name.rfind("ores_actor_i64_heap_", 0) == 0 ||
                fn.name.rfind("oreslang_source_actor_", 0) == 0 ||
                fn.name.rfind("oreslang_actor_", 0) == 0 ||
                fn.name.rfind("oreslang_wasm_actor_", 0) == 0 ||
                fn.name.rfind("oreslang_assert_", 0) == 0;
            if (reserved)
                throw std::runtime_error(
                    "reserved native runtime ABI symbol '" + fn.name + "'");
        }
        if (fn.returns_bool && fn.scalar_while)
            throw std::runtime_error("unverified typed bool native result lowering");
        // Never silently ignore a scalar continuation on a multi-function
        // synthetic Program: the single-function frame path is the only
        // certified lowering of this AST node.
        if (fn.scalar_while && source.functions.size() != 1)
            throw std::runtime_error(
                "experimental scalar loop requires exactly one standalone function");
    }
    if (source.functions.size() == 1 && source.functions.front().scalar_while) {
        if (wasm32_target || !source.classes.empty() || !source.actors.empty())
            throw std::runtime_error("experimental scalar loop unsupported target/owners");
        // This early-return path bypasses normal function emission. Reject
        // AST mutations that would erase owners, effects or actual return
        // computation, rather than trusting parser-only admission rules.
        const Function& fn = source.functions.front();
        const ScalarWhileLoop& loop = *fn.scalar_while;
        if (!source.module.empty() || !source.cooperative_loops.empty() ||
                fn.returns_bool || fn.returns_void || !fn.parameters.empty() ||
                fn.has_declared_effects || fn.declared_effects.bits != 0 ||
                !fn.statements.empty() || !fn.early_returns.empty() ||
                !fn.actor_spawns.empty() || !fn.actor_actions.empty() ||
                !fn.actor_sends.empty() || !fn.actor_stops.empty() ||
                !fn.actor_field_writes.empty() || fn.caught_throw_value ||
                !fn.catch_binding.empty() || !fn.caught_throw_defers.empty() ||
                fn.locals.size() != 1 || !fn.result ||
                fn.result->kind != Expression::Kind::Variable ||
                fn.result->name != loop.counter || fn.result->integer != 0 ||
                fn.result->left || fn.result->right ||
                !fn.result->arguments.empty() ||
                !fn.result->argument_names.empty())
            throw std::runtime_error("unverified experimental scalar-loop AST");
        const LocalBinding& local = fn.locals.front();
        if (!local.owner_mutable || local.heap_owned || local.int_array ||
                !local.class_name.empty() || local.new_class ||
                local.move_class || !local.move_source.empty() ||
                local.field_mutable || local.name != loop.counter ||
                loop.counter.empty() || !local.initializer ||
                local.initializer->kind != Expression::Kind::Integer ||
                local.initializer->integer != loop.initial ||
                local.initializer->left || local.initializer->right ||
                !local.initializer->name.empty() ||
                !local.initializer->arguments.empty() ||
                !local.initializer->argument_names.empty())
            throw std::runtime_error("unverified experimental scalar-loop AST");
        return emit_scalar_while_frame_llvm_ir(loop);
    }
    llvm::LLVMContext context;
    // A bounded, import-free Wasm guest may lower ONE synchronous actor.
    // Its state lives in private Wasm globals owned by exactly one Worker.
    // Native scheduler, ActorRef, suspension and supervisor APIs remain gated.
    if (wasm32_target) {
        if (source.actors.size() > 1)
            throw std::runtime_error("wasm32 actor guest supports one actor per instance");
        for (const auto& actor : source.actors) {
            if (actor.fields.empty() || actor.fields.size() > 16)
                throw std::runtime_error("wasm32 actor guest requires 1..16 scalar fields");
            if (actor.on_message.parameters.size() != 1 || !actor.on_message.result ||
                actor.on_message.returns_void || !actor.on_message.statements.empty() ||
                !actor.on_message.locals.empty() || !actor.on_message.early_returns.empty())
                throw std::runtime_error("wasm32 actor guest requires synchronous scalar handler");
        }
        for (const auto& cls : source.classes)
            for (const auto& field : cls.fields)
                if (field.int_array)
                    throw std::runtime_error("wasm32 class Array<int> owned fields are not implemented");
        for (const auto& fn : source.functions) {
            // A host-only caught-i64 CFG is not a certified wasm guest
            // exception ABI, even if an LLVM optimizer could emit its blocks.
            if (fn.caught_throw_value)
                throw std::runtime_error("wasm32 native caught-throw guest unsupported");
            if (!fn.actor_spawns.empty() || !fn.actor_sends.empty() ||
                !fn.actor_stops.empty() || !fn.actor_field_writes.empty())
                throw std::runtime_error("wasm32 actor operations are not implemented");
            for (const auto& local : fn.locals) {
                // Preserve the native heap-owned diagnostic independently
                // of array-backed ownership rejection.
                if (local.heap_owned)
                    throw std::runtime_error("wasm32 native heap-owned regions are not implemented");
                if (local.int_array)
                    throw std::runtime_error("wasm32 native heap-owned/Array<int> regions are not implemented");
            }
        }
    }
    // Native C entrypoints return i32; Oreslang int remains an internal i64.
    // Qualified module.main retains its exported guest symbol and gains a wrapper.
    // Browser Wasm is deliberately excluded: it has a separate host entry ABI.
    constexpr const char* guest_main_symbol = "__oreslang_guest_main";
    const auto main_it = std::find_if(source.functions.begin(), source.functions.end(),
        [](const Function& fn) { return fn.name == "main"; });
    const bool native_main = !wasm32_target && main_it != source.functions.end();
    if (native_main) {
        if (!main_it->parameters.empty())
            throw std::runtime_error("native main() must have zero arguments");
        if (source.module.empty() && std::any_of(source.functions.begin(), source.functions.end(),
                [](const Function& fn) { return fn.name == "__oreslang_guest_main"; }))
            throw std::runtime_error("reserved native entrypoint symbol collision");
    }
    llvm::Module module("oreslang-source", context);
    // The wasm32 environment is an explicit code-generation target. This
    // emits verified, target-marked IR; object/linking remains in oreslang-wasm.
    // Never infer browser capabilities from the target triple alone.
    if (wasm32_target) {
        const auto profile = TargetAbi::parse("wasm32-unknown-unknown");
        if (!profile.experimental_wasm32_ir())
            throw std::runtime_error("wasm32 target profile is not enabled");
        module.setTargetTriple(std::string(profile.triple()));
        module.setDataLayout("e-m:e-p:32:32-i64:64-n32:64-S128");
        const auto& actual = module.getDataLayout();
        if (actual.getPointerSizeInBits(0) != profile.pointer_bits()
                || actual.isLittleEndian() != (profile.endian() == "little"))
            throw std::runtime_error("wasm32 target ABI/data layout mismatch");
    }
    llvm::Type* i64 = llvm::Type::getInt64Ty(context);
    llvm::Type* loan_ptr = i64->getPointerTo();
    // LLVM 18 removed Type::getInt8PtrTy. Use opaque address-space-zero
    // pointers on modern LLVM while preserving pre-18 typed-pointer support.
#if LLVM_VERSION_MAJOR >= 18
    llvm::Type* native_ptr = llvm::PointerType::get(context, 0);
#else
    llvm::Type* native_ptr = llvm::Type::getInt8PtrTy(context);
#endif
    // The test-only native owned-int experiment must never leak declarations
    // or allocations into ordinary int code. Resolve the ABI only when a
    // program actually contains a gated experimental heap-owned local.
    const bool has_region_bindings = std::any_of(source.functions.begin(), source.functions.end(),
        [](const Function& fn) {
            return std::any_of(fn.locals.begin(), fn.locals.end(),
                [](const LocalBinding& binding) { return binding.heap_owned; });
        });
    llvm::FunctionCallee regionOpen, regionNew, regionRead, regionDrop, regionClose;
    if (has_region_bindings) {
        regionOpen = module.getOrInsertFunction("oreslang_owned_i64_region_open",
            llvm::FunctionType::get(native_ptr, {i64}, false));
        regionNew = module.getOrInsertFunction("oreslang_owned_i64_region_new",
            llvm::FunctionType::get(i64, {native_ptr, i64}, false));
        regionRead = module.getOrInsertFunction("oreslang_owned_i64_region_read",
            llvm::FunctionType::get(i64, {native_ptr, i64}, false));
        regionDrop = module.getOrInsertFunction("oreslang_owned_i64_region_drop",
            llvm::FunctionType::get(llvm::Type::getVoidTy(context), {native_ptr, i64}, false));
        regionClose = module.getOrInsertFunction("oreslang_owned_i64_region_close",
            llvm::FunctionType::get(llvm::Type::getVoidTy(context), {native_ptr}, false));
    }

    // Emit declarations only for functions that actually own Array<int>
    // locals. Opaque, unique handles are never exposed as guest values.
    const bool has_array_bindings = std::any_of(source.functions.begin(), source.functions.end(),
        [&](const Function& fn) {
            return std::any_of(fn.locals.begin(), fn.locals.end(),
                [&](const LocalBinding& b) {
                    if (b.int_array) return true;
                    if (!b.new_class) return false;
                    const auto cls = std::find_if(source.classes.begin(), source.classes.end(),
                        [&](const ClassDecl& candidate) { return candidate.name == b.class_name; });
                    return cls != source.classes.end() &&
                        std::any_of(cls->fields.begin(), cls->fields.end(),
                            [](const ClassField& fld) { return fld.int_array; });
                });
        });
    llvm::FunctionCallee arrayOpen, arrayNew, arrayAdd, arrayGet, arraySet,
                         arrayLength, arrayDrop, arrayClose, arrayAbort,
                         arrayPrepareFilledMany, arrayCommitMany, arrayUnhandled;
    if (has_array_bindings) {
        auto* voidTy = llvm::Type::getVoidTy(context);
        auto* i32 = llvm::Type::getInt32Ty(context);
        arrayOpen = module.getOrInsertFunction("oreslang_int_array_try_region_open",
            llvm::FunctionType::get(i32, {native_ptr->getPointerTo()}, false));
        arrayNew = module.getOrInsertFunction("oreslang_int_array_try_new",
            llvm::FunctionType::get(i32, {native_ptr, loan_ptr}, false));
        arrayAdd = module.getOrInsertFunction("oreslang_int_array_try_add",
            llvm::FunctionType::get(i32, {native_ptr, i64, i64}, false));
        // Each opaque handle slot is i64. Arrays of handle-slot pointers
        // require an extra pointer layer in LLVM typed-pointer toolchains.
        arrayPrepareFilledMany = module.getOrInsertFunction(
            "oreslang_int_array_try_prepare_filled_many",
            llvm::FunctionType::get(i32,
                {native_ptr, i64, loan_ptr, loan_ptr, loan_ptr}, false));
        arrayCommitMany = module.getOrInsertFunction(
            "oreslang_int_array_commit_many",
            llvm::FunctionType::get(voidTy,
                {native_ptr, i64, loan_ptr->getPointerTo(),
                 loan_ptr->getPointerTo()}, false));
        arrayAbort = module.getOrInsertFunction("abort",
            llvm::FunctionType::get(voidTy, {}, false));
        arrayUnhandled = module.getOrInsertFunction(
            "oreslang_int_array_unhandled_resource_failure",
            llvm::FunctionType::get(voidTy, {i32}, false));
        arrayGet = module.getOrInsertFunction("oreslang_int_array_get",
            llvm::FunctionType::get(i64, {native_ptr, i64, i64}, false));
        arraySet = module.getOrInsertFunction("oreslang_int_array_set",
            llvm::FunctionType::get(voidTy, {native_ptr, i64, i64, i64}, false));
        arrayLength = module.getOrInsertFunction("oreslang_int_array_length",
            llvm::FunctionType::get(i64, {native_ptr, i64}, false));
        arrayDrop = module.getOrInsertFunction("oreslang_int_array_drop",
            llvm::FunctionType::get(voidTy, {native_ptr, i64}, false));
        arrayClose = module.getOrInsertFunction("oreslang_int_array_region_close",
            llvm::FunctionType::get(voidTy, {native_ptr}, false));
    }

    // Only generated source actors resolve this native scheduler/heap ABI.
    // No actor imports appear in ordinary scalar/class compilation.
    llvm::FunctionCallee fieldInit, fieldInitMutable, fieldRead, fieldWrite;
    llvm::FunctionCallee sourceSpawn, sourceSend, sourceStop;
    std::unordered_map<std::string, llvm::Function*> actorInits, actorHandlers;
    if (!source.actors.empty() && !wasm32_target) {
        auto* voidTy = llvm::Type::getVoidTy(context);
        fieldInit = module.getOrInsertFunction("ores_actor_i64_heap_field_init",
            llvm::FunctionType::get(voidTy, {native_ptr, i64, i64, i64, i64}, false));
        fieldInitMutable = module.getOrInsertFunction("ores_actor_i64_heap_field_init_mutable",
            llvm::FunctionType::get(voidTy, {native_ptr, i64, i64, i64, i64}, false));
        fieldRead = module.getOrInsertFunction("ores_actor_i64_heap_field_read",
            llvm::FunctionType::get(i64, {native_ptr, i64, i64, i64}, false));
        fieldWrite = module.getOrInsertFunction("ores_actor_i64_heap_field_write",
            llvm::FunctionType::get(voidTy, {native_ptr, i64, i64, i64, i64}, false));
        sourceSpawn = module.getOrInsertFunction("oreslang_source_actor_spawn",
            llvm::FunctionType::get(native_ptr, {native_ptr, native_ptr, i64}, false));
        sourceSend = module.getOrInsertFunction("oreslang_source_actor_send",
            llvm::FunctionType::get(voidTy, {native_ptr, i64}, false));
        sourceStop = module.getOrInsertFunction("oreslang_source_actor_stop",
            llvm::FunctionType::get(voidTy, {native_ptr}, false));
        for (const auto& actor : source.actors) {
            const std::string prefix = "oreslang_actor_" + actor.name + "_";
            actorInits.emplace(actor.name, llvm::Function::Create(
                llvm::FunctionType::get(voidTy, {native_ptr, i64, i64}, false),
                llvm::Function::ExternalLinkage, prefix + "init", module));
            actorHandlers.emplace(actor.name, llvm::Function::Create(
                llvm::FunctionType::get(i64, {native_ptr, i64, i64, i64}, false),
                llvm::Function::ExternalLinkage, prefix + "on_message", module));
        }
    }
    std::unordered_map<std::string, llvm::Function*> declarations;
    std::unordered_map<std::string, llvm::StructType*> class_types;
    std::unordered_map<std::string, std::unordered_map<std::string, std::size_t>> field_indices;
    for (const auto& cls : source.classes) {
        std::vector<llvm::Type*> layout(cls.fields.size(), i64);
        class_types.emplace(cls.name, llvm::StructType::create(context, layout, cls.name));
        std::unordered_map<std::string, std::size_t> indices;
        for (std::size_t i = 0; i < cls.fields.size(); ++i)
            indices.emplace(cls.fields[i].name, i);
        field_indices.emplace(cls.name, std::move(indices));
    }

    // Exact by-value i64 / borrowed-i64-pointer argument types for all
    // same-module functions; no unsound conversion to i64 is permitted.
    // Unique free functions retain their ABI symbol, overloaded families use arity slots.
    std::unordered_map<std::string, std::size_t> family_sizes;
    for (const auto& fn : source.functions) ++family_sizes[fn.name];
    std::unordered_set<std::string> emitted_slots;
    for (const auto& fn : source.functions) {
        // Linker-owned names were already checked before all lowering paths.
        if (fn.returns_bool && fn.returns_void)
            throw std::runtime_error("native bool/void return flags conflict");
        if (fn.returns_bool && fn.name == "main")
            throw std::runtime_error("native bool main violates process exit ABI");
        if (fn.name == "main" && family_sizes.at(fn.name) != 1)
            throw std::runtime_error("duplicate native entrypoint 'main'");
        const std::string slot = native_function_slot(fn.name, fn.parameters.size());
        if (!emitted_slots.insert(slot).second)
            throw std::runtime_error("duplicate native function slot '" + slot + "'");
        const std::string base_symbol = native_main && source.module.empty() && fn.name == "main"
            ? guest_main_symbol
            : (source.module.empty() ? fn.name : source.module + "." + fn.name);
        const std::string symbol = family_sizes.at(fn.name) > 1
            ? native_function_slot(base_symbol, fn.parameters.size())
            : base_symbol;
        if (module.getNamedValue(symbol))
            throw std::runtime_error("native LLVM symbol collision '" + symbol + "'");
        std::vector<llvm::Type*> argument_types;
        for (const auto& param : fn.parameters)
            argument_types.push_back(param.class_name.empty()
                ? (param.mode == Parameter::Mode::Value ? i64 : loan_ptr)
                : class_types.at(param.class_name)->getPointerTo());
        llvm::FunctionType* signature = llvm::FunctionType::get(
            fn.returns_void ? llvm::Type::getVoidTy(context) :
            (fn.returns_bool ? llvm::Type::getInt1Ty(context) : i64),
            argument_types, false);
        llvm::Function* declaration = llvm::Function::Create(
            signature, llvm::Function::ExternalLinkage, symbol, module);
        if (declaration->getName().str() != symbol)
            throw std::runtime_error("native LLVM export was silently renamed");
        // Forward references must already expose parameter labels.
        std::size_t label_index = 0;
        for (auto& formal : declaration->args())
            formal.setName(fn.parameters[label_index++].name);
        if (!declarations.emplace(slot, declaration).second)
            throw std::runtime_error("duplicate native function declaration slot");
    }
    for (const auto& fn : source.functions) {
        llvm::Function* function = declarations.at(native_function_slot(fn.name, fn.parameters.size()));
        llvm::BasicBlock* entry = llvm::BasicBlock::Create(context, "entry", function);
        llvm::IRBuilder<> builder(entry);
        if (fn.returns_bool) {
            // Strictly pure, zero-argument source Boolean literal slice:
            // no integer coercion, host callbacks, heap or GC allocation.
            // All other typed bool forms remain parser-gated, and imported
            // std/assert stays rejected before LLVM module creation.
            if (!fn.result || fn.result->kind != Expression::Kind::Boolean ||
                    !fn.parameters.empty() || !fn.locals.empty() ||
                    !fn.statements.empty() || !fn.early_returns.empty() ||
                    !fn.actor_spawns.empty() || !fn.actor_actions.empty() ||
                    !fn.actor_sends.empty() || !fn.actor_stops.empty() ||
                    !fn.actor_field_writes.empty() || fn.caught_throw_value ||
                    !fn.caught_throw_defers.empty() || !fn.catch_binding.empty() ||
                    fn.scalar_while || fn.has_declared_effects ||
                    fn.declared_effects.bits != 0 ||
                    !fn.result->name.empty() || fn.result->left || fn.result->right ||
                    !fn.result->arguments.empty() ||
                    !fn.result->argument_names.empty() ||
                    (fn.result->integer != 0 && fn.result->integer != 1))
                throw std::runtime_error("unverified typed bool native result lowering");
            builder.CreateRet(builder.getInt1(fn.result->integer == 1));
            continue;
        }
        std::unordered_map<std::string, llvm::Value*> storage;
        std::unordered_map<std::string, llvm::Value*> field_storage;
        std::size_t index = 0;
        for (auto& arg : function->args()) {
            const Parameter& param = fn.parameters[index++];
            arg.setName(param.name);
            if (!param.class_name.empty()) {
                storage.emplace(param.name, &arg);
                for (const auto& [field, field_index] : field_indices.at(param.class_name)) {
                    field_storage.emplace(field_key(param.name, field),
                        builder.CreateStructGEP(class_types.at(param.class_name), &arg,
                            static_cast<unsigned>(field_index), param.name + "." + field));
                }
            } else if (param.mode == Parameter::Mode::Value) {
                llvm::Value* slot = builder.CreateAlloca(i64, nullptr, param.name + ".slot");
                builder.CreateStore(&arg, slot);
                storage.emplace(param.name, slot);
            } else {
                storage.emplace(param.name, &arg);
            }
        }

        // Every source actor ref is an opaque, unique capability owned by
        // trusted top-level main; it cannot enter scalar values or calls.
        std::unordered_map<std::string, llvm::Value*> actorRefs;
        for (const auto& spawn : fn.actor_spawns) {
            llvm::Value* ctor = builder.CreatePointerCast(actorInits.at(spawn.actor_type),
                native_ptr);
            llvm::Value* handler = builder.CreatePointerCast(actorHandlers.at(spawn.actor_type),
                native_ptr);
            const auto actor = std::find_if(source.actors.begin(), source.actors.end(),
                [&](const ActorDecl& a) { return a.name == spawn.actor_type; });
            llvm::Value* ref = builder.CreateCall(sourceSpawn, {ctor, handler,
                llvm::ConstantInt::get(i64, actor->fields.size())}, spawn.name + ".actor.ref");
            actorRefs.emplace(spawn.name, ref);
        }
        // An explicitly "owned int" lives in a bounded native C region;
        // ordinary int remains stack-owned. Both allow only call-scoped loans.
        struct LocalSlot {
            std::string name;
            llvm::Value* address;
            bool heap_owned;
            std::uint64_t bytes;
            bool int_array = false;
            std::vector<std::string> owned_array_fields;
        };
        std::vector<LocalSlot> lexical_slots;
        std::unordered_map<std::string, llvm::Value*> owned_live_flags;
        const auto heap_count = std::count_if(fn.locals.begin(), fn.locals.end(),
            [](const LocalBinding& b) { return b.heap_owned; });
        llvm::Value* region = nullptr;
        if (heap_count != 0)
            region = builder.CreateCall(regionOpen,
                {llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(i64),
                                        static_cast<std::uint64_t>(heap_count))}, "native.owned.region");
        std::unordered_set<std::string> array_names;
        std::unordered_set<std::string> array_field_keys;
        std::size_t array_owner_count = 0;
        for (const auto& local : fn.locals) {
            if (local.int_array) {
                array_names.insert(local.name);
                ++array_owner_count;
            } else if (local.new_class) {
                const auto cls = std::find_if(source.classes.begin(), source.classes.end(),
                    [&](const ClassDecl& item) { return item.name == local.class_name; });
                if (cls != source.classes.end())
                    for (const auto& fld : cls->fields)
                        if (fld.int_array) ++array_owner_count;
            }
        }
        // A compiler-owned four-array transaction stages four further
        // handles that must each have an initialized cleanup shadow before
        // the runtime can allocate. Whole-class moves add no new handles.
        for (const auto& statement : fn.statements)
            if (statement.kind == Statement::Kind::ArrayCommitFilled)
                array_owner_count += 4;
        if (array_owner_count > 64)
            throw std::runtime_error("native Array<int> per-function owned handle count exceeds 64");
        // Shadow owners are safe at every allocation failure: unlike class
        // field addresses, they are zeroed before the first checked call.
        llvm::Value* array_region = nullptr;
        llvm::Value* array_handle_shadows = nullptr;
        llvm::BasicBlock* array_failure = nullptr;
        llvm::Value* array_failure_status = nullptr;
        std::size_t next_array_shadow = 0;
        std::vector<llvm::Value*> heap_failure_slots;
        std::unordered_map<std::string, llvm::Value*> owned_failure_handles;
        // Maps each currently live source-level owner place to the token
        // shadow assigned at its original allocation. Moving an owner
        // changes the place, never the token or its failure cleanup authority.
        std::unordered_map<std::string, llvm::Value*> array_failure_places;
        if (array_owner_count) {
            array_handle_shadows = builder.CreateAlloca(i64,
                llvm::ConstantInt::get(i64, array_owner_count), "array.failure.owners");
            for (std::size_t k = 0; k < array_owner_count; ++k) {
                llvm::Value* address = builder.CreateGEP(i64, array_handle_shadows,
                    llvm::ConstantInt::get(i64, k), "array.failure.slot.init");
                builder.CreateStore(llvm::ConstantInt::get(i64, 0), address);
            }
            for (const auto& local : fn.locals) {
                if (!local.heap_owned) continue;
                llvm::Value* shadow = builder.CreateAlloca(i64, nullptr,
                    local.name + ".owned.failure.slot");
                builder.CreateStore(llvm::ConstantInt::get(i64, 0), shadow);
                heap_failure_slots.push_back(shadow);
                owned_failure_handles.emplace(local.name, shadow);
            }
            array_failure = llvm::BasicBlock::Create(context, "array.resource.failure", function);
            array_failure_status = builder.CreateAlloca(
                llvm::Type::getInt32Ty(context), nullptr, "array.failure.status.slot");
            builder.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 0),
                array_failure_status);
            llvm::Value* open_out = builder.CreateAlloca(native_ptr, nullptr, "array.region.out");
            llvm::Value* open_status = builder.CreateCall(arrayOpen, {open_out}, "array.region.status");
            llvm::BasicBlock* opened = llvm::BasicBlock::Create(
                context, "array.region.open.ok", function);
            llvm::BasicBlock* denied = llvm::BasicBlock::Create(
                context, "array.region.open.failure", function);
            builder.CreateCondBr(builder.CreateICmpEQ(open_status,
                llvm::ConstantInt::get(open_status->getType(), 0)), opened, denied);
            builder.SetInsertPoint(denied);
            // Preserve the actual typed resource status even when open fails
            // before a valid array-region pointer can be published.
            builder.CreateStore(open_status, array_failure_status);
            // A failed open published no native array, but a separate
            // experimental owned-int region may need deterministic cleanup.
            if (region != nullptr) builder.CreateCall(regionClose, {region});
            builder.CreateCall(arrayUnhandled, {open_status});
            builder.CreateUnreachable();
            builder.SetInsertPoint(opened);
            array_region = builder.CreateLoad(native_ptr, open_out, "native.int.array.region");
        }
        const RegionMoveLowering region_moves{region, regionRead, regionDrop, &owned_live_flags,
            array_region, arrayAdd, arrayGet, arraySet, arrayLength, &array_names,
            &array_field_keys, array_failure, array_failure_status, &owned_failure_handles};
        auto new_checked_array = [&](const std::string& label) -> llvm::Value* {
            if (!array_handle_shadows || next_array_shadow >= array_owner_count)
                throw std::runtime_error("missing native array failure owner slot");
            llvm::Value* address = builder.CreateGEP(i64, array_handle_shadows,
                llvm::ConstantInt::get(i64, next_array_shadow++),
                label + ".failure.owner");
            array_failure_places.emplace(label, address);
            llvm::Value* status = builder.CreateCall(arrayNew,
                {array_region, address}, label + ".array.create.status");
            llvm::BasicBlock* proceed = llvm::BasicBlock::Create(
                context, label + ".array.created", function);
            llvm::BasicBlock* denied_new = llvm::BasicBlock::Create(
                context, label + ".array.create.failure", function);
            builder.CreateCondBr(builder.CreateICmpEQ(status,
                llvm::ConstantInt::get(status->getType(), 0)),
                proceed, denied_new);
            builder.SetInsertPoint(denied_new);
            builder.CreateStore(status, array_failure_status);
            builder.CreateBr(array_failure);
            builder.SetInsertPoint(proceed);
            return builder.CreateLoad(i64, address, label + ".array.new");
        };
        for (const auto& binding : fn.locals) {
            if (binding.int_array) {
                llvm::Value* handle = new_checked_array(binding.name);
                llvm::Value* slot = builder.CreateAlloca(i64, nullptr,
                    binding.name + ".array.owner");
                builder.CreateStore(handle, slot);
                storage.emplace(binding.name, slot);
                lexical_slots.push_back({binding.name, slot, false, 8, true, {}});
                continue;
            }
            if (binding.new_class || binding.move_class) {
                llvm::StructType* shape = class_types.at(binding.class_name);
                llvm::Value* slot = builder.CreateAlloca(shape, nullptr, binding.name + ".record");
                const auto cls = std::find_if(source.classes.begin(), source.classes.end(),
                    [&](const ClassDecl& item) { return item.name == binding.class_name; });
                const std::uint64_t bytes = static_cast<std::uint64_t>(cls->fields.size()) * 8;
                builder.CreateLifetimeStart(slot,
                    llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(i64), bytes));
                // Prepare before publishing class field addresses. On any
                // failure the checked cleanup reads only initialized shadows,
                // never uninitialized class field storage.
                std::vector<std::pair<std::string, llvm::Value*>> staged_fields;
                std::vector<std::string> owned_fields;
                for (std::size_t i = 0; i < cls->fields.size(); ++i) {
                    const ClassField& item = cls->fields[i];
                    llvm::Value* address = builder.CreateStructGEP(shape, slot,
                        static_cast<unsigned>(i), binding.name + "." + item.name);
                    llvm::Value* initial = nullptr;
                    if (item.int_array) {
                        if (binding.new_class) {
                            initial = new_checked_array(binding.name + "." + item.name);
                        } else {
                            // Unique whole-class rt take: transfer the token
                            // into the new owner and disarm the original.
                            // No fallible allocation occurs during the move.
                            llvm::Value* previous = field_storage.at(
                                field_key(binding.move_source, item.name));
                            initial = builder.CreateLoad(i64, previous,
                                binding.move_source + "." + item.name + ".move.handle");
                            builder.CreateStore(initial, address);
                            builder.CreateStore(llvm::ConstantInt::get(i64, 0), previous);
                            array_failure_places.emplace(field_key(binding.name, item.name),
                                array_failure_places.at(field_key(binding.move_source, item.name)));
                        }
                        owned_fields.push_back(field_key(binding.name, item.name));
                    } else {
                        initial = binding.new_class
                            ? static_cast<llvm::Value*>(llvm::ConstantInt::getSigned(i64, item.initial))
                            : builder.CreateLoad(i64,
                                field_storage.at(field_key(binding.move_source, item.name)),
                                binding.move_source + "." + item.name + ".take");
                    }
                    if (!item.int_array || binding.new_class)
                        builder.CreateStore(initial, address);
                    staged_fields.emplace_back(field_key(binding.name, item.name), address);
                }
                for (const auto& field : staged_fields)
                    field_storage.emplace(field.first, field.second);
                for (const auto& name : owned_fields)
                    array_field_keys.insert(name);
                storage.emplace(binding.name, slot);
                lexical_slots.push_back({binding.name, slot, false, bytes, false,
                                         std::move(owned_fields)});
                continue;
            }
            llvm::Value* initial = lower(*binding.initializer, builder, i64, storage, field_storage, declarations, &region_moves);
            llvm::Value* slot = nullptr;
            if (binding.heap_owned) {
                // Store only an opaque, generation-checked lifetime token.
                // No raw region payload pointer crosses into guest LLVM IR.
                llvm::Value* handle = builder.CreateCall(regionNew,
                    {region, initial}, binding.name + ".owned.handle");
                slot = builder.CreateAlloca(i64, nullptr, binding.name + ".owned.handle.slot");
                builder.CreateStore(handle, slot);
                if (array_owner_count)
                    builder.CreateStore(handle, owned_failure_handles.at(binding.name));
                llvm::Value* live = builder.CreateAlloca(builder.getInt1Ty(), nullptr,
                    binding.name + ".owned.live");
                builder.CreateStore(builder.getTrue(), live);
                owned_live_flags.emplace(binding.name, live);
            } else {
                slot = builder.CreateAlloca(i64, nullptr, binding.name + ".stack.owned");
                builder.CreateLifetimeStart(slot, llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(i64), 8));
                builder.CreateStore(initial, slot);
            }
            storage.emplace(binding.name, slot);
            lexical_slots.push_back({binding.name, slot, binding.heap_owned, 8, false, {}});
        }
        std::vector<const Statement*> deferred_effects;
        auto lower_effect = [&](const Statement& effect) {
            if (effect.kind == Statement::Kind::ArrayCommitFilled) {
                // OwnershipChecker guarantees a live unique mutable class
                // with exactly four array fields; the class layout fixes
                // their ordering. All inputs are evaluated before staging.
                const auto binding = std::find_if(fn.locals.begin(), fn.locals.end(),
                    [&](const LocalBinding& b) { return b.name == effect.receiver; });
                if (binding == fn.locals.end() || binding->class_name.empty())
                    throw std::runtime_error("filled array commit lacks class owner");
                const auto cls = std::find_if(source.classes.begin(), source.classes.end(),
                    [&](const ClassDecl& item) { return item.name == binding->class_name; });
                if (cls == source.classes.end())
                    throw std::runtime_error("filled array commit lacks class layout");
                std::vector<std::string> owned_fields;
                for (const auto& item : cls->fields)
                    if (item.int_array)
                        owned_fields.push_back(field_key(effect.receiver, item.name));
                if (owned_fields.size() != 4 || effect.array_commit_values.size() != 4 ||
                        !array_handle_shadows || next_array_shadow + 4 > array_owner_count)
                    throw std::runtime_error("unverified filled array commit owners");

                const auto four = llvm::ConstantInt::get(i64, 4);
                llvm::Value* count = lower(*effect.array_commit_length, builder, i64,
                    storage, field_storage, declarations, &region_moves);
                llvm::Value* lengths = builder.CreateAlloca(i64, four, "arrays.commit.lengths");
                llvm::Value* fill = builder.CreateAlloca(i64, four, "arrays.commit.values");
                llvm::Value* dests = builder.CreateAlloca(loan_ptr, four, "arrays.commit.dest.ptrs");
                llvm::Value* sources = builder.CreateAlloca(loan_ptr, four, "arrays.commit.src.ptrs");
                llvm::Value* staged = builder.CreateGEP(i64, array_handle_shadows,
                    llvm::ConstantInt::get(i64, next_array_shadow),
                    "arrays.commit.staged.owners");
                const std::size_t first_stage = next_array_shadow;
                next_array_shadow += 4;
                std::vector<llvm::Value*> previous_shadows;
                for (std::size_t i = 0; i < 4; ++i) {
                    const auto index = llvm::ConstantInt::get(i64, i);
                    llvm::Value* value = lower(*effect.array_commit_values[i],
                        builder, i64, storage, field_storage, declarations, &region_moves);
                    builder.CreateStore(count, builder.CreateGEP(i64, lengths, index));
                    builder.CreateStore(value, builder.CreateGEP(i64, fill, index));
                    builder.CreateStore(field_storage.at(owned_fields[i]),
                        builder.CreateGEP(loan_ptr, dests, index));
                    builder.CreateStore(builder.CreateGEP(i64, array_handle_shadows,
                            llvm::ConstantInt::get(i64, first_stage + i)),
                        builder.CreateGEP(loan_ptr, sources, index));
                    previous_shadows.push_back(array_failure_places.at(owned_fields[i]));
                }

                llvm::Value* status = builder.CreateCall(arrayPrepareFilledMany,
                    {array_region, four, lengths, fill, staged},
                    "arrays.commit.prepare.status");
                llvm::BasicBlock* accepted = llvm::BasicBlock::Create(
                    context, "arrays.commit.prepare.ok", function);
                llvm::BasicBlock* failed = llvm::BasicBlock::Create(
                    context, "arrays.commit.prepare.failure", function);
                builder.CreateCondBr(builder.CreateICmpEQ(status,
                    llvm::ConstantInt::get(status->getType(), 0)), accepted, failed);
                builder.SetInsertPoint(failed);
                builder.CreateStore(status, array_failure_status);
                // ABI contract: failed preparation never modifies staged[] or
                // published class handles. The shared epilogue drops only
                // the original live owners and then closes their region.
                builder.CreateBr(array_failure);
                builder.SetInsertPoint(accepted);
                builder.CreateCall(arrayCommitMany, {array_region, four, dests, sources});
                // IMPORTANT: commit_many disarms *sources[i] = 0. Since we
                // supplied hidden staging shadows as those source slots, the
                // runtime also zeroed our failure-cleanup mirrors. Restore
                // each new owner token from the published destination *after*
                // the infallible native commit and before any further guest
                // operations can fail. Otherwise a later resource failure
                // would leak four live arrays and abort on region_close.
                for (std::size_t i = 0; i < 4; ++i) {
                    llvm::Value* shadow = builder.CreateGEP(i64,
                        array_handle_shadows,
                        llvm::ConstantInt::get(i64, first_stage + i),
                        "arrays.commit.cleanup.owner");
                    llvm::Value* published = builder.CreateLoad(i64,
                        field_storage.at(owned_fields[i]),
                        "arrays.commit.published.owner");
                    builder.CreateStore(llvm::ConstantInt::get(i64, 0),
                        previous_shadows[i]);
                    builder.CreateStore(published, shadow);
                    array_failure_places[owned_fields[i]] = shadow;
                }
            } else if (effect.kind == Statement::Kind::FieldWrite) {
                const std::string target = field_key(effect.receiver, effect.field);
                if (array_field_keys.count(target)) {
                    // OwnershipChecker admitted only straight-line direct
                    // rt take(local) into an exclusively owned mutable field.
                    // Both handles are already live in this private region.
                    const Expression& move = *effect.expression;
                    if (move.kind != Expression::Kind::Take || !move.left ||
                            move.left->kind != Expression::Kind::Variable)
                        throw std::runtime_error("unverified native array field move");
                    const std::string& source = move.left->name;
                    llvm::Value* original = field_storage.at(target);
                    llvm::Value* lexical = storage.at(source);
                    llvm::Value* incoming = builder.CreateLoad(i64, lexical,
                        source + ".transfer.owner");
                    llvm::Value* obsolete = builder.CreateLoad(i64, original,
                        target + ".replaced.owner");
                    // No allocation can occur between destruction and swap.
                    // The original allocation's failure shadow must also be
                    // disarmed before later checked array calls may fail.
                    builder.CreateCall(arrayDrop, {array_region, obsolete});
                    builder.CreateStore(llvm::ConstantInt::get(i64, 0),
                        array_failure_places.at(target));
                    builder.CreateStore(incoming, original);
                    builder.CreateStore(llvm::ConstantInt::get(i64, 0), lexical);
                    array_failure_places[target] = array_failure_places.at(source);
                } else {
                    llvm::Value* updated = lower(*effect.expression, builder, i64,
                        storage, field_storage, declarations, &region_moves);
                    builder.CreateStore(updated, field_storage.at(target));
                }
            } else if (effect.kind == Statement::Kind::Call) {
                lower(*effect.expression, builder, i64, storage, field_storage, declarations, &region_moves);
            } else {
                throw std::runtime_error("unsupported native conditional arm");
            }
        };
        // Branch-local trivial i64 owners are initialized only along their
        // reached predecessor and end their lifetime before the join. They
        // never enter the enclosing lexical_slots or deferred cleanup list.
        auto lower_arm = [&](const std::vector<Statement>& arm) {
            std::vector<std::string> scoped_names;
            for (const auto& action : arm) {
                if (action.kind != Statement::Kind::ScopedLocal) {
                    lower_effect(action);
                    continue;
                }
                const LocalBinding& local = *action.branch_local;
                llvm::Value* init = lower(*local.initializer, builder, i64,
                    storage, field_storage, declarations, &region_moves);
                llvm::Value* slot = builder.CreateAlloca(i64, nullptr,
                    local.name + ".arm.stack");
                builder.CreateLifetimeStart(slot,
                    llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(i64), 8));
                builder.CreateStore(init, slot);
                if (!storage.emplace(local.name, slot).second)
                    throw std::runtime_error("branch-local shadows existing LLVM storage");
                scoped_names.push_back(local.name);
            }
            for (auto it = scoped_names.rbegin(); it != scoped_names.rend(); ++it) {
                builder.CreateLifetimeEnd(storage.at(*it),
                    llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(i64), 8));
                storage.erase(*it);
            }
        };
        if (fn.caught_throw_value) {
            // Real native CFG edges. Evaluate payload once, run scoped LIFO
            // cleanups, and transfer the typed scalar into the catch binding.
            llvm::Value* thrown = lower(*fn.caught_throw_value, builder, i64,
                storage, field_storage, declarations, &region_moves);
            llvm::Value* caught = builder.CreateAlloca(i64, nullptr, "catch.i64.slot");
            builder.CreateStore(thrown, caught);
            llvm::BasicBlock* unwind = llvm::BasicBlock::Create(
                context, "throw.cleanup", function);
            llvm::BasicBlock* handler = llvm::BasicBlock::Create(
                context, "throw.catch", function);
            builder.CreateBr(unwind);
            builder.SetInsertPoint(unwind);
            for (auto it = fn.caught_throw_defers.rbegin();
                 it != fn.caught_throw_defers.rend(); ++it) {
                llvm::Value* updated = lower(*it->expression, builder, i64,
                    storage, field_storage, declarations, &region_moves);
                builder.CreateStore(updated, field_storage.at(
                    field_key(it->receiver, it->field)));
            }
            builder.CreateBr(handler);
            builder.SetInsertPoint(handler);
            if (!storage.emplace(fn.catch_binding, caught).second)
                throw std::runtime_error("duplicate native caught throw binding");
        }
        for (const auto& stmt : fn.statements) {
            if (stmt.kind == Statement::Kind::Defer) {
                deferred_effects.push_back(&stmt);
                continue;
            }
            if (stmt.kind == Statement::Kind::Conditional) {
                llvm::Value* lhs = lower(*stmt.condition_lhs, builder, i64,
                    storage, field_storage, declarations, &region_moves);
                llvm::Value* rhs = lower(*stmt.condition_rhs, builder, i64,
                    storage, field_storage, declarations, &region_moves);
                llvm::Value* condition = builder.CreateICmpEQ(lhs, rhs, "if.else.eq");
                llvm::BasicBlock* thenBlock = llvm::BasicBlock::Create(
                    context, "if.then", function);
                llvm::BasicBlock* elseBlock = llvm::BasicBlock::Create(
                    context, "if.else", function);
                llvm::BasicBlock* mergeBlock = llvm::BasicBlock::Create(
                    context, "if.merge", function);
                builder.CreateCondBr(condition, thenBlock, elseBlock);
                builder.SetInsertPoint(thenBlock);
                lower_arm(stmt.then_statements);
                builder.CreateBr(mergeBlock);
                builder.SetInsertPoint(elseBlock);
                lower_arm(stmt.else_statements);
                builder.CreateBr(mergeBlock);
                builder.SetInsertPoint(mergeBlock);
                continue;
            }
            lower_effect(stmt);
        }

        // Emit observable actor effects in source order. Reordering all sends
        // ahead of all stops incorrectly executes a stopped worker's messages.
        for (const auto& action : fn.actor_actions) {
            if (action.kind == ActorAction::Kind::Send) {
                const auto& send = fn.actor_sends.at(action.index);
                llvm::Value* payload = lower(*send.payload, builder, i64, storage, field_storage, declarations, &region_moves);
                builder.CreateCall(sourceSend, {actorRefs.at(send.reference), payload});
            } else {
                builder.CreateCall(sourceStop, {actorRefs.at(fn.actor_stops.at(action.index))});
            }
        }
        // One cleanup authority for every guarded-return path. A function
        // with only its final return and no registered defer has exactly one
        // exit by construction, so preserve its direct ret IR contract.
        // That fast path still runs the same drop and lifetime cleanup.
        auto emit_lexical_cleanup = [&]() {
            for (auto it = lexical_slots.rbegin(); it != lexical_slots.rend(); ++it) {
                if (it->int_array) {
                    llvm::Value* handle = builder.CreateLoad(i64,
                        it->address, it->name + ".array.drop.handle");
                    // A lexical rt take disarms its previous owner slot.
                    // Reject duplicate drops of moved handles by skipping 0.
                    llvm::BasicBlock* do_drop = llvm::BasicBlock::Create(
                        context, it->name + ".array.drop.live", function);
                    llvm::BasicBlock* after_drop = llvm::BasicBlock::Create(
                        context, it->name + ".array.drop.next", function);
                    builder.CreateCondBr(builder.CreateICmpNE(handle,
                        llvm::ConstantInt::get(i64, 0)), do_drop, after_drop);
                    builder.SetInsertPoint(do_drop);
                    builder.CreateCall(arrayDrop, {array_region, handle});
                    builder.CreateStore(llvm::ConstantInt::get(i64, 0), it->address);
                    builder.CreateBr(after_drop);
                    builder.SetInsertPoint(after_drop);
                } else if (it->heap_owned) {
                    llvm::Value* flag = owned_live_flags.at(it->name);
                    llvm::Value* live = builder.CreateLoad(builder.getInt1Ty(), flag,
                        it->name + ".drop.if.live");
                    llvm::BasicBlock* do_drop = llvm::BasicBlock::Create(
                        context, it->name + ".drop.yes", function);
                    llvm::BasicBlock* after_drop = llvm::BasicBlock::Create(
                        context, it->name + ".drop.next", function);
                    builder.CreateCondBr(live, do_drop, after_drop);
                    builder.SetInsertPoint(do_drop);
                    llvm::Value* handle = builder.CreateLoad(i64,
                        it->address, it->name + ".drop.handle");
                    builder.CreateCall(regionDrop, {region, handle});
                    builder.CreateStore(llvm::ConstantInt::get(i64, 0), it->address);
                    builder.CreateStore(builder.getFalse(), flag);
                    builder.CreateBr(after_drop);
                    builder.SetInsertPoint(after_drop);
                } else {
                    // A class owns each array handle independently. Drop
                    // fields in reverse declaration order before ending the
                    // containing record lifetime (no aliases may escape).
                    for (auto fld = it->owned_array_fields.rbegin();
                         fld != it->owned_array_fields.rend(); ++fld) {
                        llvm::Value* address = field_storage.at(*fld);
                        llvm::Value* handle = builder.CreateLoad(i64, address,
                            *fld + ".drop.handle");
                        // Whole-class moves clear the source field to zero.
                        // Skip disarmed places, otherwise the trusted runtime
                        // correctly treats drop(0) as a fatal invalid handle.
                        llvm::BasicBlock* do_drop = llvm::BasicBlock::Create(
                            context, *fld + ".drop.live", function);
                        llvm::BasicBlock* after_drop = llvm::BasicBlock::Create(
                            context, *fld + ".drop.next", function);
                        builder.CreateCondBr(builder.CreateICmpNE(handle,
                            llvm::ConstantInt::get(i64, 0)), do_drop, after_drop);
                        builder.SetInsertPoint(do_drop);
                        builder.CreateCall(arrayDrop, {array_region, handle});
                        builder.CreateStore(llvm::ConstantInt::get(i64, 0), address);
                        builder.CreateBr(after_drop);
                        builder.SetInsertPoint(after_drop);
                    }
                    // llvm.lifetime.end is not a source-level destructor.
                    builder.CreateLifetimeEnd(it->address,
                        llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(i64), it->bytes));
                }
            }
            if (array_region != nullptr) builder.CreateCall(arrayClose, {array_region});
            if (region != nullptr) builder.CreateCall(regionClose, {region});
        };
        if (fn.early_returns.empty() && deferred_effects.empty()) {
            // Maintain stable native IR (ret i64 42, for example) for
            // functions without a multi-exit cleanup obligation.
            llvm::Value* result = fn.result
                ? lower(*fn.result, builder, i64, storage, field_storage, declarations, &region_moves)
                : nullptr;
            emit_lexical_cleanup();
            if (fn.returns_void) builder.CreateRetVoid();
            else builder.CreateRet(result);
        } else {
            llvm::Value* return_snapshot = fn.returns_void
                ? nullptr : builder.CreateAlloca(i64, nullptr, "return.snapshot.slot");
            llvm::BasicBlock* cleanup = llvm::BasicBlock::Create(
                context, "exit.cleanup", function);
            auto branch_to_cleanup = [&](llvm::Value* value) {
                if (!fn.returns_void) builder.CreateStore(value, return_snapshot);
                builder.CreateBr(cleanup);
            };
            for (const auto& guard : fn.early_returns) {
                llvm::Value* left = lower(*guard.lhs, builder, i64, storage, field_storage,
                    declarations, &region_moves);
                llvm::Value* right = lower(*guard.rhs, builder, i64, storage, field_storage,
                    declarations, &region_moves);
                llvm::Value* equal = builder.CreateICmpEQ(left, right, "return.guard.eq");
                llvm::BasicBlock* taken = llvm::BasicBlock::Create(context, "guard.return", function);
                llvm::BasicBlock* next = llvm::BasicBlock::Create(context, "guard.next", function);
                builder.CreateCondBr(equal, taken, next);
                builder.SetInsertPoint(taken);
                llvm::Value* selected = lower(*guard.result, builder, i64,
                    storage, field_storage, declarations, &region_moves);
                branch_to_cleanup(selected);
                builder.SetInsertPoint(next);
            }
            llvm::Value* result = fn.result
                ? lower(*fn.result, builder, i64, storage, field_storage,
                    declarations, &region_moves) : nullptr;
            branch_to_cleanup(result);
            builder.SetInsertPoint(cleanup);
            // Capture the return before deferred calls can mutate class fields.
            for (auto it = deferred_effects.rbegin(); it != deferred_effects.rend(); ++it) {
                const Statement& effect = **it;
                if (effect.receiver.empty()) {
                    lower(*effect.expression, builder, i64, storage, field_storage,
                        declarations, &region_moves);
                } else {
                    llvm::Value* updated = lower(*effect.expression, builder, i64,
                        storage, field_storage, declarations, &region_moves);
                    builder.CreateStore(updated, field_storage.at(
                        field_key(effect.receiver, effect.field)));
                }
            }
            emit_lexical_cleanup();
            if (fn.returns_void) builder.CreateRetVoid();
            else builder.CreateRet(builder.CreateLoad(i64, return_snapshot, "return.snapshot"));
        }
        if (array_failure) {
            builder.SetInsertPoint(array_failure);
            // The shadow owner slots contain 0 until the corresponding
            // checked new call succeeds; drop in reverse construction order.
            for (std::size_t k = array_owner_count; k != 0; --k) {
                llvm::Value* address = builder.CreateGEP(i64, array_handle_shadows,
                    llvm::ConstantInt::get(i64, k - 1), "array.failure.drop.slot");
                llvm::Value* handle = builder.CreateLoad(i64, address, "array.failure.handle");
                llvm::BasicBlock* drop = llvm::BasicBlock::Create(
                    context, "array.failure.drop", function);
                llvm::BasicBlock* next = llvm::BasicBlock::Create(
                    context, "array.failure.next", function);
                builder.CreateCondBr(builder.CreateICmpNE(handle,
                    llvm::ConstantInt::get(i64, 0)), drop, next);
                builder.SetInsertPoint(drop);
                builder.CreateCall(arrayDrop, {array_region, handle});
                builder.CreateStore(llvm::ConstantInt::get(i64, 0), address);
                builder.CreateBr(next);
                builder.SetInsertPoint(next);
            }
            builder.CreateCall(arrayClose, {array_region});
            if (region != nullptr) {
                for (auto it = heap_failure_slots.rbegin();
                     it != heap_failure_slots.rend(); ++it) {
                    llvm::Value* handle = builder.CreateLoad(i64, *it,
                        "owned.failure.handle");
                    llvm::BasicBlock* drop = llvm::BasicBlock::Create(
                        context, "owned.failure.drop", function);
                    llvm::BasicBlock* next = llvm::BasicBlock::Create(
                        context, "owned.failure.next", function);
                    builder.CreateCondBr(builder.CreateICmpNE(handle,
                        llvm::ConstantInt::get(i64, 0)), drop, next);
                    builder.SetInsertPoint(drop);
                    builder.CreateCall(regionDrop, {region, handle});
                    builder.CreateStore(llvm::ConstantInt::get(i64, 0), *it);
                    builder.CreateBr(next);
                    builder.SetInsertPoint(next);
                }
                builder.CreateCall(regionClose, {region});
            }
            // Preserve the exact checked status through cleanup, refusing
            // unknown or success status tags before guest-level error lowering.
            // Both expected resource errors still terminate here: a typed
            // throw/Result continuation MUST NOT resume with dropped owners.
            llvm::Value* typed_status = builder.CreateLoad(
                llvm::Type::getInt32Ty(context), array_failure_status,
                "array.failure.typed.status");
            llvm::BasicBlock* quota_error = llvm::BasicBlock::Create(
                context, "array.failure.limit", function);
            llvm::BasicBlock* memory_error = llvm::BasicBlock::Create(
                context, "array.failure.no-memory", function);
            llvm::BasicBlock* invalid_status = llvm::BasicBlock::Create(
                context, "array.failure.invalid-status", function);
            auto* dispatch = builder.CreateSwitch(typed_status, invalid_status, 2);
            dispatch->addCase(llvm::ConstantInt::get(
                llvm::Type::getInt32Ty(context), 1), quota_error);
            dispatch->addCase(llvm::ConstantInt::get(
                llvm::Type::getInt32Ty(context), 2), memory_error);
            for (llvm::BasicBlock* failure : {quota_error, memory_error}) {
                builder.SetInsertPoint(failure);
                // No return to a frame whose owners were already destroyed.
                builder.CreateCall(arrayUnhandled, {typed_status});
                builder.CreateUnreachable();
            }
            builder.SetInsertPoint(invalid_status);
            builder.CreateCall(arrayAbort, {});
            builder.CreateUnreachable();
        }
    }


    // First compiler-generated heap continuation: exactly one frame-owned
    // scalar updated at a committed loop backedge. There is no actor or
    // scheduler admission here. The host owns the frame and must dispatch
    // on a single carrier with a strictly bounded fuel allowance.
    for (const auto& loop : source.cooperative_loops) {
        if (wasm32_target)
            throw std::runtime_error("wasm32 resumable source loop is unsupported");
        auto* i32 = llvm::Type::getInt32Ty(context);
        const std::string prefix = "oreslang_rt_" + loop.name + "_";
        auto* frame_type = loan_ptr;
        auto* initialize = llvm::Function::Create(
            llvm::FunctionType::get(i32, {frame_type}, false),
            llvm::Function::ExternalLinkage, prefix + "init", module);
        auto* resume = llvm::Function::Create(
            llvm::FunctionType::get(i32, {frame_type, i64}, false),
            llvm::Function::ExternalLinkage, prefix + "resume", module);
        auto* cancel = llvm::Function::Create(
            llvm::FunctionType::get(i32, {frame_type}, false),
            llvm::Function::ExternalLinkage, prefix + "cancel", module);
        auto* zero = llvm::ConstantInt::get(i64, 0);
        auto* one = llvm::ConstantInt::get(i64, 1);
        auto* two = llvm::ConstantInt::get(i64, 2);
        auto* max_fuel = llvm::ConstantInt::get(i64, 512);
        auto* init_value = llvm::ConstantInt::getSigned(i64, loop.initial);

        // INIT: only an uninitialized (zero-tagged) frame can be published.
        // Reinitializing a cancelled or active frame would resurrect work.
        {
            auto* entry = llvm::BasicBlock::Create(context, "entry", initialize);
            auto* publish = llvm::BasicBlock::Create(context, "publish", initialize);
            auto* invalid = llvm::BasicBlock::Create(context, "invalid", initialize);
            llvm::IRBuilder<> b(entry);
            auto* frame = initialize->getArg(0);
            auto* tag = b.CreateLoad(i64, frame, "state");
            auto* fresh = b.CreateICmpEQ(tag, zero);
            b.CreateCondBr(fresh, publish, invalid);
            b.SetInsertPoint(publish);
            auto* counter = b.CreateInBoundsGEP(i64, frame, one);
            b.CreateStore(init_value, counter);
            b.CreateStore(one, frame);
            b.CreateRet(llvm::ConstantInt::get(i32, 1));
            b.SetInsertPoint(invalid);
            b.CreateRet(llvm::ConstantInt::get(i32, 0));
        }
        // RESUME: cap the number of guest iterations independently of
        // untrusted host fuel; report YIELD only after a committed update.
        {
            auto* entry = llvm::BasicBlock::Create(context, "entry", resume);
            auto* body = llvm::BasicBlock::Create(context, "body", resume);
            auto* iteration = llvm::BasicBlock::Create(context, "iteration", resume);
            auto* handoff = llvm::BasicBlock::Create(context, "handoff", resume);
            auto* invalid = llvm::BasicBlock::Create(context, "invalid", resume);
            llvm::IRBuilder<> b(entry);
            auto* frame = resume->getArg(0);
            auto* fuel = resume->getArg(1);
            auto* state = b.CreateLoad(i64, frame, "state");
            auto* active = b.CreateICmpEQ(state, one);
            auto* valid_fuel = b.CreateICmpSGT(fuel, zero);
            b.CreateCondBr(b.CreateAnd(active, valid_fuel), body, invalid);
            b.SetInsertPoint(body);
            auto* bounded = b.CreateSelect(b.CreateICmpUGT(fuel, max_fuel),
                                            max_fuel, fuel, "bounded.fuel");
            b.CreateBr(iteration);
            b.SetInsertPoint(iteration);
            auto* remaining = b.CreatePHI(i64, 2, "remaining");
            remaining->addIncoming(bounded, body);
            auto* counter = b.CreateInBoundsGEP(i64, frame, one);
            auto* current = b.CreateLoad(i64, counter, "committed.counter");
            // Match the existing native i64 arithmetic: LLVM add without
            // 'nsw' or 'nuw' wraps modulo 2^64. Do not silently introduce
            // saturation into the resumable form of the same expression.
            // A guest-visible update is committed before the next yield.
            auto* next = b.CreateAdd(current, one, "next.counter");
            b.CreateStore(next, counter);
            auto* left = b.CreateSub(remaining, one);
            auto* expired = b.CreateICmpEQ(left, zero);
            b.CreateCondBr(expired, handoff, iteration);
            remaining->addIncoming(left, iteration);
            b.SetInsertPoint(handoff);
            b.CreateRet(llvm::ConstantInt::get(i32, 1)); // yielded
            b.SetInsertPoint(invalid);
            b.CreateRet(llvm::ConstantInt::getSigned(i32, -1));
        }
        // Cancellation is a trusted owner-turn action. Never interrupt
        // an in-flight native stack or revive a previously cancelled frame.
        {
            auto* entry = llvm::BasicBlock::Create(context, "entry", cancel);
            auto* retired = llvm::BasicBlock::Create(context, "retired", cancel);
            auto* invalid = llvm::BasicBlock::Create(context, "invalid", cancel);
            llvm::IRBuilder<> b(entry);
            auto* frame = cancel->getArg(0);
            b.CreateCondBr(b.CreateICmpEQ(b.CreateLoad(i64, frame), one),
                           retired, invalid);
            b.SetInsertPoint(retired);
            b.CreateStore(two, frame);
            b.CreateStore(zero, b.CreateInBoundsGEP(i64, frame, one));
            b.CreateRet(llvm::ConstantInt::get(i32, 1));
            b.SetInsertPoint(invalid);
            b.CreateRet(llvm::ConstantInt::get(i32, 0));
        }
    }

    // Native source actors execute only on OresVM scheduler turns. The
    // portable guest below uses an independent, no-import single-actor ABI.
    if (!wasm32_target) for (const auto& actor : source.actors) {
        auto* ctor = actorInits.at(actor.name);
        llvm::BasicBlock* initial = llvm::BasicBlock::Create(context, "entry", ctor);
        llvm::IRBuilder<> init(initial);
        auto a = ctor->arg_begin();
        llvm::Value* heap = &*a++;
        llvm::Value* domain = &*a++;
        llvm::Value* epoch = &*a++;
        const std::unordered_map<std::string, llvm::Value*> noStorage;
        const std::unordered_map<std::string, llvm::Function*> noFunctions;
        for (std::size_t i=0; i<actor.fields.size(); ++i) {
            const auto& field=actor.fields[i];
            llvm::Value* initialValue = lower(*field.initializer, init, i64,
                noStorage, noStorage, noFunctions);
            init.CreateCall(field.field_mutable ? fieldInitMutable : fieldInit,
                {heap, domain, epoch, llvm::ConstantInt::get(i64, i), initialValue});
        }
        init.CreateRetVoid();

        auto* handler = actorHandlers.at(actor.name);
        llvm::BasicBlock* entry = llvm::BasicBlock::Create(context, "entry", handler);
        llvm::IRBuilder<> builder(entry);
        auto args = handler->arg_begin();
        llvm::Value* actorHeap = &*args++;
        llvm::Value* currentDomain = &*args++;
        llvm::Value* currentEpoch = &*args++;
        llvm::Value* incoming = &*args++;
        std::unordered_map<std::string, llvm::Value*> values, emptyFields;
        const std::string param = actor.on_message.parameters[0].name;
        llvm::Value* pslot = builder.CreateAlloca(i64, nullptr, param + ".slot");
        builder.CreateStore(incoming, pslot);
        values.emplace(param, pslot);
        for (std::size_t i=0; i<actor.fields.size(); ++i) {
            const auto& field = actor.fields[i];
            llvm::Value* value = builder.CreateCall(fieldRead, {
                actorHeap, currentDomain, currentEpoch, llvm::ConstantInt::get(i64,i)},
                field.name + ".read");
            llvm::Value* slot = builder.CreateAlloca(i64, nullptr, field.name + ".slot");
            builder.CreateStore(value, slot);
            values.emplace(field.name, slot);
        }
        for (const auto& write : actor.on_message.actor_field_writes) {
            auto field = std::find_if(actor.fields.begin(), actor.fields.end(),
                [&](const LocalBinding& f) { return f.name == write.field; });
            if (field == actor.fields.end() || !field->field_mutable)
                throw std::runtime_error("invalid actor field write lowering");
            llvm::Value* value = lower(*write.expression, builder, i64, values, emptyFields, declarations);
            const std::size_t index = std::distance(actor.fields.begin(), field);
            builder.CreateCall(fieldWrite, {actorHeap, currentDomain, currentEpoch,
                llvm::ConstantInt::get(i64, index), value});
            builder.CreateStore(value, values.at(write.field));
        }
        llvm::Value* result = lower(*actor.on_message.result, builder, i64, values,
            emptyFields, declarations);
        builder.CreateRet(result);
    }

    // Compiler-generated Wasm guest actor v0. One module instance belongs
    // exclusively to one dedicated Worker; no native OresVM symbols/imports.
    // INIT -> ACTIVE -> STOPPED is monotonic; any illegal call traps. This
    // is NOT source ActorRef/spawn/send/suspend/supervisor lowering.
    if (wasm32_target && !source.actors.empty()) {
        const auto& actor = source.actors.front();
        auto* voidTy = llvm::Type::getVoidTy(context);
        auto* zero = llvm::ConstantInt::get(i64, 0);
        auto* one = llvm::ConstantInt::get(i64, 1);
        auto* two = llvm::ConstantInt::get(i64, 2);
        auto* lifecycle = new llvm::GlobalVariable(module, i64, false,
            llvm::GlobalValue::InternalLinkage, zero, "oreslang.wasm.actor.lifecycle");
        std::vector<llvm::GlobalVariable*> globals;
        for (const auto& field : actor.fields) {
            globals.push_back(new llvm::GlobalVariable(module, i64, false,
                llvm::GlobalValue::InternalLinkage, zero,
                "oreslang.wasm.actor.field." + field.name));
        }
        // Every export validates the guest state itself, even if a buggy
        // host invokes it after teardown or initializes twice.
        auto guard = [&](llvm::IRBuilder<>& b, llvm::Function* fn,
                         llvm::Value* expected) {
            auto* pass = llvm::BasicBlock::Create(context, "admitted", fn);
            auto* denied = llvm::BasicBlock::Create(context, "trap", fn);
            auto* state = b.CreateLoad(i64, lifecycle, "lifecycle");
            b.CreateCondBr(b.CreateICmpEQ(state, expected), pass, denied);
            b.SetInsertPoint(denied);
            // 'unreachable' alone is optimizer UB, not a reliable runtime
            // guard. Emit a real llvm.trap, which lowers to Wasm's trap
            // instruction even at -Oz; never permit duplicate INIT/TEARDOWN.
            b.CreateCall(llvm::Intrinsic::getDeclaration(&module, llvm::Intrinsic::trap));
            b.CreateUnreachable();
            b.SetInsertPoint(pass);
        };
        auto* initialize = llvm::Function::Create(
            llvm::FunctionType::get(voidTy, {}, false),
            llvm::Function::ExternalLinkage, "oreslang_wasm_actor_init", module);
        llvm::IRBuilder<> init(llvm::BasicBlock::Create(context, "entry", initialize));
        guard(init, initialize, zero);
        const std::unordered_map<std::string, llvm::Value*> noStorage;
        const std::unordered_map<std::string, llvm::Function*> noFunctions;
        for (std::size_t i = 0; i < actor.fields.size(); ++i) {
            auto* initial = lower(*actor.fields[i].initializer, init, i64,
                noStorage, noStorage, noFunctions);
            init.CreateStore(initial, globals[i]);
        }
        init.CreateStore(one, lifecycle);
        init.CreateRetVoid();

        auto* handler = llvm::Function::Create(
            llvm::FunctionType::get(i64, {i64}, false),
            llvm::Function::ExternalLinkage, "oreslang_wasm_actor_on_message", module);
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(context, "entry", handler));
        guard(b, handler, one);
        std::unordered_map<std::string, llvm::Value*> values, emptyFields;
        auto* paramSlot = b.CreateAlloca(i64, nullptr, "message.slot");
        b.CreateStore(&*handler->arg_begin(), paramSlot);
        values.emplace(actor.on_message.parameters[0].name, paramSlot);
        for (std::size_t i = 0; i < actor.fields.size(); ++i) {
            auto* slot = b.CreateAlloca(i64, nullptr, actor.fields[i].name + ".slot");
            b.CreateStore(b.CreateLoad(i64, globals[i]), slot);
            values.emplace(actor.fields[i].name, slot);
        }
        for (const auto& write : actor.on_message.actor_field_writes) {
            auto field = std::find_if(actor.fields.begin(), actor.fields.end(),
                [&](const LocalBinding& f) { return f.name == write.field; });
            if (field == actor.fields.end() || !field->field_mutable)
                throw std::runtime_error("invalid wasm32 actor field write");
            auto* value = lower(*write.expression, b, i64, values, emptyFields, noFunctions);
            b.CreateStore(value, values.at(write.field));
        }
        auto* value = lower(*actor.on_message.result, b, i64, values,
            emptyFields, noFunctions);
        // Commit all private state only after evaluating the handler; no
        // host callback or suspension may interleave during this transition.
        for (std::size_t i = 0; i < actor.fields.size(); ++i)
            b.CreateStore(b.CreateLoad(i64, values.at(actor.fields[i].name)), globals[i]);
        b.CreateRet(value);

        auto* teardown = llvm::Function::Create(
            llvm::FunctionType::get(voidTy, {}, false),
            llvm::Function::ExternalLinkage, "oreslang_wasm_actor_teardown", module);
        llvm::IRBuilder<> finish(llvm::BasicBlock::Create(context, "entry", teardown));
        guard(finish, teardown, one);
        for (auto* field : globals) finish.CreateStore(zero, field);
        finish.CreateStore(two, lifecycle);
        finish.CreateRetVoid();
    }
    if (native_main) {
        // OS/process ABI boundary: never expose i64/void Oreslang main as C main.
        // Reject invalid statuses without Unix low-byte truncation (256 -> 0).
        llvm::Type* i32 = llvm::Type::getInt32Ty(context);
        auto* host_main = llvm::Function::Create(
            llvm::FunctionType::get(i32, false),
            llvm::Function::ExternalLinkage, "main", module);
        auto* entry = llvm::BasicBlock::Create(context, "entry", host_main);
        llvm::IRBuilder<> builder(entry);
        llvm::Function* guest = declarations.at(native_function_slot("main", 0));
        llvm::Value* result = builder.CreateCall(guest);
        if (main_it->returns_void) {
            builder.CreateRet(llvm::ConstantInt::get(i32, 0));
        } else {
            llvm::Value* valid = builder.CreateICmpULE(
                result, llvm::ConstantInt::get(i64, 255), "valid.exit.status");
            llvm::Value* narrowed = builder.CreateTrunc(result, i32);
            builder.CreateRet(builder.CreateSelect(valid, narrowed,
                llvm::ConstantInt::get(i32, 1), "checked.exit.status"));
        }
    }

    std::string diagnostics;
    llvm::raw_string_ostream errors(diagnostics);
    if (llvm::verifyModule(module, &errors)) {
        errors.flush();
        throw std::runtime_error("invalid generated LLVM module: " + diagnostics);
    }
    std::string result;
    llvm::raw_string_ostream output(result);
    module.print(output, nullptr);
    output.flush();
    return result;
}

} // namespace oreslang
