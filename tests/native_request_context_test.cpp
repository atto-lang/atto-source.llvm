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
    NativeHttpExchange second_exchange;
    std::array<OresNativeActorRef,15> actors{};
    std::array<Registry::Handle,15> requests{};
    std::array<Buffers::Handle,15> bodies{};
    std::array<std::optional<Registry::BoundExchange<16,128>>,15> pins{};
    std::optional<Registry::Suspension> suspension;
    std::optional<Registry::BoundExchange<16,128>> second_pin;
    Registry::Handle second_request{};
    Buffers::Handle second_body{};
    std::atomic<unsigned> ready{0},observed{0},bad{0};
    OresNativeActorScheduler* scheduler=nullptr;
    unsigned count=0;
    bool barrier=false;
    bool await_mode=false;
    bool revoke_on_turn=false;
    bool reject_resume=false;
    unsigned mode=0;
    std::atomic<unsigned> rejected{0};
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
            if(s.second_pin && message==s.second_pin->token().dispatch_ticket) {
                const auto second=s.second_pin->token();
                CHECK(!s.contexts.with_admitted_turn(s.scheduler,exchange,token,
                       s.requests[slot],owner,[]{throw std::runtime_error("wrong request");}));
                CHECK(s.contexts.with_admitted_turn(s.scheduler,s.second_exchange,
                      second,s.second_request,owner,[&]{
                    CHECK(Registry::current()->request_id==1099);
                    CHECK(!s.contexts.with_request_bytes(*s.pins[slot],
                        [&](std::string_view){throw std::runtime_error("cross read");}));
                    CHECK(s.contexts.with_request_bytes(*s.second_pin,
                        [&](std::string_view bytes){CHECK(bytes=="GET /actor");}));
                    ++s.observed;
                }));
                CHECK(!Registry::current());
                return;
            }
            if(message!=token.dispatch_ticket) {
                CHECK(!s.contexts.with_admitted_turn(s.scheduler,exchange,token,
                      s.requests[slot],owner,[]{throw std::runtime_error("wrong ticket admitted");}));
                CHECK(!Registry::current());
                ++s.rejected;
                return;
            }
            if(s.await_mode && s.mode==2) {
                CHECK(s.suspension);
                const bool resumed=s.contexts.resume_admitted(
                    s.scheduler,exchange,token,*s.suspension,[&]{
                    CHECK(Registry::current()->request_id==1000+slot);
                    s.observed.fetch_add(1);
                });
                CHECK(resumed!=s.reject_resume);
            } else {
                CHECK(s.contexts.with_admitted_turn(s.scheduler,exchange,token,
                       s.requests[slot],owner,[&]{
                    CHECK(Registry::current()->request_id==1000+slot);
                    if(s.count>1){
                        const auto other=(slot+1)%s.count;
                        CHECK(!s.contexts.with_request_bytes(*s.pins[other],
                            [&](std::string_view){throw std::runtime_error("cross request bleed");}));
                        CHECK(Registry::current()->request_id==1000+slot);
                    }
                    CHECK(!s.contexts.with_admitted_turn(nullptr,exchange,token,
                                        s.requests[slot],owner,[]{}));
                    CHECK(s.contexts.with_request_bytes(*s.pins[slot],
                            [&](std::string_view bytes){
                        CHECK(bytes=="GET /actor");
                        if(s.revoke_on_turn){
                            CHECK(s.buffers.close(s.bodies[slot],std::uint64_t(slot+1)));
                            CHECK(s.buffers.available()==15);
                            CHECK(s.contexts.close(s.requests[slot],owner));
                            CHECK(!Registry::current());
                            CHECK(s.buffers.available()==15);
                        }
                    }));
                    if(s.mode==3){
                        // A second HTTP request for the SAME actor must be
                        // rejected on this message's scheduler DATA ticket.
                        NativeHttpExchange inner;
                        auto inner_handle=s.contexts.open(owner,info(99));
                        CHECK(inner_handle);
                        auto ingress=s.buffers.begin(slot+1);CHECK(ingress);
                        std::memcpy(ingress->data(),"GET /actor",10);
                        const auto inner_body=ingress->publish(10);
                        auto inner_pin=s.contexts.attach_exchange_with_buffer(
                            inner,*inner_handle,owner,s.buffers,inner_body,slot+1);
                        CHECK(inner_pin);
                        const auto inner_token=inner_pin->token();
                        CHECK(inner.claim(inner_token));
                        CHECK(inner_token.dispatch_ticket!=token.dispatch_ticket);
                        CHECK(!s.contexts.with_admitted_turn(
                            s.scheduler,inner,inner_token,*inner_handle,owner,[&]{
                                throw std::runtime_error("other request entered");
                            }));
                        CHECK(!s.contexts.with_request_bytes(*inner_pin,
                           [&](std::string_view){throw std::runtime_error("other request bytes");}));
                        CHECK(Registry::current()->request_id==1000+slot);
                        // Reentrant lexical scope for the SAME authenticated
                        // request still restores properly on exception.
                        bool caught=false;
                        try{
                            CHECK(s.contexts.with_admitted_turn(
                                s.scheduler,exchange,token,s.requests[slot],owner,[&]{
                                CHECK(Registry::current()->request_id==1000+slot);
                                throw std::runtime_error("inner failure");
                            }));
                        }catch(const std::runtime_error& e){
                            caught=std::string_view(e.what())=="inner failure";
                        }
                        CHECK(caught);
                        CHECK(Registry::current()->request_id==1000+slot);
                        CHECK(inner.complete(inner_token));
                        CHECK(inner.reclaim(inner_token));
                        CHECK(s.contexts.close(*inner_handle,owner));
                        CHECK(s.buffers.close(inner_body,slot+1));
                        inner_pin.reset();
                    }
                    if(s.await_mode && s.mode==1){
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
        CHECK(ores_native_actor_scheduler_prestart_carriers(scheduler));
        CHECK(ores_native_actor_scheduler_all_carriers_started(scheduler));
        for(unsigned i=0;i<n;++i){
            actors[i]=ores_native_actor_scheduler_spawn(scheduler,i);
            CHECK(actors[i].scheduler_domain && actors[i].generation);
            auto ctx=contexts.open(actors[i],info(i));CHECK(ctx);
            requests[i]=*ctx;
            auto ingest=buffers.begin(i+1);CHECK(ingest);
            std::memcpy(ingest->data(),"GET /actor",10);
            bodies[i]=ingest->publish(10);
            CHECK(bodies[i].value);
            auto pin=contexts.attach_exchange_with_buffer(exchanges[i],
                       requests[i],actors[i],buffers,bodies[i],i+1);
            CHECK(pin);
            pins[i].emplace(std::move(*pin));
            CHECK(pins[i]->token().request_registry==contexts.registry_id());
            CHECK(exchanges[i].claim(pins[i]->token()));
        }
    }
    void send_all(unsigned next_mode=0){
        mode=next_mode;
        for(unsigned i=0;i<count;++i)
            CHECK(ores_native_actor_scheduler_try_send(
                scheduler,actors[i],pins[i]->token().dispatch_ticket)
                ==ORES_NATIVE_SEND_ACCEPTED);
        CHECK(ores_native_actor_scheduler_drain(scheduler));
        CHECK(bad.load()==0);
    }
    void end(){
        if(suspension)suspension.reset();
        for(unsigned i=0;i<count;++i){
            const auto token=pins[i]->token();
            CHECK(exchanges[i].complete(token));
            CHECK(exchanges[i].reclaim(token));
            CHECK(!exchanges[i].enqueue(actors[i])); // physical pin still active
            if(!revoke_on_turn){
                CHECK(contexts.close(requests[i],actors[i]));
                CHECK(buffers.close(bodies[i],i+1));
            }
            CHECK(contexts.available()==16-count+i);
            CHECK(buffers.available()==16-count+i);
            pins[i].reset(); // only legal once exchange returned to reactor phase
            CHECK(contexts.available()==16-count+i+1);
            CHECK(buffers.available()==16-count+i+1);
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
        auto tok=s.pins[0]->token();
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
        CHECK(ores_native_actor_scheduler_try_send(
             s.scheduler,s.actors[0],tok.dispatch_ticket+1)
             ==ORES_NATIVE_SEND_ACCEPTED);
        CHECK(ores_native_actor_scheduler_drain(s.scheduler));
        CHECK(s.observed==0 && s.rejected==1 && s.bad==0);
        s.send_all();CHECK(s.observed==1);s.end();
    }
    // Real native scheduler DATA callbacks on 3, 8 and 15 worker carriers;
    // distinct actor owners, never illicit parallel DATA on one serial actor.
    for(unsigned n:{3u,8u,15u}){
        Suite s;s.begin(n,true);s.send_all();
        CHECK(s.observed==n);s.end();
    }
    // Two distinct HTTP requests use the SAME SERIAL actor but carry separate
    // exact scheduler DATA envelopes. Both can be queued at the same time;
    // each executes in the actor's own serialized DATA lane, never with the
    // other request's context or borrowed request bytes.
    {
        Suite s;s.begin(1);
        auto second=s.contexts.open(s.actors[0],Suite::info(99));
        CHECK(second);
        s.second_request=*second;
        auto in=s.buffers.begin(1);CHECK(in);
        std::memcpy(in->data(),"GET /actor",10);
        s.second_body=in->publish(10);
        auto pin=s.contexts.attach_exchange_with_buffer(s.second_exchange,
                     s.second_request,s.actors[0],s.buffers,s.second_body,1);
        CHECK(pin);
        s.second_pin.emplace(std::move(*pin));
        const auto first_ticket=s.pins[0]->token().dispatch_ticket;
        const auto second_ticket=s.second_pin->token().dispatch_ticket;
        CHECK(first_ticket!=second_ticket);
        CHECK(s.second_exchange.claim(s.second_pin->token()));
        CHECK(ores_native_actor_scheduler_try_send(s.scheduler,s.actors[0],
              first_ticket)==ORES_NATIVE_SEND_ACCEPTED);
        CHECK(ores_native_actor_scheduler_try_send(s.scheduler,s.actors[0],
              second_ticket)==ORES_NATIVE_SEND_ACCEPTED);
        CHECK(ores_native_actor_scheduler_drain(s.scheduler));
        CHECK(s.observed==2 && s.bad==0 && s.rejected==0);
        const auto t=s.second_pin->token();
        CHECK(s.second_exchange.complete(t));
        CHECK(s.second_exchange.reclaim(t));
        CHECK(s.contexts.close(s.second_request,s.actors[0]));
        CHECK(s.buffers.close(s.second_body,1));
        s.second_pin.reset();
        CHECK(s.contexts.available()==15 && s.buffers.available()==15);
        s.end();
    }
    // Exception unwinding restores the parent's current request on the same
    // scheduler-authorized carrier; no nested metadata bleeds to next turn.
    { Suite s;s.begin(1);s.send_all(3);CHECK(s.observed==1);s.end(); }
    // Suspension token is created on actor turn, remains live after carrier
    // return, and resumes via fresh scheduler admission (not callback thread).
    {
        Suite s;s.begin(1,false,true);s.send_all(1);
        CHECK(s.suspension && !Registry::current());
        CHECK(!s.contexts.resume_admitted(s.scheduler,s.exchanges[0],
                    s.pins[0]->token(),*s.suspension,[]{}));
        s.send_all(2);CHECK(s.observed==2);s.end();
    }
    // A suspended request cancelled before resumption cannot be resurrected
    // by even a correctly admitted replacement scheduler turn.
    {
        Suite s;s.begin(1,false,true);s.send_all(1);
        CHECK(s.suspension && s.contexts.available()==15);
        CHECK(s.contexts.close(s.requests[0],s.actors[0]));
        s.reject_resume=true;
        s.send_all(2);CHECK(s.observed==1 && s.bad==0);
        // Body is still pinned until exchange/continuation terminal reclamation.
        CHECK(s.buffers.close(s.bodies[0],1));
        s.revoke_on_turn=true;s.end();
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
