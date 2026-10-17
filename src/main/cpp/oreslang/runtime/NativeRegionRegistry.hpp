#pragma once
#include "oreslang/runtime/TrustedPointerWindow.hpp"
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
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
        if (!owner.runtime || !owner.actor || !owner.epoch)
            throw std::invalid_argument("invalid native grant actor identity");
        std::lock_guard<std::mutex> lock(mu_);
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
                it->second.grant.provenance != grant.provenance ||
                it->second.grant.owner != grant.owner || caller != grant.owner)
                throw std::logic_error("invalid, retiring or foreign native grant");
            window = it->second.window;
        }
        // Never hold the registry mutex while waiting on the FFI window lock:
        // an FFI callback can call the registry on its own thread.
        return window->at<T>(caller, offset);
    }

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
