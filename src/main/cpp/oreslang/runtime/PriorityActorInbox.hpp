#pragma once

#include "oreslang/runtime/BoundedActorPort.hpp"

#include <array>
#include <cstddef>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace oreslang::runtime {

/**
 * Host-runtime-only, independently bounded URGENT/NORMAL/BULK user inbox.
 *
 * This is a transport/scheduler building block, NOT a guest-visible priority
 * send capability. Compiler lowering must authenticate who may use URGENT.
 * Privileged supervisor STOP/CANCEL/PANIC delivery is separately pre-reserved
 * in SupervisorControlTurn; it must be polled before this inbox at each
 * authorized logical actor turn, never deposited in any user port.
 *
 * Every lane charges queued, preparing, AND checked-out bytes/frames until
 * the receiver acknowledges physical quiescence. Never run a user callback
 * under the fairness lock. One actor turn still belongs to its physical gate.
 */
template <typename T>
class PriorityActorInbox final {
public:
    enum class Lane { Urgent, Normal, Bulk };
    struct Limits {
        std::size_t frames;
        std::size_t bytes;
    };
    using OwnedDelivery = typename BoundedActorPort<T>::Delivery;

    class Delivery final {
    public:
        Delivery(Lane lane, OwnedDelivery&& owned)
            : lane_(lane), owned_(std::move(owned)) {}
        Delivery(Delivery&&) noexcept = default;
        Delivery& operator=(Delivery&&) noexcept = delete;
        Delivery(const Delivery&) = delete;
        Delivery& operator=(const Delivery&) = delete;

        Lane lane() const noexcept { return lane_; }
        T& value() { return owned_.value(); }
        const T& value() const { return owned_.value(); }
        std::size_t charged_bytes() const noexcept { return owned_.charged_bytes(); }
        bool acknowledge_released() { return owned_.acknowledge_released(); }
    private:
        Lane lane_;
        OwnedDelivery owned_;
    };

    PriorityActorInbox(Limits urgent, Limits normal, Limits bulk)
        : urgent_(urgent.frames, urgent.bytes),
          normal_(normal.frames, normal.bytes),
          bulk_(bulk.frames, bulk.bytes) {}

    PriorityActorInbox(const PriorityActorInbox&) = delete;
    PriorityActorInbox& operator=(const PriorityActorInbox&) = delete;

    // Called only by trusted admission after source/role/priority validation.
    // Failed offers leave the producer's T untouched and do not queue a waiter.
    bool offer(Lane lane, T&& value, std::size_t bytes) {
        return port(lane).offer(std::move(value), bytes);
    }

    /**
     * One fair dequeue opportunity. The 8:4:1 static weighted service cycle
     * is work-conserving: empty lanes are skipped; live NORMAL/BULK traffic
     * cannot be starved by continuously refilled URGENT traffic. Every lane
     * remains FIFO. This does NOT grant a concurrent actor execution turn.
     */
    std::optional<Delivery> try_receive() {
        std::lock_guard<std::mutex> guard(service_mutex_);
        for (std::size_t checked = 0; checked < service_order_.size(); ++checked) {
            const auto lane = service_order_[next_slot_];
            next_slot_ = (next_slot_ + 1) % service_order_.size();
            auto receipt = port(lane).try_receive();
            if (receipt) return Delivery(lane, std::move(*receipt));
        }
        return std::nullopt;
    }

    std::size_t used_frames(Lane lane) const { return port(lane).used_frames(); }
    std::size_t used_bytes(Lane lane) const { return port(lane).used_bytes(); }

    // Seal all lanes even if the first lane still has outstanding credits.
    // Previously admitted messages remain drainable after close.
    void close() {
        std::exception_ptr first;
        auto seal = [&first](BoundedActorPort<T>& lane) {
            try { lane.close(); }
            catch (...) { if (!first) first = std::current_exception(); }
        };
        seal(urgent_);
        seal(normal_);
        seal(bulk_);
        if (first) std::rethrow_exception(first);
    }

private:
    BoundedActorPort<T>& port(Lane lane) {
        switch (lane) {
            case Lane::Urgent: return urgent_;
            case Lane::Normal: return normal_;
            case Lane::Bulk: return bulk_;
        }
        throw std::invalid_argument("forged actor message priority");
    }

    const BoundedActorPort<T>& port(Lane lane) const {
        return const_cast<PriorityActorInbox*>(this)->port(lane);
    }

    inline static constexpr std::array<Lane, 13> service_order_{
        Lane::Urgent, Lane::Urgent, Lane::Urgent, Lane::Urgent,
        Lane::Urgent, Lane::Urgent, Lane::Urgent, Lane::Urgent,
        Lane::Normal, Lane::Normal, Lane::Normal, Lane::Normal,
        Lane::Bulk
    };

    BoundedActorPort<T> urgent_;
    BoundedActorPort<T> normal_;
    BoundedActorPort<T> bulk_;
    std::mutex service_mutex_;
    std::size_t next_slot_ = 0;
};

} // namespace oreslang::runtime
