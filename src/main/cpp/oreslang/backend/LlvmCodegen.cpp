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
        // Every admitted return edge owns the same fully initialized
        // lexical slots. Emit reverse-order deterministic cleanup for each
        // reachable exit after computing the return value. Partial-init,
        // exception, and suspension exits are intentionally not admitted.
        const auto emitReturn = [&](llvm::Value* value) {
            for (auto iter = slots.rbegin(); iter != slots.rend(); ++iter) {
                if (iter->heap) {
                    builder.CreateCall(heapDrop, {region, iter->address});
                } else {
                    builder.CreateLifetimeEnd(iter->address, llvm::ConstantInt::get(i64, 8));
                }
            }
            if (region != nullptr) builder.CreateCall(heapClose, {region});
            builder.CreateRet(value);
        };
        for (const auto& guard : fn.early_returns) {
            llvm::Value* lhs = lower(*guard.lhs, builder, i64,
                                     parameters, locals, declarations);
            llvm::Value* rhs = lower(*guard.rhs, builder, i64,
                                     parameters, locals, declarations);
            llvm::Value* condition = builder.CreateICmpEQ(lhs, rhs, "return.guard.eq");
            llvm::BasicBlock* taken = llvm::BasicBlock::Create(
                    context, "guard.return", function);
            llvm::BasicBlock* next = llvm::BasicBlock::Create(
                    context, "guard.next", function);
            builder.CreateCondBr(condition, taken, next);
            builder.SetInsertPoint(taken);
            llvm::Value* selected = lower(*guard.result, builder, i64,
                                           parameters, locals, declarations);
            emitReturn(selected);
            builder.SetInsertPoint(next);
        }
        llvm::Value* result = lower(*fn.result, builder, i64,
                                     parameters, locals, declarations);
        emitReturn(result);
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
                            && name != "oreslang_owned_i64_region_close") {
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
