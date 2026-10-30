#include "oreslang/runtime/DurableLockQueues.hpp"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <stdexcept>
#include <thread>
#include <vector>
using oreslang::runtime::DurableLockQueues;

template<class E, class F> void rejects(F&& fn) {
    bool thrown=false; try {fn();} catch (const E&) {thrown=true;} assert(thrown);
}

int main() {
    using Actor=DurableLockQueues::ActorKey;
    DurableLockQueues a(11, {2, 5, 4, 20});
    DurableLockQueues b(12, {2, 5, 4, 20});
    const Actor aa{1,1}, ab{2,1};
    auto a1=a.enqueue(aa,7,1,3);
    auto a2=a.enqueue(aa,7,2,3);
    auto different=a.enqueue(aa,9,3,3);
    auto b1=b.enqueue(aa,7,1,3);
    assert(a1.lock_head && !a2.lock_head && different.lock_head && b1.lock_head);
    // Equal numeric generation/actor/lock/operation/sequence in two VM roots
    // must not authorize a ticket against the wrong queue instance.
    DurableLockQueues another_vm_same_generation(11, {2, 5, 4, 20});
    auto forged_equal = another_vm_same_generation.enqueue(aa, 10, 1, 3);
    rejects<std::logic_error>([&]{ a.claim(forged_equal.ticket); });
    rejects<std::logic_error>([&]{ a.cancel_unstarted_head(forged_equal.ticket); });
    rejects<std::logic_error>([&]{a.finish(a1.ticket);});
    rejects<std::logic_error>([&]{a.claim(b1.ticket);});
    rejects<std::logic_error>([&]{a.claim(a2.ticket);});
    assert(a.claim(a1.ticket));
    assert(!a.claim(different.ticket)); // same actor, different lock
    rejects<std::logic_error>([&]{a.claim(a1.ticket);});
    rejects<std::logic_error>([&]{a.cancel_unstarted_head(a1.ticket);});
    rejects<std::logic_error>([&]{a.cancel_waiter(a1.ticket);});
    auto next=a.finish(a1.ticket);
    assert(next && *next==a2.ticket);
    rejects<std::logic_error>([&]{a.finish(a1.ticket);});
    assert(a.claim(different.ticket));
    assert(!a.claim(a2.ticket));
    assert(!a.finish(different.ticket));
    assert(a.claim(a2.ticket));
    assert(!a.finish(a2.ticket));
    assert(a.snapshot().entries==0);
    auto a3=a.enqueue(ab,7,4,8);
    auto a4=a.enqueue(aa,7,5,8);
    rejects<std::length_error>([&]{a.enqueue(aa,99,6,5);}); // lock capacity/byte cap
    assert(a.claim(a3.ticket));
    a.cancel_waiter(a4.ticket);
    assert(!a.finish(a3.ticket));
    auto ahead=a.enqueue(aa,7,1,1);
    assert(ahead.ticket.sequence!=a1.ticket.sequence);
    rejects<std::logic_error>([&]{a.claim(a1.ticket);});
    a.quiesce();
    rejects<std::logic_error>([&]{a.enqueue(aa,7,7,1);});
    assert(!a.drained());
    assert(!a.cancel_unstarted_head(ahead.ticket));
    assert(a.drained());

    // Race: two different queues for ONE actor cannot both claim.
    DurableLockQueues race(13, {8, 500, 500, 500});
    auto t1=race.enqueue(aa,1,1,1);
    auto t2=race.enqueue(aa,2,2,1);
    std::atomic<int> claimed{0};
    std::vector<std::thread> thr;
    thr.emplace_back([&]{if(race.claim(t1.ticket)) ++claimed;});
    thr.emplace_back([&]{if(race.claim(t2.ticket)) ++claimed;});
    for(auto& t:thr)t.join();
    assert(claimed==1);
    auto ready=race.ready_heads();
    assert(ready.size()==0); // other actor queue still blocked
    if(race.snapshot().running_actors!=1) return 3;
    return 0;
}
