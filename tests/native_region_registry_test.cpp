#define ORESLANG_NATIVE_GRANT_TESTING 1
#include "oreslang/runtime/NativeRegionRegistry.hpp"
namespace oreslang::runtime {
struct NativeRegionRegistryTestAccess {
    static NativeRegionRegistry::Grant issue(NativeRegionRegistry& registry,
        NativeRegionRegistry::Trust trust, ActorHeapIdentity owner,
        std::uint64_t region, std::uint64_t allocation, void* base,
        std::size_t bytes, TrustedPointerWindow::Mode mode) {
        return registry.issue(trust, owner, region, allocation, base, bytes, mode);
    }
};
}

#include <array>
#include <atomic>
#include <thread>
#include <cstdint>
#include <stdexcept>
using namespace oreslang::runtime;
template<class F> void reject(F f) {
    bool caught = false;
    try { f(); } catch (const std::logic_error&) { caught = true; }
    if (!caught) throw std::runtime_error("expected grant denial");
}
int main() {
    NativeRegionRegistry registry;
    // Only this test build can call the private issuance hook. Production guest
    // code cannot request a trusted grant by passing Trust::Trusted.
    auto issue = [&](NativeRegionRegistry::Trust trust, ActorHeapIdentity actor,
                     std::uint64_t region, std::uint64_t allocation, void* base,
                     std::size_t bytes, TrustedPointerWindow::Mode mode) {
        return NativeRegionRegistryTestAccess::issue(registry, trust, actor,
            region, allocation, base, bytes, mode);
    };
    const ActorHeapIdentity owner{1, 2, 3}, other{1, 4, 3}, stale{1, 2, 2};
    std::array<std::uint32_t, 4> bytes{{10, 20, 30, 40}};
    reject([&] { issue(NativeRegionRegistry::Trust::Untrusted, owner, 9, 1,
        bytes.data(), sizeof(bytes), TrustedPointerWindow::Mode::ReadWrite); });
    // Invalid enum values must never silently turn into ordinary RAM access.
    bool invalid_mode_rejected = false;
    try {
        (void)issue(NativeRegionRegistry::Trust::Trusted, owner, 9, 1,
            bytes.data(), sizeof(bytes), static_cast<TrustedPointerWindow::Mode>(255));
    } catch (const std::invalid_argument&) {
        invalid_mode_rejected = true;
    }
    if (!invalid_mode_rejected) throw std::runtime_error("invalid mode admitted");
    reject([&] { (void)issue(static_cast<NativeRegionRegistry::Trust>(255),
        owner, 9, 1, bytes.data(), sizeof(bytes),
        TrustedPointerWindow::Mode::ReadWrite); });
    auto grant = issue(NativeRegionRegistry::Trust::Trusted, owner, 9, 1,
        bytes.data(), sizeof(bytes), TrustedPointerWindow::Mode::ReadWrite);
    // Even distinct actor identities cannot obtain aliased grants over the
    // same backing bytes. Read-only aliasing also requires explicit sharing.
    reject([&] { (void)issue(NativeRegionRegistry::Trust::Trusted,
        other, 11, 4, bytes.data(), sizeof(bytes),
        TrustedPointerWindow::Mode::ReadOnly); });
    reject([&] { (void)issue(NativeRegionRegistry::Trust::Trusted,
        owner, 12, 5, bytes.data() + 1, sizeof(std::uint32_t),
        TrustedPointerWindow::Mode::ReadWrite); });
    // Disjoint adjacent windows are permitted and must not be mistaken for
    // overlapping ranges.
    std::array<std::uint32_t, 2> adjacentBacking{{55, 66}};
    auto adjacentLeft = issue(NativeRegionRegistry::Trust::Trusted,
        owner, 13, 6, adjacentBacking.data(), sizeof(std::uint32_t),
        TrustedPointerWindow::Mode::ReadOnly);
    auto adjacentRight = issue(NativeRegionRegistry::Trust::Trusted,
        other, 14, 7, adjacentBacking.data() + 1, sizeof(std::uint32_t),
        TrustedPointerWindow::Mode::ReadOnly);
    if (registry.at<std::uint32_t>(adjacentRight, other).load(other) != 66)
        throw std::runtime_error("adjacent window not readable");
    registry.revoke(adjacentRight);
    registry.revoke(adjacentLeft);
    // Independent registry instances must not accept grants that coincidentally
    // share slot, generation, region, allocation and actor identifiers.
    NativeRegionRegistry foreignRegistry;
    std::array<std::uint32_t, 4> foreignBytes{{10, 20, 30, 40}};
    auto foreignGrant = NativeRegionRegistryTestAccess::issue(foreignRegistry,
        NativeRegionRegistry::Trust::Trusted, owner, 9, 1,
        foreignBytes.data(), sizeof(foreignBytes),
        TrustedPointerWindow::Mode::ReadOnly);
    if (foreignGrant.slot != grant.slot ||
        foreignGrant.provenance != grant.provenance ||
        foreignGrant.registry_id == grant.registry_id)
        throw std::runtime_error("cross-registry replay fixture invalid");
    reject([&] { (void)foreignRegistry.at<std::uint32_t>(grant, owner); });
    reject([&] { (void)registry.at<std::uint32_t>(foreignGrant, owner); });
    reject([&] { foreignRegistry.revoke(grant); });
    foreignRegistry.revoke(foreignGrant);
    auto p = registry.at<std::uint32_t>(grant, owner);
    if (p.offset(owner, 2).load(owner) != 30) throw std::runtime_error("bad pointer");
    reject([&] { (void)registry.at<std::uint32_t>(grant, other); });
    reject([&] { (void)registry.at<std::uint32_t>(grant, stale); });
    auto forged = grant;
    ++forged.provenance.generation;
    reject([&] { (void)registry.at<std::uint32_t>(forged, owner); });
    reject([&] { registry.revoke(forged); });
    registry.revoke(grant);
    reject([&] { (void)p.load(owner); });
    reject([&] { (void)registry.at<std::uint32_t>(grant, owner); });
    reject([&] { registry.revoke(grant); });
    auto second = issue(NativeRegionRegistry::Trust::Trusted, owner, 9, 1,
        bytes.data(), sizeof(bytes), TrustedPointerWindow::Mode::ReadOnly);
    if (second.slot == grant.slot || second.provenance.generation == grant.provenance.generation)
        throw std::runtime_error("grant identity reused");
    registry.revoke(second);

    // Self-revocation must not release the allocation while an FFI callback
    // still holds its raw T*. The callback may request revocation, but the
    // caller must retry after the active foreign scope has unwound.
    auto ffiGrant = issue(NativeRegionRegistry::Trust::Trusted, owner, 9, 1,
        bytes.data(), sizeof(bytes), TrustedPointerWindow::Mode::ReadWrite);
    auto ffiPointer = registry.at<std::uint32_t>(ffiGrant, owner);
    const int callbackResult = ffiPointer.with_ffi_address(owner, [&](std::uint32_t* raw) {
        reject([&] { registry.revoke(ffiGrant); });
        if (*raw != 10) throw std::runtime_error("FFI address released inside callback");
        reject([&] { (void)registry.at<std::uint32_t>(ffiGrant, other); });
        return 7;
    });
    if (callbackResult != 7) throw std::runtime_error("FFI callback not completed");
    // Deferred revocation takes effect at callback return, before the retry
    // completes the registry cleanup.
    reject([&] { (void)ffiPointer.load(owner); });
    registry.revoke(ffiGrant);
    reject([&] { registry.revoke(ffiGrant); });

    // Exercise concurrent registry lookup against revocation. Both outcomes
    // are permitted for a racing lookup, but no access may succeed after
    // revoke() has completed.
    auto racingGrant = issue(NativeRegionRegistry::Trust::Trusted, owner, 10, 2,
        bytes.data(), sizeof(bytes), TrustedPointerWindow::Mode::ReadOnly);
    auto racingPointer = registry.at<std::uint32_t>(racingGrant, owner);
    std::atomic<bool> go{false}, ready{false}, stopped{false};
    std::thread reader([&] {
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        // Establish a real successful access before initiating revocation.
        (void)registry.at<std::uint32_t>(racingGrant, owner).load(owner);
        ready.store(true, std::memory_order_release);
        while (!stopped.load(std::memory_order_acquire)) {
            try {
                (void)registry.at<std::uint32_t>(racingGrant, owner).load(owner);
                (void)racingPointer.load(owner);
            } catch (const std::logic_error&) {
                break;
            }
        }
    });
    go.store(true, std::memory_order_release);
    while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
    registry.revoke(racingGrant);
    stopped.store(true, std::memory_order_release);
    reader.join();
    reject([&] { (void)registry.at<std::uint32_t>(racingGrant, owner); });
    reject([&] { (void)racingPointer.load(owner); });
}
