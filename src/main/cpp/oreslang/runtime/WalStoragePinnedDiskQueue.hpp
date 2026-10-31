#pragma once

#include "oreslang/runtime/ActorGroupLifecycle.hpp"
#include "oreslang/runtime/WalStorageDiskQueue.hpp"

#include <cstdint>
#include <future>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace oreslang::runtime {

// Runtime-ONLY queue lifetime loan. This owns a native disk queue and pins its
// exact NativeActorRef incarnation through physical disk + handoff retirement.
// This is NOT an Oreslang guest capability or an AsyncIoOperationLifecycle
// registration. The wrapper may only be destroyed on a trusted off-carrier
// retirement worker: WalStorageDiskQueue's destructor joins OS workers.
class WalStoragePinnedDiskQueue final {
public:
    using Scope = WalStorageFdOwner::Scope;
    using Receipt = WalStorageFdOwner::SyncedReceipt;

    static std::unique_ptr<WalStoragePinnedDiskQueue> adopt_from_isolate(
            NativeActorRef actor, std::unique_ptr<WalStorageFdOwner> owner,
            const void* authenticated_issuer, Scope scope) {
        if (actor.actor_id() != scope.actor
                || actor.generation() != scope.generation
                || actor.actor_id() == 0 || actor.generation() == 0)
            throw std::invalid_argument("WAL disk queue actor incarnation mismatch");
        return std::unique_ptr<WalStoragePinnedDiskQueue>(new WalStoragePinnedDiskQueue(
            std::move(actor), std::move(owner), authenticated_issuer, scope));
    }

    WalStoragePinnedDiskQueue(const WalStoragePinnedDiskQueue&) = delete;
    WalStoragePinnedDiskQueue& operator=(const WalStoragePinnedDiskQueue&) = delete;
    WalStoragePinnedDiskQueue(WalStoragePinnedDiskQueue&&) = delete;
    WalStoragePinnedDiskQueue& operator=(WalStoragePinnedDiskQueue&&) = delete;

    ~WalStoragePinnedDiskQueue() noexcept {
        // Last physical disk and handoff worker must retire BEFORE actor loan
        // release, otherwise ActorGroup DONE could free the native actor arena.
        queue_.reset(); // BLOCKS: strictly supervisor off-carrier teardown.
        if (loanActive_) {
            actor_.release_resource_loan();
            loanActive_ = false;
        }
    }

    std::future<Receipt> submit(const NativeActorRef& actor,
                                const void* authenticated_issuer,
                                Scope claimed, const std::vector<std::uint8_t>& frame) {
        if (!actor_.same_incarnation(actor))
            throw std::invalid_argument("foreign native actor incarnation");
        // Fail-closed pre-admission check, NOT a supervisor STOP arbitration.
        // The supervisor still MUST revoke this queue at STOP linearization.
        if (!actor_.resource_admission_open()) {
            queue_->revoke();
            throw std::logic_error("native WAL actor stopped or detached");
        }
        return queue_->submit(authenticated_issuer, claimed, frame);
    }

    void revoke() noexcept { queue_->revoke(); }
    bool quarantined() const noexcept { return queue_->quarantined(); }
    std::size_t outstanding() const noexcept { return queue_->outstanding(); }

private:
    WalStoragePinnedDiskQueue(NativeActorRef actor,
                              std::unique_ptr<WalStorageFdOwner> owner,
                              const void* issuer, Scope scope)
        : actor_(std::move(actor)) {
        if (!owner || !issuer) throw std::invalid_argument("missing WAL isolate owner");
        // Retain the exact incarnation BEFORE any thread/FD owner is published.
        // Terminal/stale actors refuse resource-loan admission here.
        actor_.retain_resource_loan();
        loanActive_ = true;
        try {
            queue_ = std::make_unique<WalStorageDiskQueue>(
                std::move(owner), issuer, scope);
        } catch (...) {
            // No queue survived construction; no worker can own an actor loan.
            actor_.release_resource_loan();
            loanActive_ = false;
            throw;
        }
    }

    NativeActorRef actor_;
    std::unique_ptr<WalStorageDiskQueue> queue_;
    bool loanActive_ = false;
};

} // namespace oreslang::runtime
