#pragma once

#include "oreslang/runtime/ActorGroupLifecycle.hpp"
#include "oreslang/runtime/ActorResourceRegistry.hpp"

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::runtime {

/**
 * Trusted supervisor quarantine. A failed resource registry may contain
 * callbacks that own or reference live native arenas; it must never die merely
 * because the actor's observer future has completed exceptionally.
 *
 * This store must outlive all terminally failed actors it acknowledges. It is
 * deliberately bounded: on exhaustion, the retirement barrier REFUSES to
 * acknowledge done, retaining ownership and exposing the failure to its host.
 */
class NativeActorResourceQuarantine final {
public:
    explicit NativeActorResourceQuarantine(std::size_t capacity)
        : capacity_(capacity), state_(std::make_unique<State>()) {
        if (capacity == 0 || capacity > 4096)
            throw std::invalid_argument("invalid native actor quarantine capacity");
        state_->entries.reserve(capacity);
    }

    NativeActorResourceQuarantine(const NativeActorResourceQuarantine&) = delete;
    NativeActorResourceQuarantine& operator=(const NativeActorResourceQuarantine&) = delete;

    ~NativeActorResourceQuarantine() {
        // A failed FFI revocation can still refer to arena memory. Destroying
        // the callbacks here might invoke their RAII destructors and free the
        // arenas prematurely. Preserve the entire bounded state allocation
        // until process termination when there are unresolved failures.
        // The process supervisor must log/alert and restart the isolate.
        std::lock_guard<std::mutex> guard(state_->mutex);
        if (!state_->entries.empty()) (void) state_.release();
    }

    bool hold(std::shared_ptr<ActorResourceRegistry> registry) {
        if (!registry || registry->phase() != ActorResourceRegistry::Phase::failed)
            throw std::invalid_argument("only failed actor resource registries can be quarantined");
        std::lock_guard<std::mutex> guard(state_->mutex);
        for (const auto& item : state_->entries) {
            if (item == registry) return true;
        }
        if (state_->entries.size() >= capacity_) return false;
        state_->entries.push_back(std::move(registry));
        return true;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return state_->entries.size();
    }

    /**
     * Trusted supervisor ONLY, following an independently verified shutdown
     * of the entire native isolation domain. Never infer that proof from a
     * deadline or a FAILED resource closer. Drops retained callback captures
     * but does NOT retry any closer. Tests call this after all fake callback
     * producers have been disabled, so LeakSanitizer can verify the success
     * and failure test paths without mistaking safety quarantine for leaks.
     */
    void release_after_verified_isolate_shutdown() {
        std::vector<std::shared_ptr<ActorResourceRegistry>> retired;
        {
            std::lock_guard<std::mutex> guard(state_->mutex);
            retired.swap(state_->entries);
        }
        // Destruct outside the quarantine mutex: RAII destructors may enter
        // other host locks, and must never observe this mutex held.
    }


private:
    struct State {
        mutable std::mutex mutex;
        std::vector<std::shared_ptr<ActorResourceRegistry>> entries;
    };
    const std::size_t capacity_;
    std::unique_ptr<State> state_;
};

/**
 * Explicit native actor done-barrier connecting a per-incarnation resource
 * registry to ActorGroup's generation-fenced runtime finalizer.
 *
 * This is host-only, nonblocking and does not execute guest code. poll() is
 * called by the trusted scheduler when a turn/loan/callback retires. It
 * refuses to settle get_done on a busy registry. A failed close must first
 * transfer long-lived ownership to supervisor quarantine; only THEN may the
 * actor's observer future fail exceptionally.
 *
 * The group must be detached/closed before RuntimeFinalizer::finalized()
 * succeeds. Source-level async tear_down remains a separate compiler task.
 */
class NativeActorRetirementBarrier final {
public:
    enum class Poll { not_retired, pending, quarantine_full, not_admitted,
                      completed, failed };

    NativeActorRetirementBarrier(
            std::shared_ptr<ActorResourceRegistry> resources,
            NativeActorGroup::RuntimeFinalizer finalizer,
            NativeActorResourceQuarantine& quarantine)
        : resources_(std::move(resources)),
          finalizer_(std::move(finalizer)), quarantine_(quarantine) {
        if (!resources_) throw std::invalid_argument("actor registry required");
    }

    NativeActorRetirementBarrier(const NativeActorRetirementBarrier&) = delete;
    NativeActorRetirementBarrier& operator=(const NativeActorRetirementBarrier&) = delete;

    void retire() {
        resources_->retire();  // closes new admissions before any done attempt
        retired_.store(true, std::memory_order_release);
    }

    Poll poll() {
        if (!retired_.load(std::memory_order_acquire)) return Poll::not_retired;
        const Poll terminal = terminal_.load(std::memory_order_acquire);
        if (terminal == Poll::completed || terminal == Poll::failed)
            return terminal;

        // A host thread may call poll while another is invoking closers.
        // Never block another carrier behind a native resource destructor.
        bool expected = false;
        if (!polling_.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel))
            return Poll::pending;
        struct Reset {
            std::atomic<bool>& flag;
            ~Reset() { flag.store(false, std::memory_order_release); }
        } reset{polling_};

        // Closing the actor's mailbox alone is insufficient: an admitted
        // guest/CONTROL turn may still be using its arena. Wait for the group
        // to detach AND for its single-writer execution lease to quiesce
        // before running any native resource destructor.
        if (!finalizer_.ready_for_resource_reclamation()) return Poll::not_admitted;

        auto result = resources_->try_reclaim();
        if (result.status == ActorResourceRegistry::Reclaim::not_retired ||
                result.status == ActorResourceRegistry::Reclaim::busy ||
                result.status == ActorResourceRegistry::Reclaim::closing)
            return Poll::pending;

        if (result.status == ActorResourceRegistry::Reclaim::failed) {
            // Quarantine must retain the exact failed registry, including any
            // native allocation closer's captured shared ownership.
            if (!quarantine_.hold(resources_)) return Poll::quarantine_full;
            if (!finalizer_.finalized("native actor resource reclamation failed"))
                return Poll::not_admitted;
            terminal_.store(Poll::failed, std::memory_order_release);
            return Poll::failed;
        }

        if (!finalizer_.finalized()) return Poll::not_admitted;
        terminal_.store(Poll::completed, std::memory_order_release);
        return Poll::completed;
    }

private:
    std::shared_ptr<ActorResourceRegistry> resources_;
    NativeActorGroup::RuntimeFinalizer finalizer_;
    NativeActorResourceQuarantine& quarantine_;
    std::atomic<bool> retired_{false};
    std::atomic<bool> polling_{false};
    std::atomic<Poll> terminal_{Poll::not_retired};
};

} // namespace oreslang::runtime
