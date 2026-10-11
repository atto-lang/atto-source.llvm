#include "oreslang/runtime/ActorGroup.hpp" // PR #54, authoritative native registry
#include "oreslang/runtime/GpuActorControlBridge.hpp"

#include <atomic>
#ifdef NDEBUG
#undef NDEBUG // Execute all lifecycle assertions under Release builds
#endif
#include <cassert>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>

using Group = oreslang::runtime::ActorGroup;
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

static oreslang::dop::OpenClArtifact make_kernel() {
    using namespace oreslang::dop;
    Plan p(Layout::AoSoA, 7, 3, 4, 16, 4);
    return lower_opencl(Kernel(p, Element::F32, Op::Scale, 2, 0), {64, 4096, 100, true});
}
int main() {
    const oreslang::gpu::Identity id{"tenant", "gpu-0", 17};
    auto actor_group = std::make_shared<Group>(2);
    auto worker = actor_group->register_spawn([] {});
    assert(actor_group->mark_ready(worker));
    worker.get_ready().get();
    auto provider = std::make_shared<Provider>();
    oreslang::gpu::GpuDispatchCoordinator coord(provider, {1, 4096});
    auto submission = coord.submit(make_kernel(), id);
    std::atomic<int> user_calls{0};
    auto route = Bridge::attach(submission, actor_group, worker,
                               [&](std::exception_ptr failure) {
                                   assert(!failure);
                                   ++user_calls;
                               });
    assert(route->route.load() == Bridge::Route::waiting);
    provider->event(nullptr);
    assert(user_calls == 0);
    provider->release_event(nullptr);
    submission.get_done().get();
    assert(user_calls == 0); // release callback is NOT actor carrier
    assert(route->route.load() == Bridge::Route::posted);
    assert(actor_group->has_pending_control(worker));
    assert(!actor_group->finalized(worker));
    assert(actor_group->run_control_turn(worker) == Group::ControlTurn::executed);
    assert(user_calls == 1);
    assert(route->route.load() == Bridge::Route::actor_executed);
    assert(actor_group->finalized(worker));
    worker.get_done().get();
    coord.close();

    // A saturated reserved CONTROL slot must reject instead of calling user code.
    auto occupied_group = std::make_shared<Group>(1);
    auto occupied = occupied_group->register_spawn([] {});
    assert(occupied_group->mark_ready(occupied));
    assert(occupied_group->enqueue_control(occupied, [] {}));
    auto failed_provider = std::make_shared<Provider>();
    oreslang::gpu::GpuDispatchCoordinator second(failed_provider, {1, 4096});
    auto attempt = second.submit(make_kernel(), id);
    std::atomic<bool> unsafe_called{false};
    auto rejected = Bridge::attach(attempt, occupied_group, occupied,
                                  [&](std::exception_ptr) { unsafe_called.store(true); });
    failed_provider->event(nullptr);
    failed_provider->release_event(nullptr);
    attempt.get_done().get();
    assert(rejected->route.load() == Bridge::Route::slot_busy);
    assert(!unsafe_called.load());
    assert(occupied_group->run_control_turn(occupied) == Group::ControlTurn::executed);
    assert(!unsafe_called.load());
    assert(occupied_group->finalized(occupied));
    second.close();
}
