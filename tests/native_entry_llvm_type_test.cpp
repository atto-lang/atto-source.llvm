#include "oreslang/backend/EntryLowering.hpp"

#include <stdexcept>
#include <string>

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"

#define REQUIRE(condition) do { \
    if (!(condition)) throw std::runtime_error("requirement failed: " #condition); \
} while (false)

int main() {
    llvm::LLVMContext context;
    auto* entry = oreslang::backend::native_entry_i64_type(context);
    auto* same = oreslang::backend::native_entry_i64_type(context);

    REQUIRE(entry == same);
    REQUIRE(entry->hasName());
    REQUIRE(entry->getName() == "ores.entry.i64.i64");
    REQUIRE(!entry->isLiteral());
    REQUIRE(!entry->isOpaque());
    REQUIRE(!entry->isPacked());
    REQUIRE(entry->getNumElements() == 2);
    REQUIRE(entry->getElementType(
                oreslang::backend::native_entry_key_index())->isIntegerTy(64));
    REQUIRE(entry->getElementType(
                oreslang::backend::native_entry_value_index())->isIntegerTy(64));

    auto* i64 = llvm::Type::getInt64Ty(context);
    auto* structural_pair = llvm::StructType::get(context, {i64, i64}, false);
    REQUIRE(structural_pair->isLiteral());
    REQUIRE(structural_pair != entry);

    // The nominal tag must not change the physical two-slot ABI. Validate the
    // supported native layouts directly in LLVM, not only through the C++ host
    // mirror struct.
    for (const char* layout_text : {
            "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128",
            "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"}) {
        llvm::DataLayout layout(layout_text);
        const auto* struct_layout = layout.getStructLayout(entry);
        REQUIRE(layout.getTypeAllocSize(entry).getFixedValue() == 16);
        REQUIRE(layout.getABITypeAlign(entry).value() == 8);
        REQUIRE(struct_layout->getElementOffset(
                    oreslang::backend::native_entry_key_index()) == 0);
        REQUIRE(struct_layout->getElementOffset(
                    oreslang::backend::native_entry_value_index()) == 8);
    }

    llvm::LLVMContext forged_context;
    auto* forged = llvm::StructType::create(
        forged_context, "ores.entry.i64.i64");
    REQUIRE(forged->isOpaque());
    bool collision_rejected = false;
    try {
        (void)oreslang::backend::native_entry_i64_type(forged_context);
    } catch (const std::runtime_error&) {
        collision_rejected = true;
    }
    REQUIRE(collision_rejected);

    llvm::Module module("native-entry-lowering-test", context);
    auto* function_type = llvm::FunctionType::get(
        i64, {i64, i64}, false);
    auto* function = llvm::Function::Create(
        function_type, llvm::Function::ExternalLinkage,
        "entry_projection_sum", module);
    auto args = function->arg_begin();
    llvm::Value* key_arg = &*args++;
    llvm::Value* value_arg = &*args;
    auto* block = llvm::BasicBlock::Create(context, "entry", function);
    llvm::IRBuilder<> builder(block);

    llvm::Value* lowered = oreslang::backend::lower_native_entry_i64(
        builder, key_arg, value_arg);
    REQUIRE(lowered->getType() == entry);
    llvm::Value* key = oreslang::backend::lower_native_entry_i64_key(
        builder, lowered);
    llvm::Value* value = oreslang::backend::lower_native_entry_i64_value(
        builder, lowered);
    builder.CreateRet(builder.CreateAdd(key, value, "entry.sum"));
    REQUIRE(!llvm::verifyModule(module));

    auto* structural_value = llvm::UndefValue::get(structural_pair);
    bool structural_projection_rejected = false;
    try {
        (void)oreslang::backend::lower_native_entry_i64_key(
            builder, structural_value);
    } catch (const std::invalid_argument&) {
        structural_projection_rejected = true;
    }
    REQUIRE(structural_projection_rejected);

    bool wrong_component_rejected = false;
    try {
        auto* i32 = llvm::ConstantInt::get(
            llvm::Type::getInt32Ty(context), 1);
        (void)oreslang::backend::lower_native_entry_i64(
            builder, i32, value_arg);
    } catch (const std::invalid_argument&) {
        wrong_component_rejected = true;
    }
    REQUIRE(wrong_component_rejected);

    return 0;
}
