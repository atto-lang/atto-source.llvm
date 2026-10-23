#include "oreslang/runtime/ActorGroupLifecycle.hpp"

#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <thread>

using oreslang::runtime::NativeActorGroup;

#define CHECK(x) do { if (!(x)) throw std::runtime_error("actor resource loan race: " #x); } while (false)

static void live_group_rejects_outstanding_loan() {
    NativeActorGroup group(191001, 1);
    auto actor = group.adopt(191001, 11, 1, [] {});
    CHECK(group.mark_ready(actor));
    actor.retain_resource_loan();
    CHECK(actor.outstanding_resource_loans() == 1);
    CHECK(!group.finalized(actor));
    CHECK(actor.outstanding_resource_loans() == 1);
    actor.release_resource_loan();
    CHECK(actor.outstanding_resource_loans() == 0);
    CHECK(group.finalized(actor));
    CHECK(!group.finalized(actor));
    actor.get_done().get();
    bool rejected = false;
    try { actor.retain_resource_loan(); }
    catch (const std::logic_error&) { rejected = true; }
    CHECK(rejected);
}

static void concurrent_registration_and_finalization_have_one_winner() {
    // A source-free native deterministic-seed contention probe against the
    // real NativeActorGroup generation ticket and terminal resource bit.
    // Every trial joins both threads before checking terminal invariants.
    std::uint32_t registration_wins = 0;
    std::uint32_t finalization_wins = 0;
    for (std::uint32_t trial = 0; trial < 2000; ++trial) {
        NativeActorGroup group(191002, 1);
        auto actor = group.adopt(191002, 100 + trial, 1, [] {});
        CHECK(group.mark_ready(actor));
        auto finalizer = group.runtime_finalizer(actor);
        group.close();
        CHECK(finalizer.ready_for_actor_turn_quiescence());
        std::atomic<int> ready{0};
        bool registered = false;
        bool refused = false;
        bool finalized = false;
        std::thread register_thread([&] {
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (ready.load(std::memory_order_acquire) != 2)
                std::this_thread::yield();
            try { actor.retain_resource_loan(); registered = true; }
            catch (const std::logic_error&) { refused = true; }
        });
        std::thread finalize_thread([&] {
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (ready.load(std::memory_order_acquire) != 2)
                std::this_thread::yield();
            finalized = finalizer.finalized();
        });
        register_thread.join();
        finalize_thread.join();

        CHECK(registered != finalized);
        CHECK(registered != refused);
        if (registered) {
            ++registration_wins;
            CHECK(!finalizer.ready_for_resource_reclamation());
            CHECK(actor.outstanding_resource_loans() == 1);
            CHECK(!finalizer.finalized());
            actor.release_resource_loan();
            CHECK(finalizer.ready_for_resource_reclamation());
            CHECK(finalizer.finalized());
        } else {
            ++finalization_wins;
            CHECK(refused);
            CHECK(actor.outstanding_resource_loans() == 0);
        }
        CHECK(!finalizer.finalized());
        actor.get_done().get();
        CHECK(actor.outstanding_resource_loans() == 0);
    }
    CHECK(registration_wins + finalization_wins == 2000);
}

int main() {
    live_group_rejects_outstanding_loan();
    concurrent_registration_and_finalization_have_one_winner();
}
