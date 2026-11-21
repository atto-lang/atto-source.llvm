#pragma once

#include "oreslang/frontend/Parser.hpp"
#include "oreslang/types/SourceNodeIdentity.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

namespace oreslang::native_ir {

// A compiler-owned *pre-optimization* source FieldWrite -> emitted StoreInst
// ledger. Captures only direct field-write instructions emitted by the real
// source lowering path. It DOES NOT prove complete SSA loan-use provenance,
// source-MIR Reinitialize.write_id binding, runtime execution or final binary
// stores. The source Program and LLVM Module must outlive this ledger.
struct FieldWriteSite {
    const Statement* source = nullptr;
    const llvm::StoreInst* store = nullptr;
    const llvm::Value* destination = nullptr;
    const llvm::Value* stored = nullptr;
    std::string owner;
    std::string field;
    std::string canonical_place;
    std::size_t source_offset = 0;
    std::uint64_t compiler_ticket = 0;
    const Function* source_function = nullptr;
    const llvm::Function* emitted_function = nullptr;
    // Value snapshots minted on the validated AST, independent of allocator
    // address and byte offset; zero is never acceptable in production.
    std::uint64_t compiler_source_node_id = 0;
    std::uint64_t compiler_source_function_id = 0;
    std::uint64_t compiler_source_unit_id = 0;
    std::string compiler_source_path;
    std::uint64_t source_checked_reinitialize_write_id = 0;
};

struct FieldWriteAudit {
    std::size_t checked_source_stores = 0;
    // This result is strictly a pre-optimization emitted-IR binding,
    // not an ownership or guest execution certificate.
    bool source_to_unoptimized_ir_bound = false;
    bool ownership_complete = false;
    bool optimized_code_verified = false;
    bool guest_verified = false;
    bool direct_source_writes_complete = false;
    bool immutable_source_identity_bound = false;
};

class FieldWriteLedger final {
public:
    // Default is for legacy isolated host-IR structural fixtures only.
    // Production codegen MUST use the validated Program constructor.
    FieldWriteLedger() = default;
    explicit FieldWriteLedger(const Program& program)
        : require_compiler_source_ids_(true),
          source_unit_id_(program.compiler_source_unit_id) {
        source_identity::validate_program_identity(program);
    }

    std::uint64_t bind(const Statement& source,
            const std::string& canonical_place,
            const llvm::Value* destination,
            llvm::StoreInst* emitted,
            const Function* source_function = nullptr,
            const llvm::Function* emitted_function = nullptr) {
        if (source.kind != Statement::Kind::FieldWrite ||
                source.receiver.empty() || source.field.empty() ||
                source.receiver.find('.') != std::string::npos ||
                source.field.find('.') != std::string::npos ||
                canonical_place != source.receiver + "." + source.field ||
                canonical_place.size() > 256 ||
                !destination || !emitted || !emitted->getParent() ||
                !emitted->getFunction() ||
                ((source_function == nullptr) != (emitted_function == nullptr)) ||
                (emitted_function && emitted->getFunction() != emitted_function) ||
                (require_compiler_source_ids_ &&
                    (!source.compiler_source_node_id ||
                     source.compiler_source_path.empty() ||
                     !source_function ||
                     !source_function->compiler_source_function_id ||
                     source_function->compiler_source_sealed_name !=
                         source_function->name ||
                     source.compiler_reinitialize_write_id !=
                         (source.compiler_checked_field_reinitialization
                             ? source.compiler_source_node_id : 0))))
            throw std::runtime_error(
                "ownership violation: invalid compiler field-write source/store origin");
        if (sites_.size() >= 4096)
            throw std::runtime_error(
                "ownership violation: compiler field-write ledger exceeds bound");
        if (emitted->getPointerOperand() != destination ||
                !emitted->getValueOperand())
            throw std::runtime_error(
                "ownership violation: field-write store destination/value mismatch");
        // Never accept a second store or source declaration with the same
        // minted ticket. A caller does not supply this ticket.
        if (!sources_.insert(&source).second ||
                !instructions_.insert(emitted).second)
            throw std::runtime_error(
                "ownership violation: duplicate compiler field-write source/store");
        const auto ticket = static_cast<std::uint64_t>(sites_.size() + 1);
        // Durable, compiler-minted source breadcrumb on the actual LLVM
        // StoreInst. It is not user-authored metadata or a runtime store
        // guarantee; optimizers may rewrite/remove this instruction.
        llvm::LLVMContext& ctx = emitted->getContext();
        auto* i64 = llvm::Type::getInt64Ty(ctx);
        emitted->setMetadata(kMetadataKind, llvm::MDNode::get(ctx, {
            llvm::MDString::get(ctx, canonical_place),
            llvm::ConstantAsMetadata::get(
                llvm::ConstantInt::get(i64, ticket)),
            llvm::ConstantAsMetadata::get(
                llvm::ConstantInt::get(i64,
                    static_cast<std::uint64_t>(source.offset)))
        }));
        const std::uint64_t checked_reinit_id =
            require_compiler_source_ids_
                ? source.compiler_reinitialize_write_id : 0;
        if (checked_reinit_id) {
            // Metadata is minted only after independent ownership checking
            // and validated AST sealing, and is attached to the real store.
            emitted->setMetadata(kReinitMetadataKind, llvm::MDNode::get(ctx, {
                llvm::ConstantAsMetadata::get(
                    llvm::ConstantInt::get(i64, source_unit_id_)),
                llvm::ConstantAsMetadata::get(
                    llvm::ConstantInt::get(i64, checked_reinit_id))
            }));
        }
        sites_.push_back({&source, emitted, destination,
            emitted->getValueOperand(), source.receiver, source.field,
            canonical_place, source.offset, ticket,
            source_function, emitted_function,
            source.compiler_source_node_id,
            source_function ? source_function->compiler_source_function_id : 0,
            source_unit_id_, source.compiler_source_path, checked_reinit_id});
        return ticket;
    }

    FieldWriteAudit verify(const llvm::Module& module) const {
        // Inspect the actual LLVM instruction list; never dereference a
        // potentially removed StoreInst until its pointer was found live.
        std::map<const llvm::Instruction*, const llvm::Function*> live;
        // Reverse inventory: an extra cloned/forged provenance-tagged store
        // must not escape the forward per-source receipt comparison.
        std::map<const llvm::StoreInst*, const llvm::Function*> tagged_stores;
        std::set<const llvm::StoreInst*> tagged_reinitializations;
        for (const auto& function : module) {
            if (function.isDeclaration()) continue;
            for (const auto& block : function)
                for (const auto& instruction : block) {
                    live.emplace(&instruction, &function);
                    if (const auto* store =
                            llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
                        if (store->getMetadata(kReinitMetadataKind)) {
                            if (tagged_reinitializations.size() >= 4096 ||
                                    !tagged_reinitializations.insert(store).second)
                                throw std::runtime_error(
                                    "ownership violation: duplicate or oversized tagged reinitialization inventory");
                        }
                        if (store->getMetadata(kMetadataKind)) {
                            if (tagged_stores.size() >= 4096 ||
                                    !tagged_stores.emplace(store, &function).second)
                                throw std::runtime_error(
                                    "ownership violation: duplicate or oversized emitted tagged store inventory");
                        }
                    }
                }
        }
        std::set<const Statement*> unique_sources;
        std::set<const llvm::Instruction*> unique_stores;
        std::set<std::uint64_t> unique_tickets;
        std::set<std::uint64_t> unique_reinitialization_ids;
        std::size_t expected_reinitializations = 0;
        for (std::size_t index = 0; index < sites_.size(); ++index) {
            const auto& record = sites_[index];
            // Source Program is stable for the duration of code emission.
            if (!record.source ||
                    record.source->kind != Statement::Kind::FieldWrite ||
                    record.source->offset != record.source_offset ||
                    record.source->receiver != record.owner ||
                    record.source->field != record.field ||
                    record.canonical_place != record.owner + "." + record.field ||
                    !record.compiler_ticket ||
                    record.compiler_ticket != index + 1 ||
                    !unique_sources.insert(record.source).second ||
                    !unique_stores.insert(record.store).second ||
                    !unique_tickets.insert(record.compiler_ticket).second)
                throw std::runtime_error(
                    "ownership violation: stale or aliased compiler field-write source");
            auto found = live.find(record.store);
            if (found == live.end())
                throw std::runtime_error(
                    "ownership violation: source field-write store missing in LLVM module");
            const auto* actual = llvm::dyn_cast<llvm::StoreInst>(found->first);
            if (!actual || actual->getFunction() != found->second ||
                    (record.emitted_function &&
                        actual->getFunction() != record.emitted_function) ||
                    actual->getPointerOperand() != record.destination ||
                    actual->getValueOperand() != record.stored)
                throw std::runtime_error(
                    "ownership violation: source field-write store mutated or substituted");
            if (record.source_checked_reinitialize_write_id) {
                ++expected_reinitializations;
                if (!unique_reinitialization_ids.insert(
                        record.source_checked_reinitialize_write_id).second ||
                        !tagged_reinitializations.count(actual))
                    throw std::runtime_error(
                        "ownership violation: reused or untagged checked reinitialization store");
                const auto* reinit_tag = actual->getMetadata(kReinitMetadataKind);
                if (!reinit_tag || reinit_tag->getNumOperands() != 2)
                    throw std::runtime_error(
                        "ownership violation: invalid checked reinitialization LLVM metadata");
                const auto uint64_tag = [&](unsigned slot) -> std::uint64_t {
                    const auto* meta = llvm::dyn_cast<llvm::ConstantAsMetadata>(
                        reinit_tag->getOperand(slot).get());
                    const auto* integer = meta
                        ? llvm::dyn_cast<llvm::ConstantInt>(meta->getValue())
                        : nullptr;
                    if (!integer || !integer->getType()->isIntegerTy(64))
                        throw std::runtime_error(
                            "ownership violation: noncanonical checked reinitialization metadata");
                    return integer->getZExtValue();
                };
                if (uint64_tag(0) != record.compiler_source_unit_id ||
                        uint64_tag(1) != record.source_checked_reinitialize_write_id)
                    throw std::runtime_error(
                        "ownership violation: checked reinitialization store tag changed");
            } else if (tagged_reinitializations.count(actual)) {
                throw std::runtime_error(
                    "ownership violation: unchecked field store forged reinitialization metadata");
            }
            const auto bound_tag = tagged_stores.find(actual);
            if (bound_tag == tagged_stores.end() ||
                    bound_tag->second != found->second)
                throw std::runtime_error(
                    "ownership violation: emitted tagged store not in reverse inventory");
            const auto* tag = actual->getMetadata(kMetadataKind);
            if (!tag || tag->getNumOperands() != 3)
                throw std::runtime_error(
                    "ownership violation: emitted field-write provenance metadata absent");
            const auto* name = llvm::dyn_cast<llvm::MDString>(
                tag->getOperand(0).get());
            const auto* minted = llvm::dyn_cast<llvm::ConstantAsMetadata>(
                tag->getOperand(1).get());
            const auto* byte_offset = llvm::dyn_cast<llvm::ConstantAsMetadata>(
                tag->getOperand(2).get());
            const auto* ticket_value = minted
                ? llvm::dyn_cast<llvm::ConstantInt>(minted->getValue())
                : nullptr;
            const auto* offset_value = byte_offset
                ? llvm::dyn_cast<llvm::ConstantInt>(byte_offset->getValue())
                : nullptr;
            if (!name || !ticket_value || !offset_value ||
                    name->getString().str() != record.canonical_place ||
                    !ticket_value->getType()->isIntegerTy(64) ||
                    !offset_value->getType()->isIntegerTy(64) ||
                    ticket_value->getZExtValue() != record.compiler_ticket ||
                    offset_value->getZExtValue() != record.source_offset)
                throw std::runtime_error(
                    "ownership violation: emitted field-write provenance metadata changed");
        }
        if (tagged_reinitializations.size() != expected_reinitializations)
            throw std::runtime_error(
                "ownership violation: orphaned LLVM reinitialization provenance tag");
        if (tagged_stores.size() != sites_.size())
            throw std::runtime_error(
                "ownership violation: invented or unpaired emitted tagged field store");
        return {sites_.size(), !sites_.empty(), false, false, false};
    }

    // Independent compiler-owned inventory from the full native Function
    // source AST, not from the emit ledger or an external JSON manifest.
    // Includes direct writes in nested branches/loops and source caught-throw
    // cleanup writes. 'Defer' effects are NOT direct FieldWrite and remain
    // outside this bounded proof.
    FieldWriteAudit verify_source_coverage(
            const Program& source, const llvm::Module& module,
            const std::map<const Function*, const llvm::Function*>&
                independent_function_declarations) const {
        if (require_compiler_source_ids_) {
            if (source.compiler_source_unit_id != source_unit_id_)
                throw std::runtime_error(
                    "ownership violation: source compilation identity changed");
            // This runs before any saved source Statement is dereferenced.
            // Node identity order rejects replaced or reused-address AST
            // nodes unless the compiler explicitly copied internal receipts,
            // which the validated language frontend never does.
            source_identity::validate_program_identity(source);
        }
        // First enumerate the currently live AST and verify all saved
        // source pointers by identity *without dereferencing them*. A Program
        // or Statement can have been erased/replaced after emission; calling
        // verify(module) first would dereference an untrusted stale pointer.
        // The production Program remains immutable during lowering, but the
        // verifier must reject stale evidence safely even under mutations.
        // The declaration registry is built independently from source
        // Program functions in production codegen, not the store ledger.
        // Do not confuse an arbitrary LLVM function with the correct
        // lowered body merely because the StoreInst belongs to it.
        if (independent_function_declarations.size() != source.functions.size())
            throw std::runtime_error(
                "ownership violation: incomplete source to native function declarations");
        std::set<const llvm::Function*> seen_native_functions;
        for (const auto& source_function : source.functions) {
            const auto it = independent_function_declarations.find(&source_function);
            if (it == independent_function_declarations.end() ||
                    !it->second || it->second->getParent() != &module ||
                    it->second->isDeclaration() ||
                    !seen_native_functions.insert(it->second).second)
                throw std::runtime_error(
                    "ownership violation: missing, foreign or aliased native function declaration");
        }
        std::map<const Statement*, const Function*> expected;
        std::size_t visited = 0;
        const auto visit = [&](const auto& self,
                const Statement& statement, const Function& function,
                std::size_t depth) -> void {
            if (depth > 64 || ++visited > 4096)
                throw std::runtime_error(
                    "ownership violation: compiler source field-write inventory exceeds bound");
            if (statement.kind == Statement::Kind::FieldWrite) {
                if (statement.receiver.empty() || statement.field.empty() ||
                        statement.receiver.find('.') != std::string::npos ||
                        statement.field.find('.') != std::string::npos ||
                        statement.receiver.size() > 255 ||
                        statement.field.size() > 255 ||
                        statement.receiver.size() + 1 + statement.field.size() > 256 ||
                        !expected.emplace(&statement, &function).second)
                    throw std::runtime_error(
                        "ownership violation: invalid or duplicated source field write");
            }
            // Scan every nested source block, including branch-local
            // statements; the emission ledger cannot selectively omit one.
            for (const auto& child : statement.then_statements)
                self(self, child, function, depth + 1);
            for (const auto& child : statement.else_statements)
                self(self, child, function, depth + 1);
        };
        for (const auto& function : source.functions) {
            for (const auto& statement : function.statements)
                visit(visit, statement, function, 0);
            for (const auto& cleanup : function.caught_throw_defers)
                visit(visit, cleanup, function, 0);
        }
        if (expected.size() != sites_.size())
            throw std::runtime_error(
                "ownership violation: omitted or invented source field-write emission");
        std::set<const Statement*> matched;
        for (const auto& site : sites_) {
            // These are pointer comparisons only. Do not dereference source,
            // store or function pointers until the corresponding independent
            // inventories have established that the objects are still live.
            const auto found = expected.find(site.source);
            const auto declaration = found != expected.end()
                ? independent_function_declarations.find(found->second)
                : independent_function_declarations.end();
            if (found == expected.end() || site.source_function != found->second ||
                    declaration == independent_function_declarations.end() ||
                    !site.emitted_function ||
                    site.emitted_function != declaration->second ||
                    (require_compiler_source_ids_ &&
                        (site.compiler_source_unit_id != source_unit_id_ ||
                         !site.compiler_source_node_id ||
                         site.compiler_source_node_id !=
                             found->first->compiler_source_node_id ||
                         site.compiler_source_path !=
                             found->first->compiler_source_path ||
                         site.source_checked_reinitialize_write_id !=
                             found->first->compiler_reinitialize_write_id ||
                         site.compiler_source_function_id !=
                             found->second->compiler_source_function_id)) ||
                    !matched.insert(site.source).second)
                throw std::runtime_error(
                    "ownership violation: source field-write/function not bound to emitted store");
        }
        if (matched.size() != expected.size())
            throw std::runtime_error(
                "ownership violation: incomplete compiler source field-write inventory");
        // Source pointers are live here. verify() independently checks actual
        // StoreInst membership in the Module before reading StoreInst fields.
        auto result = verify(module);
        for (const auto& site : sites_)
            if (site.store->getFunction() != site.emitted_function)
                throw std::runtime_error(
                    "ownership violation: source field-write emitted in wrong LLVM function");
        // Zero expected writes is a vacuous match, not positive evidence
        // of any source field-write instruction being linked to a store.
        result.direct_source_writes_complete = !expected.empty();
        result.immutable_source_identity_bound =
            require_compiler_source_ids_ && !expected.empty();
        // The direct FieldWrite inventory is NOT a complete ownership
        // loan-use / deferred-effect / optimized machine-code certificate.
        return result;
    }

    std::size_t size() const noexcept { return sites_.size(); }
    // Read-only compiler-minted direct-store receipts for the separate,
    // optional CFG Reinitialize.write_id candidate bridge. This does not
    // export ownership authority or allow an external caller to mint tickets.
    const std::vector<FieldWriteSite>& direct_store_receipts() const noexcept {
        return sites_;
    }

private:
    static constexpr const char* kMetadataKind =
        "atto.ownership.field_write.source.v1";
    static constexpr const char* kReinitMetadataKind =
        "atto.ownership.reinitialize.source.v1";
    std::vector<FieldWriteSite> sites_;
    std::set<const Statement*> sources_;
    std::set<const llvm::StoreInst*> instructions_;
    bool require_compiler_source_ids_ = false;
    std::uint64_t source_unit_id_ = 0;
};

} // namespace oreslang::native_ir
