// Common host-memory contract. This drives the real byte and scalar heaps
// against a separate value/loan model; it is not a guest compiler test.
#include "ActorArena.h"
#include "ActorOwnedI64Heap.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

struct Byte {
    using Heap = OresActorArena;
    using Turn = OresArenaTurn;
    using Slot = OresArenaHandle;
    using Loan = OresArenaLoan;
    static Heap* open() { return ores_actor_arena_open(17, 7, 8, 8, 64); }
    static Turn enter(Heap* h) { return ores_actor_arena_enter(h, 17, 7); }
    static void leave(Heap* h, Turn t) { ores_actor_arena_leave(h, t); }
    static Slot alloc(Heap* h, Turn t) { return ores_actor_arena_alloc(h, t, 8); }
    static std::int64_t read(Heap* h, Turn t, Slot s) { std::int64_t v; ores_actor_arena_read(h,t,s,0,&v,8); return v; }
    static void write(Heap* h, Turn t, Slot s, std::int64_t v) { ores_actor_arena_write(h,t,s,0,&v,8); }
    static void drop(Heap* h, Turn t, Slot s) { ores_actor_arena_drop(h,t,s); }
    static Loan borrow(Heap* h, Turn t, Slot s, bool mut) { return mut ? ores_actor_arena_borrow_mut(h,t,s) : ores_actor_arena_borrow(h,t,s); }
    static std::int64_t read_loan(Heap* h, Turn t, Loan l) { std::int64_t v; ores_actor_arena_loan_read(h,t,l,0,&v,8); return v; }
    static void write_loan(Heap* h, Turn t, Loan l, std::int64_t v) { ores_actor_arena_loan_write(h,t,l,0,&v,8); }
    static void release(Heap* h, Loan l) { ores_actor_arena_release(h,l); }
    static void retire(Heap* h) { ores_actor_arena_retire(h); }
    static int destroy(Heap* h) { return ores_actor_arena_try_destroy(h); }
};
struct Scalar {
    using Heap = OresActorI64Heap;
    using Turn = OresActorTurn;
    using Slot = OresActorSlot;
    using Loan = OresActorLoan;
    static Heap* open() { return ores_actor_i64_heap_open(17,7,8); }
    static Turn enter(Heap* h) { return ores_actor_i64_heap_enter(h,17,7); }
    static void leave(Heap* h, Turn t) { ores_actor_i64_heap_leave(h,t); }
    static Slot alloc(Heap* h, Turn t) { return ores_actor_i64_heap_new(h,t,0); }
    static std::int64_t read(Heap* h, Turn t, Slot s) { return ores_actor_i64_heap_read(h,t,s); }
    static void write(Heap* h, Turn t, Slot s, std::int64_t v) { ores_actor_i64_heap_write(h,t,s,v); }
    static void drop(Heap* h, Turn t, Slot s) { ores_actor_i64_heap_drop(h,t,s); }
    static Loan borrow(Heap* h, Turn t, Slot s, bool mut) { return mut ? ores_actor_i64_heap_borrow_mut(h,t,s) : ores_actor_i64_heap_borrow(h,t,s); }
    static std::int64_t read_loan(Heap* h, Turn t, Loan l) { return ores_actor_i64_heap_loan_read(h,t,l); }
    static void write_loan(Heap* h, Turn t, Loan l, std::int64_t v) { ores_actor_i64_heap_loan_write(h,t,l,v); }
    static void release(Heap* h, Loan l) { ores_actor_i64_heap_release(h,l); }
    static void retire(Heap* h) { ores_actor_i64_heap_retire(h); }
    static int destroy(Heap* h) { return ores_actor_i64_heap_try_destroy(h); }
};
static void require(bool condition, const char* why) { if (!condition) throw std::runtime_error(why); }
struct Random {
    std::uint64_t state;
    std::uint64_t next() { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state; }
    std::int64_t value() { return static_cast<std::int64_t>(next() % 2000001) - 1000000; }
};
template<class Api> static void model(std::uint64_t seed, unsigned steps) {
    struct Slot { typename Api::Slot token{}; std::int64_t value=0; bool live=false; unsigned readers=0; bool writer=false; };
    struct Loan { typename Api::Loan token{}; unsigned slot=0; bool mut=false, live=false; };
    struct State {
        typename Api::Heap* heap = Api::open();
        typename Api::Turn turn{};
        std::array<Slot,8> slots{};
        std::array<Loan,16> loans{};
        std::array<std::uint64_t,8> generations{};
    };
    std::array<State,2> states;
    Random random{seed};
    int current = -1;
    std::array<std::uint64_t,11> counts{};
    std::uint64_t digest = 1469598103934665603ULL;
    auto observe = [&](std::int64_t actual, std::int64_t expected) {
        require(actual == expected, "model value mismatch");
        digest = (digest ^ static_cast<std::uint64_t>(actual)) * 1099511628211ULL;
    };
    for (unsigned step=0; step<steps; ++step) {
        const auto index = static_cast<unsigned>(random.next()%2);
        if (current != static_cast<int>(index)) {
            if (current >= 0) Api::leave(states[static_cast<unsigned>(current)].heap, states[static_cast<unsigned>(current)].turn);
            states[index].turn = Api::enter(states[index].heap);
            current = static_cast<int>(index);
            ++counts[10];
        }
        auto& state = states[index];
        std::vector<unsigned> free_slots, readable, writable, occupied, active_loans, mutable_loans, free_loans;
        for (unsigned i=0;i<8;++i) {
            const auto& slot=state.slots[i];
            if (!slot.live) free_slots.push_back(i);
            else {
                occupied.push_back(i);
                if (!slot.writer) readable.push_back(i);
                if (!slot.writer && !slot.readers) writable.push_back(i);
            }
        }
        for (unsigned i=0;i<16;++i) {
            const auto& loan=state.loans[i];
            if (!loan.live) free_loans.push_back(i);
            else { active_loans.push_back(i); if (loan.mut) mutable_loans.push_back(i); }
        }
        const unsigned operation=static_cast<unsigned>(random.next()%10);
        auto pick=[&](const std::vector<unsigned>& v) { return v.at(static_cast<std::size_t>(random.next()%v.size())); };
        if (operation==0 && !free_slots.empty()) {
            auto& slot=state.slots[pick(free_slots)];
            slot.token=Api::alloc(state.heap,state.turn);
            require(slot.token.index<8, "physical slot out of range");
            auto& generation=state.generations[static_cast<std::size_t>(slot.token.index)];
            require(slot.token.generation>generation, "slot generation recycled");
            generation=slot.token.generation;
            slot.live=true; slot.value=0;
            observe(Api::read(state.heap,state.turn,slot.token),0);
            slot.value=random.value(); Api::write(state.heap,state.turn,slot.token,slot.value);
        } else if (operation==1 && !readable.empty()) {
            const auto& slot=state.slots[pick(readable)];
            observe(Api::read(state.heap,state.turn,slot.token),slot.value);
        } else if (operation==2 && !writable.empty()) {
            auto& slot=state.slots[pick(writable)];
            slot.value=random.value(); Api::write(state.heap,state.turn,slot.token,slot.value);
        } else if (operation==3 && !writable.empty()) {
            auto& slot=state.slots[pick(writable)];
            Api::drop(state.heap,state.turn,slot.token); slot.live=false;
        } else if ((operation==4 || operation==5) && !free_loans.empty()
                && !(operation==4 ? readable : writable).empty()) {
            auto& loan=state.loans[pick(free_loans)];
            loan.slot=pick(operation==4 ? readable : writable); loan.mut=operation==5;
            auto& slot=state.slots[loan.slot];
            loan.token=Api::borrow(state.heap,state.turn,slot.token,loan.mut);
            loan.live=true;
            if (loan.mut) slot.writer=true; else ++slot.readers;
        } else if (operation==6 && !active_loans.empty()) {
            const auto& loan=state.loans[pick(active_loans)];
            observe(Api::read_loan(state.heap,state.turn,loan.token),state.slots[loan.slot].value);
        } else if (operation==7 && !mutable_loans.empty()) {
            const auto& loan=state.loans[pick(mutable_loans)];
            auto& slot=state.slots[loan.slot];
            slot.value=random.value(); Api::write_loan(state.heap,state.turn,loan.token,slot.value);
        } else if (operation==8 && !active_loans.empty()) {
            auto& loan=state.loans[pick(active_loans)];
            Api::release(state.heap,loan.token);
            auto& slot=state.slots[loan.slot];
            if (loan.mut) slot.writer=false; else --slot.readers;
            loan.live=false;
        } else if (operation==9 && !active_loans.empty()) {
            const auto& loan=state.loans[pick(active_loans)];
            const auto expected=state.slots[loan.slot].value;
            Api::leave(state.heap,state.turn);
            std::int64_t actual=0;
            std::thread carrier([&] {
                auto turn=Api::enter(state.heap);
                actual=Api::read_loan(state.heap,turn,loan.token);
                Api::leave(state.heap,turn);
            });
            carrier.join();
            state.turn=Api::enter(state.heap);
            observe(actual,expected);
        } else continue;
        ++counts[operation];
    }
    require(states[0].turn.domain && states[1].turn.domain && states[0].turn.domain!=states[1].turn.domain,
            "separate heaps share domain");
    // Retirement must revoke new work but permit an existing carrier/loan
    // to unwind. Pins must prevent free before the final release.
    for (unsigned i=0;i<2;++i) {
        auto& state=states[i];
        Api::retire(state.heap);
        bool pinned=current==static_cast<int>(i);
        for (const auto& loan:state.loans) pinned=pinned || loan.live;
        if (pinned) require(Api::destroy(state.heap)==0, "freed pinned heap");
        if (current==static_cast<int>(i)) { Api::leave(state.heap,state.turn); current=-1; }
        for (auto& loan:state.loans) if (loan.live) Api::release(state.heap,loan.token);
        require(Api::destroy(state.heap)==1, "quiescent heap not reclaimed");
    }
    for (const auto count:counts) require(count>0, "seed lacks operation coverage");
    std::printf("%llu %llu",static_cast<unsigned long long>(seed),static_cast<unsigned long long>(digest));
    for (const auto count:counts) std::printf(" %llu",static_cast<unsigned long long>(count));
    std::puts("");
}

template<class Api> static void misuse(unsigned which) {
    auto* heap=Api::open();
    auto turn=Api::enter(heap);
    auto slot=Api::alloc(heap,turn);
    Api::write(heap,turn,slot,42);
    if (which==0) { Api::drop(heap,turn,slot); (void)Api::alloc(heap,turn); (void)Api::read(heap,turn,slot); }
    else if (which==1) { auto old=turn; Api::leave(heap,turn); turn=Api::enter(heap); (void)Api::read(heap,old,slot); }
    else if (which==2) { std::thread t([&] { (void)Api::read(heap,turn,slot); }); t.join(); }
    else if (which==3) { std::thread t([&] { Api::leave(heap,turn); }); t.join(); }
    else if (which==4) { auto* other=Api::open(); (void)Api::enter(other); }
    else if (which==5) { auto* other=Api::open(); Api::leave(heap,turn); auto t=Api::enter(other); (void)Api::alloc(other,t); (void)Api::read(other,t,slot); }
    else if (which==6) { (void)Api::destroy(heap); }
    else if (which==7) { Api::retire(heap); (void)Api::read(heap,turn,slot); }
    else if (which==8) { Api::retire(heap); Api::leave(heap,turn); (void)Api::enter(heap); }
    else if (which==9) { Api::retire(heap); (void)Api::alloc(heap,turn); }
    else {
        bool shared=which==10 || which==11 || which==12;
        auto loan=Api::borrow(heap,turn,slot,!shared);
        if (which==10) Api::write_loan(heap,turn,loan,0);
        else if (which==11) (void)Api::borrow(heap,turn,slot,true);
        else if (which==12) Api::drop(heap,turn,slot);
        else if (which==13) (void)Api::read(heap,turn,slot);
        else if (which==14) Api::write(heap,turn,slot,0);
        else if (which==15) (void)Api::borrow(heap,turn,slot,false);
        else if (which==16) (void)Api::borrow(heap,turn,slot,true);
        else if (which==17) { Api::release(heap,loan); Api::release(heap,loan); }
        else if (which==18) { Api::release(heap,loan); (void)Api::read_loan(heap,turn,loan); }
        else if (which==19) { Api::release(heap,loan); (void)Api::borrow(heap,turn,slot,true); Api::write_loan(heap,turn,loan,0); }
        else if (which==20) { ++loan.domain; (void)Api::read_loan(heap,turn,loan); }
        else if (which==21) { ++loan.index; (void)Api::read_loan(heap,turn,loan); }
        else if (which==22) { ++loan.generation; (void)Api::read_loan(heap,turn,loan); }
        else if (which==23) { ++loan.nonce; (void)Api::read_loan(heap,turn,loan); }
        else if (which==24) { loan.nonce=0; (void)Api::read_loan(heap,turn,loan); }
        else if (which==25) { Api::retire(heap); (void)Api::read_loan(heap,turn,loan); }
        else if (which==26) { Api::retire(heap); Api::write_loan(heap,turn,loan,0); }
        else if (which==27) { std::thread t([&] { (void)Api::read_loan(heap,turn,loan); }); t.join(); }
        else throw std::runtime_error("unknown misuse");
    }
    throw std::runtime_error("unsafe operation returned");
}
int main(int argc,char** argv) {
#if defined(__unix__) || defined(__APPLE__)
    struct rlimit limit={0,0}; (void)setrlimit(RLIMIT_CORE,&limit);
#endif
    try {
        if (argc!=5) throw std::runtime_error("mode byte|scalar seed steps OR misuse byte|scalar case 0");
        const std::string action=argv[1], mode=argv[2];
        const auto value=std::stoull(argv[3]);
        const auto steps=static_cast<unsigned>(std::stoul(argv[4]));
        if (action=="model" && mode=="byte") model<Byte>(value,steps);
        else if (action=="model" && mode=="scalar") model<Scalar>(value,steps);
        else if (action=="misuse" && mode=="byte") misuse<Byte>(static_cast<unsigned>(value));
        else if (action=="misuse" && mode=="scalar") misuse<Scalar>(static_cast<unsigned>(value));
        else throw std::runtime_error("unknown test mode");
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr,"%s\n",e.what()); return 70; }
}
