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
using oreslang::dop::Layout;

struct TestState {
    OresNativeActorScheduler* scheduler = nullptr;
    OresNativeActorRef owner = {};
    Layout layout = Layout::SoA;
    std::unique_ptr<ActorShardBudget> actor_budget;
    ActorShardBudget vm_budget{64U * 1024U * 1024U};
    std::unique_ptr<ActorShardColumns> region;
    std::vector<double> published;
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
            state.published = state.region->snapshot_column(0);
            auto copy = state.region->snapshot_column(1);
            copy[0] = 123456;
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
        assert(ores_native_actor_scheduler_try_destroy(state.scheduler));
        state.region.reset();
        assert(state.actor_budget->used_bytes() == 0);
        assert(state.vm_budget.used_bytes() == 0);
    }
    puts("actor-owned AoS/SoA/AoSoA dense native kernel parity: OK");
    return 0;
}
