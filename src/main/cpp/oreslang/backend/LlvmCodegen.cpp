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

/* Versioned shared Java/Graal + LLVM layout fingerprint. Not a capability,
 * security token or memory allocator identity. All names in this compiler
 * subset are ASCII lexer identifiers, and each field's type is i64. */
std::uint64_t actor_layout_v1(const ActorDecl& actor) {
    std::uint64_t hash = UINT64_C(14695981039346656037);
    auto octet = [&](unsigned char ch) {
        hash ^= ch;
        hash *= UINT64_C(1099511628211);
    };
    auto name = [&](const std::string& value) {
        for (unsigned char ch : value) octet(ch);
        octet(0);
    };
    for (unsigned char ch : std::string("ores.actor.fields.v1"))
        octet(ch);
    octet(0);
    name(actor.name);
    for (const ActorField& field : actor.fields) {
        name(field.name);
        name("i64");
        octet(field.mutable_field ? 1 : 0);
    }
    return hash;
}


struct ActorContext {
    llvm::Value* heap;
    llvm::Value* domain;
    llvm::Value* epoch;
    llvm::Value* fields;
    llvm::FunctionCallee get;
    const std::unordered_map<std::string, std::uint32_t>* field_index;
};

llvm::Value* lower(const Expression& expr,
                   llvm::IRBuilder<>& builder,
                   llvm::Type* i64,
                   const std::unordered_map<std::string, llvm::Value*>& parameters,
                   const std::unordered_map<std::string, llvm::Value*>& locals,
                   const std::unordered_map<std::string, llvm::Function*>& functions,
                   const ActorContext* actor = nullptr) {
    switch (expr.kind) {
        case Expression::Kind::ActorField: {
            if (!actor) throw std::runtime_error("actor field without trusted actor context");
            const auto index = actor->field_index->at(expr.name);
            return builder.CreateCall(actor->get, {
                actor->heap, actor->domain, actor->epoch, actor->fields,
                builder.getInt32(index)
            }, expr.name + ".native.actor.field");
        }
        case Expression::Kind::Integer:
            return llvm::ConstantInt::getSigned(i64, expr.integer);
        case Expression::Kind::Variable:
            if (auto local = locals.find(expr.name); local != locals.end()) {
                return builder.CreateLoad(i64, local->second, expr.name + ".load");
            }
            return parameters.at(expr.name);
        case Expression::Kind::Negate:
            return builder.CreateNeg(lower(*expr.left, builder, i64, parameters, locals, functions, actor),
                                     "neg");
        case Expression::Kind::Add:
            return builder.CreateAdd(lower(*expr.left, builder, i64, parameters, locals, functions, actor),
                                     lower(*expr.right, builder, i64, parameters, locals, functions, actor),
                                     "add");
        case Expression::Kind::Subtract:
            return builder.CreateSub(lower(*expr.left, builder, i64, parameters, locals, functions, actor),
                                     lower(*expr.right, builder, i64, parameters, locals, functions, actor),
                                     "sub");
        case Expression::Kind::Multiply:
            return builder.CreateMul(lower(*expr.left, builder, i64, parameters, locals, functions, actor),
                                     lower(*expr.right, builder, i64, parameters, locals, functions, actor),
                                     "mul");
        case Expression::Kind::Call: {
            std::vector<llvm::Value*> arguments;
            for (const auto& arg : expr.arguments) {
                arguments.push_back(lower(*arg, builder, i64, parameters, locals, functions, actor));
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

    std::unordered_map<std::string, llvm::Function*> declarations;

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
        llvm::Value* result = lower(*fn.result, builder, i64, parameters,
                                    locals, declarations);
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

    if (source.actor) {
        // Host-only generated entrypoints. Every call is authorized by the
        // supervisor's admitted turn; source cannot access these arguments.
        const ActorDecl& actor = *source.actor;
        llvm::Type* i32 = llvm::Type::getInt32Ty(context);
        llvm::Type* void_type = llvm::Type::getVoidTy(context);
        llvm::FunctionCallee fieldCreate = module.getOrInsertFunction(
            "ores_actor_i64_fields_codegen_create",
            llvm::FunctionType::get(ptr, {ptr, i64, i64, ptr, i32}, false));
        llvm::FunctionCallee fieldGet = module.getOrInsertFunction(
            "ores_actor_i64_fields_codegen_get",
            llvm::FunctionType::get(i64, {ptr, i64, i64, ptr, i32}, false));
        llvm::FunctionCallee fieldSet = module.getOrInsertFunction(
            "ores_actor_i64_fields_codegen_set",
            llvm::FunctionType::get(void_type, {ptr, i64, i64, ptr, i32, i64}, false));
        llvm::FunctionCallee fieldDrop = module.getOrInsertFunction(
            "ores_actor_i64_fields_codegen_drop",
            llvm::FunctionType::get(void_type, {ptr, i64, i64, ptr}, false));
        const std::string prefix = "ores_actor_" + actor.name + "_";
        auto entrypoint = [&](const std::string& name, llvm::Type* result,
                              const std::vector<llvm::Type*>& params) {
            return llvm::Function::Create(
                llvm::FunctionType::get(result, params, false),
                llvm::Function::ExternalLinkage, prefix + name, module);
        };
        llvm::Function* version = entrypoint("layout_version", i32, {});
        {
            llvm::IRBuilder<> builder(
                llvm::BasicBlock::Create(context, "entry", version));
            builder.CreateRet(builder.getInt32(1));
        }
        llvm::Function* layout_hash = entrypoint("layout_hash", i64, {});
        {
            llvm::IRBuilder<> builder(
                llvm::BasicBlock::Create(context, "entry", layout_hash));
            builder.CreateRet(llvm::ConstantInt::get(i64, actor_layout_v1(actor)));
        }
        auto params = [](llvm::Function* function) {
            std::vector<llvm::Value*> result;
            for (auto& arg : function->args()) result.push_back(&arg);
            return result;
        };
        std::unordered_map<std::string, std::uint32_t> field_indices;
        for (std::uint32_t i = 0; i < actor.fields.size(); ++i)
            field_indices.emplace(actor.fields[i].name, i);
        const std::unordered_map<std::string, llvm::Value*> none;
        const std::unordered_map<std::string, llvm::Function*> no_functions;
        llvm::Function* init = entrypoint("init", ptr, {ptr, i64, i64});
        {
            llvm::IRBuilder<> builder(llvm::BasicBlock::Create(context, "entry", init));
            const auto args = params(init);
            llvm::ArrayType* array = llvm::ArrayType::get(i64, actor.fields.size());
            llvm::Value* values = builder.CreateAlloca(array, nullptr, "actor.initial.values");
            for (std::uint32_t i = 0; i < actor.fields.size(); ++i) {
                llvm::Value* initial = lower(*actor.fields[i].initializer,
                                              builder, i64, none, none, no_functions);
                llvm::Value* address = builder.CreateInBoundsGEP(
                    array, values, {builder.getInt32(0), builder.getInt32(i)});
                builder.CreateStore(initial, address);
            }
            llvm::Value* first = builder.CreateInBoundsGEP(
                array, values, {builder.getInt32(0), builder.getInt32(0)});
            llvm::Value* record = builder.CreateCall(fieldCreate, {
                args[0], args[1], args[2], first,
                builder.getInt32(static_cast<std::uint32_t>(actor.fields.size()))
            }, "actor.persistent.fields");
            builder.CreateRet(record);
        }
        std::unordered_map<std::string, llvm::Function*> generated_methods;
        for (const auto& method : actor.methods) {
            std::vector<llvm::Type*> signature = {ptr, i64, i64, ptr};
            signature.insert(signature.end(), method.parameters.size(), i64);
            llvm::Function* function = entrypoint(method.name, i64, signature);
            generated_methods.emplace(method.name, function);
            llvm::IRBuilder<> builder(llvm::BasicBlock::Create(context, "entry", function));
            const auto args = params(function);
            ActorContext owned{args[0], args[1], args[2], args[3], fieldGet, &field_indices};
            std::unordered_map<std::string, llvm::Value*> inputs;
            for (std::size_t i = 0; i < method.parameters.size(); ++i) {
                args[4 + i]->setName(method.parameters[i]);
                inputs.emplace(method.parameters[i], args[4 + i]);
            }
            for (const auto& assignment : method.assignments) {
                llvm::Value* value = lower(*assignment.value, builder, i64,
                                           inputs, none, no_functions, &owned);
                builder.CreateCall(fieldSet, {
                    owned.heap, owned.domain, owned.epoch, owned.fields,
                    builder.getInt32(field_indices.at(assignment.field)), value
                });
            }
            builder.CreateRet(lower(*method.result, builder, i64, inputs,
                                    none, no_functions, &owned));
        }
        llvm::Function* drop = entrypoint("drop", void_type, {ptr, i64, i64, ptr});
        {
            llvm::IRBuilder<> builder(llvm::BasicBlock::Create(context, "entry", drop));
            const auto args = params(drop);
            builder.CreateCall(fieldDrop, {args[0], args[1], args[2], args[3]});
            builder.CreateRetVoid();
        }

        // The source actor's optional fnc main is lowered to a resumable
        // pull-style continuation, NOT an OS-carrier-blocking entrypoint.
        // ABI: step(ctx, state: *i32, result: *i64) returns 0=yield,
        // 1=finished, -1=terminal failure. Only trusted host functions may
        // admit actor work or inspect the two future readiness tickets.
        if (source.script) {
            const ActorScript& script = *source.script;
            auto host = [&](const std::string& name, llvm::Type* result,
                            const std::vector<llvm::Type*>& args) {
                return module.getOrInsertFunction(name,
                    llvm::FunctionType::get(result, args, false));
            };
            auto spawn_actor = host("ores_actor_host_spawn", i32,
                {ptr, ptr, ptr, ptr, ptr, i32, i64});
            auto ready_poll = host("ores_actor_host_poll_ready", i32, {ptr});
            auto invoke = host("ores_actor_host_call_i64", i32, {ptr, ptr, i64});
            auto result_poll = host("ores_actor_host_poll_i64", i32, {ptr, ptr});
            auto request_stop = host("ores_actor_host_stop", i32, {ptr});
            auto done_poll = host("ores_actor_host_poll_done", i32, {ptr});

            llvm::Function* step = entrypoint("program_step", i32, {ptr, ptr, ptr});
            auto arguments = params(step);
            llvm::Value* host_ctx = arguments[0];
            llvm::Value* state = arguments[1];
            llvm::Value* output = arguments[2];
            llvm::IRBuilder<> entry(llvm::BasicBlock::Create(context, "entry", step));
            auto* invalid = llvm::BasicBlock::Create(context, "invalid", step);
            auto* phase0 = llvm::BasicBlock::Create(context, "spawn", step);
            auto* phase1 = llvm::BasicBlock::Create(context, "await_ready", step);
            auto* phase2 = llvm::BasicBlock::Create(context, "enqueue_message", step);
            auto* phase3 = llvm::BasicBlock::Create(context, "await_message", step);
            auto* phase4 = llvm::BasicBlock::Create(context, "stop", step);
            auto* phase5 = llvm::BasicBlock::Create(context, "await_done", step);
            auto* decision = entry.CreateSwitch(entry.CreateLoad(i32, state), invalid, 6);
            decision->addCase(entry.getInt32(0), phase0);
            decision->addCase(entry.getInt32(1), phase1);
            decision->addCase(entry.getInt32(2), phase2);
            decision->addCase(entry.getInt32(3), phase3);
            decision->addCase(entry.getInt32(4), phase4);
            decision->addCase(entry.getInt32(5), phase5);
            llvm::IRBuilder<> bad(invalid);
            bad.CreateRet(bad.getInt32(-1));

            auto finish_phase = [&](llvm::IRBuilder<>& builder,
                                    llvm::Value* status, int next, bool terminal) {
                // Poll never waits for a future or holds an OS carrier.
                auto* pending = llvm::BasicBlock::Create(context, "yield_pending", step);
                auto* nonpending = llvm::BasicBlock::Create(context, "nonpending", step);
                builder.CreateCondBr(builder.CreateICmpEQ(status, builder.getInt32(0)),
                                     pending, nonpending);
                llvm::IRBuilder<> wait(pending);
                wait.CreateRet(wait.getInt32(0));
                llvm::IRBuilder<> test(nonpending);
                auto* failed = llvm::BasicBlock::Create(context, "host_failed", step);
                auto* succeeded = llvm::BasicBlock::Create(context, "host_succeeded", step);
                test.CreateCondBr(test.CreateICmpSLT(status, test.getInt32(0)),
                                  failed, succeeded);
                llvm::IRBuilder<> failure(failed);
                failure.CreateRet(failure.getInt32(-1));
                llvm::IRBuilder<> success(succeeded);
                success.CreateStore(success.getInt32(next), state);
                success.CreateRet(success.getInt32(terminal ? 1 : 0));
            };

            {
                llvm::IRBuilder<> builder(phase0);
                llvm::Value* status = builder.CreateCall(spawn_actor, {
                    host_ctx, init, drop, layout_hash, version,
                    builder.getInt32(static_cast<std::uint32_t>(actor.fields.size())),
                    llvm::ConstantInt::get(i64, actor_layout_v1(actor))
                });
                finish_phase(builder, status, 1, false);
            }
            {
                llvm::IRBuilder<> builder(phase1);
                finish_phase(builder, builder.CreateCall(ready_poll, {host_ctx}), 2, false);
            }
            {
                llvm::IRBuilder<> builder(phase2);
                llvm::Value* value = lower(*script.argument, builder, i64,
                                            none, none, no_functions);
                finish_phase(builder, builder.CreateCall(invoke, {
                    host_ctx, generated_methods.at(script.method), value
                }), 3, false);
            }
            {
                llvm::IRBuilder<> builder(phase3);
                finish_phase(builder, builder.CreateCall(result_poll, {host_ctx, output}),
                             4, false);
            }
            {
                llvm::IRBuilder<> builder(phase4);
                finish_phase(builder, builder.CreateCall(request_stop, {host_ctx}), 5, false);
            }
            {
                llvm::IRBuilder<> builder(phase5);
                finish_phase(builder, builder.CreateCall(done_poll, {host_ctx}), 6, true);
            }
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
                            && name != "ores_actor_i64_fields_codegen_create"
                            && name != "ores_actor_i64_fields_codegen_get"
                            && name != "ores_actor_i64_fields_codegen_set"
                            && name != "ores_actor_i64_fields_codegen_drop"
                            && name != "ores_actor_host_spawn"
                            && name != "ores_actor_host_poll_ready"
                            && name != "ores_actor_host_call_i64"
                            && name != "ores_actor_host_poll_i64"
                            && name != "ores_actor_host_stop"
                            && name != "ores_actor_host_poll_done") {
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
