#include "oreslang/runtime/ActorSpawnQuotaGate.hpp"
#include <cstdlib>
#include <iostream>
#include <thread>
#include <atomic>
using namespace oreslang::runtime;
using Gate=ActorSpawnQuotaGate<2,2>;
static void check(bool ok) {if(!ok) std::abort();}
template<class F> void denied(F f,const char* code) {
    try {f();} catch(const std::logic_error& e){check(std::string(e.what())==code);return;}
    std::abort();
}
int main() {
    Gate::Charge unit{1,1,1,1}; Gate g(unit), foreign(unit);
    auto resources=std::make_shared<ActorResourceRegistry>(1,1);
    auto p=g.reserve(unit,resources);
    check(!g.try_enter_receive(p));
    denied([&]{foreign.begin_init(p);},"E_SPAWN_STALE_PERMIT");
    auto init=g.begin_init(p);
    denied([&]{g.begin_init(p);},"E_SPAWN_INIT_AUTHORITY");
    check(g.complete_init(init,true));
    denied([&]{g.complete_init(init,true);},"E_SPAWN_INIT_AUTHORITY");
    auto a=g.try_enter_receive(p).value(), b=g.try_enter_receive(p).value();
    denied([&]{g.try_enter_receive(p);},"E_SPAWN_FULL");
    auto live=resources->acquire_turn();
    g.retire(p);
    check(resources->try_reclaim().status==ActorResourceRegistry::Reclaim::busy);
    check(!g.try_enter_receive(p));
    denied([&]{g.release_after_reclaim(p);},"E_SPAWN_IN_FLIGHT");
    g.finish_receive(a);
    denied([&]{g.finish_receive(a);},"E_SPAWN_STALE_PERMIT");
    g.finish_receive(b);
    check(resources->try_reclaim().status==ActorResourceRegistry::Reclaim::busy);
    denied([&]{g.release_after_reclaim(p);},"E_SPAWN_PHYSICAL_RECLAIM");
    resources->release_turn(live);
    check(resources->try_reclaim().status==ActorResourceRegistry::Reclaim::complete);
    g.release_after_reclaim(p); check(g.usage()==Gate::Charge{});
    for(std::uint64_t i=2;i<10002;++i) {
        auto r=std::make_shared<ActorResourceRegistry>(1,i);
        auto q=g.reserve(unit,r);
        auto qi=g.begin_init(q);g.retire(q);
        check(r->try_reclaim().status==ActorResourceRegistry::Reclaim::busy);
        check(!g.complete_init(qi,true));
        check(r->try_reclaim().status==ActorResourceRegistry::Reclaim::complete);
        g.release_after_reclaim(q);
    }
    check(g.usage()==Gate::Charge{});
    std::cout<<"LLVM_SPAWN_QUOTA_HOST_PASS rollback=10000 registry_reclaim_gate=1 guest=0\n";
}
