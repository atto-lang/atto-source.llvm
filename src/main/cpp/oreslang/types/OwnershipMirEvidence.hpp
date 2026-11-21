#pragma once

#include "oreslang/types/OwnershipCfg.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::ownership {

// Stage-zero typed-MIR bridge. Records are supplied by the caller, NOT
// authenticated SSA or real LLVM instruction receipts. No guest lowering.
enum class MirUseClass {
    Load, CallArgument, ClosureCapture, PhiEdge, ReturnValue, SendValue,
    ExceptionUnwind, Cancellation, Suspension
};
enum class FieldStoreOpcode { NativeStore, PhiOnly, Unknown };

struct TypedMirLoanUse {
    std::uint64_t site_id = 0;
    std::size_t cfg_operation = 0;
    std::uint64_t loan_id = 0;
    std::string place;
    MirUseClass classification = MirUseClass::Load;
    std::uint64_t ssa_value_id = 0;
    bool operator==(const TypedMirLoanUse& rhs) const {
        return site_id == rhs.site_id &&
            cfg_operation == rhs.cfg_operation &&
            loan_id == rhs.loan_id && place == rhs.place &&
            classification == rhs.classification &&
            ssa_value_id == rhs.ssa_value_id;
    }
};
struct TypedMirFieldWrite {
    std::size_t cfg_operation = 0;
    std::uint64_t write_id = 0;
    std::string place;
    std::uint64_t ssa_value_id = 0;
};
struct LoweredFieldStore {
    std::uint64_t write_id = 0;
    std::uint64_t instruction_id = 0;
    std::string place;
    std::uint64_t ssa_value_id = 0;
    FieldStoreOpcode opcode = FieldStoreOpcode::Unknown;
};
struct MirReceiptCandidate {
    // Even perfectly matching manifests can omit real SSA uses or fabricate
    // store emission, so this result never grants compiler or guest authority.
    bool verified = false;
    bool compiler_backed = false;
    std::size_t classified_uses = 0;
    std::size_t matched_field_stores = 0;
};

inline bool bounded_mir_class(MirUseClass kind) {
    switch (kind) {
        case MirUseClass::Load:
        case MirUseClass::CallArgument:
        case MirUseClass::ClosureCapture:
        case MirUseClass::ReturnValue:
        case MirUseClass::SendValue:
            return true;
        case MirUseClass::PhiEdge:
            // One straight-line CFG block cannot prove a predecessor
            // incoming-value use. Requires a future multi-block SSA pass.
            return false;
        case MirUseClass::ExceptionUnwind:
        case MirUseClass::Cancellation:
        case MirUseClass::Suspension:
            // Not sound until cleanup, scope, and resumed ownership are proven.
            return false;
    }
    return false;
}

inline MirReceiptCandidate inspect_typed_mir_receipt_candidate_v1(
        const Graph& graph,
        const std::vector<TypedMirLoanUse>& ssa_use_inventory,
        const std::vector<TypedMirLoanUse>& lowered_loan_edges,
        const std::vector<TypedMirFieldWrite>& mir_writes,
        const std::vector<LoweredFieldStore>& emitted_store_claims) {
    if (graph.blocks.size() != 1 || !graph.blocks[0].successors.empty())
        throw std::runtime_error("ownership violation: typed-MIR receipt requires one block");
    const auto& ops = graph.blocks[0].operations;
    if (ops.size() > 4096 || ssa_use_inventory.size() > 4096 ||
            lowered_loan_edges.size() > 4096 || mir_writes.size() > 4096 ||
            emitted_store_claims.size() > 4096)
        throw std::runtime_error("ownership violation: typed-MIR receipt exceeds bound");

    // A caller can omit a syntactic read from BOTH supplied inventories.
    // This is the same conservative closed-world rule as OwnershipLastUse:
    // after any lexical borrow, ordinary reads, whole-object reads and
    // suspension are opaque until the compiler proves their SSA origins.
    // This intentionally rejects some safe independent reads.
    if (graph.entry.size() > 4096)
        throw std::runtime_error("ownership violation: typed-MIR entry exceeds bound");
    for (const auto& item : graph.entry)
        if (item.first.size() > 256)
            violation(item.first, "typed-MIR entry place exceeds bound");
    // Entry loans represent existing borrows even when there is no
    // BorrowShared/BorrowExclusive operation in this local block.
    bool seen_borrow = false;
    for (const auto& item : graph.entry)
        if (item.second.shared_loans || item.second.exclusive_loan)
            seen_borrow = true;
    for (const auto& op : ops) {
        if (op.place.size() > 256)
            violation(op.place, "typed-MIR operation place exceeds bound");
        if (op.kind == Operation::Kind::BorrowShared ||
                op.kind == Operation::Kind::BorrowExclusive)
            seen_borrow = true;
        if (seen_borrow && (op.kind == Operation::Kind::Read ||
                op.kind == Operation::Kind::ReadObject ||
                op.kind == Operation::Kind::Suspend))
            violation(op.place, "opaque effect after borrow requires authenticated SSA coverage");
    }
    for (const auto& use : ssa_use_inventory)
        if (use.place.size() > 256)
            violation(use.place, "typed-MIR SSA use place exceeds bound");
    for (const auto& edge : lowered_loan_edges)
        if (edge.place.size() > 256)
            violation(edge.place, "typed-MIR lowered edge place exceeds bound");
    for (const auto& write : mir_writes)
        if (write.place.size() > 256)
            violation(write.place, "typed-MIR field-write place exceeds bound");
    for (const auto& store : emitted_store_claims)
        if (store.place.size() > 256)
            violation(store.place, "typed-MIR store place exceeds bound");

    // Real CFG ownership checks are necessary but not sufficient for SSA proof.
    verify(graph);
    std::map<std::size_t, const Operation*> expected_uses;
    std::map<std::uint64_t, std::pair<std::size_t, std::string>> expected_writes;
    for (std::size_t i = 0; i < ops.size(); ++i) {
        const auto& op = ops[i];
        if (op.kind == Operation::Kind::UseSharedLoan ||
                op.kind == Operation::Kind::UseExclusiveLoan)
            expected_uses.emplace(i, &op);
        if (op.kind == Operation::Kind::Reinitialize &&
                (!op.write_id || !*op.write_id ||
                 !expected_writes.emplace(*op.write_id,
                     std::make_pair(i, op.place)).second))
            violation(op.place, "duplicate typed-MIR write origin");
    }

    std::map<std::uint64_t, TypedMirLoanUse> typed_sites;
    std::set<std::size_t> seen_operations;
    for (const auto& use : ssa_use_inventory) {
        auto expected = expected_uses.find(use.cfg_operation);
        if (!use.site_id || !use.loan_id || !use.ssa_value_id ||
                use.place.empty() || !bounded_mir_class(use.classification) ||
                expected == expected_uses.end() ||
                expected->second->loan_id != use.loan_id ||
                expected->second->place != use.place ||
                !typed_sites.emplace(use.site_id, use).second ||
                !seen_operations.insert(use.cfg_operation).second)
            violation(use.place, "unproven or repeated typed SSA use origin");
    }
    if (seen_operations.size() != expected_uses.size() ||
            lowered_loan_edges.size() != typed_sites.size())
        throw std::runtime_error("ownership violation: omitted SSA use or lowered loan edge");

    std::set<std::uint64_t> lowered_sites;
    for (const auto& edge : lowered_loan_edges) {
        auto found = typed_sites.find(edge.site_id);
        if (!edge.site_id || found == typed_sites.end() ||
                !(found->second == edge) ||
                !lowered_sites.insert(edge.site_id).second)
            violation(edge.place, "lowered use edge mismatches typed SSA site");
    }
    if (lowered_sites.size() != typed_sites.size())
        throw std::runtime_error("ownership violation: incomplete lowered use inventory");

    std::map<std::uint64_t, TypedMirFieldWrite> writes;
    for (const auto& write : mir_writes) {
        auto expected = expected_writes.find(write.write_id);
        if (!write.write_id || !write.ssa_value_id || write.place.empty() ||
                expected == expected_writes.end() ||
                expected->second.first != write.cfg_operation ||
                expected->second.second != write.place ||
                !writes.emplace(write.write_id, write).second)
            violation(write.place, "unproven typed-MIR field write");
    }
    if (writes.size() != expected_writes.size() ||
            emitted_store_claims.size() != expected_writes.size())
        throw std::runtime_error("ownership violation: MIR write missing store receipt");

    std::set<std::uint64_t> consumed_writes;
    std::set<std::uint64_t> instruction_ids;
    for (const auto& store : emitted_store_claims) {
        auto found = writes.find(store.write_id);
        if (found == writes.end() || !store.write_id ||
                !store.instruction_id || !store.ssa_value_id ||
                store.opcode != FieldStoreOpcode::NativeStore ||
                store.place != found->second.place ||
                store.ssa_value_id != found->second.ssa_value_id ||
                !consumed_writes.insert(store.write_id).second ||
                !instruction_ids.insert(store.instruction_id).second)
            violation(store.place, "store claim mismatches typed-MIR field write");
    }
    if (consumed_writes.size() != writes.size())
        throw std::runtime_error("ownership violation: unpaired field write");
    return {false, false, typed_sites.size(), consumed_writes.size()};
}

} // namespace oreslang::ownership
