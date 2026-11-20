#include "oreslang/backend/LlvmCompat.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <array>

int main() {
    llvm::LLVMContext context;
    llvm::Module module("attolang-llvm-intrinsic-compat", context);
    auto* i64 = llvm::Type::getInt64Ty(context);
    constexpr std::array<llvm::Intrinsic::ID, 3> operations{
        llvm::Intrinsic::sadd_with_overflow,
        llvm::Intrinsic::ssub_with_overflow,
        llvm::Intrinsic::smul_with_overflow
    };
    for (auto operation : operations) {
        auto* intrinsic = oreslang::native_intrinsic(module, operation, {i64});
        if (!intrinsic || intrinsic->arg_size() != 2
                || !intrinsic->getReturnType()->isStructTy()) return 1;
        if (oreslang::native_intrinsic(module, operation, {i64}) != intrinsic) return 2;
    }
    auto* trap = oreslang::native_trap(module);
    if (!trap || trap->arg_size() != 0 || !trap->getReturnType()->isVoidTy()) return 3;
    if (oreslang::native_trap(module) != trap) return 4;
    return llvm::verifyModule(module, &llvm::errs()) ? 5 : 0;
}
