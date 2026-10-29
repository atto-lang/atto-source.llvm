#include "capability_io_lifecycle_v1.hpp"
#include <cassert>
#include <iostream>
#include <atomic>
#include <thread>
#include <vector>
#include <limits>
#include <stdexcept>
using oreslang::capability_io::HostLifecycleGate;
int main() {
    bool refusedOversize = false;
    try { HostLifecycleGate invalid(std::numeric_limits<std::size_t>::max()); }
    catch (const std::invalid_argument&) { refusedOversize = true; }
    assert(refusedOversize);
    HostLifecycleGate gate(1,2);
    assert(gate.reserve(false,true,true,42,1,55).nonce == 0);
    assert(gate.reserve(true,false,true,42,1,55).nonce == 0);
    assert(gate.reserve(true,true,false,42,1,55).nonce == 0);
    const auto first = gate.reserve(true,true,true,42,1,55);
    assert(first.nonce != 0 && gate.in_flight()==1);
    assert(gate.reserve(true,true,true,42,1,55).nonce==0);
    assert(!gate.dispatch(first,42,1,56));
    assert(!gate.dispatch(first,43,1,55));
    assert(gate.dispatch(first,42,1,55));
    assert(!gate.dispatch(first,42,1,55));
    assert(gate.cancel(first,42,1));
    assert(!gate.cancel(first,42,1));
    assert(!gate.retire(first,42,1,false));
    assert(gate.in_flight()==1);
    assert(gate.retire(first,42,1,true));
    assert(gate.in_flight()==0);
    const auto fresh = gate.reserve(true,true,true,42,1,55);
    assert(fresh.nonce != first.nonce);
    assert(!gate.dispatch(first,42,1,55));
    assert(gate.revoke_actor(42,1));
    assert(!gate.dispatch(fresh,42,1,55));
    assert(gate.reserve(true,true,true,42,1,55).nonce == 0);
    assert(gate.complete(fresh,42,1));
    assert(gate.retire(fresh,42,1,true));
    const auto changed_incarnation = gate.reserve(true,true,true,42,2,55);
    assert(changed_incarnation.nonce != 0);
    assert(gate.complete(changed_incarnation,42,2));
    assert(gate.in_flight()==1);
    assert(gate.retire(changed_incarnation,42,2,true));
    assert(gate.revoke_actor(42,2));
    assert(gate.revoke_actor(43,1)); // exhausted revocation ledger seals admission
    assert(gate.reserve(true,true,true,44,1,55).nonce == 0);
    // Global shutdown retains credits until the trusted host sees quiescence.
    HostLifecycleGate shutdown_gate(2,2);
    const auto started = shutdown_gate.reserve(true,true,true,77,1,88);
    const auto queued = shutdown_gate.reserve(true,true,true,78,1,89);
    assert(started.nonce != 0 && queued.nonce != 0);
    assert(shutdown_gate.dispatch(started,77,1,88));
    assert(shutdown_gate.shutdown());
    assert(!shutdown_gate.shutdown());
    assert(shutdown_gate.in_flight() == 2);
    assert(!shutdown_gate.dispatch(queued,78,1,89));
    assert(shutdown_gate.reserve(true,true,true,79,1,90).nonce == 0);
    assert(!shutdown_gate.retire(started,77,1,false));
    assert(shutdown_gate.complete(started,77,1));
    assert(shutdown_gate.retire(started,77,1,true));
    assert(shutdown_gate.retire(queued,78,1,true));
    assert(shutdown_gate.in_flight() == 0);
    // Concurrent identical dispatches must linearize at one admitted start.
    HostLifecycleGate racing(1, 1);
    const auto r = racing.reserve(true,true,true,99,1,100);
    assert(r.nonce != 0);
    std::atomic<int> successful_dispatches{0};
    std::vector<std::thread> workers;
    for (int i = 0; i < 16; ++i) {
        workers.emplace_back([&] {
            if (racing.dispatch(r,99,1,100)) successful_dispatches.fetch_add(1);
        });
    }
    for (auto& t : workers) t.join();
    assert(successful_dispatches.load() == 1);
    assert(racing.in_flight() == 1);
    assert(racing.cancel(r,99,1));
    assert(!racing.retire(r,99,1,false));
    assert(racing.retire(r,99,1,true));
    assert(racing.in_flight() == 0);
    std::cout << "CAPABILITY_IO_HOST_LIFECYCLE_REFERENCE_PASS\n";
}
