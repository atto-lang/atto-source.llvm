#include "oreslang/runtime/ActorLedgerLeaseAdapter.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

using Adapter = oreslang::runtime::ActorLedgerLeaseAdapter<4>;
using Status = Adapter::Status;
using Kind = Adapter::Kind;

#define CHECK(x) do { if (!(x)) { \
    std::fprintf(stderr, "ACTOR_LEDGER_BRIDGE_FAIL %s:%d: %s\n", \
        __FILE__, __LINE__, #x); std::abort(); \
} } while (0)

struct Fixture {
    OresNativeActorScheduler* scheduler = nullptr;
    OresNativeActorRef refs[2]{};
    std::unique_ptr<Adapter> ledger;
    Adapter::Token a{}, b{};
    OresNativeWakeTicket ticket{};
    OresNativeRetainedDataTicket retained{};
    std::atomic<int> suspended{0}, other_progress{0};
    std::atomic<int> a_first{0}, a_resumed{0}, b_guest{0}, wake_called{0};
};

extern "C" void ledger_data(void* raw, std::size_t actor, std::uint64_t payload) {
    auto& f=*static_cast<Fixture*>(raw);
    CHECK(actor<2);
    CHECK(ores_native_actor_scheduler_is_current_actor(f.scheduler, f.refs[actor]));
    if (actor==1) {
        CHECK(payload==9);
        f.other_progress.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    CHECK(f.ledger->current_actor_lease());
    Adapter::Selection selection{};
    if (payload==1) {
        CHECK(f.ledger->admit(Kind::Mail,f.a)==Status::Ok);
        CHECK(f.ledger->claim(selection)==Status::Ok);
        CHECK(selection.token.sequence==f.a.sequence);
        f.a_first.fetch_add(1, std::memory_order_relaxed);
        // Authenticate and escrow the producer-owned continuation BEFORE
        // publishing the await as suspended.
        CHECK(ores_native_actor_scheduler_reserve_wake(f.scheduler,
            f.refs[0],f.refs[0],1,1,1,1,&f.ticket));
        CHECK(f.ledger->prepare_await(f.a)==Status::Ok);
        CHECK(f.ledger->suspend(f.a)==Status::Ok);
        // Retain the ORIGINAL handler A DATA receipt, not B's later receipt.
        // A has a pinned logical frame, but no OS carrier after this callback.
        CHECK(ores_native_actor_scheduler_retain_current_data(
            f.scheduler,&f.retained));
        CHECK(f.retained.actor.generation==f.refs[0].generation);
        f.suspended.store(1,std::memory_order_release);
    } else if (payload==2) {
        // The native scheduler must not dispatch this DATA callback at all
        // until A's retained ticket was settled inside an authenticated WAKE.
        CHECK(f.a_resumed.load(std::memory_order_acquire)==1);
        CHECK(ores_native_actor_scheduler_retained_data_count(
            f.scheduler,f.refs[0])==0);
        CHECK(f.ledger->admit(Kind::Mail,f.b)==Status::Ok);
        CHECK(f.ledger->claim(selection)==Status::Ok);
        CHECK(selection.token.sequence==f.b.sequence);
        CHECK(f.ledger->finish_after_guest_exit(f.b)==Status::Ok);
        f.b_guest.fetch_add(1,std::memory_order_release);
    } else {
        CHECK(false);
    }
}

extern "C" void ledger_wake(void* raw,std::size_t actor,
                             OresNativeWakeTicket ticket,OresNativeWakeOutcome outcome) {
    auto& f=*static_cast<Fixture*>(raw);
    CHECK(actor==0);
    CHECK(outcome==ORES_NATIVE_WAKE_READY);
    CHECK(ores_native_actor_scheduler_is_current_wake(f.scheduler,ticket));
    CHECK(ores_native_actor_scheduler_is_current_actor(f.scheduler,f.refs[0]));
    CHECK(ticket.continuation_slot==f.ticket.continuation_slot);
    CHECK(ticket.slot_generation==f.ticket.slot_generation);
    CHECK(f.ledger->deliver_wake(f.a)==Status::Ok);
    Adapter::Selection selection{};
    CHECK(f.ledger->claim(selection)==Status::Ok);
    CHECK(selection.token.sequence==f.a.sequence);
    f.a_resumed.fetch_add(1,std::memory_order_release);
    CHECK(f.ledger->finish_after_guest_exit(f.a)==Status::Ok);
    // Settling A releases its mailbox credit and permits the *scheduler*
    // to dispatch B next. B must not be in the actor ledger or have executed
    // before this authenticated physical guest-exit acknowledgment.
    CHECK(f.b_guest.load(std::memory_order_acquire)==0);
    OresNativeRetainedDataTicket forged=f.retained;
    ++forged.slot_generation;
    CHECK(!ores_native_actor_scheduler_complete_retained_data(f.scheduler,forged));
    CHECK(ores_native_actor_scheduler_complete_retained_data(
        f.scheduler,f.retained));
    CHECK(!ores_native_actor_scheduler_complete_retained_data(
        f.scheduler,f.retained));
    CHECK(!ores_native_actor_scheduler_abandon_retained_data(
        f.scheduler,f.retained));
    f.wake_called.fetch_add(1,std::memory_order_release);
}

static void until(const std::atomic<int>& flag,const char* name) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(8);
    while (!flag.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now()>=deadline) {
            std::fprintf(stderr,"TIMEOUT: %s\n",name);
            std::abort();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
static void until_completed(OresNativeActorScheduler* scheduler,
                            std::uint64_t expected) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(8);
    while (ores_native_actor_scheduler_completed(scheduler)<expected) {
        if (std::chrono::steady_clock::now()>=deadline) {
            std::fprintf(stderr,"TIMEOUT: expected completed=%llu\n",
                static_cast<unsigned long long>(expected));
            std::abort();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
int main() {
    Fixture f;
    f.scheduler=ores_native_actor_scheduler_open_with_wakes(
        2,8,2,ledger_data,nullptr,nullptr,ledger_wake,&f);
    CHECK(f.scheduler);
    f.refs[0]=ores_native_actor_scheduler_spawn(f.scheduler,0);
    f.refs[1]=ores_native_actor_scheduler_spawn(f.scheduler,1);
    CHECK(f.refs[0].scheduler_domain && f.refs[1].scheduler_domain);
    f.ledger=std::make_unique<Adapter>(f.scheduler,f.refs[0],1234);
    Adapter::Token unauthorized{};
    CHECK(!f.ledger->current_actor_lease());
    CHECK(f.ledger->admit(Kind::Mail,unauthorized)==Status::Unauthorized);
    Adapter::Selection denied{};
    CHECK(f.ledger->claim(denied)==Status::Unauthorized);
    CHECK(ores_native_actor_scheduler_send(f.scheduler,f.refs[0],1));
    until(f.suspended,"A reaches logical suspension");
    CHECK(f.ledger->deliver_wake(f.a)==Status::Unauthorized);
    CHECK(ores_native_actor_scheduler_send(f.scheduler,f.refs[0],2));
    CHECK(ores_native_actor_scheduler_send(f.scheduler,f.refs[1],9));
    until(f.other_progress,"independent actor progress");
    until_completed(f.scheduler,1);
    CHECK(ores_native_actor_scheduler_accepted(f.scheduler)==3);
    CHECK(ores_native_actor_scheduler_completed(f.scheduler)==1);
    CHECK(f.a_first.load()==1 && f.a_resumed.load()==0 && f.b_guest.load()==0);
    CHECK(ores_native_actor_scheduler_retained_data_count(
        f.scheduler,f.refs[0])==1);
    CHECK(!ores_native_actor_scheduler_complete_retained_data(
        f.scheduler,f.retained));
    CHECK(!ores_native_actor_scheduler_abandon_retained_data(
        f.scheduler,f.retained));
    OresNativeWakeTicket forged=f.ticket;
    ++forged.continuation_generation;
    CHECK(ores_native_actor_scheduler_publish_wake(
        f.scheduler,forged,ORES_NATIVE_WAKE_READY)==ORES_NATIVE_WAKE_INVALID);
    CHECK(ores_native_actor_scheduler_transfer_wake(f.scheduler,f.ticket));
    CHECK(ores_native_actor_scheduler_publish_wake(
        f.scheduler,f.ticket,ORES_NATIVE_WAKE_READY)==ORES_NATIVE_WAKE_PUBLISHED);
    CHECK(ores_native_actor_scheduler_publish_wake(
        f.scheduler,f.ticket,ORES_NATIVE_WAKE_READY)==ORES_NATIVE_WAKE_INVALID);
    until(f.wake_called,"A resumed and physical DATA receipt retired");
    until(f.b_guest,"B dispatches only after A's exit and acknowledgment");
    CHECK(f.a_resumed.load()==1 && f.b_guest.load()==1);
    CHECK(ores_native_actor_scheduler_drain(f.scheduler));
    CHECK(ores_native_actor_scheduler_retained_data_count(
        f.scheduler,f.refs[0])==0);
    CHECK(ores_native_actor_scheduler_accepted(f.scheduler)==4);
    CHECK(ores_native_actor_scheduler_completed(f.scheduler)==4);
    CHECK(ores_native_actor_scheduler_wakes_completed(f.scheduler)==1);
    ores_native_actor_scheduler_retire(f.scheduler);
    CHECK(ores_native_actor_scheduler_try_destroy(f.scheduler));
    std::puts("ACTOR_LEDGER_NATIVE_SCHEDULER_BRIDGE_PASS");
}
