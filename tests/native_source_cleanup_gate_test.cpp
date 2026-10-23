#include "oreslang/runtime/NativeSourceCleanupGate.hpp"
#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <thread>

using namespace oreslang::runtime;
#define CHECK(e) do { if (!(e)) throw std::runtime_error(#e); } while (false)

int main() {
    using Phase = NativeSourceCleanupGate::Phase;
    NativeSourceCleanupTicket token{7, 9, 11};
    NativeSourceCleanupGate gate(token);
    CHECK(!gate.ready_for_reclaim());
    CHECK(!gate.request_cancel({7,9,12}));
    CHECK(!gate.complete_normally({7,8,11}));
    CHECK(!gate.try_claim_cleanup(token));
    CHECK(gate.request_cancel(token));
    CHECK(!gate.request_cancel(token));
    CHECK(!gate.complete_normally(token));
    CHECK(!gate.complete_cleanup(token));
    CHECK(gate.try_claim_cleanup(token));
    CHECK(!gate.try_claim_cleanup(token));
    for (int i=0; i<3; ++i) {
        CHECK(gate.yield_cleanup(token));
        CHECK(!gate.ready_for_reclaim());
        CHECK(gate.try_claim_cleanup(token));
    }
    CHECK(gate.complete_cleanup(token));
    CHECK(gate.ready_for_reclaim());
    CHECK(!gate.complete_cleanup(token));
    CHECK(!gate.fail_cleanup(token));

    NativeSourceCleanupGate failed({7,9,12});
    CHECK(failed.request_cancel({7,9,12}));
    CHECK(failed.try_claim_cleanup({7,9,12}));
    CHECK(failed.fail_cleanup({7,9,12}));
    CHECK(failed.incomplete());
    CHECK(!failed.complete_cleanup({7,9,12}));
    CHECK(!failed.ready_for_reclaim());

    for (std::uint64_t i=1;i<=800;++i) {
        NativeSourceCleanupTicket t{17, i, i};
        NativeSourceCleanupGate race(t);
        std::atomic<int> wins{0};
        std::thread one([&]{if (race.complete_normally(t)) ++wins;});
        std::thread two([&]{if (race.request_cancel(t)) ++wins;});
        one.join(); two.join();
        CHECK(wins==1);
        if (race.phase()==Phase::cancel_requested) {
            CHECK(!race.ready_for_reclaim());
            CHECK(race.try_claim_cleanup(t));
            CHECK(race.complete_cleanup(t));
        }
        CHECK(race.ready_for_reclaim());
    }

    // A scheduler-close/cleanup-failure racing the final trusted cleanup
    // receipt may produce exactly one terminal outcome, never both.
    for (std::uint64_t i=1;i<=800;++i) {
        NativeSourceCleanupTicket t{37, i, i + 2000};
        NativeSourceCleanupGate race(t);
        CHECK(race.request_cancel(t));
        CHECK(race.try_claim_cleanup(t));
        std::atomic<int> wins{0};
        std::thread complete([&]{ if (race.complete_cleanup(t)) ++wins; });
        std::thread incomplete([&]{ if (race.fail_cleanup(t)) ++wins; });
        complete.join(); incomplete.join();
        CHECK(wins == 1);
        CHECK(race.ready_for_reclaim() != race.incomplete());
        CHECK(!race.complete_cleanup(t) && !race.fail_cleanup(t));
        CHECK(!race.request_cancel({37, i + 1, i + 2000}));
    }
}
