#include "oreslang/backend/LlvmCodegen.hpp"

#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

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
        // Lexical scalar locals are stack-owned. Every slot becomes live
        // only after its initializer has been evaluated; all slots leave
        // lifetime before return in reverse declaration order. The generated
        // IR has no tracing-GC statepoints and calls no heap collector.
        std::unordered_map<std::string, llvm::Value*> locals;
        std::vector<llvm::Value*> lifetimeSlots;
        for (const auto& local : fn.locals) {
            llvm::Value* init = lower(*local.initializer, builder, i64,
                                      parameters, locals, declarations);
            llvm::Value* slot = builder.CreateAlloca(i64, nullptr,
                                                     local.name + ".owned");
            builder.CreateLifetimeStart(slot, llvm::ConstantInt::get(i64, 8));
            builder.CreateStore(init, slot);
            locals.emplace(local.name, slot);
            lifetimeSlots.push_back(slot);
        }
        llvm::Value* result = lower(*fn.result, builder, i64, parameters,
                                    locals, declarations);
        for (auto iter = lifetimeSlots.rbegin(); iter != lifetimeSlots.rend(); ++iter) {
            builder.CreateLifetimeEnd(*iter, llvm::ConstantInt::get(i64, 8));
        }
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
