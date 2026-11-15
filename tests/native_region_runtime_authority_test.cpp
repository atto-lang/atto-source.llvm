#define ORESLANG_NATIVE_GRANT_TESTING 1
#include "oreslang/runtime/NativeRegionRuntimeAuthority.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>

namespace oreslang::runtime {
struct NativeRegionAuthorityTestAccess {
    static std::unique_ptr<NativeRegionRuntimeAuthority> create(
            NativeRegionRegistry& registry, std::uint64_t runtime_id,
            std::size_t maximum_incarnations = 65536) {
        return std::unique_ptr<NativeRegionRuntimeAuthority>(
            new NativeRegionRuntimeAuthority(registry, runtime_id, maximum_incarnations));
    }
    static void admit(NativeRegionRuntimeAuthority& authority,
                      ActorHeapIdentity actor,
                      NativeRegionRuntimeAuthority::Admission admission) {
        authority.admit_actor(actor, admission);
    }
    static NativeRegionRegistry::Grant issue(NativeRegionRuntimeAuthority& authority,
                ActorHeapIdentity actor, std::uint64_t region,
                std::uint64_t allocation, void* base, std::size_t bytes,
                TrustedPointerWindow::Mode mode) {
        return authority.issue_region(actor, region, allocation, base, bytes, mode);
    }
    static void retire(NativeRegionRuntimeAuthority& authority, ActorHeapIdentity actor) {
        authority.retire_actor(actor);
    }
};
}
using namespace oreslang::runtime;

template<class F> static void require_denied(F&& f) {
    bool denied = false;
    try { f(); } catch (const std::logic_error&) { denied = true; }
    if (!denied) throw std::runtime_error("native broker granted forged/stale authority");
}

int main() {
    NativeRegionRegistry registry;
    using Harness = NativeRegionAuthorityTestAccess;
    using Admission = NativeRegionRuntimeAuthority::Admission;
    using Mode = TrustedPointerWindow::Mode;
    auto broker = Harness::create(registry, 29);
    const ActorHeapIdentity trusted{29, 0, 7}; // actor zero is valid
    const ActorHeapIdentity untrusted{29, 2, 7};
    const ActorHeapIdentity sibling{29, 3, 7};
    const ActorHeapIdentity wrong_runtime{30, 0, 7};
    const ActorHeapIdentity next_epoch{29, 0, 8};
    std::array<std::uint32_t, 2> memory{{11, 22}};
    std::uint32_t other_memory = 33;
    Harness::admit(*broker, trusted, Admission::TrustedNative);
    Harness::admit(*broker, untrusted, Admission::Untrusted);
    Harness::admit(*broker, sibling, Admission::TrustedNative);
    require_denied([&] { Harness::admit(*broker, untrusted, Admission::TrustedNative); });
    require_denied([&] { Harness::admit(*broker, trusted, Admission::TrustedNative); });
    require_denied([&] { Harness::admit(*broker, wrong_runtime, Admission::TrustedNative); });
    require_denied([&] { (void)Harness::issue(*broker, untrusted, 4, 1,
        memory.data(), sizeof(memory), Mode::ReadOnly); });
    require_denied([&] { (void)Harness::issue(*broker, wrong_runtime, 4, 1,
        memory.data(), sizeof(memory), Mode::ReadOnly); });
    const auto grant = Harness::issue(*broker, trusted, 5, 1,
        memory.data(), sizeof(memory), Mode::ReadWrite);
    const auto sibling_grant = Harness::issue(*broker, sibling, 6, 1,
        &other_memory, sizeof(other_memory), Mode::ReadOnly);
    auto cursor = registry.at<std::uint32_t>(grant, trusted);
    if (cursor.load(trusted) != 11)
        throw std::runtime_error("native trusted broker read failed");
    cursor.offset(trusted, 1).store(trusted, 42);
    if (memory[1] != 42)
        throw std::runtime_error("native trusted broker write failed");
    require_denied([&] { (void)registry.at<std::uint32_t>(grant, untrusted); });
    Harness::retire(*broker, trusted);
    Harness::retire(*broker, trusted); // idempotent cleanup for supervisor retries
    require_denied([&] { (void)cursor.load(trusted); });
    require_denied([&] { (void)registry.at<std::uint32_t>(grant, trusted); });
    require_denied([&] { (void)Harness::issue(*broker, trusted, 7, 1,
        memory.data(), sizeof(memory), Mode::ReadOnly); });
    require_denied([&] { Harness::admit(*broker, trusted, Admission::TrustedNative); });
    if (registry.at<std::uint32_t>(sibling_grant, sibling).load(sibling) != 33)
        throw std::runtime_error("trusted sibling revoked by unrelated retirement");
    Harness::admit(*broker, next_epoch, Admission::TrustedNative);
    const auto restarted = Harness::issue(*broker, next_epoch, 7, 1,
        memory.data(), sizeof(memory), Mode::ReadOnly);
    if (registry.at<std::uint32_t>(restarted, next_epoch).load(next_epoch) != 11)
        throw std::runtime_error("new actor incarnation denied fresh grant");
    Harness::retire(*broker, next_epoch);
    Harness::retire(*broker, sibling);
    Harness::retire(*broker, untrusted);

    // In-callback retirement is terminal for admissions, but may not return
    // quiesced while raw foreign code still holds the backing address.
    const ActorHeapIdentity active{29, 7, 7};
    std::uint32_t ffi_memory = 77;
    Harness::admit(*broker, active, Admission::TrustedNative);
    const auto active_grant = Harness::issue(*broker, active, 8, 1,
        &ffi_memory, sizeof(ffi_memory), Mode::ReadWrite);
    auto ffi = registry.at<std::uint32_t>(active_grant, active);
    bool deferred = false;
    ffi.with_ffi_address(active, [&](std::uint32_t* ptr) {
        try { Harness::retire(*broker, active); }
        catch (const std::logic_error&) { deferred = true; }
        if (!deferred || *ptr != 77)
            throw std::runtime_error("FFI callback quiescence failed");
        require_denied([&] { (void)Harness::issue(*broker, active, 9, 1,
            &ffi_memory, sizeof(ffi_memory), Mode::ReadOnly); });
    });
    if (!deferred) throw std::runtime_error("active FFI revoke was not deferred");
    Harness::retire(*broker, active);
    require_denied([&] { (void)ffi.load(active); });
    require_denied([&] { (void)registry.at<std::uint32_t>(active_grant, active); });

    // Invalid enum values are never reinterpreted as trusted admission.
    const ActorHeapIdentity bogus{29, 8, 7};
    require_denied([&] { Harness::admit(*broker, bogus, static_cast<Admission>(255)); });
    // An attacker cannot force unlimited actor epoch tombstone growth.
    auto limited = Harness::create(registry, 29, 1);
    const ActorHeapIdentity limited_actor{29, 90, 1};
    Harness::admit(*limited, limited_actor, Admission::Untrusted);
    Harness::retire(*limited, limited_actor);
    bool denied_budget = false;
    try { Harness::admit(*limited, ActorHeapIdentity{29, 90, 2}, Admission::Untrusted); }
    catch (const std::length_error&) { denied_budget = true; }
    if (!denied_budget) throw std::runtime_error("native broker exceeded actor tombstone budget");

}
