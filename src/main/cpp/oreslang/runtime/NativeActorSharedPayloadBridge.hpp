#pragma once
#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/SharedImmutablePayload.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace oreslang::runtime {
/** Native host integration with the REAL actor scheduler DATA lane.
 * The scheduler decides admission, order, actor incarnation and STOP, while
 * CONTROL remains independent. Scheduler mail carries a 64-bit (index,gen)
 * descriptor ticket, never a guest pointer. Source .atto lowering is pending.
 * Not a hard-real-time or guest-visible FFI, and not a second actor mailbox.
 */
class NativeActorSharedPayloadBridge final {
public:
    enum class SendStatus {
        admitted, busy, full, closed, invalid_sender, invalid_payload,
        invalid_target, scheduler_rejected
    };
    using Handler = void (*)(void*, OresNativeActorRef,
                             const SharedImmutablePayload&);
    static constexpr std::size_t max_slots = 65536;
    explicit NativeActorSharedPayloadBridge(
            std::size_t capacity, std::size_t max_queued_bytes,
            Handler handler, void* handler_ctx)
        : slots_(capacity), free_(capacity),
          max_bytes_(max_queued_bytes),
          handler_(handler), handler_ctx_(handler_ctx) {
        if (capacity == 0 || capacity > max_slots || !handler)
            throw std::invalid_argument("invalid shared payload bridge capacity");
        for (std::size_t i = 0; i < capacity; ++i)
            free_[i] = static_cast<std::uint32_t>(i);
        available_ = capacity;
    }
    NativeActorSharedPayloadBridge(const NativeActorSharedPayloadBridge&) = delete;
    NativeActorSharedPayloadBridge& operator=(const NativeActorSharedPayloadBridge&) = delete;
    // A scheduler callback may still reference this object until target DONE.
    ~NativeActorSharedPayloadBridge() noexcept {
        if (!finished_.load(std::memory_order_acquire)) std::abort();
    }

    // Bootstrap-only. Caller guarantees no concurrent bind or destruction.
    bool bind(OresNativeActorScheduler* scheduler, OresNativeActorRef target) {
        if (!scheduler || !target.scheduler_domain || !target.generation)
            return false;
        std::lock_guard guard(mu_);
        if (scheduler_ || finished_.load(std::memory_order_relaxed))
            return false;
        if (!ores_native_actor_scheduler_poll_ready(scheduler, target))
            return false;
        scheduler_ = scheduler;
        target_ = target;
        return true;
    }

    // Require an authenticated running actor, not an untrusted actor ID.
    SendStatus try_send_from(
            OresNativeActorRef source, const SharedImmutablePayload& payload) {
        if (!payload.valid()) return SendStatus::invalid_payload;
        std::uint64_t source_mail = 0;
        if (!scheduler_ || !ores_native_actor_scheduler_is_current_actor(
                scheduler_, source) ||
            !ores_native_actor_scheduler_current_data_mail_sequence(
                scheduler_, source, &source_mail) || !source_mail)
            return SendStatus::invalid_sender;
        std::unique_lock guard(mu_, std::try_to_lock);
        if (!guard.owns_lock()) return SendStatus::busy;
        if (closed_) return SendStatus::closed;
        const auto charge = payload.charged_bytes();
        if (charge > max_bytes_ - charged_) return SendStatus::full;
        // Exhausted per-slot generations are retired forever, never wrapped.
        std::size_t index = slots_.size();
        while (available_) {
            const auto candidate = free_[--available_];
            if (slots_[candidate].generation == UINT32_MAX) continue;
            index = candidate;
            break;
        }
        if (index == slots_.size()) return SendStatus::full;
        auto& entry = slots_[index];
        if (entry.descriptor) std::abort();
        ++entry.generation;
        entry.descriptor.emplace(payload);
        const std::uint64_t ticket =
            (std::uint64_t(entry.generation) << 32) | std::uint64_t(index + 1);
        // Scheduler's own lock linearizes admission against STOP.
        // The callback may start, but cannot take mu_ until credits commit.
        std::uint64_t admitted_mail_sequence = 0;
        const auto status = ores_native_actor_scheduler_try_send_receipted(
            scheduler_, target_, ticket, &admitted_mail_sequence);
        if (status != ORES_NATIVE_SEND_ACCEPTED) {
            entry.descriptor.reset();
            entry.mail_sequence = 0;
            free_[available_++] = static_cast<std::uint32_t>(index);
            if (status == ORES_NATIVE_SEND_MAILBOX_FULL ||
                status == ORES_NATIVE_SEND_RESOURCE_EXHAUSTED)
                return SendStatus::full;
            if (status == ORES_NATIVE_SEND_TERMINATED)
                return SendStatus::closed;
            if (status == ORES_NATIVE_SEND_INVALID_REF)
                return SendStatus::invalid_target;
            return SendStatus::scheduler_rejected;
        }
        if (!admitted_mail_sequence) std::abort();
        entry.mail_sequence = admitted_mail_sequence;
        charged_ += charge;
        return SendStatus::admitted;
    }

    // Call only from the real scheduler's target DATA handler.
    bool dispatch_data(size_t actor_slot, std::uint64_t ticket) noexcept {
        std::uint64_t current_mail_sequence = 0;
        if (!scheduler_ || actor_slot != target_.actor_slot ||
            !ores_native_actor_scheduler_is_current_actor(scheduler_, target_) ||
            !ores_native_actor_scheduler_current_data_mail_sequence(
                scheduler_, target_, &current_mail_sequence) ||
            !current_mail_sequence) {
            rejected_dispatch_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        std::optional<SharedImmutablePayload> received;
        {
            std::lock_guard guard(mu_);
            const auto low = static_cast<std::uint32_t>(ticket);
            const auto generation = static_cast<std::uint32_t>(ticket >> 32);
            if (!low || low > slots_.size() || !generation) {
                rejected_dispatch_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            const auto index = static_cast<std::size_t>(low - 1);
            auto& entry = slots_[index];
            if (entry.generation != generation || !entry.descriptor ||
                entry.mail_sequence != current_mail_sequence) {
                rejected_dispatch_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            received.emplace(std::move(*entry.descriptor));
            entry.descriptor.reset();
            entry.mail_sequence = 0;
            charged_ -= received->charged_bytes();
            free_[available_++] = static_cast<std::uint32_t>(index);
        }
        // Receiver owns its own immutable descriptor. No guest code under mu_.
        try {
            handler_(handler_ctx_, target_, *received);
            delivered_.fetch_add(1, std::memory_order_relaxed);
            return true;
        } catch (...) {
            handler_failures_.fetch_add(1, std::memory_order_relaxed);
            return false; // Never throw across native C scheduler callback.
        }
    }

    // STOP_SEALED can abandon tickets: release them after scheduler DONE.
    // Caller fences scheduler lifetime and never destroys this before close.
    bool close_after_done() {
        if (!scheduler_ ||
            !ores_native_actor_scheduler_poll_done(scheduler_, target_))
            return false;
        std::lock_guard guard(mu_);
        if (finished_.load(std::memory_order_relaxed)) return true;
        closed_ = true;
        for (auto& entry : slots_) {
            entry.descriptor.reset();
            entry.mail_sequence = 0;
        }
        charged_ = 0;
        available_ = 0;
        finished_.store(true, std::memory_order_release);
        return true;
    }

    // Host-only diagnostics; not for hard-RT actor callbacks.
    std::size_t queued_slots() const {
        std::lock_guard guard(mu_);
        std::size_t count = 0;
        for (const auto& entry : slots_) if (entry.descriptor) ++count;
        return count;
    }
    std::size_t queued_charged_bytes() const {
        std::lock_guard guard(mu_);
        return charged_;
    }
    std::uint64_t delivered() const noexcept {
        return delivered_.load(std::memory_order_relaxed);
    }
    std::uint64_t rejected_dispatch() const noexcept {
        return rejected_dispatch_.load(std::memory_order_relaxed);
    }
    std::uint64_t handler_failures() const noexcept {
        return handler_failures_.load(std::memory_order_relaxed);
    }

private:
    struct Slot {
        std::uint32_t generation = 0;
        std::uint64_t mail_sequence = 0;
        std::optional<SharedImmutablePayload> descriptor;
    };
    mutable std::mutex mu_;
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_;
    std::size_t available_ = 0;
    const std::size_t max_bytes_;
    std::size_t charged_ = 0;
    OresNativeActorScheduler* scheduler_ = nullptr;
    OresNativeActorRef target_{};
    Handler handler_;
    void* handler_ctx_;
    bool closed_ = false;
    std::atomic<bool> finished_{false};
    std::atomic<std::uint64_t> delivered_{0};
    std::atomic<std::uint64_t> rejected_dispatch_{0};
    std::atomic<std::uint64_t> handler_failures_{0};
};
} // namespace oreslang::runtime
