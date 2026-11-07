#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "llvm/IR/Argument.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Casting.h"

namespace oreslang::native_ir {

// Evidence is collected from instructions IN an LLVM module, not from a
// caller-authored JSON/CFG manifest. It is still not a proof that every
// source/typed-MIR loan use was lowered: no source LoanId mapping exists.
// Store records borrow their instruction/value pointers from the Module;
// they MUST NOT survive that Module.
struct ActualStore {
    const llvm::StoreInst* instruction = nullptr;
    const llvm::Value* pointer = nullptr;
    const llvm::Value* value = nullptr;
    std::uint64_t instruction_ordinal = 0;
};

struct NativeIrUseStoreCensus {
    std::size_t instructions = 0;
    std::size_t loads = 0;
    std::size_t calls = 0;
    std::size_t phi_nodes = 0;
    std::size_t phi_incoming = 0;
    std::size_t ssa_operand_uses = 0;
    std::size_t reciprocal_ssa_uses = 0;
    std::vector<ActualStore> stores;
    // Borrowed module owner. This census must not outlive its Module, and
    // callers must not mutate the Module concurrently with readback.
    const llvm::Module* source_module = nullptr;

    // These fields CANNOT be set to true by an instruction census. Linking
    // source loans / actual field assignments needs a separate compiler-owned
    // typed-MIR mapping that survives all lowering and optimization passes.
    bool ownership_complete = false;
    bool guest_verified = false;

    // Never dereference an instruction supplied by the caller: a pass may
    // have erased it, leaving the cached StoreInst* dangling. Instead walk
    // only LIVE instructions still owned by the original Module. Reject
    // changed store operands, reordered instructions, inserted/deleted
    // instructions, and foreign module stores. This is advisory readback,
    // NOT a stable identity across allocator reuse or a source loan proof.
    bool contains_store(const llvm::StoreInst* identity,
            const llvm::Value* destination,
            const llvm::Value* stored) const noexcept {
        if (!source_module || !identity || !destination || !stored)
            return false;
        std::uint64_t ordinal = 0;
        std::size_t store_index = 0;
        bool matched = false;
        for (const auto& function : *source_module) {
            if (function.isDeclaration()) continue;
            for (const auto& block : function) {
                for (const auto& instruction : block) {
                    ++ordinal;
                    if (const auto* live =
                            llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
                        if (store_index >= stores.size()) return false;
                        const auto& old = stores[store_index++];
                        if (old.instruction != live ||
                                old.instruction_ordinal != ordinal ||
                                old.pointer != live->getPointerOperand() ||
                                old.value != live->getValueOperand())
                            return false;
                        if (live == identity &&
                                old.pointer == destination &&
                                old.value == stored)
                            matched = true;
                    }
                }
            }
        }
        return ordinal == instructions &&
            store_index == stores.size() && matched;
    }
};

inline NativeIrUseStoreCensus collect_native_ir_ssa_census(
        const llvm::Module& module) {
    constexpr std::size_t kMaxInstructions = 2000000;
    constexpr std::size_t kMaxStores = 250000;
    NativeIrUseStoreCensus result;
    result.source_module = &module;
    for (const auto& function : module) {
        if (function.isDeclaration()) continue;
        for (const auto& block : function) {
            for (const auto& instruction : block) {
                if (++result.instructions > kMaxInstructions)
                    throw std::runtime_error(
                        "ownership violation: emitted native LLVM instruction census exceeds bound");
                // The collector never accepts supplied instruction IDs:
                // ordinal and actual StoreInst address come from this walk.
                if (const auto* store =
                        llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
                    if (result.stores.size() >= kMaxStores)
                        throw std::runtime_error(
                            "ownership violation: emitted native store census exceeds bound");
                    result.stores.push_back({store, store->getPointerOperand(),
                        store->getValueOperand(),
                        static_cast<std::uint64_t>(result.instructions)});
                }
                if (llvm::isa<llvm::LoadInst>(instruction)) ++result.loads;
                if (llvm::isa<llvm::CallBase>(instruction)) ++result.calls;
                if (const auto* phi =
                        llvm::dyn_cast<llvm::PHINode>(&instruction)) {
                    ++result.phi_nodes;
                    result.phi_incoming += phi->getNumIncomingValues();
                }
                // Count every local SSA def->use edge directly from IR
                // operands, including returns, calls and phi incoming values.
                for (const llvm::Use& operand : instruction.operands()) {
                    const auto* value = operand.get();
                    if (!value)
                        throw std::runtime_error(
                            "ownership violation: null native LLVM SSA operand");
                    if (llvm::isa<llvm::Instruction>(value) ||
                            llvm::isa<llvm::Argument>(value))
                        ++result.ssa_operand_uses;
                }
            }
        }
        // Independently derive the reciprocal use list of each SSA value,
        // and require equality with operand traversal. Do not infer lexical
        // borrow coverage from the resulting equality.
        const auto count_local_users = [&](const llvm::Value& definition) {
            for (const llvm::Use& use : definition.uses()) {
                const auto* owner =
                    llvm::dyn_cast<llvm::Instruction>(use.getUser());
                if (!owner || owner->getFunction() != &function)
                    throw std::runtime_error(
                        "ownership violation: emitted native SSA use escapes owning function");
                ++result.reciprocal_ssa_uses;
            }
        };
        for (const auto& arg : function.args())
            count_local_users(arg);
        for (const auto& block : function)
            for (const auto& instruction : block)
                count_local_users(instruction);
    }
    if (result.ssa_operand_uses != result.reciprocal_ssa_uses)
        throw std::runtime_error(
            "ownership violation: emitted native LLVM SSA use-def census mismatch");
    return result;
}

} // namespace oreslang::native_ir
