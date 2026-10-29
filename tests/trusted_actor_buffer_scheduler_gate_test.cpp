#include "oreslang/runtime/TrustedActorBufferSchedulerGate.hpp"
#include <atomic>
#include <cassert>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

using namespace oreslang::runtime;
template<class F> bool rejects(F&& f) {
    try { f(); } catch (const std::exception&) { return true; }
    return false;
}
static_assert(!std::is_constructible<TrustedActorBufferSchedulerGate::Turn,
    TrustedBufferActor>::value, "actor must not mint its own token");

int main() {
    const TrustedBufferActor a{1,1}, b{2,1}, b2{2,2};
    auto pool = std::make_shared<TrustedActorBufferPool>(25, 2048, 16);
    TrustedActorBufferSchedulerGate gate(pool);
    assert(rejects([&] { TrustedActorBufferSchedulerGate duplicate(pool); }));
    gate.register_actor(a, 1);
    gate.register_actor(b, 1);
    assert(rejects([&] {gate.register_actor(a, 1); }));
    assert(rejects([&] {gate.register_actor({1,0}, 1); }));
    assert(rejects([&] {gate.enter_turn(b2); }));
    auto ta = gate.enter_turn(a);
    assert(!gate.close()); // no mutation while an actor turn remains active
    assert(rejects([&] {gate.enter_turn(a); }));
    assert(rejects([&] {gate.retire_actor(a); }));
    auto h = gate.allocate(ta, 64);
    gate.write(ta,h,0,41);
    assert(gate.read(ta,h,0)==41);
    assert(rejects([&] {gate.send(ta,b2,h); }));
    assert(gate.read(ta,h,0)==41); // unknown target: no take
    gate.send(ta,b,h);
    assert(gate.pending(b)==1);
    assert(rejects([&] {gate.read(ta,h,0); }));
    auto extra = gate.allocate(ta,16);
    assert(rejects([&] {gate.send(ta,b,extra); })); // full: no take
    assert(gate.read(ta,extra,0)==0);
    ta.release();
    assert(rejects([&] {gate.read(ta,extra,0); }));
    assert(rejects([&] {gate.retire_actor({1,2}); }));

    auto tb = gate.enter_turn(b);
    assert(rejects([&] {gate.retire_actor(b); }));
    auto from_a = gate.receive(tb);
    assert(gate.read(tb,from_a,0)==41);
    auto loan = pool->borrow_read(from_a,b,a);
    tb.release();
    assert(rejects([&] {gate.retire_actor(b); })); // owner loan pin
    assert(rejects([&] {gate.retire_actor(a); })); // reader loan pin
    // Refuse actor reincarnation while its old borrower loan is live.
    assert(rejects([&] {gate.register_actor({1,2},1); }));
    loan.release();
    gate.retire_actor(b);
    assert(pool->used_bytes()==16); // reclaim delivered but unread
    assert(rejects([&] {gate.enter_turn(b); }));
    assert(rejects([&] {gate.register_actor(b,1); }));
    gate.register_actor(b2,1);
    auto tnew = gate.enter_turn(b2);
    assert(rejects([&] {gate.read(tnew,from_a,0); }));
    tnew.release();
    gate.retire_actor(b2);
    assert(rejects([&] {gate.register_actor(b,1); }));

    // Reject foreign turn tokens even when actor identity matches.
    auto other_pool=std::make_shared<TrustedActorBufferPool>(26,100,4);
    TrustedActorBufferSchedulerGate other(other_pool);
    other.register_actor(a,1);
    auto other_turn=other.enter_turn(a);
    assert(rejects([&] {gate.allocate(other_turn,1); }));
    other_turn.release();
    other.retire_actor(a);

    // Send versus recipient retirement: a concurrent retire cannot race
    // mailbox publication or create dangling receiver-owned buffers.
    gate.register_actor({3,1},1);
    auto ta2=gate.enter_turn(a);
    auto race_h=gate.allocate(ta2,10);
    std::atomic<bool> go{false};
    std::atomic<bool> sent{false};
    std::thread producer([&] {
        while(!go.load(std::memory_order_acquire)) std::this_thread::yield();
        try {gate.send(ta2,{3,1},race_h);sent.store(true,std::memory_order_release);}
        catch(const std::logic_error&) {}
    });
    std::thread retire([&] {
        while(!go.load(std::memory_order_acquire)) std::this_thread::yield();
        gate.retire_actor({3,1});
    });
    go.store(true,std::memory_order_release);
    producer.join();retire.join();
    if (!sent.load(std::memory_order_acquire))
        assert(gate.read(ta2,race_h,0)==0); // sender still owns on failed send
    else
        assert(rejects([&] {gate.read(ta2,race_h,0); })); // retired receiver reclaimed
    ta2.release();
    gate.retire_actor(a);
    assert(pool->used_bytes()==0);
    assert(gate.close());
    assert(rejects([&] {gate.enter_turn(a); }));
    assert(gate.close());
}
