#include "oreslang/backend/LlvmCodegen.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

namespace oreslang {
namespace {

llvm::Value* lower(const Expression& expr,
                   llvm::IRBuilder<>& builder,
                   llvm::Type* i64,
                   const std::unordered_map<std::string, llvm::Value*>& parameters,
                   const std::unordered_map<std::string, llvm::Value*>& locals,
                   const std::unordered_map<std::string, llvm::Function*>& functions) {
    switch (expr.kind) {
        case Expression::Kind::Integer:
            return llvm::ConstantInt::getSigned(i64, expr.integer);
        case Expression::Kind::Variable:
            if (auto local = locals.find(expr.name); local != locals.end()) {
                return builder.CreateLoad(i64, local->second, expr.name + ".load");
            }
            return parameters.at(expr.name);
        case Expression::Kind::Negate:
            return builder.CreateNeg(lower(*expr.left, builder, i64, parameters, locals, functions),
                                     "neg");
        case Expression::Kind::Add:
            return builder.CreateAdd(lower(*expr.left, builder, i64, parameters, locals, functions),
                                     lower(*expr.right, builder, i64, parameters, locals, functions),
                                     "add");
        case Expression::Kind::Subtract:
            return builder.CreateSub(lower(*expr.left, builder, i64, parameters, locals, functions),
                                     lower(*expr.right, builder, i64, parameters, locals, functions),
                                     "sub");
        case Expression::Kind::Multiply:
            return builder.CreateMul(lower(*expr.left, builder, i64, parameters, locals, functions),
                                     lower(*expr.right, builder, i64, parameters, locals, functions),
                                     "mul");
        case Expression::Kind::Call: {
            std::vector<llvm::Value*> arguments;
            for (const auto& arg : expr.arguments) {
                arguments.push_back(lower(*arg, builder, i64, parameters, locals, functions));
            }
            return builder.CreateCall(functions.at(expr.name), arguments, "call");
        }
    }
    throw std::runtime_error("invalid expression kind while lowering LLVM IR");
}

} // namespace

std::string emit_verified_llvm_ir(const Program& source) {
    llvm::LLVMContext context;
    llvm::Module module("oreslang-source", context);
    llvm::IntegerType* i64 = llvm::Type::getInt64Ty(context);
    llvm::Type* ptr = llvm::Type::getInt8PtrTy(context);
    llvm::FunctionCallee heapOpen = module.getOrInsertFunction(
            "oreslang_owned_i64_region_open",
            llvm::FunctionType::get(ptr, {i64}, false));
    llvm::FunctionCallee heapNew = module.getOrInsertFunction(
            "oreslang_owned_i64_region_new",
            llvm::FunctionType::get(ptr, {ptr, i64}, false));
    llvm::FunctionCallee heapDrop = module.getOrInsertFunction(
            "oreslang_owned_i64_region_drop",
            llvm::FunctionType::get(llvm::Type::getVoidTy(context), {ptr, ptr}, false));
    llvm::FunctionCallee heapClose = module.getOrInsertFunction(
            "oreslang_owned_i64_region_close",
            llvm::FunctionType::get(llvm::Type::getVoidTy(context), {ptr}, false));

    llvm::FunctionCallee fieldInit = module.getOrInsertFunction(
            "ores_actor_i64_heap_field_init",
            llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                    {ptr, i64, i64, i64, i64}, false));
    llvm::FunctionCallee fieldRead = module.getOrInsertFunction(
            "ores_actor_i64_heap_field_read",
            llvm::FunctionType::get(i64, {ptr, i64, i64, i64}, false));

    llvm::FunctionCallee fieldInitMutable = module.getOrInsertFunction(
            "ores_actor_i64_heap_field_init_mutable",
            llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                    {ptr, i64, i64, i64, i64}, false));
    llvm::FunctionCallee fieldWrite = module.getOrInsertFunction(
            "ores_actor_i64_heap_field_write",
            llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                    {ptr, i64, i64, i64, i64}, false));

    std::unordered_map<std::string, llvm::Function*> declarations;
    std::unordered_map<std::string, llvm::Function*> actorInitializers;
    std::unordered_map<std::string, llvm::Function*> actorMessageHandlers;
    std::unordered_map<std::string, std::size_t> actorFieldCounts;
    for (const auto& actor : source.actors) {
        auto* ctorType = llvm::FunctionType::get(
            llvm::Type::getVoidTy(context), {ptr, i64, i64}, false);
        actorInitializers.emplace(actor.name, llvm::Function::Create(
            ctorType, llvm::Function::ExternalLinkage,
            "oreslang_actor_" + actor.name + "_init", module));
        actorFieldCounts.emplace(actor.name, actor.fields.size());
        const auto method = std::find_if(actor.methods.begin(), actor.methods.end(),
            [](const Function& f) { return f.name == "on_message"; });
        if (method != actor.methods.end() && method->parameters.size() == 1) {
            auto* msgType = llvm::FunctionType::get(i64, {ptr, i64, i64, i64}, false);
            actorMessageHandlers.emplace(actor.name, llvm::Function::Create(
                msgType, llvm::Function::ExternalLinkage,
                "oreslang_actor_" + actor.name + "_on_message", module));
        }
    }
    const auto sourceSpawn = module.getOrInsertFunction(
        "oreslang_source_actor_spawn",
        llvm::FunctionType::get(ptr, {ptr, ptr, i64}, false));
    const auto sourceStop = module.getOrInsertFunction(
        "oreslang_source_actor_stop",
        llvm::FunctionType::get(llvm::Type::getVoidTy(context), {ptr}, false));
    const auto sourceSend = module.getOrInsertFunction(
        "oreslang_source_actor_send",
        llvm::FunctionType::get(llvm::Type::getVoidTy(context), {ptr, i64}, false));
    // Host main blocks only at its outer ABI boundary. The guest continuation
    // itself executes on the root ActorGroup CONTROL lane after a real Future
    // completes, never inline on the initializing worker.
    const auto sourceAwaitReady = module.getOrInsertFunction(
        "oreslang_source_actor_await_ready",
        llvm::FunctionType::get(i64, {ptr, ptr}, false));

    // Declare all functions first, enabling forward calls and recursion.
    for (const auto& fn : source.functions) {
        const std::string symbol = source.module.empty()
            ? fn.name : source.module + "." + fn.name;
        const std::vector<llvm::Type*> argument_types(fn.parameters.size(), i64);
        llvm::FunctionType* signature = llvm::FunctionType::get(i64, argument_types, false);
        // ABI and public/private linker visibility are not stable yet.
        declarations.emplace(fn.name, llvm::Function::Create(
            signature, llvm::Function::ExternalLinkage, symbol, module));
    }
    for (const auto& fn : source.functions) {
        llvm::Function* function = declarations.at(fn.name);
        llvm::BasicBlock* entry = llvm::BasicBlock::Create(context, "entry", function);
        llvm::IRBuilder<> builder(entry);
        std::unordered_map<std::string, llvm::Value*> parameters;
        std::size_t index = 0;
        for (auto& arg : function->args()) {
            arg.setName(fn.parameters[index]);
            parameters.emplace(fn.parameters[index], &arg);
            ++index;
        }
        std::unordered_map<std::string, llvm::Value*> actorRefs;
        for (const auto& spawn : fn.actor_spawns) {
            auto found = actorMessageHandlers.find(spawn.actor_type);
            llvm::Value* handler = found == actorMessageHandlers.end()
                ? static_cast<llvm::Value*>(llvm::ConstantPointerNull::get(
                    llvm::cast<llvm::PointerType>(ptr)))
                : static_cast<llvm::Value*>(found->second);
            auto* ref = builder.CreateCall(sourceSpawn,
                {actorInitializers.at(spawn.actor_type), handler,
                 llvm::ConstantInt::get(i64, actorFieldCounts.at(spawn.actor_type))},
                spawn.name + ".native.actor.ref");
            actorRefs.emplace(spawn.name, ref);
        }
        // Scalar locals are either stack owned ('int') or explicitly backed
        // by a bounded native lexical heap region ('owned int'). The latter
        // has one region per function activation. This is not yet an actor
        // heap that survives between mailbox turns.
        struct OwnedSlot { llvm::Value* address; bool heap; };
        std::unordered_map<std::string, llvm::Value*> locals;
        std::vector<OwnedSlot> slots;
        const auto heapCount = std::count_if(
                fn.locals.begin(), fn.locals.end(),
                [](const LocalBinding& item) { return item.heap_owned; });
        llvm::Value* region = nullptr;
        if (heapCount != 0) {
            region = builder.CreateCall(heapOpen, {
                    llvm::ConstantInt::get(i64, static_cast<std::uint64_t>(heapCount))
            }, "actor.owned.lexical.region");
        }
        for (const auto& local : fn.locals) {
            llvm::Value* initial = lower(*local.initializer, builder, i64,
                                         parameters, locals, declarations);
            llvm::Value* address = nullptr;
            if (local.heap_owned) {
                address = builder.CreateCall(heapNew, {region, initial},
                                             local.name + ".native.owned");
            } else {
                address = builder.CreateAlloca(i64, nullptr, local.name + ".stack.owned");
                builder.CreateLifetimeStart(address, llvm::ConstantInt::get(i64, 8));
                builder.CreateStore(initial, address);
            }
            locals.emplace(local.name, address);
            slots.push_back({address, local.heap_owned});
        }
        llvm::Value* result = nullptr;
        if (!fn.ready_await_actor.empty()) {
            // First compiler-generated suspend point: the program's original
            // post-await guest work is compiled into a separate callback.
            // The trusted top-level entrypoint waits outside the native pool;
            // the callback is scheduled on the exclusive root actor lane.
            auto* continuationType = llvm::FunctionType::get(i64, {ptr}, false);
            auto* resume = llvm::Function::Create(continuationType,
                llvm::Function::InternalLinkage,
                function->getName().str() + ".ready_continuation", module);
            auto* resumed = llvm::BasicBlock::Create(context, "admitted", resume);
            llvm::IRBuilder<> resumeBuilder(resumed);
            std::unordered_map<std::string, llvm::Value*> resumedActors{
                {fn.ready_await_actor, &*resume->arg_begin()}};
            const std::unordered_map<std::string, llvm::Value*> noLocals;
            const std::unordered_map<std::string, llvm::Value*> noParams;
            for (const auto& send : fn.actor_sends) {
                auto* payload = lower(*send.payload, resumeBuilder, i64,
                                      noParams, noLocals, declarations);
                resumeBuilder.CreateCall(sourceSend,
                    {resumedActors.at(send.reference), payload});
            }
            auto* resumedResult = lower(*fn.result, resumeBuilder, i64,
                                        noParams, noLocals, declarations);
            resumeBuilder.CreateRet(resumedResult);
            result = builder.CreateCall(sourceAwaitReady,
                {actorRefs.at(fn.ready_await_actor), resume}, "future.await.result");
        } else {
            result = lower(*fn.result, builder, i64, parameters,
                           locals, declarations);
            for (const auto& send : fn.actor_sends) {
                llvm::Value* payload = lower(*send.payload, builder, i64,
                                              parameters, locals, declarations);
                builder.CreateCall(sourceSend, {actorRefs.at(send.reference), payload});
            }
        }
        for (const auto& name : fn.actor_stops) {
            builder.CreateCall(sourceStop, {actorRefs.at(name)});
        }
        for (auto iter = slots.rbegin(); iter != slots.rend(); ++iter) {
            if (iter->heap) {
                builder.CreateCall(heapDrop, {region, iter->address});
            } else {
                builder.CreateLifetimeEnd(iter->address, llvm::ConstantInt::get(i64, 8));
            }
        }
        if (region != nullptr) builder.CreateCall(heapClose, {region});
        builder.CreateRet(result);
    }

    // Actor field layout is fixed and private to an admitted native actor
    // heap. Runtime/FFI code is responsible for creating a heap with at least
    // actor.fields.size() slots and passing fresh scheduler turn tokens.
    for (const auto& actor : source.actors) {
        const std::string symbol = "oreslang_actor_" + actor.name;
        auto* initTy = llvm::FunctionType::get(
                llvm::Type::getVoidTy(context), {ptr, i64, i64}, false);
        auto* initialize = actorInitializers.at(actor.name);
        if (initialize->getFunctionType() != initTy)
            throw std::runtime_error("generated actor initializer ABI mismatch");
        auto* initBlock = llvm::BasicBlock::Create(context, "entry", initialize);
        llvm::IRBuilder<> initBuilder(initBlock);
        auto arg = initialize->arg_begin();
        llvm::Value* actorHeap = &*arg++;
        llvm::Value* domain = &*arg++;
        llvm::Value* epoch = &*arg++;
        const std::unordered_map<std::string, llvm::Value*> noValues;
        const std::unordered_map<std::string, llvm::Function*> noFunctions;
        for (std::size_t index = 0; index < actor.fields.size(); ++index) {
            auto* value = lower(*actor.fields[index].initializer, initBuilder, i64,
                                noValues, noValues, noFunctions);
            initBuilder.CreateCall(actor.fields[index].field_mutable ? fieldInitMutable : fieldInit,
                 {actorHeap, domain, epoch,
                  llvm::ConstantInt::get(i64, index), value});
        }
        initBuilder.CreateRetVoid();

        for (const auto& method : actor.methods) {
            std::vector<llvm::Type*> types{ptr, i64, i64};
            types.insert(types.end(), method.parameters.size(), i64);
            auto* signature = llvm::FunctionType::get(i64, types, false);
            auto existing = actorMessageHandlers.find(actor.name);
            auto* fn = method.name == "on_message" && existing != actorMessageHandlers.end()
                    ? existing->second
                    : llvm::Function::Create(signature, llvm::Function::ExternalLinkage,
                                             symbol + "_" + method.name, module);
            if (fn->getFunctionType() != signature)
                throw std::runtime_error("generated actor message ABI mismatch");
            auto* block = llvm::BasicBlock::Create(context, "entry", fn);
            llvm::IRBuilder<> builder(block);
            auto args = fn->arg_begin();
            llvm::Value* heap = &*args++;
            llvm::Value* currentDomain = &*args++;
            llvm::Value* currentEpoch = &*args++;
            std::unordered_map<std::string, llvm::Value*> parameters;
            for (const auto& name : method.parameters) {
                parameters.emplace(name, &*args++);
            }
            std::unordered_map<std::string, llvm::Value*> locals;
            for (std::size_t index = 0; index < actor.fields.size(); ++index) {
                llvm::Value* value = builder.CreateCall(fieldRead,
                    {heap, currentDomain, currentEpoch,
                     llvm::ConstantInt::get(i64, index)}, actor.fields[index].name);
                auto* slot = builder.CreateAlloca(i64, nullptr,
                                                actor.fields[index].name + ".field");
                builder.CreateStore(value, slot);
                locals.emplace(actor.fields[index].name, slot);
            }
            struct LocalSlot {llvm::Value* slot; bool heap;};
            std::vector<LocalSlot> ownedSlots;
            const auto count = std::count_if(method.locals.begin(), method.locals.end(),
                    [](const LocalBinding& local){return local.heap_owned;});
            llvm::Value* region = nullptr;
            if (count) region = builder.CreateCall(heapOpen,
                   {llvm::ConstantInt::get(i64, static_cast<std::uint64_t>(count))});
            for (const auto& local : method.locals) {
                auto* value = lower(*local.initializer, builder, i64,
                                    parameters, locals, declarations);
                llvm::Value* address = nullptr;
                if (local.heap_owned) {
                    address = builder.CreateCall(heapNew, {region, value});
                } else {
                    address = builder.CreateAlloca(i64, nullptr, local.name + ".local");
                    builder.CreateLifetimeStart(address, llvm::ConstantInt::get(i64, 8));
                    builder.CreateStore(value, address);
                }
                locals.emplace(local.name, address);
                ownedSlots.push_back({address, local.heap_owned});
            }
            for (const auto& assignment : method.assignments) {
                const auto match = std::find_if(actor.fields.begin(), actor.fields.end(),
                    [&](const LocalBinding& f) { return f.name == assignment.field; });
                // Parser validation proves the field exists and is mutable.
                if (match == actor.fields.end() || !match->field_mutable)
                    throw std::runtime_error("invalid actor field mutation lowering");
                llvm::Value* updated = lower(*assignment.value, builder, i64,
                                             parameters, locals, declarations);
                builder.CreateCall(fieldWrite,
                    {heap, currentDomain, currentEpoch,
                     llvm::ConstantInt::get(i64, std::distance(actor.fields.begin(), match)), updated});
                builder.CreateStore(updated, locals.at(assignment.field));
            }
            auto* result = lower(*method.result, builder, i64,
                                 parameters, locals, declarations);
            for (auto iter = ownedSlots.rbegin(); iter != ownedSlots.rend(); ++iter) {
                if (iter->heap) builder.CreateCall(heapDrop, {region, iter->slot});
                else builder.CreateLifetimeEnd(iter->slot, llvm::ConstantInt::get(i64, 8));
            }
            if (region) builder.CreateCall(heapClose, {region});
            builder.CreateRet(result);
        }
    }

    std::string diagnostics;
    llvm::raw_string_ostream errors(diagnostics);
    if (llvm::verifyModule(module, &errors)) {
        errors.flush();
        throw std::runtime_error("invalid generated LLVM module: " + diagnostics);
    }
    // Strict no-tracing-GC admission. The current backend emits stack-local scalars and an audited
    // bounded native i64 region.  Any future lowering must explicitly prove that its new
    // native calls cannot invoke a collector or allocate managed objects.
    // Reject indirect/unknown external calls by default.
    for (const llvm::Function& function : module) {
        if (function.isDeclaration()) continue;
        if (function.hasGC()) {
            throw std::runtime_error("LLVM strict no-GC forbids GC strategy annotations");
        }
        for (const llvm::BasicBlock& block : function) {
            for (const llvm::Instruction& instruction : block) {
                auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                if (!call) continue;
                const llvm::Function* target = call->getCalledFunction();
                if (target == nullptr) {
                    throw std::runtime_error("LLVM strict no-GC rejects indirect calls without ownership proof");
                }
                if (target->isDeclaration()) {
                    const std::string name = target->getName().str();
                    if (name.rfind("llvm.lifetime.start", 0) != 0
                            && name.rfind("llvm.lifetime.end", 0) != 0
                            && name != "oreslang_owned_i64_region_open"
                            && name != "oreslang_owned_i64_region_new"
                            && name != "oreslang_owned_i64_region_drop"
                            && name != "oreslang_owned_i64_region_close"
                            && name != "ores_actor_i64_heap_field_init"
                            && name != "ores_actor_i64_heap_field_read"
                            && name != "ores_actor_i64_heap_field_init_mutable"
                            && name != "ores_actor_i64_heap_field_write"
                            && name != "oreslang_source_actor_spawn"
                            && name != "oreslang_source_actor_stop"
                            && name != "oreslang_source_actor_send"
                            && name != "oreslang_source_actor_await_ready") {
                        throw std::runtime_error("LLVM strict no-GC rejects unverified external call '" + name + "'");
                    }
                }
            }
        }
    }

    std::string result;
    llvm::raw_string_ostream output(result);
    module.print(output, nullptr);
    output.flush();
    return result;
}

} // namespace oreslang
