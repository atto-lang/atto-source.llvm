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

// Production compiler-owned bounded source-operation bridge.
// An actual OwnershipChecker marks a definite moved scalar field before its
// successful assignment; the parser seals its immutable source WriteId; the
// LLVM emitter attaches that ID to its *physical pre-optimization StoreInst*.
// This does NOT construct a complete whole-function typed MIR CFG. The only
// modeled transition is the single checker-admitted moved->owned field write;
// other operations, exceptional edges and LoanId uses are not certified.
struct SourceCheckedReinitializeAudit {
    std::size_t checked_moved_field_writes = 0;
    bool bounded_source_checked_stores_bound = false;
    bool full_typed_mir_complete = false;
    bool full_loan_use_coverage = false;
    bool optimized_code_verified = false;
    bool guest_verified = false;
};

inline SourceCheckedReinitializeAudit verify_source_checked_reinitialize_stores(
        const FieldWriteLedger& ledger, const Program& source,
        const llvm::Module& module,
        const std::map<const Function*, const llvm::Function*>& declarations) {
    source_identity::validate_program_identity(source);
    const auto direct = ledger.verify_source_coverage(source, module, declarations);
    std::map<const Statement*, const FieldWriteSite*> emitted;
    for (const auto& site : ledger.direct_store_receipts())
        if (!emitted.emplace(site.source, &site).second)
            throw std::runtime_error(
                "ownership violation: repeated physical source store receipt");

    std::set<std::uint64_t> claimed_ids;
    std::size_t claims = 0;
    // Enumerate the checker-marked source independently of the emitted store
    // ledger. No store ticket can create its own source reinitialization claim.
    for (const auto& function : source.functions) {
        for (std::size_t i = 0; i < function.statements.size(); ++i) {
            const auto& statement = function.statements[i];
            if (!statement.compiler_checked_field_reinitialization) continue;
            if (statement.kind != Statement::Kind::FieldWrite ||
                    statement.compiler_reinitialize_write_id == 0 ||
                    statement.compiler_reinitialize_write_id !=
                        statement.compiler_source_node_id ||
                    statement.compiler_source_path !=
                        ("f" + std::to_string(function.compiler_source_function_id) +
                         "/s" + std::to_string(i)) ||
                    statement.receiver.empty() || statement.field.empty() ||
                    !claimed_ids.insert(statement.compiler_reinitialize_write_id).second)
                throw std::runtime_error(
                    "ownership violation: malformed checker-origin straight-line reinitialization");
            const auto found = emitted.find(&statement);
            if (found == emitted.end())
                throw std::runtime_error(
                    "ownership violation: compiler-checked moved field has no physical LLVM store");
            const auto& site = *found->second;
            const auto native = declarations.find(&function);
            const std::string place = statement.receiver + "." + statement.field;
            if (native == declarations.end() ||
                    site.source_checked_reinitialize_write_id !=
                        statement.compiler_reinitialize_write_id ||
                    site.canonical_place != place ||
                    site.source_function != &function ||
                    site.emitted_function != native->second ||
                    site.compiler_source_unit_id != source.compiler_source_unit_id ||
                    !site.store || site.store->getFunction() != native->second ||
                    site.store->getPointerOperand() != site.destination)
                throw std::runtime_error(
                    "ownership violation: compiler-checked reinit WriteId not bound to its LLVM store");

            // Bounded typed ownership operation derived from the checker
            // fact, NOT from the StoreInst or caller-supplied synthetic CFG.
            // This checks the Reinitialize transition and ticket rules but
            // cannot certify the complete surrounding control flow.
            ownership::Graph local;
            local.entry.emplace(place, ownership::PlaceState(ownership::kMoved));
            local.blocks.resize(1);
            local.blocks.front().operations.emplace_back(
                ownership::Operation::Kind::Reinitialize, place,
                std::nullopt, statement.compiler_reinitialize_write_id);
            const auto checked = ownership::verify(local);
            if (checked.outgoing.size() != 1 ||
                    !checked.outgoing[0].has_value() ||
                    checked.outgoing[0]->at(place).possible != ownership::kOwned)
                throw std::runtime_error(
                    "ownership violation: checked moved-field reinitialization CFG transition invalid");
            ++claims;
        }

        // This v1 producer does not model branches, loops or caught-throw
        // cleanup. Refuse an erroneously marked nested source rather than
        // claiming full control-flow coverage.
        const auto visit = [&](const auto& self,
                const Statement& statement) -> void {
            for (const auto& child : statement.then_statements) {
                if (child.compiler_checked_field_reinitialization)
                    throw std::runtime_error(
                        "ownership violation: unchecked nested reinitialization provenance");
                self(self, child);
            }
            for (const auto& child : statement.else_statements) {
                if (child.compiler_checked_field_reinitialization)
                    throw std::runtime_error(
                        "ownership violation: unchecked nested reinitialization provenance");
                self(self, child);
            }
        };
        for (const auto& statement : function.statements) visit(visit, statement);
        for (const auto& cleanup : function.caught_throw_defers) {
            if (cleanup.compiler_checked_field_reinitialization)
                throw std::runtime_error(
                    "ownership violation: unsupported checked caught-cleanup reinitialization");
            visit(visit, cleanup);
        }
    }

    // Every claimed StoreInst must correspond to a unique checker-minted
    // source operation. The ledger independently rejects any cloned tag.
    std::size_t receipts = 0;
    for (const auto& site : ledger.direct_store_receipts())
        if (site.source_checked_reinitialize_write_id) ++receipts;
    if (receipts != claims)
        throw std::runtime_error(
            "ownership violation: unpaired compiler checked-reinitialization write receipts");

    return {claims, claims != 0 && direct.direct_source_writes_complete &&
            direct.immutable_source_identity_bound,
            false, false, false, false};
}

} // namespace oreslang::native_ir
