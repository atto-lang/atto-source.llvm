#include "capability_io_lifecycle_v1.hpp"
#include <cassert>
#include <iostream>
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
    std::cout << "CAPABILITY_IO_HOST_LIFECYCLE_REFERENCE_PASS\n";
}
