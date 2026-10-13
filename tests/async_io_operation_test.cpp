#include "oreslang/runtime/AsyncIoCompletionGate.hpp"
#include <atomic>
#include <initializer_list>
#include <stdexcept>
#include <thread>

using oreslang::runtime::AsyncIoCompletionGate;
using oreslang::runtime::AsyncIoTicket;
using oreslang::runtime::AsyncIoTerminal;

static void check(bool yes, const char* description) {
    if (!yes) throw std::runtime_error(description);
}
int main() {
    const AsyncIoTicket original{11, 42, 2};
    AsyncIoCompletionGate gate(original);
    check(!gate.finish({11, 41, 2}, AsyncIoTerminal::success), "wrong generation accepted");
    check(!gate.finish({11, 42, 3}, AsyncIoTerminal::success), "wrong operation accepted");
    check(!gate.finish({12, 42, 2}, AsyncIoTerminal::success), "wrong actor accepted");
    check(!gate.finish(original, AsyncIoTerminal::pending), "pending transition accepted");
    for (unsigned value = 6; value <= 255; ++value) {
        check(!gate.finish(original, static_cast<AsyncIoTerminal>(value)),
              "invalid terminal enum accepted");
        check(gate.terminal() == AsyncIoTerminal::pending, "invalid outcome consumed completion");
    }
    for (AsyncIoTicket invalid : {AsyncIoTicket{0, 42, 2}, AsyncIoTicket{11, 0, 2}, AsyncIoTicket{11, 42, 0}}) {
        AsyncIoCompletionGate empty(invalid);
        check(!empty.finish(invalid, AsyncIoTerminal::closed), "zero ticket identity accepted");
    }
    check(gate.finish(original, AsyncIoTerminal::success), "completion not accepted");
    check(!gate.finish(original, AsyncIoTerminal::closed), "closed overwrote success");
    check(gate.terminal() == AsyncIoTerminal::success, "success lost");
    for (int i=0;i<1000;++i) {
        const AsyncIoTicket t{11, 44, static_cast<std::uint64_t>(i+1)};
        AsyncIoCompletionGate race(t);
        std::atomic<int> wins{0};
        std::thread a([&]{if(race.finish(t, AsyncIoTerminal::success)) ++wins;});
        std::thread b([&]{if(race.finish(t, AsyncIoTerminal::cancelled)) ++wins;});
        std::thread c([&]{if(race.finish(t, AsyncIoTerminal::timed_out)) ++wins;});
        a.join();b.join();c.join();
        check(wins.load()==1, "more than one terminal winner");
        check(race.terminal() != AsyncIoTerminal::pending, "no winner");
    }
}
