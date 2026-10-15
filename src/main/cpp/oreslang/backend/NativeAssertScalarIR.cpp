#include "oreslang/backend/NativeAssertScalarIR.hpp"
#include "oreslang/runtime/NativeAssertScalarAbi.hpp"

#include <stdexcept>
#include <string>

#include "llvm/Config/llvm-config.h"
#if LLVM_VERSION_MAJOR >= 17
#include "llvm/TargetParser/Triple.h"
#include "llvm/TargetParser/Host.h"
#else
#include "llvm/ADT/Triple.h"
#include "llvm/Support/Host.h"
#endif
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

namespace oreslang {
namespace {

llvm::Constant* status(llvm::StructType* ty, std::uint32_t code,
                       EffectKind effect, std::uint8_t present,
                       std::uint8_t value) {
    auto& c = ty->getContext();
    auto* i32 = llvm::Type::getInt32Ty(c);
    auto* i8 = llvm::Type::getInt8Ty(c);
    return llvm::ConstantStruct::get(ty, {
        llvm::ConstantInt::get(i32, code),
        llvm::ConstantInt::get(i8, static_cast<unsigned>(effect)),
        llvm::ConstantInt::get(i8, present),
        llvm::ConstantInt::get(i8, value),
        llvm::ConstantInt::get(i8, 0)
    });
}

void generate_equal(llvm::Module& module, llvm::StructType* scalar,
                    llvm::StructType* result) {
    auto& c = module.getContext();
    auto* i64 = llvm::Type::getInt64Ty(c);
    auto* i8 = llvm::Type::getInt8Ty(c);
    auto* ptr = llvm::PointerType::getUnqual(c);
    auto* signature = llvm::FunctionType::get(
        llvm::Type::getVoidTy(c), {ptr, ptr, i8, ptr}, false);
    auto* fn = llvm::Function::Create(signature,
        llvm::Function::ExternalLinkage, "oreslang_assert_equal_scalar_v0", module);
    auto iter = fn->arg_begin();
    llvm::Value* left = &*iter++;
    llvm::Value* right = &*iter++;
    llvm::Value* throws_flag = &*iter++;
    llvm::Value* output = &*iter++;
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(c, "entry", fn));
    auto* a = b.CreateLoad(scalar, left, "lhs");
    auto* z = b.CreateLoad(scalar, right, "rhs");
    a->setAlignment(llvm::Align(8));
    z->setAlignment(llvm::Align(8));
    auto* bits_a = b.CreateExtractValue(a, {0}, "lhs.bits");
    auto* bits_b = b.CreateExtractValue(z, {0}, "rhs.bits");
    auto* tag_a = b.CreateExtractValue(a, {1}, "lhs.tag");
    auto* tag_b = b.CreateExtractValue(z, {1}, "rhs.tag");
    auto* one = llvm::ConstantInt::get(i8, 1);
    auto* two = llvm::ConstantInt::get(i8, 2);
    auto* zero = llvm::ConstantInt::get(i8, 0);
    auto* a_bool = b.CreateICmpEQ(tag_a, two);
    auto* b_bool = b.CreateICmpEQ(tag_b, two);
    auto* good_a = b.CreateOr(b.CreateICmpEQ(tag_a, one), a_bool);
    auto* good_b = b.CreateOr(b.CreateICmpEQ(tag_b, one), b_bool);
    good_a = b.CreateAnd(good_a, b.CreateOr(
        b.CreateNot(a_bool), b.CreateICmpULE(bits_a, llvm::ConstantInt::get(i64, 1))));
    good_b = b.CreateAnd(good_b, b.CreateOr(
        b.CreateNot(b_bool), b.CreateICmpULE(bits_b, llvm::ConstantInt::get(i64, 1))));
    // Reserved bytes are part of the typed value contract, not padding.
    for (unsigned index = 0; index < 7; ++index) {
        good_a = b.CreateAnd(good_a, b.CreateICmpEQ(
            b.CreateExtractValue(a, {2, index}), zero));
        good_b = b.CreateAnd(good_b, b.CreateICmpEQ(
            b.CreateExtractValue(z, {2, index}), zero));
    }
    auto* good_flag = b.CreateICmpULE(throws_flag, one);
    auto* valid = b.CreateAnd(b.CreateAnd(good_a, good_b), good_flag);
    auto* equal = b.CreateAnd(b.CreateICmpEQ(bits_a, bits_b),
                              b.CreateICmpEQ(tag_a, tag_b));
    auto* is_throw = b.CreateAnd(b.CreateICmpEQ(throws_flag, one), b.CreateNot(equal));
    auto* returned = status(result, 0, EffectKind::Return, 1, 0);
    auto* returned_true = status(result, 0, EffectKind::Return, 1, 1);
    auto* thrown = status(result, 1, EffectKind::Throw, 0, 0);
    auto* fatal = status(result, 0xFFFFFFFDu, EffectKind::Fatal, 0, 0);
    auto* predicate = b.CreateSelect(equal, returned_true, returned);
    auto* chosen = b.CreateSelect(valid, b.CreateSelect(is_throw, thrown, predicate), fatal);
    auto* out_store = b.CreateStore(chosen, output);
    out_store->setAlignment(llvm::Align(8));
    b.CreateRetVoid();
}

void generate_trap(llvm::Module& module, llvm::StructType* result) {
    auto& c = module.getContext();
    auto* i8 = llvm::Type::getInt8Ty(c);
    auto* i32 = llvm::Type::getInt32Ty(c);
    auto* ptr = llvm::PointerType::getUnqual(c);
    auto* signature = llvm::FunctionType::get(llvm::Type::getVoidTy(c),
                                              {ptr, ptr}, false);
    auto* fn = llvm::Function::Create(signature,
        llvm::Function::ExternalLinkage, "oreslang_assert_trap_bool_v0", module);
    auto iter = fn->arg_begin();
    llvm::Value* input = &*iter++;
    llvm::Value* output = &*iter++;
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(c, "entry", fn));
    auto* loaded = b.CreateLoad(result, input, "pending");
    loaded->setAlignment(llvm::Align(8));
    auto* code = b.CreateExtractValue(loaded, {0});
    auto* effect = b.CreateExtractValue(loaded, {1});
    auto* present = b.CreateExtractValue(loaded, {2});
    auto* value = b.CreateExtractValue(loaded, {3});
    auto* reserved = b.CreateExtractValue(loaded, {4});
    auto* e0 = llvm::ConstantInt::get(i8, 0);
    auto* e1 = llvm::ConstantInt::get(i8, 1);
    auto* is_return = b.CreateICmpEQ(effect, e0);
    auto* valid = b.CreateICmpULE(effect, llvm::ConstantInt::get(i8, 6));
    valid = b.CreateAnd(valid, b.CreateICmpEQ(reserved, e0));
    valid = b.CreateAnd(valid, b.CreateICmpULE(present, e1));
    valid = b.CreateAnd(valid, b.CreateICmpULE(value, e1));
    valid = b.CreateAnd(valid, b.CreateOr(
        b.CreateICmpEQ(present, e1), b.CreateICmpEQ(value, e0)));
    valid = b.CreateAnd(valid, b.CreateOr(
        b.CreateNot(is_return), b.CreateICmpEQ(code, llvm::ConstantInt::get(i32, 0))));
    valid = b.CreateAnd(valid, b.CreateOr(is_return, b.CreateAnd(
        b.CreateICmpEQ(present, e0), b.CreateICmpEQ(value, e0))));
    const auto* none = status(result, 0, EffectKind::Return, 0, 0);
    const auto* fatal = status(result, 0xFFFFFFFDu, EffectKind::Fatal, 0, 0);
    auto* trapped = b.CreateSelect(b.CreateICmpEQ(effect, e1),
                                    const_cast<llvm::Constant*>(none), loaded);
    auto* outputValue = b.CreateSelect(valid, trapped,
                                      const_cast<llvm::Constant*>(fatal));
    auto* store = b.CreateStore(outputValue, output);
    store->setAlignment(llvm::Align(8));
    b.CreateRetVoid();
}
} // namespace

std::string emit_native_assert_scalar_llvm_ir_v0() {
    llvm::LLVMContext context;
    llvm::Module module("oreslang.assert.scalar.abi.v0", context);
    // The C++ ABI is explicitly host-endian and 8-byte aligned. Leaving
    // LLVM's target triple unspecified permits a consumer to silently lower
    // pointer-bearing exports for an incompatible native target. Until a
    // target-specific layout proof exists, fail closed outside tested
    // little-endian 64-bit native architectures.
    const llvm::Triple target(llvm::sys::getDefaultTargetTriple());
    if ((target.getArch() != llvm::Triple::x86_64 &&
         target.getArch() != llvm::Triple::aarch64) ||
        !target.isLittleEndian() || sizeof(void*) != 8)
        throw std::runtime_error(
            "native Assert scalar ABI v0 requires little-endian x86-64 or AArch64 host");
#if LLVM_VERSION_MAJOR >= 21
    module.setTargetTriple(target);
#else
    module.setTargetTriple(target.str());
#endif
    auto* i64 = llvm::Type::getInt64Ty(context);
    auto* i32 = llvm::Type::getInt32Ty(context);
    auto* i8 = llvm::Type::getInt8Ty(context);
    auto* reserve = llvm::ArrayType::get(i8, 7);
    auto* scalar = llvm::StructType::create(context, {i64, i8, reserve},
                                            "oreslang.assert.scalar.v0");
    auto* result = llvm::StructType::create(context,
                                             {i32, i8, i8, i8, i8},
                                             "oreslang.assert.bool_result.v0");
    generate_equal(module, scalar, result);
    generate_trap(module, result);
    std::string error;
    llvm::raw_string_ostream diagnostic(error);
    if (llvm::verifyModule(module, &diagnostic))
        throw std::runtime_error("generated Assert ABI IR failed verification: " + diagnostic.str());
    std::string ir;
    llvm::raw_string_ostream stream(ir);
    module.print(stream, nullptr);
    stream.flush();
    return ir;
}
} // namespace oreslang
