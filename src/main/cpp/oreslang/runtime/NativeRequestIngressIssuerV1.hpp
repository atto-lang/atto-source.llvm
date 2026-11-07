#pragma once

// Trusted native HTTP/DATA-turn ingress issuer. NOT a source .atto ABI.
// Does not parse HTTP, authenticate a network principal, or prove tenant
// configuration: the embedding host must supply trusted tenant/isolate IDs.
// Distinct scheduler DATA-mail turns are authenticated by the scheduler,
// not caller-provided payload integers, thread_local or guest state.
// This gate is intended to be invoked by the actual native DATA handler;
// the compiler/runtime must NOT expose RequestContextLocalV1::open, raw
// registry admissions or this issuer's constructor to guest source.

#include "oreslang/runtime/NativeRequestDataMailGateV1.hpp"
#include "oreslang/runtime/RequestLocalActorTerminationRegistryV1.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

namespace oreslang::runtime {

class NativeRequestIngressIssuerV1 final {
public:
    using Ptr = std::unique_ptr<NativeRequestIngressIssuerV1>;

    // Bootstrap before actor work; keep issuer and registry alive until all
    // DATA callbacks finish. Registry shutdown/reclamation must precede
    // scheduler destruction. All capacities are fixed and preallocated.
    static Ptr create(OresNativeActorScheduler* scheduler,
                      RequestLocalActorTerminationRegistryV1* registry,
                      std::uint64_t trusted_tenant,
                      std::uint64_t trusted_isolate) {
        if (!scheduler || !registry || !trusted_tenant || !trusted_isolate ||
            !registry->belongs_to_scheduler(scheduler))
            return {};
        const auto slots = ores_native_actor_scheduler_actor_capacity_v1(
            scheduler);
        if (slots == 0 ||
            slots > RequestLocalActorTerminationRegistryV1::max_actor_slots ||
            !registry->claim_data_ingress_issuer_once())
            return {};
        return Ptr(new NativeRequestIngressIssuerV1(
            scheduler, registry, trusted_tenant, trusted_isolate, slots));
    }

    NativeRequestIngressIssuerV1(const NativeRequestIngressIssuerV1&) = delete;
    NativeRequestIngressIssuerV1& operator=(const NativeRequestIngressIssuerV1&) = delete;

    // This returns a DATA-turn gate and never a raw RequestLocal pointer.
    // On any failure no request is published: provisional contexts are
    // closed. An exact scheduler DATA mail can only be issued once.
    std::unique_ptr<NativeRequestDataMailGateV1> admit_current_data_turn(
            OresNativeActorRef actor, TaskCancellation::Ptr cancellation) {
        if (!cancellation || !actor.scheduler_domain || !actor.generation ||
            actor.actor_slot >= slot_count_) return {};
        std::uint64_t authenticated_mail = 0;
        if (!ores_native_actor_scheduler_current_data_mail_sequence(
                scheduler_, actor, &authenticated_mail) ||
            !authenticated_mail ||
            !ores_native_actor_scheduler_is_current_actor(scheduler_, actor))
            return {};

        RequestIdentityV1 identity;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const std::size_t slot_index =
                static_cast<std::size_t>(actor.actor_slot);
            Slot& slot = slots_[slot_index];
            if ((slot.scheduler_domain &&
                    slot.scheduler_domain != actor.scheduler_domain) ||
                (slot.actor_generation &&
                    slot.actor_generation != actor.generation) ||
                authenticated_mail <= slot.last_issued_data_mail ||
                next_request_ == 0 ||
                next_request_ == std::numeric_limits<std::uint64_t>::max())
                return {};
            // A failed admission still burns this monotonic mail receipt.
            // A guest cannot replay the same DATA turn after cancellation,
            // STOP, failed allocation or issuer validation.
            slot.scheduler_domain = actor.scheduler_domain;
            slot.actor_generation = actor.generation;
            slot.last_issued_data_mail = authenticated_mail;
            identity = RequestIdentityV1{
                tenant_, isolate_, next_request_++, 1, actor};
        }

        auto request = registry_->admit_from_trusted_ingress(
            identity, std::move(cancellation));
        if (!request) return {};
        // Construct the actual production DATA gate on the issuing carrier.
        // It authenticates the exact original-mail receipt again, and binds
        // it exactly once against STOP/cancellation. If STOP wins, fail shut.
        try {
            return std::make_unique<NativeRequestDataMailGateV1>(
                *scheduler_, request, actor, authenticated_mail);
        } catch (...) {
            request->close();
            return {};
        }
    }

    std::uint64_t issued_request_count() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return next_request_ - 1;
    }

private:
    struct Slot {
        std::uint64_t scheduler_domain = 0;
        std::uint64_t actor_generation = 0;
        std::uint64_t last_issued_data_mail = 0;
    };
    NativeRequestIngressIssuerV1(
            OresNativeActorScheduler* scheduler,
            RequestLocalActorTerminationRegistryV1* registry,
            std::uint64_t tenant, std::uint64_t isolate,
            std::size_t slots)
        : scheduler_(scheduler), registry_(registry),
          tenant_(tenant), isolate_(isolate),
          slot_count_(slots), slots_(new Slot[slots]) {}

    OresNativeActorScheduler* const scheduler_;
    RequestLocalActorTerminationRegistryV1* const registry_;
    const std::uint64_t tenant_;
    const std::uint64_t isolate_;
    const std::size_t slot_count_;
    std::unique_ptr<Slot[]> slots_;
    mutable std::mutex mutex_;
    std::uint64_t next_request_ = 1;
};

} // namespace oreslang::runtime
