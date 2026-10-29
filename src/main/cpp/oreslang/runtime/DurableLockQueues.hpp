#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace oreslang::runtime {

// Host-only tenant scheduler substrate. A queue is not a durable log or guest ABI.
// Its tickets are host-issued values, never authenticated user capabilities.
class DurableLockQueues final {
public:
    struct Limits {
        std::size_t max_locks = 1024;
        std::size_t max_entries = 8192;
        std::size_t max_entries_per_lock = 128;
        std::size_t max_payload_bytes = 16 * 1024 * 1024;
    };
    struct ActorKey {
        std::uint64_t id = 0;
        std::uint64_t generation = 0;
        bool operator==(const ActorKey& other) const noexcept {
            return id == other.id && generation == other.generation;
        }
    };
    struct Ticket {
        std::uint64_t isolate_generation = 0;
        std::uint64_t lock_id = 0;
        std::uint64_t sequence = 0;
        std::uint64_t operation_id = 0;
        ActorKey actor{};
        bool operator==(const Ticket& other) const noexcept {
            return isolate_generation == other.isolate_generation &&
                   lock_id == other.lock_id && sequence == other.sequence &&
                   operation_id == other.operation_id && actor == other.actor;
        }
    };
    struct Admission { Ticket ticket; bool lock_head; };
    struct Snapshot {
        std::size_t locks;
        std::size_t entries;
        std::size_t payload_bytes;
        std::size_t running_actors;
        bool accepting;
    };

    explicit DurableLockQueues(std::uint64_t generation)
        : DurableLockQueues(generation, Limits{}) {}
    DurableLockQueues(std::uint64_t generation, Limits limits)
        : generation_(generation), limits_(limits) {
        if (!generation_ || !limits.max_locks || !limits.max_entries ||
            !limits.max_entries_per_lock || !limits.max_payload_bytes)
            throw std::invalid_argument("positive isolate generation and queue budgets required");
    }
    DurableLockQueues(const DurableLockQueues&) = delete;
    DurableLockQueues& operator=(const DurableLockQueues&) = delete;

    Admission enqueue(ActorKey actor, std::uint64_t lock_id,
                      std::uint64_t operation_id, std::size_t bytes) {
        if (!actor.id || !actor.generation || !lock_id || !operation_id || !bytes)
            throw std::invalid_argument("actor, lock, operation and bytes must be nonzero");
        std::lock_guard<std::mutex> guard(mu_);
        if (!accepting_) throw std::logic_error("tenant is quiescing");
        if (live_operations_.count(operation_id))
            throw std::logic_error("operation already pending in tenant");
        if (entries_ >= limits_.max_entries ||
            bytes > limits_.max_payload_bytes - payload_bytes_)
            throw std::length_error("tenant queue budget exhausted");
        auto it = locks_.find(lock_id);
        if (it == locks_.end() && locks_.size() >= limits_.max_locks)
            throw std::length_error("tenant lock budget exhausted");
        if (it != locks_.end() && it->second.size() >= limits_.max_entries_per_lock)
            throw std::length_error("lock depth exceeded");
        if (next_sequence_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("ticket sequence exhausted");

        live_operations_.insert(operation_id);
        try {
            auto pair = locks_.try_emplace(lock_id);
            it = pair.first;
            try {
                Ticket ticket{generation_, lock_id, next_sequence_, operation_id, actor};
                const bool head = it->second.empty();
                it->second.push_back(Entry{ticket, bytes, false});
                ++next_sequence_;
                ++entries_;
                payload_bytes_ += bytes;
                return {ticket, head};
            } catch (...) {
                if (pair.second && it->second.empty()) locks_.erase(it);
                throw;
            }
        } catch (...) {
            live_operations_.erase(operation_id);
            throw;
        }
    }

    // Scheduler-only: returns false when another lock already owns this actor.
    // No callback runs under this bookkeeping mutex. Claim is atomic across
    // all lock queues in this tenant; it prevents actor reentrancy.
    bool claim(Ticket ticket) {
        std::lock_guard<std::mutex> guard(mu_);
        auto it = checked_front(ticket);
        if (it->second.front().claimed)
            throw std::logic_error("ticket already claimed");
        if (running_actors_.count(ticket.actor)) return false;
        running_actors_.insert(ticket.actor); // may throw before state change
        it->second.front().claimed = true;
        return true;
    }

    // Only call AFTER the actor execution/continuation was physically retired.
    // Returns the next FIFO head; scheduler also rescans ready_heads() to wake
    // any other lock queued behind this actor's global non-reentrancy gate.
    std::optional<Ticket> finish(Ticket ticket) {
        std::lock_guard<std::mutex> guard(mu_);
        auto it = checked_front(ticket);
        if (!it->second.front().claimed || !running_actors_.count(ticket.actor))
            throw std::logic_error("finish requires a previously claimed owner");
        running_actors_.erase(ticket.actor);
        remove_head(it);
        if (it->second.empty()) { locks_.erase(it); return std::nullopt; }
        return it->second.front().ticket;
    }

    // Head cancellation is valid only before a scheduler claim, including on
    // shutdown. If a head is claimed, its physical owner must finish it.
    std::optional<Ticket> cancel_unstarted_head(Ticket ticket) {
        std::lock_guard<std::mutex> guard(mu_);
        auto it = checked_front(ticket);
        if (it->second.front().claimed)
            throw std::logic_error("cannot cancel a running owner");
        remove_head(it);
        if (it->second.empty()) { locks_.erase(it); return std::nullopt; }
        return it->second.front().ticket;
    }

    void cancel_waiter(Ticket ticket) {
        std::lock_guard<std::mutex> guard(mu_);
        auto it = locks_.find(ticket.lock_id);
        if (ticket.isolate_generation != generation_ || it == locks_.end())
            throw std::logic_error("foreign or stale queued ticket");
        // No empty entries are retained in locks_.
        for (auto pos = std::next(it->second.begin()); pos != it->second.end(); ++pos) {
            if (pos->ticket == ticket) {
                const auto bytes = pos->payload_bytes;
                it->second.erase(pos);
                live_operations_.erase(ticket.operation_id);
                --entries_;
                payload_bytes_ -= bytes;
                return;
            }
        }
        throw std::logic_error("not a waiting ticket");
    }

    bool has_actor(ActorKey actor) const {
        std::lock_guard<std::mutex> guard(mu_);
        for (const auto& lane : locks_)
            for (const auto& entry : lane.second)
                if (entry.ticket.actor == actor) return true;
        return false;
    }

    std::vector<Ticket> ready_heads() const {
        std::lock_guard<std::mutex> guard(mu_);
        std::vector<Ticket> result;
        result.reserve(locks_.size());
        for (const auto& lane : locks_) {
            const Entry& e = lane.second.front();
            if (!e.claimed && !running_actors_.count(e.ticket.actor))
                result.push_back(e.ticket);
        }
        // Multiple ready heads can refer to one actor. The scheduler must
        // claim, and only one of those claims can succeed at a time.
        return result;
    }
    void quiesce() {
        std::lock_guard<std::mutex> guard(mu_);
        accepting_ = false;
    }
    Snapshot snapshot() const {
        std::lock_guard<std::mutex> guard(mu_);
        return {locks_.size(), entries_, payload_bytes_, running_actors_.size(), accepting_};
    }
    bool drained() const {
        std::lock_guard<std::mutex> guard(mu_);
        return !accepting_ && entries_ == 0 && running_actors_.empty();
    }

private:
    struct ActorHash {
        std::size_t operator()(ActorKey key) const noexcept {
            return std::hash<std::uint64_t>{}(key.id) ^
                (std::hash<std::uint64_t>{}(key.generation) << 1);
        }
    };
    struct Entry { Ticket ticket; std::size_t payload_bytes; bool claimed; };
    using LockMap = std::unordered_map<std::uint64_t, std::deque<Entry>>;
    LockMap::iterator checked_front(const Ticket& ticket) {
        auto it = locks_.find(ticket.lock_id);
        if (ticket.isolate_generation != generation_ || it == locks_.end() ||
            it->second.empty() || !(it->second.front().ticket == ticket))
            throw std::logic_error("ticket is not this isolate's FIFO head");
        return it;
    }
    void remove_head(LockMap::iterator it) {
        const Entry entry = it->second.front();
        it->second.pop_front();
        live_operations_.erase(entry.ticket.operation_id);
        --entries_;
        payload_bytes_ -= entry.payload_bytes;
    }
    const std::uint64_t generation_;
    const Limits limits_;
    mutable std::mutex mu_;
    LockMap locks_;
    std::unordered_set<std::uint64_t> live_operations_;
    std::unordered_set<ActorKey, ActorHash> running_actors_;
    std::uint64_t next_sequence_ = 1;
    std::size_t entries_ = 0;
    std::size_t payload_bytes_ = 0;
    bool accepting_ = true;
};
} // namespace oreslang::runtime
