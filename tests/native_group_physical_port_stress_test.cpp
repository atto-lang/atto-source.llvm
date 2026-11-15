// Exact host components: NativeActorGroup, BoundedActorPort, fixed slots and
// ActorMailboxCompletion. This deliberately does NOT exercise PriorityActorInbox.
#include "oreslang/runtime/ActorGroupLifecycle.hpp"
#include "oreslang/runtime/BoundedActorPort.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <thread>
#define CHECK(x) do { if (!(x)) throw std::runtime_error("group/port oracle: " #x); } while(false)
using oreslang::runtime::NativeActorGroup;
using oreslang::runtime::BoundedActorPort;
using oreslang::runtime::ActorMailboxCompletion;

static void control_preempts_user_turn_and_ack_fences() {
    NativeActorGroup group(710001,1);
    auto actor=group.adopt(710001,1,1,[]{});
    CHECK(group.mark_ready(actor));
    auto finalizer=group.runtime_finalizer(actor);
    auto ledger=std::make_shared<ActorMailboxCompletion>(4);
    BoundedActorPort<int> port(4,64,ledger);
    int first=10,second=20,third=30;
    for (int *p : {&first,&second,&third}) {
        actor.retain_resource_loan();
        CHECK(port.offer(std::move(*p),8,static_cast<std::uint32_t>(*p)));
    }
    CHECK(actor.outstanding_resource_loans()==3);
    CHECK(group.enqueue_control(actor,[]{}));
    bool user_ran=false;
    CHECK(group.run_user_turn(actor,[&]{user_ran=true;})==NativeActorGroup::UserTurn::busy);
    CHECK(!user_ran);
    CHECK(group.run_control_turn(actor)==NativeActorGroup::ControlTurn::executed);
    CHECK(group.run_user_turn(actor,[&]{
        auto found=port.try_receive_tagged(30,4);
        CHECK(found.status==BoundedActorPort<int>::SelectiveStatus::Matched);
        CHECK(found.delivery && found.delivery->sequence()==3 && found.delivery->value()==30);
        CHECK(found.delivery->acknowledge_released());
        ledger->complete(found.delivery->sequence());
        actor.release_resource_loan();
        user_ran=true;
    })==NativeActorGroup::UserTurn::executed);
    CHECK(user_ran && ledger->watermark()==0 && port.used_frames()==2);
    group.close();
    CHECK(finalizer.ready_for_actor_turn_quiescence());
    CHECK(!finalizer.ready_for_resource_reclamation());
    while(auto owned=port.try_receive()) {
        CHECK(owned->acknowledge_released());
        ledger->complete(owned->sequence(),true);
        actor.release_resource_loan();
    }
    CHECK(ledger->watermark()==3);
    CHECK(ledger->drain_status(3)==ActorMailboxCompletion::DrainStatus::Aborted);
    CHECK(finalizer.ready_for_resource_reclamation());
    CHECK(finalizer.finalized());
    actor.get_done().get();
    port.close();
}

static void race_stop_against_resource_admission() {
    for(int trial=0;trial<600;trial++) {
        std::uint64_t runtime=720000+static_cast<std::uint64_t>(trial);
        NativeActorGroup group(runtime,1);
        auto actor=group.adopt(runtime,1,1,[]{});
        CHECK(group.mark_ready(actor));
        auto finalizer=group.runtime_finalizer(actor);
        auto ledger=std::make_shared<ActorMailboxCompletion>(2);
        BoundedActorPort<int> port(1,8,ledger);
        auto owned=std::make_unique<int>(trial);
        std::atomic<int> gate{0};
        bool admitted=false,denied=false;
        std::thread sender([&]{
            gate.fetch_add(1,std::memory_order_acq_rel);
            while(gate.load(std::memory_order_acquire)<2)std::this_thread::yield();
            try {
                actor.retain_resource_loan();
                admitted=port.offer_owned(owned,8,1);
                if(!admitted) actor.release_resource_loan();
            } catch(const std::logic_error&) { denied=true; }
        });
        std::thread stop([&]{
            gate.fetch_add(1,std::memory_order_acq_rel);
            while(gate.load(std::memory_order_acquire)<2)std::this_thread::yield();
            group.stop_workers();
        });
        sender.join(); stop.join();
        CHECK(admitted!=denied);
        CHECK(admitted ? !owned : bool(owned));
        CHECK(port.used_frames()==static_cast<std::size_t>(admitted));
        CHECK(ledger->snapshot()==static_cast<std::uint64_t>(admitted));
        group.close();
        CHECK(finalizer.ready_for_actor_turn_quiescence());
        if(admitted) {
            CHECK(!finalizer.ready_for_resource_reclamation());
            auto receipt=port.try_receive();
            CHECK(receipt && receipt->value()==trial);
            CHECK(receipt->acknowledge_released());
            ledger->complete(receipt->sequence(),true);
            actor.release_resource_loan();
        }
        CHECK(finalizer.ready_for_resource_reclamation());
        CHECK(finalizer.finalized());
        actor.get_done().get();
        port.close();
    }
}
int main(){control_preempts_user_turn_and_ack_fences();race_stop_against_resource_admission();}
