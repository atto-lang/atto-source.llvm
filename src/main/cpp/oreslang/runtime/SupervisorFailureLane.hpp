#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace oreslang {

/**
 * Native actor-supervision CONTROL lane, independent of data mailboxes.
 * Reserve a node before admitting a child. After reservation, publication
 * writes bounded data into that node and appends an intrusive FIFO link:
 * no per-failure queue growth, heap allocation or tracing GC.
 *
 * The caller is responsible for scheduling the supervisor's logical lane.
 * This class never runs callbacks under its mutex, and is not guest lowering.
 */
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
        std::string parent_id;

        Failure(ActorKey actor, Kind kind, std::string summary,
                std::string parent_id = {})
            : actor(std::move(actor)), kind(kind),
              summary(std::move(summary)), parent_id(std::move(parent_id)) {}
    };

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

    void reserve(const ActorKey& actor) {
        if (actor.id.empty()) throw std::invalid_argument("empty actor identity");
        std::lock_guard<std::mutex> lock(mu_);
        if (children_.size() >= capacity_) {
            throw std::logic_error("supervision slots exhausted");
        }
        auto [it, inserted] = children_.try_emplace(actor);
        if (!inserted) throw std::logic_error("duplicate supervision reservation");
        // std::map does not relocate nodes; this pointer survives until ack.
        it->second.actor = &it->first;
    }

    bool publish(Failure failure) {
        return publish_preallocated(failure.actor, failure.kind,
                                    failure.summary, failure.parent_id);
    }

    /**
     * Allocation-free terminal publication after pre-reservation, barring
     * platform mutex/runtime behavior. Fail closed for unknown generations.
     * Does not execute handlers or acquire an ordinary mailbox lock.
     */
    bool publish_preallocated(const ActorKey& actor, Kind kind,
                              std::string_view summary,
                              std::string_view parent_id = {}) {
        std::lock_guard<std::mutex> lock(mu_);
        Slot& slot = require(actor);
        if (slot.has_event) return false;
        slot.kind = kind;
        store(slot.summary, slot.summary_length, summary);
        store(slot.parent_id, slot.parent_length, parent_id);
        slot.has_event = true;
        enqueue(slot);
        return true;
    }

    std::optional<Failure> poll() {
        std::lock_guard<std::mutex> lock(mu_);
        if (!head_) return std::nullopt;
        Slot* slot = head_;
        if (!slot->queued || slot->in_flight || !slot->has_event) {
            throw std::logic_error("supervisor queue invariant");
        }
        // Materialize before unlink. On allocation failure, CONTROL remains
        // queued and no in-flight message becomes inaccessible.
        Failure failure{*slot->actor, slot->kind,
            std::string(slot->summary.data(), slot->summary_length),
            std::string(slot->parent_id.data(), slot->parent_length)};
        unlink_head();
        slot->in_flight = true;
        return failure;
    }

    void ack(const ActorKey& actor) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = children_.find(actor);
        if (it == children_.end() || !it->second.in_flight) {
            throw std::logic_error("event is not in flight");
        }
        children_.erase(it);
    }

    void retry(const ActorKey& actor) {
        std::lock_guard<std::mutex> lock(mu_);
        Slot& slot = require(actor);
        if (!slot.in_flight) throw std::logic_error("event is not in flight");
        slot.in_flight = false;
        enqueue(slot);
    }

    void retire_gracefully(const ActorKey& actor) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = children_.find(actor);
        if (it == children_.end()) throw std::logic_error("unknown/stale actor generation");
        if (it->second.has_event) throw std::logic_error("failure requires ack");
        children_.erase(it);
    }

    bool dispatch_one(Handler& handler) {
        auto event = poll();
        if (!event) return false;
        try {
            switch (event->kind) {
                case Kind::Throw: handler.on_throw(*event); break;
                case Kind::Raise: handler.on_raise(*event); break;
                case Kind::Panic: handler.on_panic(*event); break;
                case Kind::Cancel: handler.on_cancel(*event); break;
                case Kind::HostFailure: handler.on_host_failure(*event); break;
            }
            ack(event->actor);
        } catch (...) {
            retry(event->actor);
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
        return pending_count_;
    }

private:
    struct Slot {
        const ActorKey* actor = nullptr;
        Kind kind = Kind::HostFailure;
        std::array<char, 512> summary{};
        std::array<char, 128> parent_id{};
        std::size_t summary_length = 0;
        std::size_t parent_length = 0;
        bool has_event = false;
        bool in_flight = false;
        bool queued = false;
        Slot* next = nullptr;
    };

    template <std::size_t N>
    static void store(std::array<char, N>& buffer, std::size_t& length,
                      std::string_view text) {
        length = std::min(N, text.size());
        if (length != 0) std::memcpy(buffer.data(), text.data(), length);
    }

    Slot& require(const ActorKey& actor) {
        auto it = children_.find(actor);
        if (it == children_.end()) throw std::logic_error("unknown/stale actor generation");
        return it->second;
    }

    void enqueue(Slot& slot) {
        if (slot.queued || slot.in_flight) throw std::logic_error("double queue");
        slot.next = nullptr;
        slot.queued = true;
        if (tail_) tail_->next = &slot;
        else head_ = &slot;
        tail_ = &slot;
        ++pending_count_;
    }

    void unlink_head() {
        Slot* current = head_;
        head_ = current->next;
        if (!head_) tail_ = nullptr;
        current->next = nullptr;
        current->queued = false;
        --pending_count_;
    }

    const std::size_t capacity_;
    mutable std::mutex mu_;
    std::map<ActorKey, Slot> children_;
    Slot* head_ = nullptr;
    Slot* tail_ = nullptr;
    std::size_t pending_count_ = 0;
};

} // namespace oreslang
