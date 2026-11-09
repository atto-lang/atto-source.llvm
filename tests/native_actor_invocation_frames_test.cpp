#include "oreslang/runtime/ActorInvocationFrames.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>

using Ledger = oreslang::runtime::ActorExecutionLedger<3>;
using Frames = oreslang::runtime::ActorInvocationFrames<3,4,64,96>;
using LS = Ledger::Status;
using FS = Frames::Status;

static void require(bool ok,const char* what) {
    if (!ok) { std::fprintf(stderr,"FAIL: %s\n",what); std::exit(1); }
}

int main() {
    Ledger ledger(42,7);
    Frames frames(42,7);
    Ledger::Token first{}, second{}, third{};
    Ledger::Selection selected{};

    require(ledger.admit(Ledger::Kind::Mail,first)==LS::Ok,"admit receive A");
    require(ledger.admit(Ledger::Kind::Mail,second)==LS::Ok,"admit receive B");
    require(frames.bind(first)==FS::Ok,"bind A's own logical stack");
    require(frames.bind(second)==FS::Ok,"bind B's distinct queued stack");
    require(frames.bind(first)==FS::Busy,"duplicate stack bind denied");
    require(frames.snapshot().live_stacks==2,"two separate stack identities");

    require(ledger.claim_under_lease(selected)==LS::Ok &&
            selected.token.sequence==first.sequence,"A enters before B");
    require(frames.enter(first)==FS::Ok,"enter A under caller-held actor lease");
    require(frames.enter(second)==FS::Busy,"B cannot execute concurrently");
    Frames::FrameRef caller{},callee{};
    require(frames.push(first,24,caller)==FS::Ok,"receive frame allocation");
    require(frames.push(first,16,callee)==FS::Ok,"nested CALL frame");
    const std::uint8_t values[]={4,2,9};
    require(frames.write(first,callee,1,values,3)==FS::Ok,"callee local write");
    require(frames.snapshot().charged_bytes==40 &&
            frames.snapshot().live_frames==2,"two nested frames charged");

    require(ledger.prepare_await(first)==LS::Ok,"arm A await");
    require(ledger.suspend(first)==LS::Ok,"ledger A suspended");
    require(frames.suspend(first)==FS::Ok,"frame A suspended without reclaim");
    require(frames.snapshot().charged_bytes==40,"suspended bytes stay charged");
    Frames::FrameRef impossible{};
    require(frames.push(first,1,impossible)==FS::NotRunning,
            "suspended stack not writable");
    require(ledger.claim_under_lease(selected)==LS::NoEligible,
            "serial DATA B cannot overtake suspended A");
    require(ledger.wake(first)==LS::Ok,"A becomes ready");
    require(ledger.claim_under_lease(selected)==LS::Ok &&
            selected.token.sequence==first.sequence,"resume exact A token");
    require(frames.enter(first)==FS::Ok,"same invocation resumes");

    std::uint8_t observed[3]={};
    require(frames.read(first,callee,1,observed,3)==FS::Ok &&
            observed[0]==4 && observed[1]==2 && observed[2]==9,
            "CALL local survives suspension");
    require(frames.pop(first,caller)==FS::NoFrame,
            "non-LIFO pop rejected without changing authenticated frame");
    require(frames.pop(first,callee)==FS::Ok,"return from nested CALL");
    require(frames.pop(first,caller)==FS::Ok,"return from receive");
    require(frames.retire_after_guest_exit(first)==FS::Ok,
            "A stack retires after physical guest exit");
    require(ledger.finish_after_exit(first)==LS::Ok,"ledger A physically retired");
    require(frames.snapshot().charged_bytes==0,"A bytes fully reclaimed");
    require(frames.bind(first)==FS::Stale,"retired A token cannot rebind");

    require(ledger.claim_under_lease(selected)==LS::Ok &&
            selected.token.sequence==second.sequence,"B receives after A");
    require(frames.enter(second)==FS::Ok,"B starts with own stack");
    Frames::FrameRef bframe{};
    require(frames.push(second,24,bframe)==FS::Ok,"B frame");
    std::uint8_t zero=255;
    require(frames.read(second,bframe,0,&zero,1)==FS::Ok && zero==0,
            "recycled logical slots start zero");
    require(frames.read(first,callee,0,&zero,1)==FS::Stale,
            "retired A frame cannot access B");
    require(frames.pop(second,bframe)==FS::Ok,"B normal return");
    require(frames.retire_after_guest_exit(second)==FS::Ok,"B frames retire");
    require(ledger.finish_after_exit(second)==LS::Ok,"B ledger retire");

    require(ledger.admit(Ledger::Kind::Mail,third)==LS::Ok,"reused ledger slot");
    require(frames.bind(third)==FS::Ok,"new generation of recycled slot binds");
    require(frames.bind(first)==FS::Stale,"ABA token remains denied after reuse");
    require(ledger.cancel(third)==LS::Ok,"queued cancellation");
    require(frames.retire_queued(third)==FS::Ok,"queued stack no frames reclaimed");
    require(frames.snapshot().live_stacks==0,"no leaked stack identities");

    Ledger stopped(64,1);
    Frames cancelled(64,1);
    Ledger::Token t{};
    require(stopped.admit(Ledger::Kind::Mail,t)==LS::Ok,"cancel A admission");
    require(cancelled.bind(t)==FS::Ok,"bind cancel A");
    require(stopped.claim_under_lease(selected)==LS::Ok,"cancel A start");
    require(cancelled.enter(t)==FS::Ok,"cancel A frame start");
    Frames::FrameRef pending{};
    require(cancelled.push(t,32,pending)==FS::Ok,"A owns pending frame");
    require(stopped.prepare_await(t)==LS::Ok,"cancel arm");
    require(stopped.suspend(t)==LS::Ok,"cancel suspend");
    require(cancelled.suspend(t)==FS::Ok,"cancel stack stays resident");
    require(stopped.cancel(t)==LS::Ok,"cancel request");
    require(cancelled.snapshot().charged_bytes==32,"cancel cannot reclaim early");
    require(stopped.claim_under_lease(selected)==LS::Ok &&
            selected.dispatch==Ledger::Dispatch::CancelCleanup,
            "supervisor cleanup turn");
    require(cancelled.enter_for_cleanup(t)==FS::Ok,
            "physical cleanup turn reacquires non-executable stack");
    require(cancelled.push(t,1,pending)==FS::NotRunning,
            "cleanup lease cannot dispatch guest frames");
    require(cancelled.retire_after_guest_exit(t,true)==FS::Ok,"cleanup scrubs frame");
    require(stopped.finish_after_exit(t)==LS::Ok,"cleanup acknowledged");
    require(cancelled.snapshot().charged_bytes==0,"no bytes after cleanup");
    require(cancelled.read(t,pending,0,&zero,1)==FS::Stale,
            "cancelled stale frame rejected");
    require(stopped.wake(t)==LS::Stale,"cancelled wake denied");

    // Actor-global stack quota remains independent of per-slot reservation.
    // This portion simulates future opt-in concurrent receive admission;
    // the serial ledger above intentionally forbids executing B during await.
    using Small = oreslang::runtime::ActorInvocationFrames<3,4,64,64>;
    Small quota(81,2);
    Ledger quotaLedger(81,2);
    Ledger::Token qa{},qb{};
    require(quotaLedger.admit(Ledger::Kind::Mail,qa)==LS::Ok,"quota A admitted");
    require(quotaLedger.admit(Ledger::Kind::Mail,qb)==LS::Ok,"quota B admitted");
    require(quota.bind(qa)==Small::Status::Ok &&
            quota.bind(qb)==Small::Status::Ok,"quota roots have separate identity");
    require(quota.enter(qa)==Small::Status::Ok,"quota A running");
    Small::FrameRef forty{},exceeded{};
    require(quota.push(qa,40,forty)==Small::Status::Ok,"quota A uses 40");
    require(quota.suspend(qa)==Small::Status::Ok,"quota A suspended");
    require(quota.enter(qb)==Small::Status::Ok,"future opt-in B entered");
    require(quota.push(qb,25,exceeded)==Small::Status::QuotaExceeded,
            "aggregate 64-byte actor quota cannot be bypassed");
    require(quota.poisoned(),"swallowed quota error is actor terminal");
    require(quota.push(qb,1,exceeded)==Small::Status::Poisoned,
            "poisoned actor cannot resume guest");
    require(quota.retire_after_guest_exit(qb,true)==Small::Status::Ok,
            "B cleanup permitted despite terminal poison");
    require(quota.snapshot().charged_bytes==40,"A suspended still charged");
    require(quota.enter_for_cleanup(qa)==Small::Status::Ok,
            "poisoned actor keeps bounded physical cleanup access");
    require(quota.push(qa,1,exceeded)==Small::Status::Poisoned,
            "cleanup entry cannot restart poisoned guest");
    require(quota.retire_after_guest_exit(qa,true)==Small::Status::Ok,
            "suspended A is scrubbed by physical cleanup");
    require(quota.snapshot().charged_bytes==0 &&
            quota.snapshot().live_stacks==0,
            "poisoned actor eventually reclaims all invocation stacks");

    // Independent actor heap/stack domain is not poisoned by A's exhaustion.
    Frames sibling(82,2);
    Ledger siblingLedger(82,2);
    Ledger::Token sb{};
    require(siblingLedger.admit(Ledger::Kind::Mail,sb)==LS::Ok,"sibling event");
    require(sibling.bind(sb)==FS::Ok && sibling.enter(sb)==FS::Ok,
            "sibling still runnable");
    Frames::FrameRef sf{};
    require(sibling.push(sb,8,sf)==FS::Ok,"sibling frame");
    require(sibling.pop(sb,sf)==FS::Ok &&
            sibling.retire_after_guest_exit(sb)==FS::Ok,
            "sibling completes independently");

    // Bounds checked before touching storage; an admitted guest breach is
    // terminal for its own actor, without touching the other actor.
    Frames bad(84,3);
    Ledger badLedger(84,3);
    Ledger::Token bt{};
    require(badLedger.admit(Ledger::Kind::Mail,bt)==LS::Ok,"bounds event");
    require(bad.bind(bt)==FS::Ok && bad.enter(bt)==FS::Ok,"bounds enter");
    Frames::FrameRef br{};
    require(bad.push(bt,4,br)==FS::Ok,"bounds frame");
    require(bad.write(bt,br,3,values,3)==FS::OutOfBounds,
            "out-of-bounds write rejected and actor poisoned");
    require(bad.poisoned() && !sibling.poisoned(),
            "bounds fault isolated to actor");
    require(bad.retire_after_guest_exit(bt,true)==FS::Ok,
            "poisoned owner can still physically cleanup");

    std::puts("ActorInvocationFrames: host-only assertions passed");
}
