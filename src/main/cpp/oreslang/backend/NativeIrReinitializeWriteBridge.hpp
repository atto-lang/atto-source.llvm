#pragma once

#include "oreslang/backend/NativeIrFieldWriteLedger.hpp"
#include "oreslang/types/OwnershipCfg.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

namespace oreslang::native_ir {

// This checks only *candidate* Reinitialize.write_id claims against real
// pre-optimization StoreInst receipts minted by production LLVM codegen.
// The graphs passed here MUST ultimately be produced by typed MIR from the
// same validated Program, not reconstructed from this ledger. Until that
// integration exists this is a host-level characterization helper ONLY.
// It does not prove completeness of all moved-field source effects.
struct CandidateReinitializeAudit {
    std::size_t checked_claims = 0;
    bool candidate_write_id_store_bound = false;
    bool typed_mir_complete = false;
    bool full_loan_use_coverage = false;
    bool optimized_code_verified = false;
    bool guest_verified = false;
};

inline CandidateReinitializeAudit verify_candidate_reinitialize_store_bridge(
        const FieldWriteLedger& ledger,
        const Program& source,
        const llvm::Module& module,
        const std::map<const Function*, const llvm::Function*>& declarations,
        const std::map<const Function*, ownership::Graph>& candidate_graphs) {
    // The source-to-StoreInst exact bijection, function registry, metadata
    // integrity, and live LLVM instructions are established independently.
    const auto source_audit =
        ledger.verify_source_coverage(source, module, declarations);
    if (candidate_graphs.size() != source.functions.size() ||
            source.functions.empty())
        throw std::runtime_error(
            "ownership violation: incomplete candidate reinitialization CFG function inventory");

    const auto& receipts = ledger.direct_store_receipts();
    // A candidate may not independently mint or substitute a ticket; it
    // must name an existing production-store receipt. That receipt belongs
    // to exactly one source Function and one actual LLVM StoreInst.
    std::map<std::uint64_t, const FieldWriteSite*> by_ticket;
    for (const auto& site : receipts) {
        if (!site.compiler_ticket ||
                !by_ticket.emplace(site.compiler_ticket, &site).second)
            throw std::runtime_error(
                "ownership violation: duplicated compiler field-write receipt ticket");
    }

    std::set<std::uint64_t> used_tickets;
    std::size_t matched = 0;
    for (const auto& function : source.functions) {
        const auto graph_it = candidate_graphs.find(&function);
        const auto native_it = declarations.find(&function);
        if (graph_it == candidate_graphs.end() || native_it == declarations.end())
            throw std::runtime_error(
                "ownership violation: missing source function CFG or native identity");

        // Verify ownership state, control-flow shape, duplicate IDs,
        // borrow/projection restrictions, and reachability before accepting
        // any candidate write. A claimant cannot use a dead CFG block.
        const auto analysis = ownership::verify(graph_it->second);
        const auto& blocks = graph_it->second.blocks;
        if (analysis.incoming.size() != blocks.size())
            throw std::runtime_error(
                "ownership violation: malformed candidate reinitialization analysis");
        for (std::size_t b = 0; b < blocks.size(); ++b) {
            for (const auto& operation : blocks[b].operations) {
                if (operation.kind != ownership::Operation::Kind::Reinitialize)
                    continue;
                if (!analysis.incoming[b].has_value() ||
                        !operation.write_id || !*operation.write_id ||
                        !used_tickets.insert(*operation.write_id).second)
                    throw std::runtime_error(
                        "ownership violation: unreachable, missing or repeated field reinitialization ticket");
                const auto site_it = by_ticket.find(*operation.write_id);
                if (site_it == by_ticket.end())
                    throw std::runtime_error(
                        "ownership violation: reinitialization ticket has no emitted LLVM store");
                const auto& site = *site_it->second;
                if (site.source_function != &function ||
                        site.emitted_function != native_it->second ||
                        site.canonical_place != operation.place ||
                        !site.store || !site.destination ||
                        !site.compiler_ticket)
                    throw std::runtime_error(
                        "ownership violation: reinitialization place/function/store does not match receipt");
                ++matched;
            }
        }
    }
    if (matched != used_tickets.size())
        throw std::runtime_error(
            "ownership violation: inconsistent reinitialization candidate inventory");

    // This is a scoped candidate bridge, never a proof that the compiler's
    // typed MIR included all source writes or loans. A zero-claim result
    // cannot promote even the limited candidate binding flag.
    return {matched, matched != 0 && source_audit.direct_source_writes_complete,
            false, false, false, false};
}

} // namespace oreslang::native_ir
