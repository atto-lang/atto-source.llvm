#pragma once
#include "oreslang/runtime/TrustedPointerWindow.hpp"
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace oreslang::runtime {

// Host-only authority registry. Never expose this object to guest source or FFI.
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
        std::unique_ptr<TrustedPointerWindow> window;
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
        auto window = std::make_unique<TrustedPointerWindow>(
            owner, provenance, base, bytes, mode);
        const Grant grant{next_slot_, provenance, owner};
        entries_.emplace(grant.slot, Entry{grant, std::move(window)});
        ++next_slot_;
        ++next_generation_;
        return grant;
    }

    template<class T> typename TrustedPointerWindow::template Cursor<T>
    at(const Grant& grant, ActorHeapIdentity caller, std::size_t offset = 0) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = entries_.find(grant.slot);
        if (it == entries_.end() || it->second.grant.provenance != grant.provenance ||
            it->second.grant.owner != grant.owner || caller != grant.owner)
            throw std::logic_error("invalid or foreign native grant");
        return it->second.window->at<T>(caller, offset);
    }

    void revoke(const Grant& grant) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = entries_.find(grant.slot);
        if (it == entries_.end() || it->second.grant.provenance != grant.provenance ||
            it->second.grant.owner != grant.owner)
            throw std::logic_error("invalid native grant revocation");
        it->second.window->revoke();
        entries_.erase(it);
    }
};
} // namespace oreslang::runtime
