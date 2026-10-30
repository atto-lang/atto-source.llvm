// Actual NativeActorScheduler admissions, not hand-written carrier claims.
// This test requires linking NativeActorScheduler.c into the CTest target.
#include "oreslang/runtime/NativeRequestContext.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
using namespace oreslang::runtime;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while(false)
using Registry=NativeRequestContext<16>;
using Buffers=RequestSlotTable<16,128>;
static_assert(!std::is_constructible_v<Registry::Suspension,
    Registry::Handle,OresNativeActorRef,std::uint64_t,Registry::Lease&&>);
static_assert(!std::is_constructible_v<Registry::ExchangePin,
    NativeHttpExchange::Token,Registry::Lease&&>);

struct Suite {
    Registry contexts;
    Buffers buffers;
    std::array<NativeHttpExchange,15> exchanges{};
    std::array<OresNativeActorRef,15> actors{};
    std::array<Registry::Handle,15> requests{};
    std::array<Buffers::Handle,15> bodies{};
    std::array<std::optional<Registry::ExchangePin>,15> pins{};
    std::optional<Registry::Suspension> suspension;
    std::atomic<unsigned> ready{0},observed{0},bad{0};
    OresNativeActorScheduler* scheduler=nullptr;
    unsigned count=0;
    bool barrier=false;
    bool await_mode=false;
    bool revoke_on_turn=false;
    static Registry::Metadata info(unsigned n) noexcept {
        return {1000+n,2000+n,3000,987654321};
    }
    static void dispatch(void* raw,size_t slot,std::uint64_t message) noexcept {
        auto& s=*static_cast<Suite*>(raw);
        try {
            CHECK(slot<s.count);
            const auto owner=s.actors[slot];
            CHECK(ores_native_actor_scheduler_is_current_actor(s.scheduler,owner));
            OresNativeExecutionContext ctx{};
            CHECK(ores_native_actor_scheduler_current_execution_context(s.scheduler,&ctx));
            CHECK(ctx.allocation.owner==slot);
            auto& exchange=s.exchanges[slot];
            const auto token=exchange.token();
            CHECK(!Registry::current()); // carrier callback does not inherit stale TLS
            if(s.await_mode && message==2) {
                CHECK(s.suspension);
                CHECK(s.contexts.resume_admitted(s.scheduler,exchange,token,*s.suspension,[&]{
                    CHECK(Registry::current()->request_id==1000+slot);
                    s.observed.fetch_add(1);
                }));
            } else {
                CHECK(s.contexts.with_admitted_turn(s.scheduler,exchange,token,
                       s.requests[slot],owner,[&]{
                    CHECK(Registry::current()->request_id==1000+slot);
                    CHECK(!s.contexts.with_admitted_turn(nullptr,exchange,token,
                                        s.requests[slot],owner,[]{}));
                    CHECK(s.contexts.with_request_bytes(s.buffers,s.bodies[slot],
                            std::uint64_t(slot+1),[&](std::string_view bytes){
                        CHECK(bytes=="GET /actor");
                        if(s.revoke_on_turn){
                            CHECK(s.buffers.close(s.bodies[slot],std::uint64_t(slot+1)));
                            CHECK(s.buffers.available()==15);
                            CHECK(s.contexts.close(s.requests[slot],owner));
                            CHECK(!Registry::current());
                            CHECK(s.buffers.available()==15);
                        }
                    }));
                    if(s.await_mode && message==1){
                        s.suspension=s.contexts.suspend_current(exchange,token);
                        CHECK(s.suspension.has_value());
                    }
                    if(s.barrier){
                        s.ready.fetch_add(1);
                        const auto deadline=std::chrono::steady_clock::now()+
                                            std::chrono::seconds(8);
                        while(s.ready.load()<s.count &&
                              std::chrono::steady_clock::now()<deadline)
                            std::this_thread::yield();
                        CHECK(s.ready.load()==s.count);
                    }
                    s.observed.fetch_add(1);
                }));
            }
            CHECK(!Registry::current());
        }catch(...){s.bad.fetch_add(1);}
    }
    void begin(unsigned n,bool concurrent=false,bool await=false,bool revoke=false){
        count=n;barrier=concurrent;await_mode=await;revoke_on_turn=revoke;
        scheduler=ores_native_actor_scheduler_open(n,4,n,dispatch,this);
        CHECK(scheduler);
        for(unsigned i=0;i<n;++i){
            actors[i]=ores_native_actor_scheduler_spawn(scheduler,i);
            CHECK(actors[i].scheduler_domain && actors[i].generation);
            auto ctx=contexts.open(actors[i],info(i));CHECK(ctx);
            requests[i]=*ctx;
            auto ingest=buffers.begin(i+1);CHECK(ingest);
            std::memcpy(ingest->data(),"GET /actor",10);
            bodies[i]=ingest->publish(10);
            CHECK(bodies[i].value);
            pins[i]=contexts.attach_exchange(exchanges[i],requests[i],actors[i]);
            CHECK(pins[i] && pins[i]->token.request_registry==contexts.registry_id());
            CHECK(exchanges[i].claim(pins[i]->token));
        }
    }
    void send_all(unsigned message=0){
        for(unsigned i=0;i<count;++i)
            CHECK(ores_native_actor_scheduler_try_send(
                scheduler,actors[i],message)==ORES_NATIVE_SEND_ACCEPTED);
        CHECK(ores_native_actor_scheduler_drain(scheduler));
        CHECK(bad.load()==0);
    }
    void end(){
        if(suspension)suspension.reset();
        for(unsigned i=0;i<count;++i){
            const auto token=pins[i]->token;
            CHECK(exchanges[i].complete(token));
            CHECK(exchanges[i].reclaim(token));
            if(!revoke_on_turn){
                CHECK(contexts.close(requests[i],actors[i]));
                CHECK(buffers.close(bodies[i],i+1));
            }
            CHECK(contexts.available()==16-count+i);
            pins[i].reset(); // only legal once exchange returned to reactor phase
            CHECK(contexts.available()==16-count+i+1);
        }
        CHECK(buffers.available()==16);
        for(unsigned i=0;i<count;++i)CHECK(ores_native_actor_scheduler_stop(scheduler,actors[i]));
        ores_native_actor_scheduler_retire(scheduler);
        CHECK(ores_native_actor_scheduler_try_destroy(scheduler));
        scheduler=nullptr;
    }
};
int main(){
    // A raw matching token never confers current execution authority.
    {
        Suite s;s.begin(1);
        auto tok=s.pins[0]->token;
        CHECK(!s.contexts.with_admitted_turn(s.scheduler,s.exchanges[0],tok,
               s.requests[0],s.actors[0],[]{}));
        CHECK(!s.contexts.with_admitted_turn(nullptr,s.exchanges[0],tok,
               s.requests[0],s.actors[0],[]{}));
        CHECK(!s.contexts.suspend_current(s.exchanges[0],tok));
        CHECK(!Registry::current());
        auto wrong=s.actors[0];wrong.generation++;
        CHECK(!s.contexts.with_admitted_turn(s.scheduler,s.exchanges[0],tok,
               s.requests[0],wrong,[]{}));
        CHECK(!s.exchanges[0].claim({tok.generation,tok.actor,
                tok.request_context,tok.request_registry+1}));
        s.send_all();CHECK(s.observed==1);s.end();
    }
    // Real native scheduler DATA callbacks on 3, 8 and 15 worker carriers;
    // distinct actor owners, never illicit parallel DATA on one serial actor.
    for(unsigned n:{3u,8u,15u}){
        Suite s;s.begin(n,true);s.send_all();
        CHECK(s.observed==n);s.end();
    }
    // Suspension token is created on actor turn, remains live after carrier
    // return, and resumes via fresh scheduler admission (not callback thread).
    {
        Suite s;s.begin(1,false,true);s.send_all(1);
        CHECK(s.suspension && !Registry::current());
        CHECK(!s.contexts.resume_admitted(s.scheduler,s.exchanges[0],
                    s.pins[0]->token,*s.suspension,[]{}));
        s.send_all(2);CHECK(s.observed==2);s.end();
    }
    // Cancel while a transport borrow is live: old read view survives,
    // subsequent context lookups and new borrows reject, both slots stay
    // allocated until the correct physical leases are released.
    {
        Suite s;s.begin(1,false,false,true);s.send_all();
        CHECK(s.observed==1 && s.contexts.available()==15);
        CHECK(s.buffers.available()==16);
        CHECK(!s.contexts.retain(s.requests[0],s.actors[0]));
        s.end();
    }
    // Stale handle cannot alias a later actor incarnation or request.
    {
        Suite s;const OresNativeActorRef owner{31,5,7};
        auto a=s.contexts.open(owner,Suite::info(1));CHECK(a);
        CHECK(s.contexts.close(*a,owner));
        auto b=s.contexts.open({31,5,8},Suite::info(1));CHECK(b);
        CHECK(a->value!=b->value);
        CHECK(!s.contexts.lookup(*a,owner));
        CHECK(!s.contexts.lookup(*b,owner));
        CHECK(s.contexts.close(*b,{31,5,8}));
    }
}
