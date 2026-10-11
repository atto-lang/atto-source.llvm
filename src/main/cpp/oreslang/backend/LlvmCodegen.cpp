#include "oreslang/backend/LlvmCodegen.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "llvm/Config/llvm-config.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

namespace oreslang {
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
llvm::Value* lower(const Expression& expr,
                   llvm::IRBuilder<>& builder,
                   llvm::Type* i64,
                   const std::unordered_map<std::string, llvm::Value*>& storage,
                   const std::unordered_map<std::string, llvm::Value*>& field_storage,
                   const std::unordered_map<std::string, llvm::Function*>& functions) {
    switch (expr.kind) {
        case Expression::Kind::Integer:
            return llvm::ConstantInt::getSigned(i64, expr.integer);
        case Expression::Kind::Variable:
            return builder.CreateLoad(i64, storage.at(expr.name), expr.name + ".read");
        case Expression::Kind::Field:
            return builder.CreateLoad(i64, field_storage.at(field_key(expr.left->name, expr.name)),
                                      expr.left->name + "." + expr.name + ".read");
        case Expression::Kind::Borrow:
        case Expression::Kind::BorrowMut:
            // Only addressable named parameters are admitted by OwnershipChecker.
            return storage.at(expr.left->name);
        case Expression::Kind::Negate:
            return builder.CreateNeg(lower(*expr.left, builder, i64, storage, field_storage, functions), "neg");
        case Expression::Kind::Copy:
        case Expression::Kind::Take:
        case Expression::Kind::Share:
            // These remain primitive value operations, never graph references.
            return lower(*expr.left, builder, i64, storage, field_storage, functions);
        case Expression::Kind::Add: {
            llvm::Value* left = lower(*expr.left, builder, i64, storage, field_storage, functions);
            llvm::Value* right = lower(*expr.right, builder, i64, storage, field_storage, functions);
            return builder.CreateAdd(left, right, "add");
        }
        case Expression::Kind::Subtract: {
            llvm::Value* left = lower(*expr.left, builder, i64, storage, field_storage, functions);
            llvm::Value* right = lower(*expr.right, builder, i64, storage, field_storage, functions);
            return builder.CreateSub(left, right, "sub");
        }
        case Expression::Kind::Multiply: {
            llvm::Value* left = lower(*expr.left, builder, i64, storage, field_storage, functions);
            llvm::Value* right = lower(*expr.right, builder, i64, storage, field_storage, functions);
            return builder.CreateMul(left, right, "mul");
        }
        case Expression::Kind::Call: {
            std::vector<llvm::Value*> arguments;
            for (const auto& arg : expr.arguments)
                arguments.push_back(lower(*arg, builder, i64, storage, field_storage, functions));
            llvm::Function* callee = functions.at(expr.name);
            return builder.CreateCall(callee, arguments,
                callee->getReturnType()->isVoidTy() ? "" : "call");
        }
    }
    throw std::runtime_error("invalid expression kind while lowering LLVM IR");
}

} // namespace

std::string emit_verified_llvm_ir(const Program& source, bool wasm32_target) {
    llvm::LLVMContext context;
    // Fail closed for Wasm guests without the native actor and heap ABI.
    if (wasm32_target) {
        if (!source.actors.empty())
            throw std::runtime_error("wasm32 source actors are not implemented");
        for (const auto& fn : source.functions) {
            if (!fn.actor_spawns.empty() || !fn.actor_sends.empty() ||
                !fn.actor_stops.empty() || !fn.actor_field_writes.empty())
                throw std::runtime_error("wasm32 actor operations are not implemented");
            for (const auto& local : fn.locals)
                if (local.heap_owned)
                    throw std::runtime_error("wasm32 native heap-owned regions are not implemented");
        }
    }
    llvm::Module module("oreslang-source", context);
    // The wasm32 environment is an explicit code-generation target. This
    // emits verified, target-marked IR; object/linking remains in oreslang-wasm.
    // Never infer browser capabilities from the target triple alone.
    if (wasm32_target) {
        module.setTargetTriple("wasm32-unknown-unknown");
        module.setDataLayout("e-m:e-p:32:32-i64:64-n32:64-S128");
    }
    if (wasm32_target && !source.actors.empty())
        throw std::runtime_error("native actor scheduler/heap ABI unavailable for wasm32");
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
    llvm::FunctionCallee regionOpen, regionNew, regionDrop, regionClose;
    if (has_region_bindings) {
        regionOpen = module.getOrInsertFunction("oreslang_owned_i64_region_open",
            llvm::FunctionType::get(native_ptr, {i64}, false));
        regionNew = module.getOrInsertFunction("oreslang_owned_i64_region_new",
            llvm::FunctionType::get(native_ptr, {native_ptr, i64}, false));
        regionDrop = module.getOrInsertFunction("oreslang_owned_i64_region_drop",
            llvm::FunctionType::get(llvm::Type::getVoidTy(context), {native_ptr, native_ptr}, false));
        regionClose = module.getOrInsertFunction("oreslang_owned_i64_region_close",
            llvm::FunctionType::get(llvm::Type::getVoidTy(context), {native_ptr}, false));
    }

    // Only generated source actors resolve this native scheduler/heap ABI.
    // No actor imports appear in ordinary scalar/class compilation.
    llvm::FunctionCallee fieldInit, fieldInitMutable, fieldRead, fieldWrite;
    llvm::FunctionCallee sourceSpawn, sourceSend, sourceStop;
    std::unordered_map<std::string, llvm::Function*> actorInits, actorHandlers;
    if (!source.actors.empty()) {
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
    for (const auto& fn : source.functions) {
        const std::string symbol = source.module.empty()
            ? fn.name : source.module + "." + fn.name;
        std::vector<llvm::Type*> argument_types;
        for (const auto& param : fn.parameters)
            argument_types.push_back(param.class_name.empty()
                ? (param.mode == Parameter::Mode::Value ? i64 : loan_ptr)
                : class_types.at(param.class_name)->getPointerTo());
        llvm::FunctionType* signature = llvm::FunctionType::get(
            fn.returns_void ? llvm::Type::getVoidTy(context) : i64,
            argument_types, false);
        declarations.emplace(fn.name, llvm::Function::Create(
            signature, llvm::Function::ExternalLinkage, symbol, module));
    }
    for (const auto& fn : source.functions) {
        llvm::Function* function = declarations.at(fn.name);
        llvm::BasicBlock* entry = llvm::BasicBlock::Create(context, "entry", function);
        llvm::IRBuilder<> builder(entry);
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
            llvm::Value* address;
            bool heap_owned;
            std::uint64_t bytes;
        };
        std::vector<LocalSlot> lexical_slots;
        const auto heap_count = std::count_if(fn.locals.begin(), fn.locals.end(),
            [](const LocalBinding& b) { return b.heap_owned; });
        llvm::Value* region = nullptr;
        if (heap_count != 0)
            region = builder.CreateCall(regionOpen,
                {llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(i64),
                                        static_cast<std::uint64_t>(heap_count))}, "native.owned.region");
        for (const auto& binding : fn.locals) {
            if (binding.new_class || binding.move_class) {
                llvm::StructType* shape = class_types.at(binding.class_name);
                llvm::Value* slot = builder.CreateAlloca(shape, nullptr, binding.name + ".record");
                const auto cls = std::find_if(source.classes.begin(), source.classes.end(),
                    [&](const ClassDecl& item) { return item.name == binding.class_name; });
                const std::uint64_t bytes = static_cast<std::uint64_t>(cls->fields.size()) * 8;
                builder.CreateLifetimeStart(slot,
                    llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(i64), bytes));
                for (std::size_t i = 0; i < cls->fields.size(); ++i) {
                    const ClassField& item = cls->fields[i];
                    llvm::Value* address = builder.CreateStructGEP(shape, slot,
                        static_cast<unsigned>(i), binding.name + "." + item.name);
                    // The admitted stack class contains only trivially movable i64 fields.
                    llvm::Value* initial = binding.new_class
                        ? static_cast<llvm::Value*>(llvm::ConstantInt::getSigned(i64, item.initial))
                        : builder.CreateLoad(i64,
                            field_storage.at(field_key(binding.move_source, item.name)),
                            binding.move_source + "." + item.name + ".take");
                    builder.CreateStore(initial, address);
                    field_storage.emplace(field_key(binding.name, item.name), address);
                }
                storage.emplace(binding.name, slot);
                lexical_slots.push_back({slot, false, bytes});
                continue;
            }
            llvm::Value* initial = lower(*binding.initializer, builder, i64, storage, field_storage, declarations);
            llvm::Value* slot = nullptr;
            if (binding.heap_owned) {
                slot = builder.CreateCall(regionNew, {region, initial}, binding.name + ".heap.owned");
            } else {
                slot = builder.CreateAlloca(i64, nullptr, binding.name + ".stack.owned");
                builder.CreateLifetimeStart(slot, llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(i64), 8));
                builder.CreateStore(initial, slot);
            }
            storage.emplace(binding.name, slot);
            lexical_slots.push_back({slot, binding.heap_owned, 8});
        }
        std::vector<const Expression*> deferred_calls;
        for (const auto& stmt : fn.statements) {
            if (stmt.kind == Statement::Kind::Defer) {
                deferred_calls.push_back(stmt.expression.get());
                continue;
            }
            if (stmt.kind == Statement::Kind::FieldWrite) {
                llvm::Value* updated = lower(*stmt.expression, builder, i64, storage, field_storage, declarations);
                builder.CreateStore(updated, field_storage.at(field_key(stmt.receiver, stmt.field)));
            } else {
                lower(*stmt.expression, builder, i64, storage, field_storage, declarations);
            }
        }

        // Emit observable actor effects in source order. Reordering all sends
        // ahead of all stops incorrectly executes a stopped worker's messages.
        for (const auto& action : fn.actor_actions) {
            if (action.kind == ActorAction::Kind::Send) {
                const auto& send = fn.actor_sends.at(action.index);
                llvm::Value* payload = lower(*send.payload, builder, i64, storage, field_storage, declarations);
                builder.CreateCall(sourceSend, {actorRefs.at(send.reference), payload});
            } else {
                builder.CreateCall(sourceStop, {actorRefs.at(fn.actor_stops.at(action.index))});
            }
        }
        // Each taken return terminates in its own reverse-order cleanup
        // block. All bindings were initialized before the first guard, so
        // every return owns the same lexical slots and native region.
        // Branch-local class creation / effectful conditions fail admission.
        auto emit_cleanup_return = [&](llvm::Value* value) {
            // Capture/evaluate at scope exit, in LIFO order after return
            // evaluation and before releasing lexical owned values.
            for (auto it = deferred_calls.rbegin(); it != deferred_calls.rend(); ++it)
                lower(**it, builder, i64, storage, field_storage, declarations);
            for (auto it = lexical_slots.rbegin(); it != lexical_slots.rend(); ++it) {
                if (it->heap_owned) builder.CreateCall(regionDrop, {region, it->address});
                else builder.CreateLifetimeEnd(it->address,
                    llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(i64), it->bytes));
            }
            if (region != nullptr) builder.CreateCall(regionClose, {region});
            if (fn.returns_void) builder.CreateRetVoid();
            else builder.CreateRet(value);
        };
        for (const auto& guard : fn.early_returns) {
            llvm::Value* left = lower(*guard.lhs, builder, i64, storage, field_storage, declarations);
            llvm::Value* right = lower(*guard.rhs, builder, i64, storage, field_storage, declarations);
            llvm::Value* equal = builder.CreateICmpEQ(left, right, "return.guard.eq");
            llvm::BasicBlock* taken = llvm::BasicBlock::Create(context, "guard.return", function);
            llvm::BasicBlock* next = llvm::BasicBlock::Create(context, "guard.next", function);
            builder.CreateCondBr(equal, taken, next);
            builder.SetInsertPoint(taken);
            llvm::Value* selected = lower(*guard.result, builder, i64,
                storage, field_storage, declarations);
            emit_cleanup_return(selected);
            builder.SetInsertPoint(next);
        }
        llvm::Value* result = fn.result
            ? lower(*fn.result, builder, i64, storage, field_storage, declarations) : nullptr;
        emit_cleanup_return(result);
    }


    // Generated actor methods execute *only* from the admitted native
    // scheduler turn, with persistent actor-owned heap and fresh turn tokens.
    for (const auto& actor : source.actors) {
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
