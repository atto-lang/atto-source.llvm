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

std::uint64_t actor_mail_schema_v1(const ActorDecl& actor) {
    // An ordered, host-authenticated message contract fingerprint.
    // Not a serializer by itself; the envelope additionally carries a
    // version and explicit finite variant index.
    std::uint64_t hash = UINT64_C(14695981039346656037);
    auto octet = [&](unsigned char ch) {
        hash ^= ch;
        hash *= UINT64_C(1099511628211);
    };
    auto name = [&](const std::string& value) {
        for (unsigned char ch : value) octet(ch);
        octet(0);
    };
    name("ores.actor.mail.v1");
    name(actor.name);
    for (const auto& method : actor.methods) {
        if (method.name != "receive") continue;
        for (const auto& field : method.accepted_message_variants) {
            name(field);
            name("i64");
        }
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
        case Expression::Kind::Match:
        case Expression::Kind::Switch: {
            // Arm-local return is an SSA result, not a function return.
            llvm::Value* subject = lower(*expr.left, builder, i64,
                                          parameters, locals, functions, actor);
            auto* function = builder.GetInsertBlock()->getParent();
            auto* merged = llvm::BasicBlock::Create(
                    builder.getContext(), "dispatch.value.merge", function);
            std::vector<std::pair<llvm::Value*, llvm::BasicBlock*>> values;
            bool covered = false;
            for (const auto& arm : expr.arms) {
                llvm::BasicBlock* next = nullptr;
                if (arm.fallback) {
                    covered = true;
                } else {
                    llvm::Value* selected = builder.getFalse();
                    for (const auto number : arm.cases) {
                        auto* equal = builder.CreateICmpEQ(
                                subject, llvm::ConstantInt::getSigned(i64, number));
                        selected = builder.CreateOr(selected, equal);
                    }
                    auto* body = llvm::BasicBlock::Create(
                            builder.getContext(), "dispatch.value.arm", function);
                    next = llvm::BasicBlock::Create(
                            builder.getContext(), "dispatch.value.next", function);
                    builder.CreateCondBr(selected, body, next);
                    builder.SetInsertPoint(body);
                }
                if (!arm.value) throw std::runtime_error(
                        "value match/switch arm has no result");
                llvm::Value* value = lower(*arm.value, builder, i64,
                                            parameters, locals, functions, actor);
                auto* predecessor = builder.GetInsertBlock();
                builder.CreateBr(merged);
                values.emplace_back(value, predecessor);
                if (arm.fallback) break;
                builder.SetInsertPoint(next);
            }
            if (!covered) throw std::runtime_error(
                    "native value match/switch must be exhaustive");
            builder.SetInsertPoint(merged);
            auto* phi = builder.CreatePHI(
                    i64, static_cast<unsigned>(values.size()), "dispatch.collected.value");
            for (const auto& [value, predecessor] : values)
                phi->addIncoming(value, predecessor);
            return phi;
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
        auto release_activation = [&](llvm::IRBuilder<>& emit) {
            for (auto iter = slots.rbegin(); iter != slots.rend(); ++iter) {
                if (iter->heap)
                    emit.CreateCall(heapDrop, {region, iter->address});
                else
                    emit.CreateLifetimeEnd(iter->address,
                                           llvm::ConstantInt::get(i64, 8));
            }
            if (region != nullptr) emit.CreateCall(heapClose, {region});
        };

        // Procedural do-dispatch has no collected result/PHI. Returning arms
        // exit the containing function and reclaim all owned lexical slots;
        // unmatched or nonreturning arms continue after the entire dispatch.
        for (const auto& action : fn.procedural) {
            const auto& dispatch = *action.dispatch;
            llvm::Value* subject = lower(*dispatch.left, builder, i64,
                                          parameters, locals, declarations);
            auto* continuation = llvm::BasicBlock::Create(
                    context, "dispatch.do.continue", function);
            bool covered = false;
            for (const auto& arm : dispatch.arms) {
                llvm::BasicBlock* next = nullptr;
                if (arm.fallback) {
                    covered = true;
                } else {
                    llvm::Value* selected = builder.getFalse();
                    for (const auto number : arm.cases) {
                        auto* equal = builder.CreateICmpEQ(
                                subject, llvm::ConstantInt::getSigned(i64, number));
                        selected = builder.CreateOr(selected, equal);
                    }
                    auto* body = llvm::BasicBlock::Create(
                            context, "dispatch.do.arm", function);
                    next = llvm::BasicBlock::Create(
                            context, "dispatch.do.next", function);
                    builder.CreateCondBr(selected, body, next);
                    builder.SetInsertPoint(body);
                }
                if (arm.value) {
                    llvm::Value* returned = lower(*arm.value, builder, i64,
                                                  parameters, locals, declarations);
                    release_activation(builder);
                    builder.CreateRet(returned);
                } else {
                    builder.CreateBr(continuation);
                }
                if (arm.fallback) break;
                builder.SetInsertPoint(next);
            }
            if (!covered) builder.CreateBr(continuation);
            builder.SetInsertPoint(continuation);
        }

        if (fn.result) {
            llvm::Value* result = lower(*fn.result, builder, i64, parameters,
                                        locals, declarations);
            release_activation(builder);
            builder.CreateRet(result);
        } else {
            // A terminal return-do already emitted returns for every case.
            builder.CreateUnreachable();
        }
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
        llvm::Function* mail_hash = entrypoint("message_schema_hash", i64, {});
        {
            llvm::IRBuilder<> builder(
                llvm::BasicBlock::Create(context, "entry", mail_hash));
            builder.CreateRet(llvm::ConstantInt::get(
                i64, actor_mail_schema_v1(actor)));
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
            const bool union_receiver = method.name == "receive"
                    && method.accepted_message_variants.size() > 1;
            std::vector<llvm::Type*> signature = {ptr, i64, i64, ptr};
            if (union_receiver) {
                // Transport tag is a native host capability: guest source
                // cannot invoke this generated receive pointer directly.
                signature.push_back(i32);
                signature.push_back(i64);
            } else {
                signature.insert(signature.end(), method.parameters.size(), i64);
            }
            llvm::Function* function = entrypoint(method.name, i64, signature);
            generated_methods.emplace(method.name, function);
            llvm::IRBuilder<> builder(llvm::BasicBlock::Create(context, "entry", function));
            const auto args = params(function);
            ActorContext owned{args[0], args[1], args[2], args[3], fieldGet, &field_indices};
            if (union_receiver) {
                // An invalid tag has no access to actor-owned state. Distinct
                // variants have distinct block labels and a finite exhaustive
                // case map established by parser/typechecker.
                auto* rejected = llvm::BasicBlock::Create(context, "invalid_variant", function);
                llvm::IRBuilder<> invalid(rejected);
                invalid.CreateRet(llvm::ConstantInt::getSigned(i64, -1));
                auto* decision = builder.CreateSwitch(args[4], rejected,
                        static_cast<unsigned>(method.accepted_message_variants.size()));
                for (std::size_t i = 0; i < method.accepted_message_variants.size(); ++i) {
                    const auto& field = method.accepted_message_variants[i];
                    const auto& arm = *std::find_if(method.variant_arms.begin(),
                            method.variant_arms.end(),
                            [&](const ActorVariantArm& candidate) {
                                return candidate.field == field;
                            });
                    auto* selected = llvm::BasicBlock::Create(
                            context, "message_" + field, function);
                    decision->addCase(builder.getInt32(static_cast<std::uint32_t>(i)), selected);
                    llvm::IRBuilder<> emit(selected);
                    std::unordered_map<std::string, llvm::Value*> input{{field, args[5]}};
                    for (const auto& assignment : arm.assignments) {
                        llvm::Value* v = lower(*assignment.value, emit, i64,
                                               input, none, no_functions, &owned);
                        emit.CreateCall(fieldSet, {
                            owned.heap, owned.domain, owned.epoch, owned.fields,
                            emit.getInt32(field_indices.at(assignment.field)), v
                        });
                    }
                    // Host-only result can be forwarded through a separate
                    // output port; public send(...) never returns it.
                    emit.CreateRet(emit.CreateCall(fieldGet, {
                        owned.heap, owned.domain, owned.epoch, owned.fields,
                        emit.getInt32(0)
                    }));
                }
                continue;
            }
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

        // Compiler-only actor message send. Guest code cannot invoke
        // generated actor business methods. The ONLY user send boundary is
        // ActorRef.send(infer struct{typedField: int}); the receive function
        // pointer is a trusted host callback admitted on that actor's lane.
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
            auto send_message = host("ores_actor_host_send_i64", i32, {ptr, ptr, i64});
            auto done_poll = host("ores_actor_host_poll_done", i32, {ptr});
            auto cancel = host("ores_actor_host_cancel", i32, {ptr});

            llvm::Function* step = entrypoint("program_step", i32, {ptr, ptr, ptr});
            auto args = params(step);
            llvm::Value* ctx = args[0];
            llvm::Value* state = args[1];
            llvm::Value* output = args[2];
            llvm::IRBuilder<> builder(llvm::BasicBlock::Create(context, "entry", step));
            auto* invalid = llvm::BasicBlock::Create(context, "invalid", step);
            auto* spawn = llvm::BasicBlock::Create(context, "spawn", step);
            auto* ready = llvm::BasicBlock::Create(context, "ready", step);
            auto* send = llvm::BasicBlock::Create(context, "send_typed", step);
            auto* done = llvm::BasicBlock::Create(context, "await_done", step);
            auto* decision = builder.CreateSwitch(builder.CreateLoad(i32, state), invalid, 4);
            decision->addCase(builder.getInt32(0), spawn);
            decision->addCase(builder.getInt32(1), ready);
            decision->addCase(builder.getInt32(2), send);
            decision->addCase(builder.getInt32(3), done);
            llvm::IRBuilder<> bad(invalid);
            bad.CreateCall(cancel, {ctx});
            bad.CreateRet(bad.getInt32(-1));

            auto phase = [&](llvm::IRBuilder<>& emit, llvm::Value* status,
                             int next, bool terminal) {
                auto* pending = llvm::BasicBlock::Create(context, "pending", step);
                auto* resolved = llvm::BasicBlock::Create(context, "resolved", step);
                emit.CreateCondBr(emit.CreateICmpEQ(status, emit.getInt32(0)),
                                  pending, resolved);
                llvm::IRBuilder<> yield(pending);
                yield.CreateRet(yield.getInt32(0));
                llvm::IRBuilder<> check(resolved);
                auto* failure = llvm::BasicBlock::Create(context, "error", step);
                auto* success = llvm::BasicBlock::Create(context, "success", step);
                check.CreateCondBr(check.CreateICmpSLT(status, check.getInt32(0)),
                                   failure, success);
                llvm::IRBuilder<> on_failure(failure);
                on_failure.CreateCall(cancel, {ctx});
                on_failure.CreateRet(on_failure.getInt32(-1));
                llvm::IRBuilder<> on_success(success);
                on_success.CreateStore(on_success.getInt32(next), state);
                if (terminal) on_success.CreateStore(llvm::ConstantInt::get(i64, 0), output);
                on_success.CreateRet(on_success.getInt32(terminal ? 1 : 0));
            };
            {
                llvm::IRBuilder<> emit(spawn);
                auto* status = emit.CreateCall(spawn_actor, {
                    ctx, init, drop, layout_hash, version,
                    emit.getInt32(static_cast<std::uint32_t>(actor.fields.size())),
                    llvm::ConstantInt::get(i64, actor_layout_v1(actor))
                });
                phase(emit, status, 1, false);
            }
            {
                llvm::IRBuilder<> emit(ready);
                phase(emit, emit.CreateCall(ready_poll, {ctx}), 2, false);
            }
            {
                llvm::IRBuilder<> emit(send);
                llvm::Value* value = lower(*script.argument, emit, i64,
                                            none, none, no_functions);
                phase(emit, emit.CreateCall(send_message, {
                    ctx, generated_methods.at("receive"), value
                }), 3, false);
            }
            {
                llvm::IRBuilder<> emit(done);
                phase(emit, emit.CreateCall(done_poll, {ctx}), 4, true);
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
                            && name != "ores_actor_host_send_i64"
                            && name != "ores_actor_host_poll_done"
                            && name != "ores_actor_host_cancel") {
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
