#include "oreslang/runtime/NativeRegionRegistry.hpp"
#include <array>
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
    const ActorHeapIdentity owner{1, 2, 3}, other{1, 4, 3}, stale{1, 2, 2};
    std::array<std::uint32_t, 4> bytes{{10, 20, 30, 40}};
    reject([&] { registry.issue(NativeRegionRegistry::Trust::Untrusted, owner, 9, 1,
        bytes.data(), sizeof(bytes), TrustedPointerWindow::Mode::ReadWrite); });
    auto grant = registry.issue(NativeRegionRegistry::Trust::Trusted, owner, 9, 1,
        bytes.data(), sizeof(bytes), TrustedPointerWindow::Mode::ReadWrite);
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
    auto second = registry.issue(NativeRegionRegistry::Trust::Trusted, owner, 9, 1,
        bytes.data(), sizeof(bytes), TrustedPointerWindow::Mode::ReadOnly);
    if (second.slot == grant.slot || second.provenance.generation == grant.provenance.generation)
        throw std::runtime_error("grant identity reused");
    registry.revoke(second);

    // Self-revocation must not release the allocation while an FFI callback
    // still holds its raw T*. The callback may request revocation, but the
    // caller must retry after the active foreign scope has unwound.
    auto ffiGrant = registry.issue(NativeRegionRegistry::Trust::Trusted, owner, 9, 1,
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
}
