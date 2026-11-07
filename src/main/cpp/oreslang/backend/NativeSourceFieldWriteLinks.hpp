#pragma once

#include "oreslang/backend/NativeIrOwnershipCensus.hpp"

#include <cstddef>
#include <cstdint>
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
            SourceFieldWriteKind kind, const llvm::StoreInst& emitted) {
        constexpr std::size_t kMaxSites = 4096;
        if (sites_.size() >= kMaxSites || place.empty() ||
                place.size() > 256 || place.front() == '.' ||
                place.back() == '.' || place.find("..") != std::string::npos ||
                emitted.getFunction() != &function ||
                !emitted.getPointerOperand() || !emitted.getValueOperand())
            throw std::runtime_error(
                "ownership violation: unbounded or invalid source field store site");
        if (!sites_.empty() &&
                sites_.back().emission_site != sites_.size())
            throw std::runtime_error(
                "ownership violation: source field store site sequence lost");
        sites_.push_back({static_cast<std::uint64_t>(sites_.size() + 1),
            &function, &emitted, emitted.getPointerOperand(),
            emitted.getValueOperand(), place, kind});
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
        std::set<const llvm::StoreInst*> used;
        std::set<std::uint64_t> site_ids;
        std::size_t count = 0;
        for (const auto& link : sites_) {
            bool owning_function = false;
            for (const auto& fn : module)
                if (&fn == link.function) owning_function = true;
            if (!link.emission_site || link.emission_site != count + 1 ||
                    !site_ids.insert(link.emission_site).second ||
                    !owning_function || !link.function || !link.store ||
                    !link.destination || !link.value ||
                    link.place.empty() || link.place.size() > 256 ||
                    link.place.front() == '.' || link.place.back() == '.' ||
                    link.place.find("..") != std::string::npos ||
                    !used.insert(link.store).second ||
                    !census.contains_store(link.store, link.destination, link.value))
                throw std::runtime_error(
                    "ownership violation: source field write lacks matching live LLVM StoreInst");
            // contains_store scans the whole module. Check the physical
            // StoreInst was emitted in the function attributed by the source
            // compiler; never dereference a stale link.store pointer.
            bool in_function = false;
            for (const auto& fn : module) {
                if (&fn != link.function) continue;
                for (const auto& block : fn)
                    for (const auto& instruction : block)
                        if (&instruction == link.store)
                            in_function = true;
            }
            if (!in_function)
                throw std::runtime_error(
                    "ownership violation: source field write belongs to wrong LLVM function");
            ++count;
        }
        return {false, false, count};
    }
};

} // namespace oreslang::native_ir
