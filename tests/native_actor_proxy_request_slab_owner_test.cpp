#include "oreslang/runtime/ActorOwnedProxyRequestSlab.hpp"
#include "oreslang/runtime/NativeActorScheduler.h"

#if defined(NDEBUG)
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>

using oreslang::runtime::ActorOwnedProxyRequestSlab;
using oreslang::runtime::ActorShardBudget;
using oreslang::runtime::ProxyRequestSlab;

struct State {
    OresNativeActorScheduler* scheduler = nullptr;
    OresNativeActorRef owner = {};
    ActorOwnedProxyRequestSlab::Layout layout = ProxyRequestSlab::Layout::SoA;
    std::uint32_t lanes = 1;
    std::unique_ptr<ActorShardBudget> actor_budget;
    ActorShardBudget vm_budget{16U * 1024U * 1024U};
    std::unique_ptr<ActorOwnedProxyRequestSlab> slab;
    std::unique_ptr<ActorOwnedProxyRequestSlab> other_slab;
    ActorOwnedProxyRequestSlab::Receipt other_pin{};
    std::uint64_t stale_ticket = 0;
    std::uint64_t ticket = 0;
    ActorOwnedProxyRequestSlab::Receipt pin{};
    bool foreign_denied = false;
    bool failed = false;
};

static void on_message(void* context, std::size_t actor, std::uint64_t step) {
    auto& state = *static_cast<State*>(context);
    try {
        if (actor == 1 && step == 3) {
            bool denied = false;
            try { (void)state.slab->settle(state.pin); }
            catch (const std::logic_error&) { denied = true; }
            state.foreign_denied = denied;
            return;
        }
        assert(actor == 0);
        if (step == 1) {
            // Incorrect actor generation may not construct a slab, and quota
            // must remain untouched after rejected construction.
            auto future = state.owner;
            ++future.generation;
            bool wrong_owner_rejected = false;
            try {
                ActorOwnedProxyRequestSlab wrong(state.scheduler, future,
                    *state.actor_budget, state.vm_budget,
                    state.layout, 1, state.lanes);
            } catch (const std::logic_error&) {
                wrong_owner_rejected = true;
            }
            assert(wrong_owner_rejected);
            assert(state.actor_budget->used_bytes() == 0);

            ActorShardBudget tiny{1, state.scheduler, state.owner};
            bool quota_rejected = false;
            try {
                ActorOwnedProxyRequestSlab oversized(state.scheduler, state.owner,
                    tiny, state.vm_budget, state.layout, 1, state.lanes);
            } catch (const std::length_error&) { quota_rejected = true; }
            assert(quota_rejected && tiny.used_bytes() == 0);

            state.slab = std::make_unique<ActorOwnedProxyRequestSlab>(
                state.scheduler, state.owner, *state.actor_budget,
                state.vm_budget, state.layout, 1, state.lanes);
            assert(state.actor_budget->used_bytes() == state.vm_budget.used_bytes());
            assert(state.actor_budget->used_bytes() > 0);
            if (state.layout == ProxyRequestSlab::Layout::SoA) {
                // 16,385 requests occupy 1 MiB + 64 bytes of live SoA data;
                // the existing C ActorArena only admits persistent slots up
                // to 1 MiB, so this must cross a physical arena page boundary.
                const auto before = state.actor_budget->used_bytes();
                {
                    ActorOwnedProxyRequestSlab multi_page(state.scheduler, state.owner,
                        *state.actor_budget, state.vm_budget,
                        state.layout, 16385, 1);
                    assert(state.actor_budget->used_bytes() == before + 2U * 1024U * 1024U);
                    auto sample = multi_page.admit(10, 7, 17);
                    assert(sample && multi_page.route(sample) == 17);
                    assert(multi_page.retire(sample));
                    multi_page.close();
                }
                assert(state.actor_budget->used_bytes() == before);
                assert(state.vm_budget.used_bytes() == before);
            }
            state.stale_ticket = state.slab->admit(5, 1, 42);
            assert(state.stale_ticket != 0 && state.slab->route(state.stale_ticket) == 42);
            state.pin = state.slab->pin(state.stale_ticket);
            assert(state.pin.claim != 0 && state.slab->retire(state.stale_ticket));
            // A second slab deliberately mints the same raw low-level
            // claim. Identity/callback provenance must block misdelivery.
            state.other_slab = std::make_unique<ActorOwnedProxyRequestSlab>(
                state.scheduler, state.owner, *state.actor_budget,
                state.vm_budget, state.layout, 1, state.lanes);
            const auto second_ticket = state.other_slab->admit(7, 3, 88);
            state.other_pin = state.other_slab->pin(second_ticket);
            assert(state.other_pin.claim == state.pin.claim);
            assert(state.other_pin.slab_identity != state.pin.slab_identity);
            assert(!state.other_slab->settle(state.pin));
            assert(state.other_slab->retire(second_ticket));
            assert(state.other_slab->live() == 1);
            assert(state.slab->live() == 1 && state.slab->full());
            assert(state.slab->admit(6, 2, 55) == 0);
        } else if (step == 2) {
            assert(state.slab->settle(state.pin));
            assert(!state.slab->settle(state.pin));
            assert(state.slab->live() == 0);
            state.ticket = state.slab->admit(6, 2, 55);
            assert(state.ticket != 0 && state.ticket != state.stale_ticket);
            assert(state.slab->route(state.ticket) == 55);
            assert(!state.slab->retire(state.stale_ticket));
            assert(!state.slab->settle(state.pin));
            // Tampered actor generation and claim correlation reject safely.
            auto tampered = state.other_pin;
            ++tampered.actor_generation;
            assert(!state.other_slab->settle(tampered));
            auto wrong_request = state.other_pin;
            wrong_request.request = state.ticket;
            assert(!state.other_slab->settle(wrong_request));
            assert(state.other_slab->settle(state.other_pin));
            assert(!state.other_slab->settle(state.other_pin));
            assert(state.other_slab->live() == 0);
            assert(state.slab->retire(state.ticket));
            assert(state.slab->live() == 0);
        } else if (step == 4) {
            assert(state.slab->live() == 0);
            state.slab->close();
            state.other_slab->close();
        } else {
            state.failed = true;
        }
    } catch (...) { state.failed = true; }
}

int main() {
    for (auto layout : {ProxyRequestSlab::Layout::SoA,
                         ProxyRequestSlab::Layout::AoSoA}) {
        for (auto lanes : {1U, 16U, 32U, 64U}) {
            if (layout == ProxyRequestSlab::Layout::AoSoA && lanes == 1) continue;
            if (layout == ProxyRequestSlab::Layout::SoA && lanes != 1) continue;
            State state;
            state.layout = layout;
            state.lanes = lanes;
            state.scheduler = ores_native_actor_scheduler_open(2, 4, 2, on_message, &state);
            assert(state.scheduler != nullptr);
            state.owner = ores_native_actor_scheduler_spawn(state.scheduler, 0);
            assert(state.owner.scheduler_domain != 0);
            state.actor_budget = std::make_unique<ActorShardBudget>(
                16U * 1024U * 1024U, state.scheduler, state.owner);
            assert(ores_native_actor_scheduler_send(state.scheduler, state.owner, 1));
            assert(ores_native_actor_scheduler_send(state.scheduler, state.owner, 2));
            assert(ores_native_actor_scheduler_drain(state.scheduler));
            assert(!state.failed);
            bool host_denied = false;
            try { (void)state.slab->route(state.ticket); }
            catch (const std::logic_error&) { host_denied = true; }
            assert(host_denied);
            auto other = ores_native_actor_scheduler_spawn(state.scheduler, 1);
            assert(other.scheduler_domain != 0);
            assert(ores_native_actor_scheduler_send(state.scheduler, other, 3));
            assert(ores_native_actor_scheduler_drain(state.scheduler));
            assert(!state.failed && state.foreign_denied);
            assert(ores_native_actor_scheduler_send(state.scheduler, state.owner, 4));
            assert(ores_native_actor_scheduler_drain(state.scheduler));
            assert(!state.failed);
            ores_native_actor_scheduler_retire(state.scheduler);
            assert(oreslang::runtime::retire_actor_shard_budget_after_done(
                state.scheduler, state.owner, *state.actor_budget));
            assert(state.actor_budget->retired());
            assert(ores_native_actor_scheduler_try_destroy(state.scheduler));
            state.slab.reset(); // scrub+free before paired accounting refund
            state.other_slab.reset();
            assert(state.actor_budget->used_bytes() == 0);
            assert(state.vm_budget.used_bytes() == 0);
        }
    }
    std::puts("NATIVE_ACTOR_PROXY_REQUEST_SLAB_OWNER_OK");
}
