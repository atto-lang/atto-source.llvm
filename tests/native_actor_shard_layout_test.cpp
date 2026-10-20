#include "oreslang/runtime/ActorShardColumns.hpp"
#include "oreslang/runtime/NativeActorScheduler.h"

#if defined(NDEBUG)
#undef NDEBUG // Keep native proof assertions effective with -O2 -DNDEBUG
#endif
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

using oreslang::runtime::ActorShardColumns;
using oreslang::runtime::ActorShardBudget;
using oreslang::runtime::ActorColumnSnapshot;
using oreslang::dop::Layout;

struct TestState {
    OresNativeActorScheduler* scheduler = nullptr;
    OresNativeActorRef owner = {};
    Layout layout = Layout::SoA;
    std::unique_ptr<ActorShardBudget> actor_budget;
    ActorShardBudget vm_budget{64U * 1024U * 1024U};
    std::unique_ptr<ActorShardColumns> region;
    std::vector<std::unique_ptr<ActorColumnSnapshot>> snapshots;
    std::vector<double> published = std::vector<double>(65, 0.0);
    bool foreign_rejected = false;
    bool failed = false;
};

static void on_message(void* ctx, std::size_t actor, std::uint64_t step) {
    auto& state = *static_cast<TestState*>(ctx);
    try {
        if (actor == 1 && step == 3) {
            try {
                (void)state.region->read(0, 0);
                state.failed = true;
            } catch (const std::logic_error&) {
                state.foreign_rejected = true;
            }
            bool snapshot_rejected = false;
            try {
                (void)state.snapshots.back()->read(0);
            } catch (const std::logic_error&) {
                snapshot_rejected = true;
            }
            assert(snapshot_rejected);
            double output[65] = {};
            bool copy_rejected = false;
            try { state.snapshots.back()->copy_into(output, 65); }
            catch (const std::logic_error&) { copy_rejected = true; }
            assert(copy_rejected);
            bool close_rejected = false;
            try { state.snapshots.back()->close(); }
            catch (const std::logic_error&) { close_rejected = true; }
            assert(close_rejected);
            return;
        }
        assert(actor == 0);
        if (step == 1) {
            auto region = std::make_unique<ActorShardColumns>(
                state.scheduler, state.owner,
                *state.actor_budget, state.vm_budget, state.layout, 65, 3,
                state.layout == Layout::AoSoA ? 8 : 1);
            for (std::size_t i = 0; i < 65; ++i) {
                region->write(i, 0, static_cast<double>(i));
                region->write(i, 1, static_cast<double>(2 * i));
                region->write(i, 2, -1);
            }
            region->integrate(0, 1, .5);
            state.region = std::move(region);
            ActorShardBudget tiny{64, state.scheduler, state.owner};
            bool insufficient_budget = false;
            try {
                ActorShardColumns excess(state.scheduler, state.owner,
                    tiny, state.vm_budget, state.layout, 65, 3,
                    state.layout == Layout::AoSoA ? 8 : 1);
            } catch (const std::length_error&) {
                insufficient_budget = true;
            }
            assert(insufficient_budget);
            assert(tiny.used_bytes() == 0);
            ActorShardBudget tiny_vm{64};
            bool insufficient_vm_budget = false;
            try {
                ActorShardColumns excess(state.scheduler, state.owner,
                    *state.actor_budget, tiny_vm, state.layout, 65, 3,
                    state.layout == Layout::AoSoA ? 8 : 1);
            } catch (const std::length_error&) {
                insufficient_vm_budget = true;
            }
            assert(insufficient_vm_budget);
            assert(tiny_vm.used_bytes() == 0);
            assert(state.actor_budget->used_bytes() ==
                   state.region->rows() * state.region->fields() *
                   sizeof(double) +
                   (state.layout == Layout::AoSoA ? 7 * state.region->fields() *
                   sizeof(double) : 0));
            assert(state.actor_budget->used_bytes() > 0);
            ActorShardBudget incorrect(
                65536, state.scheduler,
                OresNativeActorRef{state.owner.scheduler_domain,
                    state.owner.actor_slot + 1, state.owner.generation});
            bool mismatched_budget_rejected = false;
            try {
                ActorShardColumns wrong(state.scheduler, state.owner,
                    incorrect, state.vm_budget, state.layout, 65, 3,
                    state.layout == Layout::AoSoA ? 8 : 1);
            } catch (const std::invalid_argument&) {
                mismatched_budget_rejected = true;
            }
            assert(mismatched_budget_rejected);
            assert(incorrect.used_bytes() == 0);
            // A valid slot does not authorize a budget from an old or
            // future generation: hot reload/retire fencing is exact.
            ActorShardBudget wrong_generation(
                65536, state.scheduler,
                OresNativeActorRef{state.owner.scheduler_domain,
                    state.owner.actor_slot, state.owner.generation + 1});
            bool stale_generation_rejected = false;
            try {
                ActorShardColumns wrong(state.scheduler, state.owner,
                    wrong_generation, state.vm_budget, state.layout, 65, 3,
                    state.layout == Layout::AoSoA ? 8 : 1);
            } catch (const std::invalid_argument&) {
                stale_generation_rejected = true;
            }
            assert(stale_generation_rejected);
            assert(wrong_generation.used_bytes() == 0);
        } else if (step == 2) {
            state.region->integrate(0, 1, 1);
            const std::uint64_t prior = state.actor_budget->used_bytes();
            for (int k = 0; k < 1000; ++k) {
                state.snapshots.emplace_back(state.region->snapshot_column_leased(0));
            }
            assert(state.snapshots.back()->read(64) == 256.0);
            assert(state.actor_budget->used_bytes() == prior + 1000U * 65U * sizeof(double));
            for (int k = 0; k < 500; ++k) {
                state.snapshots[k]->close();
                state.snapshots[k]->close();
                bool revoked = false;
                try {
                    (void)state.snapshots[k]->read(0);
                } catch (const std::logic_error&) {
                    revoked = true;
                }
                assert(revoked);
            }
            assert(state.actor_budget->used_bytes() == prior + 500U * 65U * sizeof(double));
            std::vector<double> into(67, -1.0);
            state.region->copy_column_into(0, into.data() + 1, 65);
            assert(into[0] == -1.0 && into[66] == -1.0);
            for (std::size_t i = 0; i < 65; ++i) {
                assert(into[i + 1] == static_cast<double>(4 * i));
            }
            bool short_copy_rejected = false;
            try {
                state.region->copy_column_into(0, into.data(), 64);
            } catch (const std::out_of_range&) {
                short_copy_rejected = true;
            }
            assert(short_copy_rejected);
            state.snapshots.back()->copy_into(
                state.published.data(), state.published.size());
            bool raw_snapshot_rejected = false;
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
            try {
                // Deliberately exercise the source-compatibility trap.
                (void)state.region->snapshot_column(1);
            } catch (const std::logic_error&) {
                raw_snapshot_rejected = true;
            }
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
            assert(raw_snapshot_rejected);
            assert(state.region->read(0, 1) == 0);
        } else {
            state.failed = true;
        }
    } catch (...) {
        state.failed = true;
    }
}

int main() {
    for (auto layout : {Layout::AoS, Layout::SoA, Layout::AoSoA}) {
        TestState state;
        state.layout = layout;
        state.scheduler = ores_native_actor_scheduler_open(2, 4, 2, on_message, &state);
        assert(state.scheduler != nullptr);
        state.owner = ores_native_actor_scheduler_spawn(state.scheduler, 0);
        assert(state.owner.scheduler_domain != 0);
        state.actor_budget = std::make_unique<ActorShardBudget>(16U * 1024U * 1024U, state.scheduler, state.owner);
        assert(ores_native_actor_scheduler_send(state.scheduler, state.owner, 1));
        assert(ores_native_actor_scheduler_send(state.scheduler, state.owner, 2));
        assert(ores_native_actor_scheduler_drain(state.scheduler));
        assert(!state.failed);
        assert(state.published.size() == 65);
        for (std::size_t i = 0; i < 65; ++i) {
            assert(state.published[i] == static_cast<double>(4 * i));
        }
        bool rejected = false;
        try {
            (void)state.region->read(0, 0);
        } catch (const std::logic_error&) {
            rejected = true;
        }
        assert(rejected && "host must not read live actor-owned columns");
        OresNativeActorRef other =
            ores_native_actor_scheduler_spawn(state.scheduler, 1);
        assert(other.scheduler_domain && other.actor_slot != state.owner.actor_slot);
        assert(ores_native_actor_scheduler_send(state.scheduler, other, 3));
        assert(ores_native_actor_scheduler_drain(state.scheduler));
        assert(!state.failed && state.foreign_rejected &&
               "a different actor in the same scheduler cannot borrow the shard");
        ores_native_actor_scheduler_retire(state.scheduler);
        assert(oreslang::runtime::retire_actor_shard_budget_after_done(
                state.scheduler, state.owner, *state.actor_budget));
        assert(state.actor_budget->retired());
        assert(!state.actor_budget->try_reserve(1));
        // The actor and its snapshots may still be held by the trusted host,
        // but none may enter a new logical turn after their retirement fence.
        assert(ores_native_actor_scheduler_try_destroy(state.scheduler));
        state.region.reset();
        assert(state.actor_budget->used_bytes() == 500U * 65U * sizeof(double));
        state.snapshots.clear(); // RAII refund after scheduler stopped
        assert(state.actor_budget->used_bytes() == 0);
        assert(state.vm_budget.used_bytes() == 0);
    }
    puts("actor-owned AoS/SoA/AoSoA dense native kernel parity: OK");
    return 0;
}
