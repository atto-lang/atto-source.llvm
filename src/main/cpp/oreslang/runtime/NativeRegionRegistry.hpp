#pragma once
#include "oreslang/runtime/TrustedPointerWindow.hpp"
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace oreslang::runtime {

// Host-only region bookkeeping, NOT an authentication boundary. The Trust
// enum and actor identity are caller-supplied and forgeable by native code.
// Only a supervisor-controlled admission path may invoke issue(). Never expose
// this object, its issue() method, or raw Grant values to guest source or FFI.
// The caller must keep mapped backing memory alive through revoke()/retire().
class NativeRegionRegistry final {
public:
    enum class Trust { Trusted, Untrusted };
    struct Grant final {
        std::uint64_t slot;
        NativeRegionIdentity provenance;
        ActorHeapIdentity owner;
    };
private:
    struct Entry {
        Grant grant;
        std::shared_ptr<TrustedPointerWindow> window;
        bool retiring = false;
    };
    std::mutex mu_;
    std::unordered_map<std::uint64_t, Entry> entries_;
    std::uint64_t next_slot_ = 1;
    std::uint64_t next_generation_ = 1;
public:
    NativeRegionRegistry() = default;
    NativeRegionRegistry(const NativeRegionRegistry&) = delete;
    NativeRegionRegistry& operator=(const NativeRegionRegistry&) = delete;

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
        // Do not consume identity on failed validation/allocation.
        const auto provenance = NativeRegionIdentity::supervisor_minted(
            region, next_generation_, allocation);
        auto window = std::make_shared<TrustedPointerWindow>(
            owner, provenance, base, bytes, mode);
        const Grant grant{next_slot_, provenance, owner};
        const auto inserted = entries_.emplace(grant.slot, Entry{grant, std::move(window), false});
        if (!inserted.second)
            throw std::logic_error("native grant slot collision");
        ++next_slot_;
        ++next_generation_;
        return grant;
    }

    template<class T> typename TrustedPointerWindow::template Cursor<T>
    at(const Grant& grant, ActorHeapIdentity caller, std::size_t offset = 0) {
        std::shared_ptr<TrustedPointerWindow> window;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = entries_.find(grant.slot);
            if (it == entries_.end() || it->second.retiring ||
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
            if (it == entries_.end() || it->second.retiring ||
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
