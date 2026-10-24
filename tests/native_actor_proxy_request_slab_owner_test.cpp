#include "oreslang/runtime/ActorOwnedProxyRequestSlab.hpp"
#include "oreslang/runtime/NativeActorScheduler.h"

#if defined(NDEBUG)
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

using oreslang::runtime::ActorOwnedProxyRequestSlab;
using oreslang::runtime::ActorShardBudget;
using oreslang::runtime::NativeTransportCallbackGate;
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
    NativeTransportCallbackGate::Permit outstanding{};
    std::optional<NativeTransportCallbackGate::DrainReceipt> other_proof;
    bool foreign_denied = false;
    bool closed_foreign_denied = false;
    bool failed = false;
};

static void on_message(void* context, std::size_t actor, std::uint64_t step) {
    auto& state = *static_cast<State*>(context);
    try {
        if (actor == 1 && step == 5) {
            bool close_denied = false, read_denied = false;
            try { state.slab->close(); }
            catch (const std::logic_error&) { close_denied = true; }
            try { (void)state.slab->capacity(); }
            catch (const std::logic_error&) { read_denied = true; }
            state.closed_foreign_denied = close_denied && read_denied;
            return;
        }
        if (actor == 1 && step == 3) {
            bool denied = false;
            try { (void)state.slab->settle(state.pin); }
            catch (const std::logic_error&) { denied = true; }
            bool identity_denied = false;
            try { (void)state.slab->identity(); }
            catch (const std::logic_error&) { identity_denied = true; }
            bool quiescence_denied = false;
            assert(state.other_proof);
            try { state.other_slab->confirm_transport_quiescence(*state.other_proof); }
            catch (const std::logic_error&) { quiescence_denied = true; }
            state.foreign_denied = denied && identity_denied && quiescence_denied;
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
                    auto& callbacks = multi_page.transport_callbacks();
                    callbacks.stop_admission();
                    auto drain = callbacks.try_drain();
                    assert(drain);
                    bool live_confirmation_denied = false;
                    try { multi_page.confirm_transport_quiescence(*drain); }
                    catch (const std::logic_error&) { live_confirmation_denied = true; }
                    assert(live_confirmation_denied && multi_page.live() == 1);
                    assert(multi_page.retire(sample));
                    bool unconfirmed_denied = false;
                    try { multi_page.close(); }
                    catch (const std::logic_error&) { unconfirmed_denied = true; }
                    assert(unconfirmed_denied);
                    multi_page.confirm_transport_quiescence(*drain);
                    multi_page.close();
                }
                assert(state.actor_budget->used_bytes() == before);
                assert(state.vm_budget.used_bytes() == before);
            }
            state.stale_ticket = state.slab->admit(5, 1, 42);
            assert(state.stale_ticket != 0 && state.slab->route(state.stale_ticket) == 42);
            state.pin = state.slab->pin(state.stale_ticket);
            state.outstanding = state.slab->transport_callbacks().try_admit();
            assert(state.outstanding);
            assert(state.pin.claim != 0 && state.slab->retire(state.stale_ticket));
            // A retired request with a pending external completion still
            // owns its physical slot. Closing must fail without refunding
            // quota, invalidating the active receipt, or closing the slab.
            const auto charged_before_close = state.actor_budget->used_bytes();
            bool pinned_close_rejected = false;
            try { state.slab->close(); }
            catch (const std::logic_error&) { pinned_close_rejected = true; }
            assert(pinned_close_rejected);
            assert(state.slab->live() == 1);
            assert(state.slab->pending(state.stale_ticket) == 1);
            assert(state.actor_budget->used_bytes() == charged_before_close);
            assert(state.slab->route(state.stale_ticket) == 42);
            assert(!state.slab->transport_callbacks().try_drain());
            assert(state.slab->pending(state.stale_ticket) == 1);
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
            state.outstanding.release();
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
            auto& second_gate = state.other_slab->transport_callbacks();
            second_gate.stop_admission();
            state.other_proof = second_gate.try_drain();
            assert(state.other_proof);
            bool wrong_slab_proof_denied = false;
            try { state.slab->confirm_transport_quiescence(*state.other_proof); }
            catch (const std::logic_error&) { wrong_slab_proof_denied = true; }
            assert(wrong_slab_proof_denied);
            assert(state.slab->retire(state.ticket));
            assert(state.slab->live() == 0);
        } else if (step == 4) {
            assert(state.slab->live() == 0);
            auto& callbacks = state.slab->transport_callbacks();
            auto late = callbacks.try_admit();
            assert(late); // an admitted callback can outlive its logical request
            callbacks.stop_admission();
            assert(!callbacks.try_admit());
            // Close is not the only race: neither a new request nor a new
            // callback receipt can be admitted after the freeze linearizes.
            const auto charged_on_freeze = state.actor_budget->used_bytes();
            bool frozen_request_denied = false;
            try { (void)state.slab->admit(9, 9, 9); }
            catch (const std::logic_error&) { frozen_request_denied = true; }
            assert(frozen_request_denied && state.slab->live() == 0);
            bool frozen_pin_denied = false;
            try { (void)state.slab->pin(state.ticket); }
            catch (const std::logic_error&) { frozen_pin_denied = true; }
            assert(frozen_pin_denied);
            assert(state.actor_budget->used_bytes() == charged_on_freeze);
            assert(!callbacks.try_drain());
            // Logical quiescence (live==0) must NOT permit page reclamation
            // until the trusted transport has explicitly acknowledged that
            // its timers/wakers and callback envelopes have been joined.
            bool missing_transport_fence_denied = false;
            try { state.slab->close(); }
            catch (const std::logic_error&) { missing_transport_fence_denied = true; }
            assert(missing_transport_fence_denied);
            assert(state.slab->live() == 0);
            late.release(); // simulates physical I/O completion retirement
            auto drain = callbacks.try_drain();
            assert(drain);
            state.slab->confirm_transport_quiescence(*drain);
            state.slab->confirm_transport_quiescence(*drain); // idempotent
            bool admission_after_fence_denied = false;
            try { (void)state.slab->admit(7, 10, 20); }
            catch (const std::logic_error&) { admission_after_fence_denied = true; }
            assert(admission_after_fence_denied);
            bool pin_after_fence_denied = false;
            try { (void)state.slab->pin(state.ticket); }
            catch (const std::logic_error&) { pin_after_fence_denied = true; }
            assert(pin_after_fence_denied);
            state.slab->close();
            // A successful close invalidates the complete host API, not
            // only mutating operations: no stale metadata reads after close.
            bool closed_read_denied = false;
            try { (void) state.slab->capacity(); }
            catch (const std::logic_error&) { closed_read_denied = true; }
            assert(closed_read_denied);
            bool closed_admission_denied = false;
            try { (void) state.slab->admit(8, 2, 3); }
            catch (const std::logic_error&) { closed_admission_denied = true; }
            assert(closed_admission_denied);
            bool closed_completion_denied = false;
            try { (void)state.slab->settle(state.pin); }
            catch (const std::logic_error&) { closed_completion_denied = true; }
            assert(closed_completion_denied); // stale async callback after close
            state.slab->close(); // idempotent owner cleanup
            // Deliberately omit other_slab.close() after the host explicitly
            // joins its I/O callbacks; only the combined transport fence,
            // logical drain, and exact scheduler-done gate allows the
            // trusted retired destructor to scrub and refund its pages.
            assert(state.other_proof);
            state.other_slab->confirm_transport_quiescence(*state.other_proof);
        } else {
            state.failed = true;
        }
    } catch (...) { state.failed = true; }
}

int main() {
    // Identity exhaustion must be permanent, not a 64-bit wrap to reused
    // slab IDs. This test is single-threaded and resets the process-local
    // counter before constructing any actor slabs.
    auto& counter = oreslang::runtime::next_proxy_slab_identity;
    const auto starting_identity = counter.load();
    counter.store(UINT64_MAX - 1U);
    assert(oreslang::runtime::reserve_proxy_slab_identity() == UINT64_MAX - 1U);
    bool identity_exhausted = false;
    try {
        (void) oreslang::runtime::reserve_proxy_slab_identity();
    } catch (const std::overflow_error&) { identity_exhausted = true; }
    assert(identity_exhausted && counter.load() == UINT64_MAX);
    counter.store(starting_identity);
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
            bool identity_denied = false;
            try { (void)state.slab->identity(); }
            catch (const std::logic_error&) { identity_denied = true; }
            assert(identity_denied);
            auto other = ores_native_actor_scheduler_spawn(state.scheduler, 1);
            assert(other.scheduler_domain != 0);
            assert(ores_native_actor_scheduler_send(state.scheduler, other, 3));
            assert(ores_native_actor_scheduler_drain(state.scheduler));
            assert(!state.failed && state.foreign_denied);
            assert(ores_native_actor_scheduler_send(state.scheduler, state.owner, 4));
            assert(ores_native_actor_scheduler_drain(state.scheduler));
            assert(!state.failed);
            assert(ores_native_actor_scheduler_send(state.scheduler, other, 5));
            assert(ores_native_actor_scheduler_drain(state.scheduler));
            assert(!state.failed && state.closed_foreign_denied);
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
