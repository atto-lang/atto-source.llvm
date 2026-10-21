#include "oreslang/backend/EntryLowering.hpp"

#include <stdexcept>
#include <string>

#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Type.h"

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

    return 0;
}
