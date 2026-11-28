#pragma once
#include "oreslang/runtime/TrustedPointerWindow.hpp"
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <tuple>
#include <vector>
#include <unordered_map>

namespace oreslang::runtime {

// Only trusted runtime integration may define this admission authority.
// A test-only shim is separately enabled when compiling registry tests.
class NativeRegionRuntimeAuthority;
#ifdef ORESLANG_NATIVE_GRANT_TESTING
struct NativeRegionRegistryTestAccess;
#endif

// Host-only region bookkeeping. Native issuance is private, reserved for the
// runtime's trusted admission authority; guest code must never see this API.
// Native C++ in the TCB can always bypass language-level memory isolation.
// The caller must keep mapped backing memory alive through revoke()/retire().
class NativeRegionRegistry final {
    friend class NativeRegionRuntimeAuthority;
#ifdef ORESLANG_NATIVE_GRANT_TESTING
    friend struct NativeRegionRegistryTestAccess;
#endif
public:
    enum class Trust { Trusted, Untrusted };
    struct Grant final {
        std::uint64_t slot;
        std::uint64_t registry_id;
        NativeRegionIdentity provenance;
        ActorHeapIdentity owner;
    };
private:
    struct Entry {
        Grant grant;
        std::shared_ptr<TrustedPointerWindow> window;
        bool retiring = false;
        std::uintptr_t begin = 0;
        std::uintptr_t end = 0;
    };
    inline static std::atomic<std::uint64_t> next_registry_id_{1};
    const std::uint64_t registry_id_;
    std::mutex mu_;
    std::unordered_map<std::uint64_t, Entry> entries_;
    std::uint64_t next_slot_ = 1;
    std::uint64_t next_generation_ = 1;
    // A fenced actor incarnation must never be allowed to mint fresh native
    // grants in this registry, even after its old bytes are reclaimed.
    using ActorKey = std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>;
    std::set<ActorKey> fenced_actors_;
    static ActorKey actor_key(ActorHeapIdentity a) noexcept {
        return {a.runtime, a.actor, a.epoch};
    }
public:
    NativeRegionRegistry() : registry_id_(next_registry_id_.fetch_add(1, std::memory_order_relaxed)) {
        if (registry_id_ == 0 || registry_id_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("native region registry identity exhausted");
    }
    NativeRegionRegistry(const NativeRegionRegistry&) = delete;
    NativeRegionRegistry& operator=(const NativeRegionRegistry&) = delete;

private:
    // This operation must be called only after trusted runtime admission.
    // A caller-supplied enum is NOT an authentication proof.
    Grant issue(Trust trust, ActorHeapIdentity owner, std::uint64_t region,
                std::uint64_t allocation, void* base, std::size_t bytes,
                TrustedPointerWindow::Mode mode) {
        if (trust != Trust::Trusted)
            throw std::logic_error("untrusted actor cannot receive native grant");
        if (!owner.runtime || !owner.epoch)
            throw std::invalid_argument("invalid native grant actor identity");
        std::lock_guard<std::mutex> lock(mu_);
        if (fenced_actors_.count(actor_key(owner)))
            throw std::logic_error("actor incarnation is fenced from native grants");
        if (next_slot_ == std::numeric_limits<std::uint64_t>::max() ||
            next_generation_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("native grant identity exhausted");
        // A second grant over overlapping virtual memory would defeat actor
        // isolation even if both handles have different owner identities.
        // Fail closed until the supervisor has a formally shared-memory model.
        if (!base || !bytes)
            throw std::invalid_argument("empty native grant region");
        const auto begin = reinterpret_cast<std::uintptr_t>(base);
        if (bytes > std::numeric_limits<std::uintptr_t>::max() - begin)
            throw std::invalid_argument("overflowing native grant region");
        const auto end = begin + bytes;
        for (const auto& existing : entries_) {
            if (begin < existing.second.end && existing.second.begin < end)
                throw std::logic_error("overlapping native memory grants are forbidden");
        }
        // Do not consume identity on failed validation/allocation.
        const auto provenance = NativeRegionIdentity::supervisor_minted(
            region, next_generation_, allocation);
        auto window = std::make_shared<TrustedPointerWindow>(
            owner, provenance, base, bytes, mode);
        const Grant grant{next_slot_, registry_id_, provenance, owner};
        const auto inserted = entries_.emplace(grant.slot, Entry{grant, std::move(window), false, begin, end});
        if (!inserted.second)
            throw std::logic_error("native grant slot collision");
        ++next_slot_;
        ++next_generation_;
        return grant;
    }

public:
    template<class T> typename TrustedPointerWindow::template Cursor<T>
    at(const Grant& grant, ActorHeapIdentity caller, std::size_t offset = 0) {
        std::shared_ptr<TrustedPointerWindow> window;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = entries_.find(grant.slot);
            if (it == entries_.end() || grant.registry_id != registry_id_ || it->second.retiring ||
                fenced_actors_.count(actor_key(caller)) ||
                it->second.grant.provenance != grant.provenance ||
                it->second.grant.owner != grant.owner || caller != grant.owner)
                throw std::logic_error("invalid, retiring or foreign native grant");
            window = it->second.window;
        }
        // Never hold the registry mutex while waiting on the FFI window lock:
        // an FFI callback can call the registry on its own thread.
        return window->at<T>(caller, offset);
    }

    // Supervisor lifecycle/trust-downgrade fence. New grants fail immediately;
    // existing windows are synchronously revoked before a successful return.
    // An active foreign callback on this same thread cannot be quiesced here:
    // the method throws and retains the retiring entry for a later retry.
    // The supervisor must keep backing allocations mapped until it succeeds.
private:
    // Only NativeRegionRuntimeAuthority (or the isolated test shim) can
    // deactivate another actor's complete native authority.
    void fence_actor(ActorHeapIdentity owner) {
        if (!owner.runtime || !owner.epoch)
            throw std::invalid_argument("invalid native fence actor identity");
        // Allocate the snapshot before changing admission policy. A failed
        // allocation must not leave a half-fenced actor with live cursors.
        std::vector<std::pair<std::uint64_t, std::shared_ptr<TrustedPointerWindow>>> pending;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (const auto& pair : entries_) {
                if (pair.second.grant.owner == owner)
                    pending.emplace_back(pair.first, pair.second.window);
            }
            // Permanent for this exact incarnation; a newer epoch is distinct.
            fenced_actors_.insert(actor_key(owner));
            for (const auto& pair : pending) {
                const auto it = entries_.find(pair.first);
                if (it != entries_.end()) it->second.retiring = true;
            }
        }
        // Revoke every grant, even if a same-thread callback defers one of
        // them; returning early would leave other cached cursors live.
        bool deferred = false;
        for (const auto& pair : pending) {
            // Never hold mu_ while waiting for a callback; it might call back
            // into this registry. Cached cursors share the revoked state.
            if (!pair.second->revoke()) {
                deferred = true;
                continue;
            }
            std::lock_guard<std::mutex> lock(mu_);
            const auto it = entries_.find(pair.first);
            if (it != entries_.end() && it->second.window == pair.second)
                entries_.erase(it);
        }
        if (deferred)
            throw std::logic_error(
                "native actor fence deferred by active same-thread FFI; retry after callback");
    }

public:
    void revoke(const Grant& grant) {
        std::shared_ptr<TrustedPointerWindow> window;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = entries_.find(grant.slot);
            if (it == entries_.end() || grant.registry_id != registry_id_ || it->second.retiring ||
                it->second.grant.provenance != grant.provenance ||
                it->second.grant.owner != grant.owner)
                throw std::logic_error("invalid or already retiring native grant");
            it->second.retiring = true;
            window = it->second.window;
        }
        // Call without mu_: otherwise callback -> registry plus external
        // revoke -> window creates a mutex-order inversion / deadlock.
        const bool quiesced = window->revoke();
        std::lock_guard<std::mutex> lock(mu_);
        auto it = entries_.find(grant.slot);
        // A concurrent actor-wide fence may already have completed this
        // window's revocation and erased the grant. That is success, not an
        // authority revival or a reason to reinsert the stale entry.
        if (it == entries_.end() && fenced_actors_.count(actor_key(grant.owner)))
            return;
        if (it == entries_.end() || it->second.window != window)
            throw std::logic_error("native grant changed during revocation");
        if (!quiesced) {
            // Reentrant FFI must not allow the caller to unmap memory while the
            // callback still holds its raw address. Keep the grant for retry.
            it->second.retiring = false;
            throw std::logic_error("native FFI callback still holds region; retry revoke after return");
        }
        entries_.erase(it);
    }
};
} // namespace oreslang::runtime
