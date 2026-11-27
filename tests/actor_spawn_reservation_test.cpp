#include "ActorSpawnReservation.hpp"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>
using oreslang::spawn_reservation::Reservation;
static unsigned checks;
static void check(bool ok) { if (!ok) std::abort(); ++checks; }
template<class F> static void deny(std::string_view code, F f) {
    try { f(); } catch (const std::logic_error& e) { check(code == e.what()); return; }
    std::abort();
}
static bool zero(Reservation::Charge c) { return c.heap == 0 && c.stacks == 0 && c.mailbox_bytes == 0 && c.messages == 0; }
int main() {
    const Reservation::Charge full{10,20,16,10}, half{5,10,8,5};
    Reservation gate(full), foreign(full);
    deny("E_SPAWN_POLICY", [&]{ (void)gate.reserve(full,0); });
    deny("E_SPAWN_QUOTA", [&]{ (void)gate.reserve({0,20,16,10},1); });
    check(zero(gate.usage()));
    auto p=gate.reserve(half,1), q=gate.reserve(half,2);
    deny("E_SPAWN_QUOTA", [&]{ (void)gate.reserve(half,1); });
    deny("E_SPAWN_STALE_PERMIT", [&]{ (void)foreign.begin_init(p); });
    check(!gate.try_enter_receive(p));
    auto init=gate.begin_init(p);
    deny("E_SPAWN_INIT_AUTHORITY", [&]{ (void)gate.begin_init(p); });
    deny("E_SPAWN_NOT_RETIRED", [&]{ gate.release_after_quiescence(p); });
    check(gate.complete_init(init,true));
    deny("E_SPAWN_INIT_AUTHORITY", [&]{ (void)gate.complete_init(init,true); });
    auto first=gate.try_enter_receive(p); check(first.has_value());
    check(!gate.try_enter_receive(p));
    check(gate.leave_receive(*first));
    auto second=gate.try_enter_receive(p); check(second.has_value());
    check(!gate.leave_receive(*first)); check(!gate.try_enter_receive(p));
    gate.retire(p); check(!gate.try_enter_receive(p));
    deny("E_SPAWN_IN_FLIGHT", [&]{ gate.release_after_quiescence(p); });
    check(gate.leave_receive(*second)); gate.release_after_quiescence(p);
    deny("E_SPAWN_STALE_PERMIT", [&]{ (void)gate.begin_init(p); });
    auto qi=gate.begin_init(q); gate.retire(q);
    deny("E_SPAWN_IN_FLIGHT", [&]{ gate.release_after_quiescence(q); });
    check(!gate.complete_init(qi,true)); gate.release_after_quiescence(q); check(zero(gate.usage()));
    for (int round=0; round<100; ++round) {
        auto a=gate.reserve(full,round%2==0?1:2);
        check(gate.complete_init(gate.begin_init(a),true));
        std::atomic<bool> go{false};
        std::vector<std::optional<Reservation::ReceiveTicket>> results(16);
        std::vector<std::thread> workers;
        for (std::size_t i=0;i<results.size();++i) workers.emplace_back([&,i]{
            while(!go.load(std::memory_order_acquire)) std::this_thread::yield();
            results[i]=gate.try_enter_receive(a);
        });
        go.store(true,std::memory_order_release);
        for(auto& t:workers) t.join();
        int admitted=0;
        for(const auto& x:results) if(x) ++admitted;
        check(admitted==(round%2==0?1:2)); check(!gate.try_enter_receive(a));
        gate.retire(a); check(!gate.try_enter_receive(a));
        for(const auto& x:results) if(x) check(gate.leave_receive(*x));
        gate.release_after_quiescence(a); check(zero(gate.usage()));
    }
    for(int i=0;i<100000;++i) {
        auto a=gate.reserve(full,1); auto t=gate.begin_init(a);
        check(!gate.complete_init(t,false)); gate.release_after_quiescence(a);
    }
    check(zero(gate.usage()));
    std::cout<<"NATIVE_SPAWN_RESERVATION_HOST_PASS "<<checks<<" checks; 1600 concurrent attempts; 100000 rollback cycles\n";
}
