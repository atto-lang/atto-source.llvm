#pragma once

// Production DATA-turn entrypoint for native RequestLocal host storage.
// Unlike a caller-provided current_mail callback, this adapter asks the
// NativeActorScheduler for its own original DATA-mail admission receipt.
// WAKE/SourceTask resume intentionally remains UNAUTHORIZED until separate
// runtime-issued original-mail provenance has been implemented and tested.
// This is not an AOT guest ABI, tenant sandbox or hard-real-time allocation.

#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/RequestContextLocalV1.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace oreslang::runtime {

class NativeRequestDataMailGateV1 final {
    friend class NativeRequestWakeGateV1;
public:
    NativeRequestDataMailGateV1(
            OresNativeActorScheduler& scheduler,
            RequestContextLocalV1::Ptr request,
            OresNativeActorRef actor,
            std::uint64_t original_admitted_mail)
        : scheduler_(&scheduler), request_(std::move(request)),
          actor_(actor), original_mail_(original_admitted_mail) {
        if (!request_ || !request_->identity().valid()
                || !request_->belongs_to(actor_)
                || !original_mail_
                || !request_->bind_original_mail_once(original_mail_)) {
            throw std::invalid_argument("invalid native DATA-mail request binding");
        }
    }

    NativeRequestDataMailGateV1(const NativeRequestDataMailGateV1&) = delete;
    NativeRequestDataMailGateV1& operator=(const NativeRequestDataMailGateV1&) = delete;

    bool authorized() const {
        std::uint64_t authenticated_mail = 0;
        return ores_native_actor_scheduler_current_data_mail_sequence(
                    scheduler_, actor_, &authenticated_mail)
            && authenticated_mail == original_mail_ && request_->active();
    }

    template <typename T>
    bool set(const RequestLocalKeyV1<T>& key, T value) {
        if (!authorized()) return false;
        // RequestContextLocalV1 atomically arbitrates request cancellation
        // and quota. Source scheduler STOP and request cancellation must be
        // linked by trusted ingress before this is exposed to guest code.
        return request_->set(key, std::move(value));
    }

    template <typename T>
    std::optional<T> get_copy(const RequestLocalKeyV1<T>& key) const {
        if (!authorized()) return std::nullopt;
        auto snapshot = request_->get_copy(key);
        // Prevent an already-retired DATA turn's snapshot from escaping
        // if the physical actor/request origin changed during copying.
        if (!snapshot || !authorized()) return std::nullopt;
        return snapshot;
    }

    template <typename T>
    bool remove(const RequestLocalKeyV1<T>& key) {
        if (!authorized()) return false;
        return request_->remove(key);
    }

private:
    OresNativeActorScheduler* const scheduler_;
    RequestContextLocalV1::Ptr const request_;
    const OresNativeActorRef actor_;
    const std::uint64_t original_mail_;
};

} // namespace oreslang::runtime
