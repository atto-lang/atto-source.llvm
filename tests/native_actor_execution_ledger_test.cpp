#include "oreslang/runtime/ActorExecutionLedger.hpp"
#include <cstdio>
#include <cstdlib>
using L = oreslang::runtime::ActorExecutionLedger<3>;
using S = L::Status;
using K = L::Kind;
using D = L::Dispatch;
static void test(bool ok, const char* msg) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", msg); std::exit(1); }
}
int main() {
    L actor(42,3);
    L::Token a{}, b{}, c{}, d{};
    L::Selection out{};
    test(actor.admit(K::Mail,a)==S::Ok, "A admission");
    test(actor.admit(K::Mail,b)==S::Ok, "B admission");
    test(actor.admit(K::Timer,c)==S::Ok, "timer callback admission");
    test(actor.admit(K::Mail,d)==S::Full, "bounded admission");
    test(actor.claim_under_lease(out)==S::Ok && out.token.sequence==a.sequence,"A first");
    test(actor.claim_under_lease(out)==S::Busy,"no parallel execution");
    test(actor.suspend(a)==S::Ok,"A suspended");
    test(actor.wake(b)==S::NotSuspended,"B cannot impersonate A wake");
    test(!actor.snapshot().runnable,"no overtaking while A is suspended");
    test(actor.claim_under_lease(out)==S::NoEligible,"suspended A blocks later handlers");
    L::Token forged=a;
    ++forged.generation;
    test(actor.wake(forged)==S::Stale,"stale actor generation");
    forged=a; ++forged.sequence;
    test(actor.wake(forged)==S::Stale,"wrong event sequence");
    test(actor.wake(a)==S::Ok,"resume A");
    test(actor.wake(a)==S::AlreadyReady,"duplicate notice no second event");
    test(actor.snapshot().runnable,"ready continuation can be dispatched");
    test(actor.claim_under_lease(out)==S::Ok && out.token.sequence==a.sequence,
         "A resumes before later B");
    test(actor.finish_after_exit(a)==S::Ok,"retire A after physical exit");
    test(actor.claim_under_lease(out)==S::Ok && out.token.sequence==b.sequence,"B executes");
    test(actor.finish_after_exit(b)==S::Ok,"finish B");
    test(actor.claim_under_lease(out)==S::Ok && out.kind==K::Timer,"timer is new turn");
    test(actor.finish_after_exit(c)==S::Ok,"finish timer callback");
    test(actor.snapshot().live==0,"all capacity returned");
    test(actor.admit(K::Mail,d)==S::Ok,"slot recycled");
    test(actor.wake(a)==S::Stale,"old token no slot ABA");
    test(actor.cancel(d)==S::Ok,"queued cancellation");
    test(actor.cancel(d)==S::Stale,"duplicate cancel cannot alter quota");

    L stop(7,1);
    L::Token x{},y{};
    test(stop.admit(K::Mail,x)==S::Ok,"x admitted");
    test(stop.admit(K::Mail,y)==S::Ok,"y admitted");
    test(stop.claim_under_lease(out)==S::Ok,"claim x");
    test(stop.suspend(x)==S::Ok,"suspend x");
    test(stop.cancel(x)==S::Ok,"cancel x while suspended");
    test(stop.cancel(x)==S::AlreadyCancelled,"idempotent cancellation");
    test(stop.wake(x)==S::AlreadyCancelled,"late wake does not resurrect x");
    test(stop.snapshot().live==2,"cancellation retains physical frame");
    test(stop.stop_and_request_cleanup()==S::Ok,"stop fences admissions");
    test(stop.snapshot().live==1,"stop discards queued y, not x");
    test(!stop.physically_retired(),"stop is not quiescence");
    test(stop.admit(K::Mail,d)==S::Closed,"post-stop admission denied");
    test(stop.claim_under_lease(out)==S::Ok &&
         out.dispatch==D::CancelCleanup && out.token.sequence==x.sequence,
         "same suspended turn performs cleanup");
    test(!stop.physically_retired(),"running cleanup retains actor heap");
    test(stop.finish_after_exit(x)==S::Ok,"finish cleanup");
    test(stop.physically_retired(),"physical retirement acknowledged");
    test(stop.wake(x)==S::Stale,"retired wake invalid");

    L running(91,4);
    test(running.admit(K::Deferred,a)==S::Ok,"deferred callback admitted");
    test(running.claim_under_lease(out)==S::Ok,"deferred callback running");
    test(running.cancel(a)==S::Ok,"cancel running callback");
    test(running.snapshot().live==1 && running.snapshot().executing,
        "cancel cannot reclaim running event");
    test(running.suspend(a)==S::Cancelled,"cancelled running event cannot suspend");
    test(running.stop_and_request_cleanup()==S::Ok,"stop running actor");
    test(!running.physically_retired(),"running callback still alive");
    test(running.finish_after_exit(a)==S::Ok,"exit physical callback");
    test(running.physically_retired(),"now reclaimed");

    L other(92,4);
    test(other.admit(K::Mail,b)==S::Ok,"other actor admitted");
    test(other.wake(a)==S::Stale,"cross actor wake rejected");
    test(other.snapshot().revision==1,"stale wake does not mutate revision");
    test(other.stop_and_request_cleanup()==S::Ok,"stop other");
    test(other.physically_retired(),"queued-only close retires");
    std::puts("ActorExecutionLedger: all assertions passed");
}
