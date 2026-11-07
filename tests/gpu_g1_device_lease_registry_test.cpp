#include "oreslang/backend/GpuG1DeviceLeaseRegistry.hpp"
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace oreslang::gpu::g1;
using Ledger = DeviceLeaseRegistry;
static std::uint64_t checked = 0;
static void check(bool yes) { ++checked; if (!yes) std::abort(); }
template <typename F> static void rejects(F f) {
    bool rejected = false;
    try { f(); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected);
}
int main() {
    const Ledger::Limits limits{4096, 8, 32, 8, 1024, 16, 16};
    Ledger ledger(limits);
    const auto queue=ledger.provider_register_queue(1,2,3);
    const auto alternate=ledger.provider_register_queue(1,2,4);
    const auto x=ledger.provider_register_allocation(1,2,10,256);
    const auto y=ledger.provider_register_allocation(1,2,11,256);
    const auto z=ledger.provider_register_allocation(1,2,12,256);
    auto rx=Ledger::View{x,0,128,Ledger::Access::Read};
    auto ry=Ledger::View{y,0,128,Ledger::Access::Read};
    auto wz=Ledger::View{z,0,128,Ledger::Access::Write};
    const auto ticket=ledger.provider_submit(queue,{rx,ry,wz});
    check(ticket>0);
    check(ledger.snapshot().inflight==1);
    rejects([&]{ledger.provider_retire_allocation(z);});
    rejects([&]{ledger.provider_retire_queue(queue);});
    rejects([&]{(void)ledger.provider_submit(alternate,{wz});});
    check(ledger.snapshot().inflight==1);
    auto rz=Ledger::View{z,0,128,Ledger::Access::Read};
    rejects([&]{(void)ledger.provider_submit(alternate,{rz});});
    // A read-only loan may overlap reads while a separate mutable loan waits.
    const auto shared=ledger.provider_submit(alternate,{rx});
    check(ledger.snapshot().inflight==2);
    rejects([&]{ledger.provider_ack_event(alternate,ticket,Ledger::Event::Completed);});
    ledger.provider_request_cancel(queue,ticket);
    check(ledger.snapshot().cancelled_pending==1);
    rejects([&]{ledger.provider_retire_allocation(x);});
    rejects([&]{(void)ledger.provider_submit(alternate,{wz});});
    ledger.provider_ack_event(queue,ticket,Ledger::Event::CancelledQuiescent);
    check(ledger.snapshot().cancelled_pending==0);
    check(ledger.snapshot().inflight==1);
    rejects([&]{ledger.provider_ack_event(queue,ticket,Ledger::Event::Completed);});
    rejects([&]{ledger.provider_ack_event(alternate,shared,Ledger::Event::CancelledQuiescent);});
    ledger.provider_ack_event(alternate,shared,Ledger::Event::Completed);
    check(ledger.snapshot().inflight==0);
    ledger.provider_retire_queue(queue);
    rejects([&]{(void)ledger.provider_submit(queue,{wz});});
    const auto newQueue=ledger.provider_register_queue(1,2,3);
    check(newQueue.generation()==queue.generation()+1);
    rejects([&]{ledger.provider_request_cancel(queue,shared);});
    ledger.provider_retire_allocation(z);
    rejects([&]{(void)ledger.provider_submit(newQueue,{wz});});
    const auto newZ=ledger.provider_register_allocation(1,2,12,256);
    check(newZ.generation()==z.generation()+1);
    rejects([&]{(void)ledger.provider_submit(newQueue,{wz});});
    const auto wz2=Ledger::View{newZ,0,128,Ledger::Access::Write};
    const auto t2=ledger.provider_submit(newQueue,{rx,ry,wz2});
    ledger.provider_ack_event(newQueue,t2,Ledger::Event::Completed);
    check(ledger.snapshot().inflight==0);
    rejects([&]{(void)ledger.provider_submit(newQueue,{{newZ,252,8,Ledger::Access::Write}});});
    rejects([&]{(void)ledger.provider_submit(newQueue,{{newZ,2,4,Ledger::Access::Write}});});
    rejects([&]{(void)ledger.provider_submit(newQueue,{{newZ,0,0,Ledger::Access::Write}});});
    rejects([&]{(void)ledger.provider_submit(newQueue,{});});
    rejects([&]{(void)ledger.provider_submit(newQueue,{{newZ,0,1028,Ledger::Access::Write}});});
    rejects([&]{(void)ledger.provider_submit(newQueue,{{newZ,0,128,static_cast<Ledger::Access>(99)}});});
    rejects([&]{(void)ledger.provider_submit(newQueue,{rx,ry,wz2,wz2});});
    const auto other=ledger.provider_register_allocation(2,2,21,256);
    rejects([&]{(void)ledger.provider_submit(newQueue,{{other,0,128,Ledger::Access::Read}});});
    // Registry namespace is a capability boundary, even for matching IDs.
    Ledger separate(limits);
    const auto foreignQueue=separate.provider_register_queue(1,2,3);
    rejects([&]{(void)ledger.provider_submit(foreignQueue,{rx});});
    const auto foreignAllocation=separate.provider_register_allocation(1,2,10,256);
    rejects([&]{(void)ledger.provider_submit(newQueue,{{foreignAllocation,0,128,Ledger::Access::Read}});});
    // Tombstoned identities remain bounded even after repeated retire/recreate.
    Ledger bounded({512,1,2,2,128,1,1});
    const auto bq=bounded.provider_register_queue(8,8,8);
    const auto ba=bounded.provider_register_allocation(8,8,8,64);
    rejects([&]{(void)bounded.provider_register_queue(8,8,9);});
    rejects([&]{(void)bounded.provider_register_allocation(8,8,9,64);});
    auto br=Ledger::View{ba,0,64,Ledger::Access::Read};
    auto bw=Ledger::View{ba,0,64,Ledger::Access::Write};
    rejects([&]{(void)bounded.provider_submit(bq,{br,bw});});
    const auto inflight1=bounded.provider_submit(bq,{br});
    const auto inflight2=bounded.provider_submit(bq,{br});
    rejects([&]{(void)bounded.provider_submit(bq,{br});});
    rejects([&]{bounded.provider_retire_queue(bq);});
    bounded.provider_ack_event(bq,inflight1,Ledger::Event::Completed);
    bounded.provider_ack_event(bq,inflight2,Ledger::Event::Completed);
    bounded.provider_retire_allocation(ba);
    bounded.provider_retire_queue(bq);
    const auto bq2=bounded.provider_register_queue(8,8,8);
    const auto ba2=bounded.provider_register_allocation(8,8,8,64);
    check(bq2.generation()==2 && ba2.generation()==2);
    rejects([&]{(void)bounded.provider_submit(bq2,{br});});
    check(bounded.snapshot().tracked_queue_ids==1);
    check(bounded.snapshot().tracked_allocation_ids==1);
    rejects([&]{(void)bounded.provider_register_queue(8,8,9);});
    rejects([&]{(void)bounded.provider_register_allocation(8,8,9,64);});
    // Resident quota is tenant-scoped and maintained on retirement.
    rejects([&]{(void)ledger.provider_register_allocation(1,2,50,4096);});
    check(ledger.snapshot().resident_bytes==1024);
    // Two submissions for disjoint write intervals may coexist.
    const auto first=ledger.provider_submit(newQueue,{{newZ,0,64,Ledger::Access::Write}});
    const auto second=ledger.provider_submit(alternate,{{newZ,64,64,Ledger::Access::Write}});
    check(ledger.snapshot().inflight==2);
    rejects([&]{(void)ledger.provider_submit(alternate,{{newZ,60,8,Ledger::Access::Write}});});
    ledger.provider_ack_event(newQueue,first,Ledger::Event::Completed);
    ledger.provider_ack_event(alternate,second,Ledger::Event::Completed);
    // Exactly one winner when eight threads compete for the same exclusive view.
    std::atomic<bool> start{false};
    std::atomic<int> successes{0},denied{0};
    std::atomic<std::uint64_t> winner{0};
    std::vector<std::thread> workers;
    for(int i=0;i<8;++i) workers.emplace_back([&]{
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        try { const auto t=ledger.provider_submit(newQueue,{wz2});
              winner.store(t,std::memory_order_release);successes.fetch_add(1); }
        catch (const std::invalid_argument&) { denied.fetch_add(1); }
    });
    start.store(true,std::memory_order_release);
    for(auto& worker:workers)worker.join();
    check(successes==1);
    check(denied==7);
    check(ledger.snapshot().inflight==1);
    ledger.provider_request_cancel(newQueue,winner.load());
    rejects([&]{ledger.provider_retire_allocation(newZ);});
    ledger.provider_ack_event(newQueue,winner.load(),Ledger::Event::Completed);
    check(ledger.snapshot().inflight==0);
    // Deterministic repeated ownership/cancellation fence cycle.
    for(int i=0;i<128;++i) {
        auto tx=ledger.provider_submit(newQueue,{wz2});
        rejects([&]{(void)ledger.provider_submit(alternate,{wz2});});
        ledger.provider_request_cancel(newQueue,tx);
        rejects([&]{ledger.provider_retire_allocation(newZ);});
        ledger.provider_ack_event(newQueue,tx,Ledger::Event::CancelledQuiescent);
        check(ledger.snapshot().inflight==0);
    }
    // Quarantine never invents quiescence or permits unsafe reclamation.
    const auto t3=ledger.provider_submit(newQueue,{wz2});
    ledger.provider_quarantine_device(2);
    check(ledger.snapshot().quarantined_queues==2);
    rejects([&]{(void)ledger.provider_register_queue(1,2,99);});
    rejects([&]{(void)ledger.provider_register_allocation(1,2,99,256);});
    // Physical device loss must fence every tenant sharing that device ID.
    rejects([&]{(void)ledger.provider_register_queue(2,2,99);});
    rejects([&]{(void)ledger.provider_register_allocation(2,2,99,256);});
    // Quarantine is limited to one device domain, not all tenants/devices.
    const auto safeQueue=ledger.provider_register_queue(1,9,99);
    const auto safeAlloc=ledger.provider_register_allocation(1,9,99,256);
    const auto independent=ledger.provider_submit(safeQueue,{{safeAlloc,0,8,Ledger::Access::Write}});
    ledger.provider_ack_event(safeQueue,independent,Ledger::Event::Completed);
    check(ledger.snapshot().inflight==1);
    ledger.provider_retire_allocation(safeAlloc);
    ledger.provider_retire_queue(safeQueue);
    rejects([&]{ledger.provider_ack_event(newQueue,t3,Ledger::Event::Completed);});
    rejects([&]{ledger.provider_retire_allocation(newZ);});
    rejects([&]{ledger.provider_retire_queue(newQueue);});
    rejects([&]{(void)ledger.provider_submit(newQueue,{wz2});});
    check(ledger.snapshot().inflight==1);
    std::cout<<"PASS "<<checked<<" GPU G1 provider-ledger host-only lifecycle assertions\n";
}
