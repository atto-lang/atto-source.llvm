#include "oreslang/runtime/SharedDomainSchedulerBridge.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

using oreslang::runtime::SharedDomainArena;
using oreslang::runtime::SharedDomainReadWriteGate;
using oreslang::runtime::SharedDomainSchedulerBridge;

template <class T, class = void>
struct has_public_domain_id : std::false_type {};
template <class T>
struct has_public_domain_id<T,
    std::void_t<decltype(std::declval<const T&>().domain_id())>> : std::true_type {};
static_assert(!has_public_domain_id<SharedDomainSchedulerBridge::Permit>::value,
    "shared-domain permit must not expose numeric domain coordinates");

static void require_at(bool value, int line) {
    if (!value)
        throw std::runtime_error(
            "shared-domain scheduler bridge test failed at line " + std::to_string(line));
}
#define require(value) require_at((value), __LINE__)
template <class F>
static void reject(F&& fn) {
    bool denied = false;
    try { fn(); } catch (const std::exception&) { denied = true; }
    require(denied);
}

static void gate_fairness_regression() {
    SharedDomainReadWriteGate gate;
    const OresNativeActorIdentity reader1{11, 1};
    const OresNativeActorIdentity writer{12, 1};
    const OresNativeActorIdentity reader2{13, 1};

    const auto r1 = gate.request(reader1, SharedDomainReadWriteGate::Mode::Read);
    const auto w = gate.request(writer, SharedDomainReadWriteGate::Mode::Write);
    const auto r2 = gate.request(reader2, SharedDomainReadWriteGate::Mode::Read);
    require(r1.admission == SharedDomainReadWriteGate::Admission::Acquired);
    require(w.admission == SharedDomainReadWriteGate::Admission::Queued);
    require(r2.admission == SharedDomainReadWriteGate::Admission::Queued);

    auto ready = gate.release(r1.token, reader1);
    require(ready.size() == 1);
    require(ready[0].token == w.token);
    require(ready[0].mode == SharedDomainReadWriteGate::Mode::Write);
    require(gate.try_claim(w.token, writer, SharedDomainReadWriteGate::Mode::Write));
    require(!gate.try_claim(w.token, writer, SharedDomainReadWriteGate::Mode::Write));

    ready = gate.release(w.token, writer);
    require(ready.size() == 1);
    require(ready[0].token == r2.token);
    require(ready[0].mode == SharedDomainReadWriteGate::Mode::Read);
    require(gate.try_claim(r2.token, reader2, SharedDomainReadWriteGate::Mode::Read));
    require(gate.release(r2.token, reader2).empty());
}

struct Context {
    SharedDomainSchedulerBridge* bridge = nullptr;
    SharedDomainSchedulerBridge* creation_bridge = nullptr;
    SharedDomainArena::Control* creation_control = nullptr;
    OresNativeActorScheduler* scheduler = nullptr;
    std::optional<SharedDomainSchedulerBridge::Permit> writer;
    std::optional<SharedDomainSchedulerBridge::Permit> reader;
    std::optional<SharedDomainSchedulerBridge::Request> reader_request;
    SharedDomainArena::Handle handle{};
    std::atomic<bool> writer_entered{false};
    std::atomic<bool> reader_queued{false};
    std::atomic<bool> reader_resumed{false};
    std::atomic<bool> wrong_permit_denied{false};
    std::atomic<bool> fake_wake_denied{false};
    std::atomic<bool> replayed_wake_denied{false};
    std::atomic<bool> completion_started{false};
    std::atomic<bool> release_completion{false};
    std::atomic<bool> completion_released{false};
    std::atomic<bool> current_ref_resolved{false};
    std::atomic<bool> current_member_granted{false};
    std::atomic<std::uint64_t> observed{0};
    std::thread completion;
};

static void on_message(void* raw, std::size_t actor, std::uint64_t payload) {
    auto& ctx = *static_cast<Context*>(raw);
    require(ctx.bridge != nullptr);
    if (actor == 0 && payload == 1) {
        const OresNativeActorCapabilityWake fake{1, 1, ORES_NATIVE_CAPABILITY_READY};
        ctx.fake_wake_denied.store(
            !ores_native_actor_scheduler_is_current_capability_wake(ctx.scheduler, fake),
            std::memory_order_release);

        reject([&] {
            (void)ctx.bridge->request(
                *ctx.reader, SharedDomainSchedulerBridge::Mode::Read);
        });
        ctx.wrong_permit_denied.store(true, std::memory_order_release);

        const auto request = ctx.bridge->request(
            *ctx.writer, SharedDomainSchedulerBridge::Mode::Write);
        require(request.acquired());
        {
            auto access = ctx.bridge->enter(*ctx.writer, request);
            ctx.handle = access.allocate(sizeof(std::uint64_t));
            const std::uint64_t value = 0x1122334455667788ULL;
            access.write(ctx.handle, &value, sizeof(value));
            ctx.writer_entered.store(true, std::memory_order_release);
            while (!ctx.reader_queued.load(std::memory_order_acquire))
                std::this_thread::yield();
        }
        return;
    }
    if (actor == 1 && payload == 1) {
        const auto request = ctx.bridge->request(
            *ctx.reader, SharedDomainSchedulerBridge::Mode::Read);
        require(!request.acquired());
        ctx.reader_request = request;
        ctx.reader_queued.store(true, std::memory_order_release);
        return;
    }
    if (actor == 0 && payload == 2) {
        ctx.bridge->track_current_completion(
            *ctx.writer,
            [&](auto cleanup) {
                ctx.completion = std::thread(
                    [&ctx, owned = std::move(cleanup)]() mutable {
                        ctx.completion_started.store(true, std::memory_order_release);
                        while (!ctx.release_completion.load(std::memory_order_acquire))
                            std::this_thread::yield();
                        owned();
                        ctx.completion_released.store(true, std::memory_order_release);
                    });
            });
        return;
    }
    if (actor == 0 && payload == 3) {
        require(ctx.creation_bridge != nullptr);
        require(ctx.creation_control != nullptr);
        OresNativeActorRef current{0, 0, 0};
        require(ores_native_actor_scheduler_current_actor_ref(
            ctx.scheduler, &current));
        require(current.scheduler_domain != 0);
        require(current.actor_slot == actor);
        require(current.generation != 0);
        ctx.current_ref_resolved.store(true, std::memory_order_release);

        auto permit = ctx.creation_bridge->grant_current_member(
            *ctx.creation_control, true, true);
        const auto request = ctx.creation_bridge->request(
            permit, SharedDomainSchedulerBridge::Mode::Write);
        require(request.acquired());
        {
            auto access = ctx.creation_bridge->enter(permit, request);
            const auto handle = access.allocate(sizeof(std::uint64_t));
            const std::uint64_t expected = 0xA5A55A5AF0F00F0FULL;
            access.write(handle, &expected, sizeof(expected));
            std::uint64_t observed = 0;
            access.read(handle, &observed, sizeof(observed));
            require(observed == expected);
            access.drop(handle);
        }
        ctx.current_member_granted.store(true, std::memory_order_release);
        return;
    }
    throw std::runtime_error("unexpected scheduler DATA callback");
}

static void on_capability(
        void* raw, std::size_t actor, OresNativeActorCapabilityWake wake) {
    auto& ctx = *static_cast<Context*>(raw);
    require(actor == 1);
    require(ctx.reader_request.has_value());
    {
        auto access = ctx.bridge->resume(
            *ctx.reader, wake, SharedDomainSchedulerBridge::Mode::Read);
        std::uint64_t value = 0;
        access.read(ctx.handle, &value, sizeof(value));
        ctx.observed.store(value, std::memory_order_release);
    }
    reject([&] {
        (void)ctx.bridge->resume(
            *ctx.reader, wake, SharedDomainSchedulerBridge::Mode::Read);
    });
    ctx.replayed_wake_denied.store(true, std::memory_order_release);
    ctx.reader_resumed.store(true, std::memory_order_release);
}

static void wait_for(const std::atomic<bool>& flag, const char* label) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
        if (flag.load(std::memory_order_acquire)) return;
        std::this_thread::yield();
    }
    throw std::runtime_error(std::string("timed out waiting for ") + label);
}

int main() {
    gate_fairness_regression();

    SharedDomainArena arena;
    auto control = arena.take_control();
    Context ctx;

    auto* scheduler = ores_native_actor_scheduler_open_capability_wakes(
        2, 8, 2, on_message, nullptr, nullptr, on_capability, &ctx);
    require(scheduler != nullptr);
    ctx.scheduler = scheduler;
    const auto writer_ref = ores_native_actor_scheduler_spawn(scheduler, 0);
    const auto reader_ref = ores_native_actor_scheduler_spawn(scheduler, 1);
    require(writer_ref.scheduler_domain && reader_ref.scheduler_domain);

    OresNativeActorIdentity writer_identity{0, 0};
    OresNativeActorIdentity reader_identity{0, 0};
    require(ores_native_actor_scheduler_resolve_actor_identity(
        scheduler, writer_ref, &writer_identity));
    require(ores_native_actor_scheduler_resolve_actor_identity(
        scheduler, reader_ref, &reader_identity));
    require(writer_identity.principal != 0 && reader_identity.principal != 0);
    require(writer_identity.principal != reader_identity.principal);
    OresNativeActorIdentity outside{123, 456};
    require(!ores_native_actor_scheduler_current_actor_identity(
        scheduler, &outside));
    require(outside.principal == 0 && outside.incarnation == 0);
    OresNativeActorRef outside_ref{123, 456, 789};
    require(!ores_native_actor_scheduler_current_actor_ref(
        scheduler, &outside_ref));
    require(outside_ref.scheduler_domain == 0 &&
            outside_ref.actor_slot == 0 && outside_ref.generation == 0);

    SharedDomainSchedulerBridge bridge(arena, scheduler);
    ctx.bridge = &bridge;
    ctx.writer = bridge.grant_member(control, writer_ref, true, true);
    ctx.reader = bridge.grant_member(control, reader_ref, true, false);

    SharedDomainArena creation_arena;
    auto creation_control = creation_arena.take_control();
    SharedDomainSchedulerBridge creation_bridge(creation_arena, scheduler);
    ctx.creation_bridge = &creation_bridge;
    ctx.creation_control = &creation_control;
    reject([&] {
        (void)creation_bridge.grant_current_member(
            creation_control, true, true);
    });

    reject([&] {
        (void)bridge.request(*ctx.writer, SharedDomainSchedulerBridge::Mode::Write);
    });

    require(ores_native_actor_scheduler_send(scheduler, writer_ref, 1));
    wait_for(ctx.writer_entered, "writer entry");
    require(ores_native_actor_scheduler_send(scheduler, reader_ref, 1));
    wait_for(ctx.reader_queued, "reader queue");
    wait_for(ctx.reader_resumed, "reader resume");
    require(ctx.wrong_permit_denied.load(std::memory_order_acquire));
    require(ctx.fake_wake_denied.load(std::memory_order_acquire));
    require(ctx.replayed_wake_denied.load(std::memory_order_acquire));
    require(ctx.observed.load(std::memory_order_acquire)
        == 0x1122334455667788ULL);
    require(ores_native_actor_scheduler_capability_wakes_completed(scheduler) == 1);

    /* Automatic completion attachment owns a lifetime-only pin after the
     * actor turn exits. Logical retirement seals new byte authority but
     * physical reclaim remains blocked until the completion carrier releases. */
    require(ores_native_actor_scheduler_send(scheduler, writer_ref, 2));
    wait_for(ctx.completion_started, "completion registration");
    require(!arena.try_retire(control));
    reject([&] {
        (void)bridge.request(*ctx.writer, SharedDomainSchedulerBridge::Mode::Read);
    });
    ctx.release_completion.store(true, std::memory_order_release);
    wait_for(ctx.completion_released, "completion release");
    ctx.completion.join();
    require(arena.try_retire(control));
    require(arena.try_retire(control));

    require(ores_native_actor_scheduler_send(scheduler, writer_ref, 3));
    wait_for(ctx.current_ref_resolved, "current ActorRef resolution");
    wait_for(ctx.current_member_granted, "current actor membership grant");
    require(creation_arena.try_retire(creation_control));
    require(creation_arena.try_retire(creation_control));

    ores_native_actor_scheduler_retire(scheduler);
    require(ores_native_actor_scheduler_drain(scheduler));
    require(ores_native_actor_scheduler_try_destroy(scheduler));

    std::cout << "shared-domain scheduler bridge tests passed\n";
}
