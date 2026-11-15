// Actual scheduler-backed HOST actor admission and revocation. Not .atto guest
// lowering, native-image AOT, or an untrusted-process isolation proof.
#include "oreslang/runtime/NativeActorSupervisor.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <thread>

using oreslang::runtime::NativeActorSupervisor;

struct Context {
    NativeActorSupervisor* bridge = nullptr;
    OresNativeActorRef refs[2]{};
    std::array<std::uint32_t, 2> trusted_backing{{11, 22}};
    std::uint32_t untrusted_backing = 88;
    std::atomic<unsigned> finished{0};
    std::atomic<bool> failed{false};
    std::atomic<bool> foreign_denied{false};
    std::atomic<bool> untrusted_denied{false};
};

extern "C" void actor_callback(void* opaque, size_t slot, std::uint64_t) {
    auto& ctx = *static_cast<Context*>(opaque);
    try {
        if (slot == 0) {
            const auto ref = ctx.refs[0];
            auto grant = ctx.bridge->issue_current_region(ref, 101, 7);
            if (ctx.bridge->read_current<std::uint32_t>(ref, grant) != 11)
                throw std::runtime_error("trusted actor pointer load failed");
            ctx.bridge->write_current<std::uint32_t>(
                ref, grant, 99, sizeof(std::uint32_t));
            if (ctx.bridge->read_current<std::uint32_t>(
                    ref, grant, sizeof(std::uint32_t)) != 99)
                throw std::runtime_error("trusted actor pointer store failed");
            // Even a trusted turn cannot impersonate another actor's ref.
            auto forged = ref;
            forged.actor_slot = ctx.refs[1].actor_slot;
            bool denied = false;
            try {
                (void)ctx.bridge->read_current<std::uint32_t>(forged, grant);
            } catch (const std::logic_error&) { denied = true; }
            if (!denied) throw std::runtime_error("foreign actor pointer succeeded");
            // No raw base/address parameter exists at this callsite, and
            // a made-up region or allocation ID cannot mint a grant.
            bool unknown_region_denied = false;
            try { (void)ctx.bridge->issue_current_region(ref, 101, 999); }
            catch (const std::logic_error&) { unknown_region_denied = true; }
            if (!unknown_region_denied)
                throw std::runtime_error("unregistered pointer region admitted");
            ctx.foreign_denied.store(true, std::memory_order_release);
        } else if (slot == 1) {
            bool denied = false;
            try {
                (void)ctx.bridge->issue_current_region(ctx.refs[1], 102, 7);
            } catch (const std::logic_error&) { denied = true; }
            if (!denied) throw std::runtime_error("untrusted native pointer succeeded");
            ctx.untrusted_denied.store(true, std::memory_order_release);
        } else throw std::runtime_error("unexpected actor slot");
    } catch (...) {
        ctx.failed.store(true, std::memory_order_release);
    }
    ctx.finished.fetch_add(1, std::memory_order_release);
}

int main() {
    Context ctx;
    OresNativeActorScheduler* scheduler =
        ores_native_actor_scheduler_open(2, 4, 2, actor_callback, &ctx);
    if (!scheduler) throw std::runtime_error("native scheduler open failed");
    NativeActorSupervisor bridge(scheduler, 45);
    ctx.bridge = &bridge;
    ctx.refs[0] = ores_native_actor_scheduler_spawn(scheduler, 0);
    ctx.refs[1] = ores_native_actor_scheduler_spawn(scheduler, 1);
    if (!ctx.refs[0].scheduler_domain || !ctx.refs[1].scheduler_domain)
        throw std::runtime_error("native scheduler spawn failed");

    auto forged = ctx.refs[0];
    forged.scheduler_domain ^= 1;
    bool denied = false;
    try {
        bridge.admit_spawned(forged, NativeActorSupervisor::Admission::TrustedNative);
    } catch (const std::logic_error&) { denied = true; }
    if (!denied) throw std::runtime_error("forged scheduler ref admitted");
    bridge.admit_spawned(ctx.refs[0], NativeActorSupervisor::Admission::TrustedNative);
    bridge.admit_spawned(ctx.refs[1], NativeActorSupervisor::Admission::Untrusted);
    bridge.register_mapped_region(
        ctx.refs[0], 101, 7, ctx.trusted_backing.data(),
        sizeof(ctx.trusted_backing), NativeActorSupervisor::Mode::ReadWrite);
    // Duplicate registration and untrusted mappings are rejected before
    // any actor callback can request a native grant.
    bool duplicate_denied = false;
    try {
        bridge.register_mapped_region(
            ctx.refs[0], 101, 7, ctx.trusted_backing.data(),
            sizeof(ctx.trusted_backing), NativeActorSupervisor::Mode::ReadWrite);
    } catch (const std::logic_error&) { duplicate_denied = true; }
    if (!duplicate_denied)
        throw std::runtime_error("native mapped region duplicate admitted");
    bool untrusted_mapping_denied = false;
    try {
        bridge.register_mapped_region(
            ctx.refs[1], 102, 7, &ctx.untrusted_backing,
            sizeof(ctx.untrusted_backing), NativeActorSupervisor::Mode::ReadWrite);
    } catch (const std::logic_error&) { untrusted_mapping_denied = true; }
    if (!untrusted_mapping_denied)
        throw std::runtime_error("untrusted actor mapped region admitted");

    // External host threads lack the exact actor turn; trust alone is never
    // permission to issue a raw pointer.
    denied = false;
    try {
        (void)bridge.issue_current_region(ctx.refs[0], 101, 7);
    } catch (const std::logic_error&) { denied = true; }
    if (!denied) throw std::runtime_error("outside-turn pointer issue admitted");

    if (ores_native_actor_scheduler_try_send(
            scheduler, ctx.refs[0], 1) != ORES_NATIVE_SEND_ACCEPTED ||
        ores_native_actor_scheduler_try_send(
            scheduler, ctx.refs[1], 1) != ORES_NATIVE_SEND_ACCEPTED)
        throw std::runtime_error("native scheduler send failed");

    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::seconds(10);
    while (ctx.finished.load(std::memory_order_acquire) < 2 &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    if (ctx.finished.load(std::memory_order_acquire) != 2 ||
        ctx.failed.load(std::memory_order_acquire) ||
        !ctx.foreign_denied.load(std::memory_order_acquire) ||
        !ctx.untrusted_denied.load(std::memory_order_acquire) ||
        ctx.trusted_backing[1] != 99)
        throw std::runtime_error("native pointer scheduler actor test failed");

    // A forged domain may not revoke a correctly admitted actor.
    denied = false;
    try { bridge.stop_and_revoke(forged); }
    catch (const std::logic_error&) { denied = true; }
    if (!denied) throw std::runtime_error("forged native actor retirement admitted");

    bridge.stop_and_revoke(ctx.refs[0]);
    bridge.stop_and_revoke(ctx.refs[1]);
    if (!ores_native_actor_scheduler_wait_done(scheduler, ctx.refs[0]) ||
        !ores_native_actor_scheduler_wait_done(scheduler, ctx.refs[1]))
        throw std::runtime_error("native actor termination did not quiesce");

    // Terminal observations can be safely retried, but may never reauthorize.
    bridge.revoke_terminal(ctx.refs[0]);
    bridge.revoke_terminal(ctx.refs[1]);
    if (!ores_native_actor_scheduler_try_destroy(scheduler))
        throw std::runtime_error("native actor scheduler teardown failed");
}
