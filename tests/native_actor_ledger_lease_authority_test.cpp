// Executable host-boundary test with a synthetic authority oracle.
// The adapter/header is exact Git source; this does not run the real scheduler.
#include "oreslang/runtime/ActorLedgerLeaseAdapter.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

struct OresNativeActorScheduler { unsigned instance = 7; };
static thread_local bool holds_lease = false;
static thread_local OresNativeActorRef owned_actor{};
static std::atomic<unsigned> oracle_queries{0};
extern "C" int ores_native_actor_scheduler_is_current_actor(
   OresNativeActorScheduler* s, OresNativeActorRef a) {
    oracle_queries.fetch_add(1, std::memory_order_relaxed);
    return s != nullptr && s->instance == 7 && holds_lease &&
           a.scheduler_domain == owned_actor.scheduler_domain &&
           a.actor_slot == owned_actor.actor_slot &&
           a.generation == owned_actor.generation;
}
using Adapter = oreslang::runtime::ActorLedgerLeaseAdapter<3>;
using S = Adapter::Status;
static void verify(bool ok, const char* message, int trial) {
    if (!ok) {std::fprintf(stderr,"LEASE_BOUNDARY_FAIL round=%d %s\n",trial,message);std::abort();}
}
int main() {
    for (int trial=0;trial<2000;++trial) {
        oracle_queries.store(0, std::memory_order_relaxed);
        OresNativeActorScheduler sched{};
        const OresNativeActorRef a{33,5,static_cast<std::uint64_t>(trial+1)};
        const OresNativeActorRef b{33,6,static_cast<std::uint64_t>(trial+1)};
        Adapter alpha(&sched,a,55), beta(&sched,b,56), no_scheduler(nullptr,a,57);
        Adapter::Token token{};
        Adapter::Selection sel{};
        Adapter::Snapshot before{};
        verify(alpha.admit(Adapter::Kind::Mail,token)==S::Unauthorized,"admit without lease",trial);
        verify(!alpha.snapshot(before),"snapshot without lease",trial);
        verify(no_scheduler.admit(Adapter::Kind::Mail,token)==S::Unauthorized,"null scheduler",trial);
        owned_actor=a;holds_lease=true;
        verify(beta.admit(Adapter::Kind::Mail,token)==S::Unauthorized,"cross actor denied",trial);
        verify(alpha.admit(Adapter::Kind::Mail,token)==S::Ok,"admission with lease",trial);
        const auto first=token;
        verify(alpha.claim(sel)==S::Ok && sel.token.sequence==first.sequence,"lease claim",trial);
        verify(alpha.prepare_await(first)==S::Ok,"registered await",trial);
        holds_lease=false;
        std::thread outsider([&] {
            verify(alpha.deliver_wake(first)==S::Unauthorized,"foreign wake",trial);
            verify(alpha.cancel(first)==S::Unauthorized,"foreign cancel",trial);
            verify(alpha.finish_after_guest_exit(first)==S::Unauthorized,"foreign exit",trial);
            verify(alpha.stop_and_request_cleanup()==S::Unauthorized,"foreign stop",trial);
            Adapter::Snapshot snap{};
            verify(!alpha.snapshot(snap),"foreign snapshot",trial);
        });
        outsider.join();
        verify(alpha.suspend(first)==S::Unauthorized,"suspend after lease revoked",trial);
        holds_lease=true;
        verify(alpha.deliver_wake(first)==S::Ok,"authenticated early WAKE",trial);
        verify(alpha.suspend(first)==S::AlreadyReady,"early WAKE inline",trial);
        verify(alpha.finish_after_guest_exit(first)==S::Ok,"physical exit",trial);
        verify(alpha.admit(Adapter::Kind::Mail,token)==S::Ok,"second admission",trial);
        verify(alpha.claim(sel)==S::Ok,"second claim",trial);
        verify(alpha.prepare_await(token)==S::Ok,"second arm",trial);
        verify(alpha.suspend(token)==S::Ok,"second suspended",trial);
        holds_lease=false;
        verify(alpha.stop_and_request_cleanup()==S::Unauthorized,"STOP must require lease",trial);
        holds_lease=true;
        verify(alpha.stop_and_request_cleanup()==S::Ok,"authorized STOP",trial);
        verify(alpha.claim(sel)==S::Ok && sel.dispatch==Adapter::Ledger::Dispatch::CancelCleanup,
               "physical cleanup turn",trial);
        verify(alpha.finish_after_guest_exit(token)==S::Ok,"physical cleanup exit",trial);
        verify(alpha.admit(Adapter::Kind::Mail,token)==S::Closed,"STOP closes admission",trial);
        verify(alpha.snapshot(before) && before.closed && before.live==0,"closed and empty",trial);
        holds_lease=false;
        verify(oracle_queries.load()>10,"authority oracle not consulted",trial);
    }
    std::puts("ACTOR_LEDGER_LEASE_BOUNDARY_PASS rounds=2000");
}
