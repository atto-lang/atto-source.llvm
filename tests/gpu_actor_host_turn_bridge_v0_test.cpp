#include "oreslang/runtime/GpuActorHostTurnBridgeV0.hpp"

#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <atomic>
#include <thread>

using oreslang::SupervisorFailureLane;
using oreslang::gpu_actor::HostTurnBridgeV0;

namespace {
int checks = 0;
void check(bool value) {
    if (!value) throw std::runtime_error("GPU_ACTOR_HOST_BRIDGE_ASSERTION_FAILED");
    ++checks;
}
template <typename F>
void denied(F&& action, const char* code) {
    bool rejected = false;
    try { action(); }
    catch (const std::exception& e) {
        rejected = std::string(e.what()).find(code) != std::string::npos;
    }
    check(rejected);
}
std::unique_ptr<HostTurnBridgeV0> actor(
        SupervisorFailureLane& lane, const std::string& id,
        std::uint64_t generation = 1, std::int64_t state = 0) {
    return std::make_unique<HostTurnBridgeV0>(
        SupervisorFailureLane::ActorKey{id, generation},
        "root_supervisor", lane,
        oreslang::gpu::Identity{"tenant_A", "device_0", generation},
        4096, state);
}
void success_and_order() {
    SupervisorFailureLane lane(1);
    auto a = actor(lane, "actor-1", 3, 10);
    auto t = a->prepare(4);
    check(a->has_pending_turn());
    check(a->committed_state() == 10);
    denied([&] { (void)a->prepare(4); }, "GPU_ACTOR_TURN_INFLIGHT");
    denied([&] { a->finish_after_provider_event(t, true, true); }, "GPU_ACTOR_FINISH_INVALID");
    denied([&] { a->mark_submitted(t); }, "GPU_ACTOR_SUBMISSION_INVALID");
    a->begin_enqueue(t);
    a->mark_submitted(t);
    denied([&] { a->mark_submitted(t); }, "GPU_ACTOR_SUBMISSION_INVALID");
    denied([&] { a->finish_after_provider_event(t, true, false); }, "GPU_ACTOR_DEVICE_NOT_QUIESCENT");
    check(a->committed_state() == 10);
    a->finish_after_provider_event(t, true, true); // trusted host seam, NOT real provider proof
    check(a->committed_state() == 14);
    check(a->fence_status() == oreslang::gpu::Status::Idle);
    check(!a->has_pending_turn());
    denied([&] { a->finish_after_provider_event(t, true, true); }, "GPU_ACTOR_STALE_TURN");
    auto t2 = a->prepare(6);
    check(t2.sequence == t.sequence + 1);
    a->begin_enqueue(t2);
    a->mark_submitted(t2);
    a->finish_after_provider_event(t2, true, true);
    check(a->committed_state() == 20);
    check(lane.pending_count() == 0);
    check(a->request_close());
    check(a->closed());
    denied([&] { a->acknowledge_backend_buffer_release(false); }, "GPU_ACTOR_BUFFER_RELEASE_UNPROVEN");
    a->acknowledge_backend_buffer_release(true);
    check(a->fence_status() == oreslang::gpu::Status::Closed);
    auto event=lane.poll();
    check(event.has_value());
    check(event->kind == SupervisorFailureLane::Kind::Cancel);
    check(event->actor.id == "actor-1");
    check(event->actor.generation == 3);
    lane.ack(event->actor);
}
void cancellation_after_submit() {
    SupervisorFailureLane lane(1);
    auto a = actor(lane, "actor-cancel");
    auto t = a->prepare(11);
    a->begin_enqueue(t);
    a->mark_submitted(t);
    check(a->request_close());
    check(!a->closed());
    check(a->fence_status() == oreslang::gpu::Status::Poisoned);
    denied([&] { (void)a->prepare(1); }, "GPU_ACTOR_CLOSED");
    denied([&] { a->finish_after_provider_event(t, true, true); }, "GPU_ACTOR_FINISH_INVALID");
    denied([&] { a->acknowledge_backend_buffer_release(true); }, "GPU_ACTOR_BUFFER_RELEASE_UNPROVEN");
    denied([&] { a->acknowledge_cancelled_provider_event(t,false); }, "GPU_ACTOR_DEVICE_NOT_QUIESCENT");
    check(a->committed_state() == 0);
    check(a->has_pending_turn());
    check(lane.pending_count() == 0);
    a->acknowledge_cancelled_provider_event(t,true);
    check(a->closed());
    check(!a->has_pending_turn());
    check(a->committed_state() == 0);
    check(lane.pending_count() == 1);
    denied([&] { a->acknowledge_cancelled_provider_event(t,true); }, "GPU_ACTOR_STALE_TURN");
    a->acknowledge_backend_buffer_release(true);
    auto ev=lane.poll();
    check(ev && ev->kind == SupervisorFailureLane::Kind::Cancel);
    check(ev->parent_id == "root_supervisor");
    lane.ack(ev->actor);
}
void cancellation_before_enqueue() {
    SupervisorFailureLane lane(1);
    auto a = actor(lane,"actor-preenqueue");
    auto t=a->prepare(20);
    check(a->request_close());
    check(a->closed());
    check(a->committed_state() == 0);
    check(!a->has_pending_turn());
    denied([&] { a->mark_submitted(t); }, "GPU_ACTOR_STALE_TURN");
    check(a->fence_status() == oreslang::gpu::Status::Idle);
    a->acknowledge_backend_buffer_release(true);
    check(a->fence_status() == oreslang::gpu::Status::Closed);
    auto ev=lane.poll();
    check(ev && ev->kind == SupervisorFailureLane::Kind::Cancel);
    lane.ack(ev->actor);
}
void failed_kernel_delivers_control_once() {
    SupervisorFailureLane lane(1);
    auto a=actor(lane,"actor-fail");
    auto t=a->prepare(10);
    a->begin_enqueue(t);
    a->mark_submitted(t);
    a->finish_after_provider_event(t,false,true);
    check(a->committed_state() == 0);
    check(a->closed());
    check(a->fence_status() == oreslang::gpu::Status::Poisoned);
    check(lane.pending_count() == 1);
    denied([&] { a->finish_after_provider_event(t,false,true); }, "GPU_ACTOR_STALE_TURN");
    denied([&] { (void)a->prepare(1); }, "GPU_ACTOR_CLOSED");
    a->acknowledge_backend_buffer_release(true);
    auto ev=lane.poll();
    check(ev && ev->kind == SupervisorFailureLane::Kind::HostFailure);
    check(ev->actor.id == "actor-fail");
    check(ev->parent_id == "root_supervisor");
    lane.ack(ev->actor);
}
void invalid_authority_and_overflow() {
    SupervisorFailureLane lane(4);
    denied([&] { HostTurnBridgeV0 bridge({"",1},"root",lane,{"tenant","device",1},8); },
           "GPU_ACTOR_INVALID_AUTHORITY");
    denied([&] { HostTurnBridgeV0 bridge({"x",0},"root",lane,{"tenant","device",1},8); },
           "GPU_ACTOR_INVALID_AUTHORITY");
    denied([&] { HostTurnBridgeV0 bridge({"x",1},"root",lane,{"tenant","device",0},8); },
           "GPU_ACTOR_INVALID_AUTHORITY");
    auto a=actor(lane,"actor-over-max",1,std::numeric_limits<std::int64_t>::max());
    denied([&] { (void)a->prepare(1); }, "GPU_ACTOR_STAGE_OVERFLOW");
    check(!a->has_pending_turn());
    auto low=actor(lane,"actor-under-min",1,std::numeric_limits<std::int64_t>::min());
    denied([&] { (void)low->prepare(-1); }, "GPU_ACTOR_STAGE_OVERFLOW");
    check(!low->has_pending_turn());
    auto legal=low->prepare(1); low->begin_enqueue(legal); low->mark_submitted(legal);
    low->finish_after_provider_event(legal,true,true);
    check(low->committed_state() == std::numeric_limits<std::int64_t>::min()+1);
    check(a->request_close());
    check(low->request_close());
}
void stale_turn_forgery() {
    SupervisorFailureLane lane(1);
    auto a=actor(lane,"stale",8);
    auto t=a->prepare(3);
    denied([&] { a->mark_submitted({7,t.sequence}); }, "GPU_ACTOR_STALE_TURN");
    denied([&] { a->mark_submitted({8,t.sequence+1}); }, "GPU_ACTOR_STALE_TURN");
    a->begin_enqueue(t);
    a->mark_submitted(t);
    denied([&] { a->finish_after_provider_event({8,t.sequence+1},true,true); },
           "GPU_ACTOR_STALE_TURN");
    a->finish_after_provider_event(t,true,true);
    check(a->committed_state()==3);
    check(a->request_close());
}
void population_1024_host_only() {
    constexpr int count=1024;
    SupervisorFailureLane lane(count);
    std::vector<std::unique_ptr<HostTurnBridgeV0>> actors;
    actors.reserve(count);
    for(int i=0;i<count;i++) actors.push_back(actor(lane,"gpu-"+std::to_string(i)));
    // This is a deterministic host-only model using the actual native
    // actor-credit, fence and supervisor classes: NO physical device work.
    for(int step=0;step<4;step++) {
        std::vector<HostTurnBridgeV0::Turn> batch;
        batch.reserve(count);
        for(auto& a:actors) batch.push_back(a->prepare(step+1));
        for(int i=count-1;i>=0;i--) {
            actors[i]->begin_enqueue(batch[i]);
            actors[i]->mark_submitted(batch[i]);
        }
        for(int i=0;i<count;i++) actors[i]->finish_after_provider_event(batch[i],true,true);
    }
    for(auto& a:actors) {
        check(a->committed_state()==10);
        check(!a->has_pending_turn());
        check(a->fence_status()==oreslang::gpu::Status::Idle);
        denied([&] { a->retire_gracefully_after_backend_release(false); },
               "GPU_ACTOR_GRACEFUL_RETIRE_UNPROVEN");
        a->retire_gracefully_after_backend_release(true);
        check(a->closed());
        check(a->fence_status()==oreslang::gpu::Status::Closed);
    }
    check(lane.pending_count()==0);
    check(lane.reserved_count()==0);
}
void stop_during_provider_enqueue_window() {
    SupervisorFailureLane lane(1);
    auto a=actor(lane,"enqueue-close");
    const auto t=a->prepare(99);
    a->begin_enqueue(t); // true provider would now begin external API call
    check(a->request_close());
    check(a->has_pending_turn()); // never pre-enqueue abort while call is live
    check(!a->closed());
    check(a->fence_status() == oreslang::gpu::Status::InFlight);
    denied([&]{a->acknowledge_backend_buffer_release(true);},
           "GPU_ACTOR_BUFFER_RELEASE_UNPROVEN");
    a->mark_submitted(t); // durable enqueue was late relative to close
    check(a->fence_status() == oreslang::gpu::Status::Poisoned);
    denied([&]{a->finish_after_provider_event(t,true,true);},"GPU_ACTOR_FINISH_INVALID");
    denied([&]{a->acknowledge_cancelled_provider_event(t,false);},
           "GPU_ACTOR_DEVICE_NOT_QUIESCENT");
    check(a->committed_state()==0);
    a->acknowledge_cancelled_provider_event(t,true);
    check(a->closed() && !a->has_pending_turn());
    a->acknowledge_backend_buffer_release(true);
    const auto ev=lane.poll();
    check(ev && ev->kind == SupervisorFailureLane::Kind::Cancel);
    lane.ack(ev->actor);
}
void enqueue_aborts_distinguish_failure_from_close() {
    SupervisorFailureLane lane(2);
    auto failed=actor(lane,"enqueue-failed");
    auto t=failed->prepare(17);
    failed->begin_enqueue(t);
    failed->abort_enqueue(t);
    check(failed->closed());
    check(failed->committed_state()==0);
    check(failed->fence_status() == oreslang::gpu::Status::Idle);
    denied([&]{failed->mark_submitted(t);},"GPU_ACTOR_STALE_TURN");
    auto ev=lane.poll();
    check(ev && ev->kind == SupervisorFailureLane::Kind::HostFailure);
    lane.ack(ev->actor);

    auto cancelled=actor(lane,"enqueue-cancelled");
    t=cancelled->prepare(20);
    cancelled->begin_enqueue(t);
    check(cancelled->request_close());
    check(!cancelled->closed() && cancelled->has_pending_turn());
    cancelled->abort_enqueue(t); // provider proves command was NOT enqueued
    check(cancelled->closed());
    check(cancelled->committed_state()==0);
    ev=lane.poll();
    check(ev && ev->kind == SupervisorFailureLane::Kind::Cancel);
    lane.ack(ev->actor);
}
void device_loss_is_cohort_failure_not_actor_isolation() {
    SupervisorFailureLane lane(3);
    auto first=actor(lane,"lost-a");
    auto second=actor(lane,"lost-b");
    auto idle=actor(lane,"lost-idle");

    auto x=first->prepare(3);
    first->begin_enqueue(x);
    first->mark_submitted(x);
    auto y=second->prepare(7);
    second->begin_enqueue(y);

    check(first->report_device_loss());
    check(second->report_device_loss());
    check(idle->report_device_loss());
    check(first->fence_status() == oreslang::gpu::Status::Poisoned);
    check(second->fence_status() == oreslang::gpu::Status::InFlight);
    check(!first->closed() && !second->closed() && idle->closed());
    denied([&]{second->acknowledge_backend_buffer_release(true);},
           "GPU_ACTOR_BUFFER_RELEASE_UNPROVEN");
    second->mark_submitted(y);
    check(second->fence_status() == oreslang::gpu::Status::Poisoned);
    first->acknowledge_cancelled_provider_event(x,true);
    second->acknowledge_cancelled_provider_event(y,true);
    check(first->committed_state()==0 && second->committed_state()==0);
    first->acknowledge_backend_buffer_release(true);
    second->acknowledge_backend_buffer_release(true);
    idle->acknowledge_backend_buffer_release(true);
    for(int i=0;i<3;i++) {
        auto event=lane.poll();
        check(event && event->kind == SupervisorFailureLane::Kind::HostFailure);
        check(event->parent_id=="root_supervisor");
        lane.ack(event->actor);
    }
    check(lane.pending_count()==0);
}
void concurrent_claim_one_actor() {
    SupervisorFailureLane lane(1);
    auto a=actor(lane,"contended");
    std::atomic<int> admitted{0}, deniedCount{0};
    std::atomic<bool> begin{false};
    auto worker=[&] {
        while(!begin.load(std::memory_order_acquire)) std::this_thread::yield();
        try { (void)a->prepare(1); admitted.fetch_add(1); }
        catch (const std::logic_error&) { deniedCount.fetch_add(1); }
    };
    std::thread first(worker),second(worker);
    begin.store(true,std::memory_order_release);
    first.join();second.join();
    check(admitted.load()==1 && deniedCount.load()==1);
    check(a->request_close()); // pre-enqueue abort, no event
}
} // namespace

int main() {
    success_and_order();
    cancellation_after_submit();
    cancellation_before_enqueue();
    failed_kernel_delivers_control_once();
    invalid_authority_and_overflow();
    stale_turn_forgery();
    population_1024_host_only();
    stop_during_provider_enqueue_window();
    enqueue_aborts_distinguish_failure_from_close();
    device_loss_is_cohort_failure_not_actor_isolation();
    concurrent_claim_one_actor();
    if(checks < 3100) throw std::runtime_error("test coverage under expected 1024 population threshold");
    std::cout << "GPU_ACTOR_HOST_BRIDGE_PASS checks=" << checks
              << " actors=1024 turns=4096 physical_gpu=false" << std::endl;
    return 0;
}
