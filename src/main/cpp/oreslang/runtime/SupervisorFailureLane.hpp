#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

namespace oreslang {

// CONTROL-plane substrate only: the source compiler does not yet lower
// guest ActorGroup hooks or throw/raise/panic into this channel.
class SupervisorFailureLane {
public:
    struct ActorKey {
        std::string id;
        std::uint64_t generation;

        bool operator<(const ActorKey& rhs) const {
            return std::tie(id, generation) < std::tie(rhs.id, rhs.generation);
        }
    };

    enum class Kind { Throw, Raise, Panic, Cancel, HostFailure };

    struct Failure {
        ActorKey actor;
        Kind kind;
        std::string summary;
    };

    // Pure virtual methods require exhaustive host-side dispatch coverage.
    // Guest on_panic/on_raise/on_throw compiler enforcement is not implemented.
    struct Handler {
        virtual ~Handler() = default;
        virtual void on_throw(const Failure&) = 0;
        virtual void on_raise(const Failure&) = 0;
        virtual void on_panic(const Failure&) = 0;
        virtual void on_cancel(const Failure&) = 0;
        virtual void on_host_failure(const Failure&) = 0;
    };

    explicit SupervisorFailureLane(std::size_t capacity) : capacity_(capacity) {
        if (capacity == 0) throw std::invalid_argument("capacity must be positive");
    }

    // Call before spawning each child. Capacity exhaustion rejects admission.
    void reserve(const ActorKey& actor) {
        if (actor.id.empty()) throw std::invalid_argument("empty actor identity");
        std::lock_guard<std::mutex> lock(mu_);
        if (children_.count(actor)) throw std::logic_error("duplicate supervision reservation");
        if (children_.size() >= capacity_) throw std::logic_error("supervision slots exhausted");
        children_.emplace(actor, Slot{});
    }

    // Terminal publication never competes with the user's data mailbox.
    // At most one terminal event per reserved actor generation is admitted.
    bool publish(Failure failure) {
        std::lock_guard<std::mutex> lock(mu_);
        auto& slot = require(failure.actor);
        if (slot.event) return false;
        if (failure.summary.size() > 512) failure.summary.resize(512);
        slot.event = std::move(failure);
        pending_.push_back(slot.event->actor);
        return true;
    }

    // Marks an event in flight. Reservation persists until explicit ack.
    std::optional<Failure> poll() {
        std::lock_guard<std::mutex> lock(mu_);
        if (pending_.empty()) return std::nullopt;
        auto actor = pending_.front();
        pending_.pop_front();
        auto& slot = require(actor);
        if (slot.in_flight || !slot.event) throw std::logic_error("queue invariant");
        slot.in_flight = true;
        return slot.event;
    }

    void ack(const ActorKey& actor) {
        std::lock_guard<std::mutex> lock(mu_);
        auto& slot = require(actor);
        if (!slot.in_flight) throw std::logic_error("event is not in flight");
        children_.erase(actor);
    }

    void retry(const ActorKey& actor) {
        std::lock_guard<std::mutex> lock(mu_);
        auto& slot = require(actor);
        if (!slot.in_flight) throw std::logic_error("event is not in flight");
        slot.in_flight = false;
        pending_.push_back(actor);
    }

    void retire_gracefully(const ActorKey& actor) {
        std::lock_guard<std::mutex> lock(mu_);
        auto& slot = require(actor);
        if (slot.event) throw std::logic_error("failure requires ack");
        children_.erase(actor);
    }

    // Never invoke guest/host handler while holding the CONTROL queue mutex.
    bool dispatch_one(Handler& handler) {
        const auto failure = poll();
        if (!failure) return false;
        try {
            switch (failure->kind) {
                case Kind::Throw: handler.on_throw(*failure); break;
                case Kind::Raise: handler.on_raise(*failure); break;
                case Kind::Panic: handler.on_panic(*failure); break;
                case Kind::Cancel: handler.on_cancel(*failure); break;
                case Kind::HostFailure: handler.on_host_failure(*failure); break;
            }
            ack(failure->actor);
        } catch (...) {
            retry(failure->actor);
            throw;
        }
        return true;
    }

    std::size_t reserved_count() const {
        std::lock_guard<std::mutex> lock(mu_);
        return children_.size();
    }
    std::size_t pending_count() const {
        std::lock_guard<std::mutex> lock(mu_);
        return pending_.size();
    }

private:
    struct Slot {
        std::optional<Failure> event;
        bool in_flight = false;
    };

    Slot& require(const ActorKey& actor) {
        const auto found = children_.find(actor);
        if (found == children_.end()) throw std::logic_error("unknown/stale actor generation");
        return found->second;
    }

    const std::size_t capacity_;
    mutable std::mutex mu_;
    std::map<ActorKey, Slot> children_;
    std::deque<ActorKey> pending_;
};

} // namespace oreslang
