#include "oreslang/runtime/ActorSpawnChargePlan.hpp"
#include <cstdlib>
#include <iostream>
#include <string_view>
using namespace oreslang::spawn_charge;
static int checks;
static void check(bool ok) { if (!ok) std::abort(); ++checks; }
template<class F> static void deny(std::string_view code, F f) {
    try { f(); } catch (const std::invalid_argument& e) { check(code == e.what()); return; }
    std::abort();
}
int main() {
    const Policy p{false,4,2,8,1000,100,50,4096};
    const Defaults d{Affinity::sticky,500,50,10};
    const Request absent{};
    auto x = resolve(p,d,absent);
    check(x.carrier_cap == 2 && x.heap == 500 && x.stacks == 400 && x.mailbox_bytes == 4096 && x.messages == 10);
    x = resolve(p,d,Request{1,Affinity::pinned,300,20,3});
    check(x.carrier_cap == 1 && x.affinity == Affinity::pinned && x.heap == 300 && x.stacks == 160 && x.mailbox_bytes == 4096 && x.messages == 3);
    for (auto cap : {0, -1, 5}) deny("E_SPAWN_POLICY", [&]{ auto r=absent; r.carriers=cap; (void)resolve(p,d,r); });
    deny("E_SPAWN_POLICY", [&]{ auto r=absent; r.affinity=Affinity::pinned; (void)resolve(p,d,r); });
    deny("E_SPAWN_POLICY", [&]{ auto r=absent; r.affinity=static_cast<Affinity>(99); (void)resolve(p,d,r); });
    auto serial=p; serial.serial=true; serial.carrier_cap=serial.default_carriers=1;
    check(resolve(serial,d,absent).carrier_cap == 1);
    deny("E_SPAWN_POLICY", [&]{ auto r=absent; r.carriers=2; (void)resolve(serial,d,r); });
    deny("E_SPAWN_POLICY", [&]{ auto bad=serial; bad.carrier_cap=2; (void)resolve(bad,d,absent); });
    for (auto heap : {0,-1,1001}) deny("E_SPAWN_QUOTA", [&]{ auto r=absent; r.heap_bytes=heap; (void)resolve(p,d,r); });
    deny("E_SPAWN_QUOTA", [&]{ auto r=absent; r.stack_bytes=101; (void)resolve(p,d,r); });
    deny("E_SPAWN_QUOTA", [&]{ auto r=absent; r.mailbox_messages=51; (void)resolve(p,d,r); });
    deny("E_SPAWN_QUOTA", [&]{ auto bad=d; bad.heap_bytes=0; auto r=absent; r.heap_bytes=10; (void)resolve(p,bad,r); });
    auto huge=p; huge.live_stack_slots=std::numeric_limits<std::int64_t>::max();
    deny("E_SPAWN_QUOTA", [&]{ (void)resolve(huge,d,absent); });
    check(resolve(huge,Defaults{Affinity::none,1,1,1},absent).stacks == std::numeric_limits<std::int64_t>::max());
    deny("E_SPAWN_POLICY", [&]{ auto bad=p; bad.mailbox_byte_charge=0; (void)resolve(bad,d,absent); });
    std::cout << "NATIVE_SPAWN_CHARGE_PASS " << checks << " checks; host only\n";
}
