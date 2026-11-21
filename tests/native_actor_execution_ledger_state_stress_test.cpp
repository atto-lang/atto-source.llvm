#include "oreslang/runtime/ActorExecutionLedger.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
using Ledger = oreslang::runtime::ActorExecutionLedger<5>;
using S = Ledger::Status;
using K = Ledger::Kind;
static std::uint64_t rng=0x6f7265736c616e67ULL;
static std::uint64_t next() noexcept { rng^=rng<<13; rng^=rng>>7; rng^=rng<<17; return rng; }
static void check(bool yes, const char* msg, int round, int step) {
    if (!yes) {std::fprintf(stderr,"FAIL %s round=%d step=%d\n",msg,round,step);std::exit(1);}
}
int main() {
    std::size_t actions=0,closed=0,retired=0,stale_wakes=0;
    for(int round=0;round<2000;++round) {
        Ledger ledger(static_cast<std::uint64_t>(round+1),9);
        std::vector<Ledger::Token> seen;
        std::uint64_t revision=0;
        for(int i=0;i<200;++i) {
            const auto before=ledger.snapshot();
            const auto op=next()%8;
            Ledger::Token t{};
            if(!seen.empty()) t=seen[next()%seen.size()];
            switch(op) {
                case 0:case 1: {
                    K k=static_cast<K>(next()%3);
                    S s=ledger.admit(k,t);
                    if(s==S::Ok) seen.push_back(t);
                    else check(s==S::Full,"admit failure unexpected",round,i);
                    break;
                }
                case 2: {Ledger::Selection sel{}; S s=ledger.claim_under_lease(sel);
                    check(s==S::Ok || s==S::Busy || s==S::NoEligible,"claim status",round,i);break;}
                case 3: if(!seen.empty()) {
                    S s=ledger.prepare_await(t);
                    check(s==S::Ok||s==S::Stale||s==S::NotRunning||s==S::Cancelled||s==S::AlreadyReady,"arm status",round,i);
                }break;
                case 4: if(!seen.empty()) {
                    S s=ledger.suspend(t);
                    check(s==S::Ok||s==S::Stale||s==S::NotRunning||s==S::Cancelled||s==S::AwaitNotArmed||s==S::Busy||s==S::AlreadyReady,"suspend status",round,i);
                }break;
                case 5: if(!seen.empty()) {
                    S s=ledger.wake(t);
                    check(s==S::Ok||s==S::Stale||s==S::NotSuspended||s==S::AlreadyReady||s==S::AlreadyCancelled,"wake status",round,i);
                }break;
                case 6: if(!seen.empty()) {
                    S s=ledger.cancel(t);
                    check(s==S::Ok||s==S::Stale||s==S::AlreadyCancelled,"cancel status",round,i);
                }break;
                case 7: if(!seen.empty()) {
                    S s=ledger.finish_after_exit(t);
                    check(s==S::Ok||s==S::Stale||s==S::NotRunning,"finish status",round,i);
                }break;
            }
            const auto snap=ledger.snapshot();
            check(snap.live<=5,"bounded capacity",round,i);
            check(!(snap.executing && snap.suspended),"nonreentrant",round,i);
            check(snap.revision>=revision,"revision monotonic",round,i);
            if(!snap.live) check(!snap.executing&&!snap.suspended&&!snap.runnable,"zero live state",round,i);
            revision=snap.revision;
            (void)before;
            ++actions;
        }
        check(ledger.stop_and_request_cleanup()==S::Ok,"stop accepted",round,201);++closed;
        Ledger::Token t{};
        check(ledger.admit(K::Mail,t)==S::Closed,"poststop admission",round,202);
        // Settle physical callback exit/cleanup only when a real active token exists.
        for(int k=0;k<25 && !ledger.physically_retired();++k) {
            bool worked=false;
            for(const auto& t:seen) if(ledger.finish_after_exit(t)==S::Ok) {worked=true; break;}
            if(worked) continue;
            Ledger::Selection sel{};
            S s=ledger.claim_under_lease(sel);
            if(s==S::Ok) {check(sel.dispatch==Ledger::Dispatch::CancelCleanup,"cleanup mode",round,203);worked=true;}
            else check(s==S::Busy||s==S::NoEligible,"closed claim status",round,204);
            if(!worked) break;
        }
        check(ledger.physically_retired(),"STOP physical cleanup converges",round,205);
        ++retired;
        for(const auto& t:seen){if(ledger.wake(t)==S::Stale)++stale_wakes;}
    }
    std::printf("STRESS_OK rounds=%zu actions=%zu stopped=%zu retired=%zu stale_wakes=%zu\n",retired,actions,closed,retired,stale_wakes);
}
