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
    // Host/FFI enum-cast values outside the declared Kind set must not
    // allocate a slot, advance the actor revision or change the output token.
    L::Token rejected{17,18,19,20,21};
    for (unsigned raw=3; raw<=255; ++raw) {
        auto invalid=static_cast<K>(static_cast<std::uint8_t>(raw));
        test(actor.admit(invalid,rejected)==S::Invalid,"invalid Kind rejected");
        auto snap=actor.snapshot();
        test(snap.revision==0 && snap.live==0 && !snap.runnable &&
             !snap.executing && !snap.suspended,"invalid Kind leaves ledger unchanged");
    }
    test(rejected.actor==17 && rejected.generation==18 && rejected.sequence==19 &&
         rejected.slot_generation==20 && rejected.slot==21,
         "invalid Kind leaves output token untouched");
    test(actor.admit(K::Mail,a)==S::Ok, "A admission");
    test(actor.admit(K::Mail,b)==S::Ok, "B admission");
    test(actor.admit(K::Timer,c)==S::Ok, "timer callback admission");
    test(actor.admit(K::Mail,d)==S::Full, "bounded admission");
    test(actor.claim_under_lease(out)==S::Ok && out.token.sequence==a.sequence,"A first");
    test(actor.claim_under_lease(out)==S::Busy,"no parallel execution");
    test(actor.suspend(a)==S::AwaitNotArmed,"await registration required");
    test(actor.prepare_await(a)==S::Ok,"A await armed");
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
    test(stop.prepare_await(x)==S::Ok,"arm x await");
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
    // Completion in the registration->suspend race is latched. The same
    // logical handler continues inline; no actor carrier is parked or B run.
    L early(300,5);
    L::Token ea{},eb{};
    test(early.admit(K::Mail,ea)==S::Ok,"early A admitted");
    test(early.admit(K::Mail,eb)==S::Ok,"early B admitted");
    test(early.claim_under_lease(out)==S::Ok,"early A entered");
    test(early.wake(ea)==S::NotSuspended,"unregistered wake rejected");
    test(early.prepare_await(ea)==S::Ok,"early await armed first");
    test(early.wake(ea)==S::Ok,"early completion latched");
    test(early.wake(ea)==S::AlreadyReady,"early duplicate rejected");
    test(early.suspend(ea)==S::AlreadyReady,"no lost early wake");
    test(early.snapshot().executing && !early.snapshot().suspended,
         "early completion retains actor lease");
    test(early.claim_under_lease(out)==S::Busy,"early B cannot overtake");
    test(early.finish_after_exit(ea)==S::Ok,"early A physically finished");
    test(early.claim_under_lease(out)==S::Ok && out.token.sequence==eb.sequence,
         "early B now permitted");
    test(early.finish_after_exit(eb)==S::Ok,"early B finished");

    // Later completion must wake A's suspended logical turn, not enter B.
    L late(301,1);
    L::Token la{},lb{};
    test(late.admit(K::Mail,la)==S::Ok,"late A admitted");
    test(late.admit(K::Mail,lb)==S::Ok,"late B admitted");
    test(late.claim_under_lease(out)==S::Ok,"late A entered");
    test(late.prepare_await(la)==S::Ok,"late await armed");
    test(late.suspend(la)==S::Ok,"late A suspended");
    test(late.claim_under_lease(out)==S::NoEligible,"later B blocked");
    test(late.wake(la)==S::Ok,"late completion reactivates A");
    test(late.claim_under_lease(out)==S::Ok && out.token.sequence==la.sequence,
         "late A resumes before B");
    test(late.finish_after_exit(la)==S::Ok,"late A exits");
    test(late.claim_under_lease(out)==S::Ok && out.token.sequence==lb.sequence,
         "late B enters");
    test(late.finish_after_exit(lb)==S::Ok,"late B exits");

    // Cancel-after-early-completion must not revive the handler or permit
    // a suspension that would lose the cancellation cleanup requirement.
    L cancelled(302,1);
    L::Token ca{};
    test(cancelled.admit(K::Mail,ca)==S::Ok,"cancel A admitted");
    test(cancelled.claim_under_lease(out)==S::Ok,"cancel A running");
    test(cancelled.prepare_await(ca)==S::Ok,"cancel await armed");
    test(cancelled.wake(ca)==S::Ok,"cancel early ready");
    test(cancelled.cancel(ca)==S::Ok,"cancel before suspend");
    test(cancelled.suspend(ca)==S::Cancelled,"cancel prevents new suspension");
    test(cancelled.wake(ca)==S::AlreadyCancelled,"cancelled wake fails");
    test(cancelled.finish_after_exit(ca)==S::Ok,"cancel physical retirement");
    std::puts("ActorExecutionLedger: all assertions passed");
}
