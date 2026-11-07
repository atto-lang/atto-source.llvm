#pragma once

#include "oreslang/types/OwnershipCfg.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Bounded typed-MIR stage-zero adapter. This is NOT a general NLL inference
// algorithm and is deliberately NOT wired into guest lowering. The caller
// must supply the complete already-checked set of lexical loan use sites.
// A proof object with missing uses is not sound; until the compiler mints
// it from SSA def-use, do not promote this to automatic release.
namespace oreslang::ownership {

struct ProvenLastUse {
    std::uint64_t loan_id = 0;
    std::string place;
    Operation::Kind borrow_kind = Operation::Kind::BorrowShared;
    std::size_t borrow_site = 0;
    std::size_t final_use_site = 0;
};

inline Graph lower_explicit_last_use_evidence_v1(
        const Graph& graph, const std::vector<ProvenLastUse>& proofs) {
    if (graph.blocks.size() != 1 || !graph.blocks[0].successors.empty())
        throw std::runtime_error(
            "ownership violation: last-use lowering requires straight-line proven CFG");
    const auto& operations = graph.blocks[0].operations;
    std::map<std::uint64_t, ProvenLastUse> expected;
    std::set<std::uint64_t> unproven;
    for (std::size_t index = 0; index < operations.size(); ++index) {
        const auto& op = operations[index];
        if (op.kind != Operation::Kind::BorrowShared &&
                op.kind != Operation::Kind::BorrowExclusive) continue;
        if (!op.loan_id || !*op.loan_id)
            violation(op.place, "last-use loan lacks compiler-minted lexical ID");
        if (!unproven.insert(*op.loan_id).second)
            violation(op.place, "duplicate lexical last-use loan identity");
    }
    if (operations.size() > 4096 || proofs.size() > 4096 ||
            proofs.size() != unproven.size())
        throw std::runtime_error(
            "ownership violation: incomplete last-use compiler provenance");
    for (const auto& proof : proofs) {
        if (!proof.loan_id || !unproven.count(proof.loan_id) ||
                !expected.emplace(proof.loan_id, proof).second)
            violation(proof.place, "unknown or duplicate last-use proof");
        if (proof.borrow_site >= operations.size() ||
                proof.final_use_site >= operations.size() ||
                proof.final_use_site <= proof.borrow_site)
            violation(proof.place, "invalid last-use source range");
        const auto& borrow = operations[proof.borrow_site];
        if (borrow.kind != proof.borrow_kind ||
                (borrow.kind != Operation::Kind::BorrowShared &&
                 borrow.kind != Operation::Kind::BorrowExclusive) ||
                borrow.loan_id != proof.loan_id || borrow.place != proof.place)
            violation(proof.place, "last-use proof does not match source borrow");
        const auto& last = operations[proof.final_use_site];
        const auto wanted_use = borrow.kind == Operation::Kind::BorrowShared
            ? Operation::Kind::UseSharedLoan : Operation::Kind::UseExclusiveLoan;
        if (last.kind != wanted_use || last.loan_id != proof.loan_id ||
                last.place != proof.place)
            violation(proof.place, "last-use proof lacks final explicit use");
        // No source-visible release, post-final borrow use, or borrowed-source
        // operation may be hidden by insertion of a release.
        for (std::size_t i = proof.borrow_site + 1; i < operations.size(); ++i) {
            const auto& op = operations[i];
            if (op.loan_id != proof.loan_id) continue;
            if (op.kind == Operation::Kind::ReleaseShared ||
                    op.kind == Operation::Kind::ReleaseExclusive)
                violation(proof.place, "mixed explicit and inferred loan release");
            if (i > proof.final_use_site &&
                    (op.kind == Operation::Kind::UseSharedLoan ||
                     op.kind == Operation::Kind::UseExclusiveLoan))
                violation(proof.place, "loan used after claimed last use");
        }
    }
    // A syntactic owner read after an annotated borrow might actually be
    // an access through the borrowed view. Until typed SSA classifies every
    // read/call argument by LoanId, fail closed on ANY unclassified read
    // occurring after a tracked borrow. This intentionally also rejects
    // safe independent/owner reads: precision needs a future compiler proof.
    // Move/Drop are separately validated against active loans by verify().
    std::size_t earliest_borrow = operations.size();
    for (const auto& item : expected)
        if (item.second.borrow_site < earliest_borrow)
            earliest_borrow = item.second.borrow_site;
    for (std::size_t i = 0; i < operations.size(); ++i) {
        const auto& op = operations[i];
        if (i > earliest_borrow && (op.kind == Operation::Kind::Read ||
                op.kind == Operation::Kind::ReadObject))
            violation(op.place, "unclassified read after borrow requires typed SSA use proof");
        if (op.kind != Operation::Kind::UseSharedLoan &&
                op.kind != Operation::Kind::UseExclusiveLoan) continue;
        if (!op.loan_id || !expected.count(*op.loan_id) ||
                i <= expected.at(*op.loan_id).borrow_site ||
                i > expected.at(*op.loan_id).final_use_site ||
                op.place != expected.at(*op.loan_id).place)
            violation(op.place, "use lacks complete lexical last-use proof");
    }

    Graph lowered = graph;
    lowered.blocks[0].operations.clear();
    for (std::size_t i = 0; i < operations.size(); ++i) {
        lowered.blocks[0].operations.push_back(operations[i]);
        for (const auto& [loan_id, proof] : expected) {
            if (proof.final_use_site == i)
                lowered.blocks[0].operations.emplace_back(
                    proof.borrow_kind == Operation::Kind::BorrowShared
                        ? Operation::Kind::ReleaseShared
                        : Operation::Kind::ReleaseExclusive,
                    proof.place, loan_id);
        }
    }
    // No assumption about source syntax counts as proof: run the regular
    // powerset/loan verifier on every emitted operation and CFG exit.
    verify(lowered);
    return lowered;
}

} // namespace oreslang::ownership
