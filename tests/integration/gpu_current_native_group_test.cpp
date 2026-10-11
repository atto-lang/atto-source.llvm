#include "oreslang/runtime/ActorGroupLifecycle.hpp" // authoritative current-main NativeActorGroup
#include "oreslang/runtime/GpuActorControlBridge.hpp"

#include <atomic>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <exception>
#include <functional>
#include <memory>

using Group = oreslang::runtime::NativeActorGroup;
using Bridge = oreslang::gpu::GpuActorControlBridge<Group>;

struct Provider final : oreslang::gpu::GpuDispatchCoordinator::Provider {
    using Completion = oreslang::gpu::GpuDispatchCoordinator::Completion;
    Completion event, release_event;
    void launch(const oreslang::dop::OpenClArtifact&,
                const oreslang::gpu::Identity&, oreslang::gpu::Ticket,
                Completion done) override { event = std::move(done); }
    void release(const oreslang::gpu::Identity&, std::uint64_t,
                 Completion done) override { release_event = std::move(done); }
};

static oreslang::dop::OpenClArtifact kernel() {
    using namespace oreslang::dop;
    Plan shape(Layout::AoSoA, 7, 3, 4, 16, 4);
    return lower_opencl(Kernel(shape, Element::F32, Op::Scale, 2, 0),
                        {64, 4096, 100, true});
}
int main() {
    const oreslang::gpu::Identity device{"tenant", "gpu", 17};
    auto group = std::make_shared<Group>(1001, 2);
    auto actor = group->adopt(1001, 9, 1, [] {});
    assert(group->mark_ready(actor));
    auto provider = std::make_shared<Provider>();
    oreslang::gpu::GpuDispatchCoordinator dispatch(provider, {1, 4096});
    auto submission = dispatch.submit(kernel(), device);
    std::atomic<int> actor_calls{0};
    auto receipt = Bridge::attach(submission, group, actor, [&](std::exception_ptr error) {
        assert(!error);
        ++actor_calls;
    });
    provider->event(nullptr);
    provider->release_event(nullptr);
    submission.get_done().get();
    assert(actor_calls == 0); // device callback can enqueue, not execute user code
    assert(receipt->route.load() == Bridge::Route::posted);
    assert(group->has_pending_control(actor));
    assert(!group->finalized(actor)); // queued CONTROL is an active actor root
    assert(group->run_control_turn(actor) == Group::ControlTurn::executed);
    assert(actor_calls == 1);
    assert(group->finalized(actor));
    actor.get_done().get();
    dispatch.close();

    auto crowded = std::make_shared<Group>(1002, 1);
    auto worker = crowded->adopt(1002, 1, 1, [] {});
    assert(crowded->mark_ready(worker));
    assert(crowded->enqueue_control(worker, [] {}));
    auto p2 = std::make_shared<Provider>();
    oreslang::gpu::GpuDispatchCoordinator second(p2, {1, 4096});
    auto rejected_job = second.submit(kernel(), device);
    std::atomic<bool> called{false};
    auto r2 = Bridge::attach(rejected_job, crowded, worker,
                              [&](std::exception_ptr) { called.store(true); });
    p2->event(nullptr);
    p2->release_event(nullptr);
    rejected_job.get_done().get();
    assert(r2->route.load() == Bridge::Route::slot_busy && !called.load());
    assert(crowded->run_control_turn(worker) == Group::ControlTurn::executed);
    assert(!called.load());
    assert(crowded->finalized(worker));
    second.close();

    auto closing = std::make_shared<Group>(1003, 1);
    auto owner = closing->adopt(1003, 1, 1, [] {});
    assert(closing->mark_ready(owner));
    assert(closing->enqueue_control(owner, [] {}));
    closing->close();
    assert(owner.control_delivery_lost()); // host supervisor must reconcile
    assert(closing->finalized(owner));
}
