#pragma once
#include "oreslang/runtime/TrustedActorBufferSchedulerGate.hpp"
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace oreslang::runtime {

// Narrow HOST-ONLY typed actor DATA delivery adapter, NOT a guest ABI.
// A real source on_message(owned Array<int>) must lower through a distinct
// typed runtime entry; it must NEVER reinterpret the scalar i64 mailbox.
// Admission, owner transfer, FIFO and STOP are provided by the scheduler gate.
class TrustedActorBufferDelivery final {
public:
    using Gate = TrustedActorBufferSchedulerGate;
    using Handle = TrustedActorBufferPool::Handle;

    class Received final {
        friend class TrustedActorBufferDelivery;
        Gate& gate_;
        Gate::Turn& turn_;
        Handle value_;
        bool active_ = true;
        Received(Gate& gate, Gate::Turn& turn, Handle value)
            : gate_(gate), turn_(turn), value_(std::move(value)) {}
    public:
        Received(const Received&) = delete;
        Received& operator=(const Received&) = delete;
        Received(Received&&) = delete;
        Received& operator=(Received&&) = delete;
        std::uint8_t read(std::size_t index) const {
            if (!active_) throw std::logic_error("already consumed DATA");
            return gate_.read(turn_, value_, index);
        }
        void release() {
            if (!active_) return;
            gate_.drop(turn_, value_);
            active_ = false;
        }
        // If a host handler throws, its admitted DATA is reclaimed before
        // its scheduler Turn exits. No duplicate delivery or retained alias.
        ~Received() noexcept {
            if (!active_) return;
            try { gate_.drop(turn_, value_); }
            catch (...) { std::terminate(); }
        }
    };

    explicit TrustedActorBufferDelivery(
            std::shared_ptr<TrustedActorBufferPool> pool)
        : gate_(std::move(pool)) {}
    TrustedActorBufferDelivery(const TrustedActorBufferDelivery&) = delete;
    TrustedActorBufferDelivery& operator=(const TrustedActorBufferDelivery&) = delete;

    // All of these operations are scheduler-controlled HOST APIs. Actor
    // generation is supplied by the verified runtime, never by guest source.
    void admit(TrustedBufferActor actor, std::size_t mailbox_capacity) {
        gate_.register_actor(actor, mailbox_capacity);
    }
    Gate::Turn enter_turn(TrustedBufferActor actor) {
        return gate_.enter_turn(actor);
    }
    Handle allocate(const Gate::Turn& sender, std::size_t size) {
        return gate_.allocate(sender, size);
    }
    void write(const Gate::Turn& sender, const Handle& h,
               std::size_t offset, std::uint8_t byte) {
        gate_.write(sender, h, offset, byte);
    }
    void send(const Gate::Turn& sender, TrustedBufferActor target, const Handle& h) {
        gate_.send(sender, target, h);
    }
    // The sender can explicitly reclaim a rejected message. An accepted
    // transfer invalidates the old sender alias, so this also rejects
    // accidental double drop after successful publication.
    void drop(const Gate::Turn& sender, const Handle& h) {
        gate_.drop(sender, h);
    }

    template <typename F>
    decltype(auto) deliver_one(TrustedBufferActor target, F&& handler) {
        // Receiver turn remains active throughout handler execution and
        // stack unwinding. No borrowed pointer or raw guest handle escapes.
        auto turn = gate_.enter_turn(target);
        Received received(gate_, turn, gate_.receive(turn));
        using Result = std::invoke_result_t<F, Received&>;
        static_assert(std::is_void<Result>::value || std::is_integral<Result>::value ||
                      std::is_enum<Result>::value,
                      "typed DATA handler may return only a scalar value, not borrowed pointers");
        if constexpr (std::is_void<Result>::value) {
            std::forward<F>(handler)(received);
            received.release();
        } else {
            Result result = std::forward<F>(handler)(received);
            received.release();
            return result;
        }
    }

    std::size_t pending(TrustedBufferActor actor) const {
        return gate_.pending(actor);
    }
    void retire(TrustedBufferActor actor) { gate_.retire_actor(actor); }
    bool close() noexcept { return gate_.close(); }
private:
    Gate gate_;
};
} // namespace oreslang::runtime
