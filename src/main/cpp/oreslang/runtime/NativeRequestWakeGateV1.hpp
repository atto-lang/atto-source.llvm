#pragma once

// Host-only RequestLocal provenance for the exact SourceTask wake escrow.
// This token can be constructed only while a trusted NativeRequestDataMailGateV1
// is authorized for its original DATA-mail. The scheduler later authenticates
// the exact WAKE ticket and physical producer actor, and the outcome MUST be
// READY. CANCELLED/FAILED, STOP, foreign producer, stale slot generation and
// unrelated callback all deny request-local access.
//
// This does NOT wire compiler-generated .atto TaskLocal or a full source Task
// frame; native SourceTask lowering must own and retire this lease explicitly.

#include "oreslang/runtime/NativeRequestDataMailGateV1.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace oreslang::runtime {

class NativeRequestWakeGateV1 final {
public:
    static std::unique_ptr<NativeRequestWakeGateV1> capture(
            const NativeRequestDataMailGateV1& data_origin,
            OresNativeWakeTicket ticket) {
        const OresNativeActorRef actor = data_origin.actor_;
        if (!data_origin.authorized()
                || ticket.producer.scheduler_domain != actor.scheduler_domain
                || ticket.producer.actor_slot != actor.actor_slot
                || ticket.producer.generation != actor.generation
                || !ticket.target.scheduler_domain || !ticket.target.generation
                || !ticket.slot_generation || !ticket.continuation_slot
                || !ticket.continuation_generation || !ticket.code_generation
                || !ticket.descriptor_generation
                || !ores_native_actor_scheduler_current_data_issued_wake(
                    data_origin.scheduler_, ticket, data_origin.original_mail_))
            return {};
        return std::unique_ptr<NativeRequestWakeGateV1>(
            new NativeRequestWakeGateV1(
                data_origin.scheduler_, data_origin.request_,
                data_origin.original_mail_, ticket));
    }

    NativeRequestWakeGateV1(const NativeRequestWakeGateV1&) = delete;
    NativeRequestWakeGateV1& operator=(const NativeRequestWakeGateV1&) = delete;

    bool authorized() const {
        return original_mail_ != 0
            && ores_native_actor_scheduler_is_current_ready_wake(
                scheduler_, ticket_)
            && request_->active();
    }

    template <typename T>
    bool set(const RequestLocalKeyV1<T>& key, T value) {
        if (!authorized()) return false;
        return request_->set(key, std::move(value));
    }

    template <typename T>
    std::optional<T> get_copy(const RequestLocalKeyV1<T>& key) const {
        if (!authorized()) return std::nullopt;
        auto snapshot = request_->get_copy(key);
        if (!snapshot || !authorized()) return std::nullopt;
        return snapshot;
    }

    template <typename T>
    bool remove(const RequestLocalKeyV1<T>& key) {
        if (!authorized()) return false;
        return request_->remove(key);
    }

private:
    NativeRequestWakeGateV1(
            OresNativeActorScheduler* scheduler,
            RequestContextLocalV1::Ptr request,
            std::uint64_t original_mail,
            OresNativeWakeTicket ticket)
        : scheduler_(scheduler), request_(std::move(request)),
          original_mail_(original_mail), ticket_(ticket) {}

    OresNativeActorScheduler* const scheduler_;
    RequestContextLocalV1::Ptr const request_;
    const std::uint64_t original_mail_;
    const OresNativeWakeTicket ticket_;
};

} // namespace oreslang::runtime
