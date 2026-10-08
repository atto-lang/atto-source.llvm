#include "oreslang/backend/LlvmCodegen.hpp"

#include <stdexcept>
#include <string>

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

std::string emit_verified_llvm_ir(const ConstantFunction& source) {
    llvm::LLVMContext context;
    llvm::Module module("oreslang-source", context);
    const std::string symbol = source.module.empty()
        ? source.name : source.module + "." + source.name;
    // The initial slice returns one signed i64 and accepts zero arguments.
    // It intentionally does not implement Oreslang calling conventions yet.
    llvm::FunctionType* type = llvm::FunctionType::get(
        llvm::Type::getInt64Ty(context), /*isVarArg=*/false);
    llvm::Function* function = llvm::Function::Create(
        type, llvm::Function::ExternalLinkage, symbol, module);
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(context, "entry", function);
    llvm::IRBuilder<> builder(entry);
    builder.CreateRet(llvm::ConstantInt::getSigned(
        llvm::Type::getInt64Ty(context), source.value));

    std::string diagnostic;
    llvm::raw_string_ostream errors(diagnostic);
    if (llvm::verifyModule(module, &errors)) {
        errors.flush();
        throw std::runtime_error("invalid generated LLVM module: " + diagnostic);
    }

    std::string result;
    llvm::raw_string_ostream output(result);
    module.print(output, nullptr);
    output.flush();
    return result;
}
} // namespace oreslang
