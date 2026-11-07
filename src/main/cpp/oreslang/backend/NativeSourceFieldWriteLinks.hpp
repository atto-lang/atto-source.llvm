#pragma once

#include "oreslang/backend/NativeIrOwnershipCensus.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::native_ir {

// This is an emitted-instruction link for the source FieldWrite paths,
// NOT an authenticated CFG Reinitialize.write_id or full borrow/SSA proof.
enum class SourceFieldWriteKind { Direct, Deferred, CatchCleanup };

struct SourceFieldWriteLink {
    std::uint64_t emission_site = 0; // minted in emission order, never source supplied
    std::uint64_t reinitialize_write_id = 0; // validator-minted 0 for ordinary writes
    const llvm::Function* function = nullptr;
    const llvm::StoreInst* store = nullptr;
    const llvm::Value* destination = nullptr;
    const llvm::Value* value = nullptr;
    std::string place;
    SourceFieldWriteKind kind = SourceFieldWriteKind::Direct;
};

struct SourceFieldWriteCheck {
    bool source_ownership_complete = false;
    bool guest_verified = false;
    std::size_t physically_matched_writes = 0;
};

class CompilerFieldWriteLinks {
    std::vector<SourceFieldWriteLink> sites_;
public:
    // Must be invoked at the *actual* production LLVM CreateStore site.
    // A source claim without a physically created StoreInst is not recordable.
    void record(const llvm::Function& function, const std::string& place,
            SourceFieldWriteKind kind, const llvm::StoreInst& emitted,
            std::uint64_t reinitialize_write_id = 0) {
        constexpr std::size_t kMaxSites = 4096;
        if (sites_.size() >= kMaxSites || place.empty() ||
                place.size() > 256 || place.front() == '.' ||
                place.back() == '.' || place.find("..") != std::string::npos ||
                (reinitialize_write_id &&
                    (reinitialize_write_id > 4096 ||
                     kind != SourceFieldWriteKind::Direct)) ||
                emitted.getFunction() != &function ||
                !emitted.getPointerOperand() || !emitted.getValueOperand())
            throw std::runtime_error(
                "ownership violation: unbounded or invalid source field store site");
        if (!sites_.empty() &&
                sites_.back().emission_site != sites_.size())
            throw std::runtime_error(
                "ownership violation: source field store site sequence lost");
        if (reinitialize_write_id)
            for (const auto& other : sites_)
                if (other.reinitialize_write_id == reinitialize_write_id)
                    throw std::runtime_error(
                        "ownership violation: duplicate compiler restoration ticket");
        sites_.push_back({static_cast<std::uint64_t>(sites_.size() + 1),
            reinitialize_write_id, &function, &emitted,
            emitted.getPointerOperand(), emitted.getValueOperand(), place, kind});
    }

    const std::vector<SourceFieldWriteLink>& sites() const noexcept {
        return sites_;
    }

    // Compare against actual module-owned instructions *after* verifyModule,
    // using live-only readback. The borrowed Module must remain alive and
    // unmodified during the check. In particular the source/LLVM provenance
    // record is not a durable handle across optimizer generations.
    SourceFieldWriteCheck inspect(const llvm::Module& module,
            const NativeIrUseStoreCensus& census) const {
        if (census.source_module != &module || sites_.size() > 4096)
            throw std::runtime_error(
                "ownership violation: field write link refers to foreign IR module");
        // One bounded module readback, not one full module scan per source
        // write. A naive per-site scan could require 4096 * 2,000,000
        // instruction visits (resource-exhaustion attack surface).
        // Index only LIVE StoreInsts; never dereference a stale claim.
        using LiveStore = std::pair<const llvm::Function*, const ActualStore*>;
        std::map<const llvm::StoreInst*, LiveStore> live_stores;
        std::uint64_t ordinal = 0;
        std::size_t store_index = 0;
        for (const auto& function : module) {
            if (function.isDeclaration()) continue;
            for (const auto& block : function) {
                for (const auto& instruction : block) {
                    ++ordinal;
                    if (const auto* actual =
                            llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
                        if (store_index >= census.stores.size())
                            throw std::runtime_error(
                                "ownership violation: inserted native store invalidates source write snapshot");
                        const auto& snapshot = census.stores[store_index++];
                        if (snapshot.instruction != actual ||
                                snapshot.instruction_ordinal != ordinal ||
                                snapshot.pointer != actual->getPointerOperand() ||
                                snapshot.value != actual->getValueOperand() ||
                                !live_stores.emplace(actual,
                                    std::make_pair(&function, &snapshot)).second)
                            throw std::runtime_error(
                                "ownership violation: modified physical store invalidates source write snapshot");
                    }
                }
            }
        }
        if (ordinal != census.instructions ||
                store_index != census.stores.size())
            throw std::runtime_error(
                "ownership violation: stale native instruction census");

        std::set<const llvm::StoreInst*> used;
        std::set<std::uint64_t> restoration_tickets;
        std::size_t count = 0;
        for (const auto& link : sites_) {
            const auto live = live_stores.find(link.store);
            if (!link.emission_site || link.emission_site != count + 1 ||
                    !link.function || !link.store ||
                    !link.destination || !link.value ||
                    link.place.empty() || link.place.size() > 256 ||
                    link.place.front() == '.' || link.place.back() == '.' ||
                    link.place.find("..") != std::string::npos ||
                    (link.reinitialize_write_id &&
                        (link.reinitialize_write_id > 4096 ||
                         link.kind != SourceFieldWriteKind::Direct ||
                         !restoration_tickets.insert(
                             link.reinitialize_write_id).second)) ||
                    !used.insert(link.store).second ||
                    live == live_stores.end() ||
                    live->second.first != link.function ||
                    live->second.second->pointer != link.destination ||
                    live->second.second->value != link.value)
                throw std::runtime_error(
                    "ownership violation: source field write lacks matching live LLVM StoreInst");
            ++count;
        }
        return {false, false, count};
    }
};

} // namespace oreslang::native_ir
