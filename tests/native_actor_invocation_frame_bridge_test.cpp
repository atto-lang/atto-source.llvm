#include "oreslang/runtime/ActorInvocationFrameBridge.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

using Bridge=oreslang::runtime::ActorInvocationFrameBridge<4,8,96,192>;
using Status=Bridge::Status;
using FrameStatus=Bridge::FrameStatus;
using Kind=Bridge::Kind;

#define CHECK(x) do { if (!(x)) { \
    std::fprintf(stderr,"ACTOR_FRAME_BRIDGE_FAIL %s:%d: %s\n", \
       __FILE__,__LINE__,#x); std::abort(); \
} } while (0)

struct Fixture {
    OresNativeActorScheduler* scheduler=nullptr;
    OresNativeActorRef actors[2]{};
    std::unique_ptr<Bridge> frames;
    Bridge::Token a{},b{};
    Bridge::FrameRef root{},callee{};
    OresNativeWakeTicket wake{};
    OresNativeRetainedDataTicket retained{};
    std::atomic<int> a_suspended{0},a_resumed{0},b_processed{0};
    std::atomic<int> sibling_processed{0},wake_completed{0};
};

extern "C" void on_data(void* raw,std::size_t actor,std::uint64_t payload) {
    auto& s=*static_cast<Fixture*>(raw);
    CHECK(actor<2);
    CHECK(ores_native_actor_scheduler_is_current_actor(s.scheduler,s.actors[actor]));
    if (actor==1) {
        CHECK(payload==99);
        s.sibling_processed.fetch_add(1,std::memory_order_release);
        return;
    }
    CHECK(s.frames->has_actor_lease());
    Bridge::Selection selected{};
    if (payload==1) {
        CHECK(s.frames->admit(Kind::Mail,s.a)==Status::Ok);
        CHECK(s.frames->claim(selected)==Status::Ok &&
                selected.token.sequence==s.a.sequence);
        CHECK(s.frames->push(s.a,24,s.root).ok());
        CHECK(s.frames->push(s.a,16,s.callee).ok());
        const std::uint8_t saved[]={0x2a,0x11,0x09};
        CHECK(s.frames->write(s.a,s.callee,3,saved,3).ok());
        Bridge::Ledger::Snapshot ledger{};
        Bridge::Frames::Snapshot bytes{};
        CHECK(s.frames->snapshot(ledger,bytes));
        CHECK(bytes.live_frames==2 && bytes.charged_bytes==40);
        CHECK(ores_native_actor_scheduler_reserve_wake(
                s.scheduler,s.actors[0],s.actors[0],1,1,1,1,&s.wake));
        CHECK(s.frames->prepare_await(s.a)==Status::Ok);
        CHECK(s.frames->suspend(s.a)==Status::Ok);
        CHECK(ores_native_actor_scheduler_retain_current_data(
                s.scheduler,&s.retained));
        CHECK(s.retained.actor.generation==s.actors[0].generation);
        s.a_suspended.store(1,std::memory_order_release);
    } else if (payload==2) {
        // Native scheduler DATA B cannot run while A's prior receive lease
        // remains retained, even if a different physical worker is available.
        CHECK(s.a_resumed.load(std::memory_order_acquire)==1);
        CHECK(s.frames->admit(Kind::Mail,s.b)==Status::Ok);
        CHECK(s.frames->claim(selected)==Status::Ok);
        CHECK(selected.token.sequence==s.b.sequence);
        Bridge::FrameRef frame{};
        CHECK(s.frames->push(s.b,12,frame).ok());
        std::uint8_t unused=0;
        auto stale=s.frames->read(s.a,s.callee,0,&unused,1);
        CHECK(stale.admission==Status::Ok &&
                stale.frame==FrameStatus::Stale);
        CHECK(s.frames->pop(s.b,frame).ok());
        CHECK(s.frames->finish_after_guest_exit(s.b)==Status::Ok);
        s.b_processed.fetch_add(1,std::memory_order_release);
    } else {
        CHECK(false);
    }
}

extern "C" void on_wake(void* raw,std::size_t actor,
                        OresNativeWakeTicket ticket,OresNativeWakeOutcome result) {
    auto& s=*static_cast<Fixture*>(raw);
    CHECK(actor==0 && result==ORES_NATIVE_WAKE_READY);
    CHECK(ores_native_actor_scheduler_is_current_wake(s.scheduler,ticket));
    CHECK(ores_native_actor_scheduler_is_current_actor(s.scheduler,s.actors[0]));
    CHECK(s.frames->deliver_wake(s.a)==Status::Ok);
    Bridge::Selection selected{};
    CHECK(s.frames->claim(selected)==Status::Ok);
    CHECK(selected.token.sequence==s.a.sequence);
    std::uint8_t actual[3]={};
    CHECK(s.frames->read(s.a,s.callee,3,actual,3).ok());
    CHECK(actual[0]==0x2a && actual[1]==0x11 && actual[2]==0x09);
    CHECK(s.frames->pop(s.a,s.root).frame==FrameStatus::NoFrame);
    CHECK(s.frames->pop(s.a,s.callee).ok());
    CHECK(s.frames->pop(s.a,s.root).ok());
    CHECK(s.frames->finish_after_guest_exit(s.a)==Status::Ok);
    CHECK(s.frames->deliver_wake(s.a)==Status::Stale);
    s.a_resumed.fetch_add(1,std::memory_order_release);
    CHECK(ores_native_actor_scheduler_complete_retained_data(
            s.scheduler,s.retained));
    CHECK(!ores_native_actor_scheduler_complete_retained_data(
            s.scheduler,s.retained));
    s.wake_completed.fetch_add(1,std::memory_order_release);
}

static void until(std::atomic<int>& value,const char* name) {
    const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(8);
    while (!value.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now()>limit) {
            std::fprintf(stderr,"TIMEOUT %s\n",name);
            std::abort();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

int main() {
    Fixture s;
    s.scheduler=ores_native_actor_scheduler_open_with_wakes(
            2,8,2,on_data,nullptr,nullptr,on_wake,&s);
    CHECK(s.scheduler);
    s.actors[0]=ores_native_actor_scheduler_spawn(s.scheduler,0);
    s.actors[1]=ores_native_actor_scheduler_spawn(s.scheduler,1);
    CHECK(s.actors[0].scheduler_domain && s.actors[1].scheduler_domain);
    s.frames=std::make_unique<Bridge>(s.scheduler,s.actors[0],1234);

    Bridge::Token rejected{};
    CHECK(!s.frames->has_actor_lease());
    CHECK(s.frames->admit(Kind::Mail,rejected)==Status::Unauthorized);
    Bridge::Selection selection{};
    CHECK(s.frames->claim(selection)==Status::Unauthorized);
    Bridge::FrameRef frame{};
    CHECK(s.frames->push(s.a,1,frame).admission==Status::Unauthorized);
    CHECK(s.frames->finish_after_guest_exit(s.a,true)==Status::Unauthorized);

    CHECK(ores_native_actor_scheduler_send(s.scheduler,s.actors[0],1));
    until(s.a_suspended,"A suspended");
    CHECK(s.frames->deliver_wake(s.a)==Status::Unauthorized);
    CHECK(s.frames->read(s.a,s.callee,0,nullptr,0).admission==Status::Unauthorized);
    CHECK(ores_native_actor_scheduler_send(s.scheduler,s.actors[0],2));
    CHECK(ores_native_actor_scheduler_send(s.scheduler,s.actors[1],99));
    until(s.sibling_processed,"independent sibling actor progresses");
    CHECK(s.a_resumed.load(std::memory_order_acquire)==0);
    CHECK(s.b_processed.load(std::memory_order_acquire)==0);
    CHECK(ores_native_actor_scheduler_retained_data_count(
            s.scheduler,s.actors[0])==1);

    OresNativeWakeTicket forged=s.wake;
    ++forged.continuation_generation;
    CHECK(ores_native_actor_scheduler_publish_wake(
            s.scheduler,forged,ORES_NATIVE_WAKE_READY)==ORES_NATIVE_WAKE_INVALID);
    CHECK(ores_native_actor_scheduler_transfer_wake(s.scheduler,s.wake));
    CHECK(ores_native_actor_scheduler_publish_wake(
            s.scheduler,s.wake,ORES_NATIVE_WAKE_READY)==ORES_NATIVE_WAKE_PUBLISHED);
    until(s.wake_completed,"A resumes and retires original stack");
    until(s.b_processed,"B receives only after A retirement");
    CHECK(s.a_resumed.load()==1 && s.b_processed.load()==1);

    CHECK(ores_native_actor_scheduler_drain(s.scheduler));
    ores_native_actor_scheduler_retire(s.scheduler);
    CHECK(ores_native_actor_scheduler_try_destroy(s.scheduler));
    std::puts("ACTOR_INVOCATION_FRAME_NATIVE_SCHEDULER_BRIDGE_PASS");
}
