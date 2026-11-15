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
    std::atomic<bool> actor_mapping_denied{false};
};

extern "C" void actor_callback(void* opaque, size_t slot, std::uint64_t) {
    auto& ctx = *static_cast<Context*>(opaque);
    try {
        if (slot == 0) {
            const auto ref = ctx.refs[0];
            // An actor callback may request a registered grant but may not
            // register arbitrary host addresses into the supervisor catalog.
            bool actor_registration_rejected = false;
            try {
                ctx.bridge->register_mapped_region(
                    ref, 105, 8, &ctx.untrusted_backing,
                    sizeof(ctx.untrusted_backing),
                    NativeActorSupervisor::Mode::ReadOnly);
            } catch (const std::logic_error&) {
                actor_registration_rejected = true;
            }
            if (!actor_registration_rejected)
                throw std::runtime_error("actor turn registered native RAM");
            ctx.actor_mapping_denied.store(true, std::memory_order_release);
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

    // Distinct IDs must never authorize aliasing the same backing bytes.
    bool overlap_denied = false;
    try {
        bridge.register_mapped_region(
            ctx.refs[0], 103, 8, ctx.trusted_backing.data(),
            sizeof(std::uint32_t), NativeActorSupervisor::Mode::ReadOnly);
    } catch (const std::logic_error&) { overlap_denied = true; }
    if (!overlap_denied) throw std::runtime_error("overlapping native mapping admitted");
    bool mmio_denied = false;
    try {
        bridge.register_mapped_region(
            ctx.refs[0], 104, 8, &ctx.untrusted_backing,
            sizeof(ctx.untrusted_backing),
            NativeActorSupervisor::Mode::MmioReadWrite);
    } catch (const std::logic_error&) { mmio_denied = true; }
    if (!mmio_denied) throw std::runtime_error("unapproved MMIO mapping admitted");

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
        !ctx.actor_mapping_denied.load(std::memory_order_acquire) ||
        ctx.trusted_backing[1] != 99)
        throw std::runtime_error("native pointer scheduler actor test failed");

    // A forged domain may not revoke a correctly admitted actor.
    denied = false;
    try { bridge.stop_and_revoke(forged); }
    catch (const std::logic_error&) { denied = true; }
    if (!denied) throw std::runtime_error("forged native actor retirement admitted");

    // Race an ordinary supervisor registration with STOP. Either the region
    // wins before STOP and gets purged after quiescence, or STOP wins and
    // registration is rejected. It may never survive as an admitted grant.
    std::uint32_t concurrent_backing = 123;
    std::atomic<bool> start_race{false};
    std::atomic<bool> unexpected_registration_error{false};
    std::thread registration_racer([&] {
        while (!start_race.load(std::memory_order_acquire))
            std::this_thread::yield();
        try {
            bridge.register_mapped_region(
                ctx.refs[0], 111, 8, &concurrent_backing,
                sizeof(concurrent_backing), NativeActorSupervisor::Mode::ReadOnly);
        } catch (const std::logic_error&) {
            // Expected if STOP linearizes before the registration.
        } catch (...) {
            unexpected_registration_error.store(true, std::memory_order_release);
        }
    });
    start_race.store(true, std::memory_order_release);
    // Trusted actor holds a real previously issued window. Stop directly
    // through the scheduler, bypassing the explicit pointer adapter.
    if (!ores_native_actor_scheduler_stop_sealed(scheduler, ctx.refs[0]))
        throw std::runtime_error("trusted scheduler external STOP failed");
    registration_racer.join();
    // STOP must NOT publish terminal DONE while a supervisor-approved
    // native RAM mapping still owns its scheduler resource pin.
    if (ores_native_actor_scheduler_poll_done(scheduler, ctx.refs[0]))
        throw std::runtime_error("actor DONE published before pointer fence");
    if (bridge.fence_stopped_actors() != 1)
        throw std::runtime_error("trusted external STOP was not fenced");
    if (bridge.fence_stopped_actors() != 0)
        throw std::runtime_error("trusted stopped actor fenced twice");
    // The fence releases the exact-generation external-holder loan. Scheduler
    // termination may now complete, but not one instruction earlier.
    if (!ores_native_actor_scheduler_wait_done(scheduler, ctx.refs[0]))
        throw std::runtime_error("actor DONE blocked after pointer fence release");
    // The explicit supervisor path is still safe to retry after maintenance.
    bridge.stop_and_revoke(ctx.refs[0]);
    if (unexpected_registration_error.load(std::memory_order_acquire))
        throw std::runtime_error("native pointer registration/STOP race failed");
    // Supervisor stop is terminal authority revocation. Even the privileged
    // host thread cannot register fresh windows after STOP seals admission.
    denied = false;
    try {
        bridge.register_mapped_region(
            ctx.refs[0], 110, 10, &ctx.untrusted_backing,
            sizeof(ctx.untrusted_backing), NativeActorSupervisor::Mode::ReadOnly);
    } catch (const std::logic_error&) { denied = true; }
    if (!denied)
        throw std::runtime_error("retired actor accepted a new native region");
    // Untrusted actors also receive terminal bookkeeping, but never owned
    // any native pointer grant.
    bridge.stop_and_revoke(ctx.refs[1]);
    if (bridge.fence_stopped_actors() != 0)
        throw std::runtime_error("quiesced actors re-fenced");
    if (!ores_native_actor_scheduler_wait_done(scheduler, ctx.refs[0]) ||
        !ores_native_actor_scheduler_wait_done(scheduler, ctx.refs[1]))
        throw std::runtime_error("native actor termination did not quiesce");

    // Terminal observations can be safely retried, but may never reauthorize.
    bridge.revoke_terminal(ctx.refs[0]);
    bridge.revoke_terminal(ctx.refs[1]);
    // Scheduler destruction requires an explicit global retirement after
    // all actor lanes and their native pointer grants have quiesced.
    ores_native_actor_scheduler_retire(scheduler);
    if (!ores_native_actor_scheduler_try_destroy(scheduler))
        throw std::runtime_error("native actor scheduler teardown failed");

    // A runtime-wide retirement initiated outside the pointer adapter must
    // ALSO be unable to publish terminal DONE while mapped RAM is pinned.
    // The external supervisor maintenance fence clears the exact generation.
    OresNativeActorScheduler* shutdown_scheduler =
        ores_native_actor_scheduler_open(1, 2, 1, actor_callback, &ctx);
    if (!shutdown_scheduler)
        throw std::runtime_error("global-retirement scheduler open failed");
    {
        NativeActorSupervisor shutdown_bridge(shutdown_scheduler, 46);
        const auto shutdown_actor =
            ores_native_actor_scheduler_spawn(shutdown_scheduler, 0);
        if (!shutdown_actor.scheduler_domain)
            throw std::runtime_error("global-retirement actor spawn failed");
        shutdown_bridge.admit_spawned(
            shutdown_actor, NativeActorSupervisor::Admission::TrustedNative);
        std::uint32_t shutdown_backing = 47;
        shutdown_bridge.register_mapped_region(
            shutdown_actor, 301, 1, &shutdown_backing,
            sizeof(shutdown_backing), NativeActorSupervisor::Mode::ReadOnly);
        ores_native_actor_scheduler_retire(shutdown_scheduler);
        if (ores_native_actor_scheduler_poll_done(shutdown_scheduler, shutdown_actor))
            throw std::runtime_error(
                "global-retirement DONE published before pointer loan drain");
        if (shutdown_bridge.fence_stopped_actors() != 1)
            throw std::runtime_error(
                "global-retirement pointer mapping not revoked");
        if (!ores_native_actor_scheduler_wait_done(shutdown_scheduler, shutdown_actor))
            throw std::runtime_error(
                "global-retirement completion not published after pointer fence");
        if (!ores_native_actor_scheduler_try_destroy(shutdown_scheduler))
            throw std::runtime_error("global-retirement scheduler teardown failed");
    }
}
