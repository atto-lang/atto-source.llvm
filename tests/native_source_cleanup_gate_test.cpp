#include "oreslang/runtime/NativeSourceCleanupGate.hpp"
#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <thread>

namespace oreslang::runtime {
struct NativeSourceCleanupGateTestPeer {
    static bool normal(NativeSourceCleanupGate& g, NativeSourceCleanupTicket t) noexcept {
        return g.complete_normally(t);
    }
    static bool claim(NativeSourceCleanupGate& g, NativeSourceCleanupTicket t) noexcept {
        return g.try_claim_cleanup(t);
    }
    static bool yield(NativeSourceCleanupGate& g, NativeSourceCleanupTicket t) noexcept {
        return g.yield_cleanup(t);
    }
    static bool complete(NativeSourceCleanupGate& g, NativeSourceCleanupTicket t) noexcept {
        return g.complete_cleanup(t);
    }
};
} // namespace oreslang::runtime

using namespace oreslang::runtime;
#define CHECK(e) do { if (!(e)) throw std::runtime_error(#e); } while (false)

int main() {
    using Phase = NativeSourceCleanupGate::Phase;
    NativeSourceCleanupTicket token{7, 9, 11};
    NativeSourceCleanupGate gate(token);
    CHECK(!gate.ready_for_reclaim());
    CHECK(!gate.request_cancel({7,9,12}));
    CHECK(!NativeSourceCleanupGateTestPeer::normal(gate, {7,8,11}));
    CHECK(!NativeSourceCleanupGateTestPeer::claim(gate, token));
    CHECK(gate.request_cancel(token));
    CHECK(!gate.request_cancel(token));
    CHECK(!NativeSourceCleanupGateTestPeer::normal(gate, token));
    CHECK(!NativeSourceCleanupGateTestPeer::complete(gate, token));
    CHECK(NativeSourceCleanupGateTestPeer::claim(gate, token));
    CHECK(!NativeSourceCleanupGateTestPeer::claim(gate, token));
    for (int i=0; i<3; ++i) {
        CHECK(NativeSourceCleanupGateTestPeer::yield(gate, token));
        CHECK(!gate.ready_for_reclaim());
        CHECK(NativeSourceCleanupGateTestPeer::claim(gate, token));
    }
    CHECK(NativeSourceCleanupGateTestPeer::complete(gate, token));
    CHECK(gate.ready_for_reclaim());
    CHECK(!NativeSourceCleanupGateTestPeer::complete(gate, token));
    CHECK(!gate.fail_cleanup(token));

    NativeSourceCleanupGate failed({7,9,12});
    CHECK(failed.request_cancel({7,9,12}));
    CHECK(NativeSourceCleanupGateTestPeer::claim(failed, {7,9,12}));
    CHECK(failed.fail_cleanup({7,9,12}));
    CHECK(failed.incomplete());
    CHECK(!NativeSourceCleanupGateTestPeer::complete(failed, {7,9,12}));
    CHECK(!failed.ready_for_reclaim());

    for (std::uint64_t i=1;i<=800;++i) {
        NativeSourceCleanupTicket t{17, i, i};
        NativeSourceCleanupGate race(t);
        std::atomic<int> wins{0};
        std::thread one([&]{if (NativeSourceCleanupGateTestPeer::normal(race, t)) ++wins;});
        std::thread two([&]{if (race.request_cancel(t)) ++wins;});
        one.join(); two.join();
        CHECK(wins==1);
        if (race.phase()==Phase::cancel_requested) {
            CHECK(!race.ready_for_reclaim());
            CHECK(NativeSourceCleanupGateTestPeer::claim(race, t));
            CHECK(NativeSourceCleanupGateTestPeer::complete(race, t));
        }
        CHECK(race.ready_for_reclaim());
    }

    // A scheduler-close/cleanup-failure racing the final trusted cleanup
    // receipt may produce exactly one terminal outcome, never both.
    for (std::uint64_t i=1;i<=800;++i) {
        NativeSourceCleanupTicket t{37, i, i + 2000};
        NativeSourceCleanupGate race(t);
        CHECK(race.request_cancel(t));
        CHECK(NativeSourceCleanupGateTestPeer::claim(race, t));
        std::atomic<int> wins{0};
        std::thread complete([&]{ if (NativeSourceCleanupGateTestPeer::complete(race, t)) ++wins; });
        std::thread incomplete([&]{ if (race.fail_cleanup(t)) ++wins; });
        complete.join(); incomplete.join();
        CHECK(wins == 1);
        CHECK(race.ready_for_reclaim() != race.incomplete());
        CHECK(!NativeSourceCleanupGateTestPeer::complete(race, t) && !race.fail_cleanup(t));
        CHECK(!race.request_cancel({37, i + 1, i + 2000}));
    }
}
