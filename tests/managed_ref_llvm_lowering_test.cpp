#include "oreslang/backend/ManagedRefLowering.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static void checked_llvm_primitive() {
    llvm::LLVMContext context;
    llvm::Module module("oreslang.checked.actor.compact32", context);
    module.setDataLayout("e-p:64:64-i64:64-n8:16:32:64-S128");
    llvm::IRBuilder<> builder(context);
    llvm::Type* i8 = llvm::Type::getInt8Ty(context);
    llvm::Type* i32 = llvm::Type::getInt32Ty(context);
    llvm::Type* i64 = llvm::Type::getInt64Ty(context);
    llvm::Type* i1 = llvm::Type::getInt1Ty(context);
    llvm::FunctionType* signature = llvm::FunctionType::get(i64,
        {llvm::PointerType::get(i8, 0), i64, i32, i1}, false);
    auto* fn = llvm::Function::Create(signature,
        llvm::GlobalValue::ExternalLinkage, "actor_load_i64", module);
    auto it = fn->arg_begin();
    llvm::Value* base = &*it++;
    llvm::Value* length = &*it++;
    llvm::Value* stored32 = &*it++;
    llvm::Value* live = &*it++;
    auto* start = llvm::BasicBlock::Create(context, "entry", fn);
    auto* invalid = llvm::BasicBlock::Create(context, "reject", fn);
    builder.SetInsertPoint(start);
    auto* checked = oreslang::backend::lower_checked_actor_compressed32_address(
        builder, base, length, stored32, live, 8, invalid);
    builder.CreateRet(builder.CreateLoad(i64, checked, "actor.value"));
    builder.SetInsertPoint(invalid);
    builder.CreateUnreachable();

    check(!llvm::verifyFunction(*fn, &llvm::errs()),
          "compressed32 LLVM function verification failed");
    check(!llvm::verifyModule(module, &llvm::errs()),
          "compressed32 LLVM module verification failed");
    std::string ir;
    llvm::raw_string_ostream out(ir);
    module.print(out, nullptr);
    out.flush();
    check(ir.find("i32 %") != std::string::npos,
          "LLVM ABI unexpectedly widened the stored compact reference");
    check(ir.find("actor.ref.byte.offset") != std::string::npos,
          "LLVM offset shift was not generated");
    check(ir.find("actor.ref.nonnull") != std::string::npos,
          "LLVM null rejection was not generated");
    check(ir.find("actor.ref.actor.provenance") != std::string::npos,
          "LLVM actor provenance gate was not generated");
    check(ir.find("actor.ref.access.valid") != std::string::npos,
          "LLVM byte-range check was not generated");
    check(ir.find("getelementptr inbounds") == std::string::npos,
          "LLVM incorrectly asserted unchecked inbounds pointer provenance");
    check(ir.find("ptrtoint") == std::string::npos,
          "LLVM lowering attempted unsafe pointer-int normalization");
}

static void deny_native32_lowering() {
    llvm::LLVMContext context;
    llvm::Module module("oreslang.compact.reject32", context);
    module.setDataLayout("e-p:32:32-i64:64-n8:16:32-S64");
    llvm::IRBuilder<> builder(context);
    auto* i8 = llvm::Type::getInt8Ty(context);
    auto* i32 = llvm::Type::getInt32Ty(context);
    auto* i64 = llvm::Type::getInt64Ty(context);
    auto* i1 = llvm::Type::getInt1Ty(context);
    auto* fn = llvm::Function::Create(llvm::FunctionType::get(
        i64, {llvm::PointerType::get(i8, 0), i64, i32, i1}, false),
        llvm::GlobalValue::ExternalLinkage, "invalid_target", module);
    auto it = fn->arg_begin();
    auto* base = &*it++;
    auto* bytes = &*it++;
    auto* packed = &*it++;
    auto* live = &*it++;
    auto* entry = llvm::BasicBlock::Create(context, "entry", fn);
    auto* reject = llvm::BasicBlock::Create(context, "reject", fn);
    builder.SetInsertPoint(entry);
    bool refused = false;
    try {
        (void) oreslang::backend::lower_checked_actor_compressed32_address(
            builder, base, bytes, packed, live, 8, reject);
    } catch (const std::invalid_argument&) {
        refused = true;
    }
    check(refused, "native32 target was admitted into native64-only lowering");
}

int main() {
    checked_llvm_primitive();
    deny_native32_lowering();
}
