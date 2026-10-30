#include "oreslang/runtime/NativeRequestContext.hpp"
#include <atomic>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace oreslang::runtime;
#define CHECK(x) do {if(!(x))throw std::runtime_error(#x);}while(false)
using Registry=NativeRequestContext<16>;
static const OresNativeActorRef actor{21,7,3}, newer{21,7,4}, foreign{22,7,3};
static Registry::Metadata info(unsigned n){return {n,n+100,1,987654321};}
int main(){
    Registry contexts;
    NativeHttpExchange exch;
    const auto h=contexts.open(actor,info(1));CHECK(h.has_value());
    CHECK(!contexts.open(actor,info(1))); // tenant/request duplicate
    CHECK(exch.enqueue(actor,h->value,contexts.registry_id()));
    const auto token=exch.token();
    CHECK(token.request_context==h->value);
    CHECK(!exch.claim({token.generation,newer,token.request_context,token.request_registry}));
    CHECK(!exch.claim({token.generation,actor,token.request_context+1,token.request_registry}));
    Registry other_registry;
    CHECK(other_registry.registry_id()!=contexts.registry_id());
    CHECK(!exch.claim({token.generation,actor,token.request_context,other_registry.registry_id()}));
    CHECK(exch.claim(token));
    CHECK(!contexts.with_admitted_turn(exch,token,*h,newer,[]{}));
    CHECK(!contexts.with_admitted_turn(exch,token,*h,foreign,[]{}));
    CHECK(!Registry::current());
    CHECK(contexts.with_admitted_turn(exch,token,*h,actor,[&]{
        CHECK(Registry::current()->request_id==1);
        NativeHttpExchange nested;
        const auto h2=contexts.open(actor,info(2));CHECK(h2);
        CHECK(nested.enqueue(actor,h2->value,contexts.registry_id()));const auto tok2=nested.token();
        CHECK(nested.claim(tok2));
        try {
            CHECK(contexts.with_admitted_turn(nested,tok2,*h2,actor,[&]{
                CHECK(Registry::current()->request_id==2);
                throw std::runtime_error("callback failure");
            }));
        } catch(const std::runtime_error&){}
        CHECK(Registry::current()->request_id==1);
        CHECK(contexts.close(*h2,actor));
        CHECK(nested.complete(tok2));CHECK(nested.reclaim(tok2));
    }));
    CHECK(!Registry::current());
    // Create a live continuation only within the actual admitted turn.
    CHECK(!contexts.suspend_current(exch,token));
    std::optional<Registry::Suspension> awaiting;
    CHECK(contexts.with_admitted_turn(exch,token,*h,actor,[&]{
        awaiting=contexts.suspend_current(exch,token);
        CHECK(awaiting);
    }));
    CHECK(awaiting);
    // Physical carrier exits; another thread resumes from the retained link.
    std::thread resumed([&]{
        CHECK(!Registry::current());
        CHECK(contexts.resume_admitted(exch,token,*awaiting,[&]{
            CHECK(Registry::current()->trace_id==101);
        }));
        CHECK(!Registry::current());
    });resumed.join();
    CHECK(exch.complete(token));
    CHECK(contexts.close(*h,actor)); // pin remains, no reuse
    CHECK(contexts.available()==15);
    CHECK(!contexts.with_admitted_turn(exch,token,*h,actor,[]{}));
    CHECK(!contexts.retain(*h,actor));
    CHECK(!contexts.resume_admitted(exch,token,*awaiting,[]{}));
    awaiting.reset();CHECK(contexts.available()==16);
    CHECK(exch.reclaim(token));
    const auto h3=contexts.open(newer,info(1));CHECK(h3);
    CHECK(!contexts.lookup(*h,actor));CHECK(!contexts.lookup(*h3,actor));
    CHECK(contexts.lookup(*h3,newer)->request_id==1);
    CHECK(contexts.close(*h3,newer));
    CHECK(contexts.available()==16);
    // 3/8/15 REAL OS threads. Each acts for a separate actor; this does not
    // claim that one default serial actor permits 15 simultaneous DATA turns.
    for(unsigned concurrency:{3u,8u,15u}){
        std::vector<NativeHttpExchange> exchanges(concurrency);
        std::vector<Registry::Handle> handles;
        std::vector<OresNativeActorRef> owners;
        handles.reserve(concurrency);
        owners.reserve(concurrency);
        for(unsigned i=0;i<concurrency;++i){
            const OresNativeActorRef owner{actor.scheduler_domain,actor.actor_slot+i,actor.generation};
            owners.push_back(owner);
            auto slot=contexts.open(owner,info(100+i));CHECK(slot);
            handles.push_back(*slot);CHECK(exchanges[i].enqueue(owner,slot->value,contexts.registry_id()));
            CHECK(exchanges[i].claim(exchanges[i].token()));
        }
        std::atomic<unsigned> ready{0},observed{0};
        std::vector<std::thread> threads;
        for(unsigned i=0;i<concurrency;++i)threads.emplace_back([&,i]{
            const auto tok=exchanges[i].token();
            CHECK(contexts.with_admitted_turn(exchanges[i],tok,handles[i],owners[i],[&]{
                ++ready;while(ready.load()<concurrency)std::this_thread::yield();
                CHECK(Registry::current()->request_id==100+i);
                ++observed;
            }));
            CHECK(!Registry::current());
        });
        for(auto& t:threads) t.join();
        CHECK(observed==concurrency);
        for(unsigned i=0;i<concurrency;++i){
            CHECK(exchanges[i].complete(exchanges[i].token()));
            CHECK(contexts.close(handles[i],owners[i]));
            CHECK(exchanges[i].reclaim(exchanges[i].token()));
        }
        CHECK(contexts.available()==16);
    }
}
