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
}
