#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace oreslang::runtime {

// Trusted host-only queue admission substrate. A single instance is owned by
// exactly one tenant IsolateVM incarnation. This is not persistent storage,
// a security sandbox, or an Oreslang guest-callable ABI.
class DurableLockQueues final {
public:
    struct Limits {
        std::size_t max_locks = 1024;
        std::size_t max_entries = 8192;
        std::size_t max_entries_per_lock = 128;
        std::size_t max_payload_bytes = 16 * 1024 * 1024;
    };
    struct Ticket {
        std::uint64_t isolate_generation = 0;
        std::uint64_t lock_id = 0;
        std::uint64_t sequence = 0;
        std::uint64_t operation_id = 0;
        bool operator==(const Ticket& other) const noexcept {
            return isolate_generation == other.isolate_generation &&
                   lock_id == other.lock_id && sequence == other.sequence &&
                   operation_id == other.operation_id;
        }
    };
    struct Admission { Ticket ticket; bool runnable; };
    struct Snapshot {
        std::size_t locks;
        std::size_t entries;
        std::size_t payload_bytes;
        bool accepting;
    };

    explicit DurableLockQueues(std::uint64_t isolate_generation)
        : DurableLockQueues(isolate_generation, Limits{}) {}
    DurableLockQueues(std::uint64_t isolate_generation, Limits limits)
        : generation_(isolate_generation), limits_(limits) {
        if (!generation_ || !limits.max_locks || !limits.max_entries ||
            !limits.max_entries_per_lock || !limits.max_payload_bytes)
            throw std::invalid_argument("isolate generation and queue quotas must be positive");
    }
    DurableLockQueues(const DurableLockQueues&) = delete;
    DurableLockQueues& operator=(const DurableLockQueues&) = delete;

    // Only the admitted owner executes a turn; the scheduler must never run
    // callbacks while holding this queue's internal bookkeeping mutex.
    Admission enqueue(std::uint64_t lock_id, std::uint64_t operation_id,
                      std::size_t payload_bytes) {
        if (!lock_id || !operation_id || !payload_bytes)
            throw std::invalid_argument("lock, operation, and payload must be nonzero");
        std::lock_guard<std::mutex> guard(mu_);
        if (!accepting_) throw std::logic_error("tenant queue is quiescing");
        if (live_operations_.count(operation_id))
            throw std::logic_error("duplicate outstanding operation");
        if (entries_ >= limits_.max_entries ||
            payload_bytes > limits_.max_payload_bytes - payload_bytes_)
            throw std::length_error("tenant queue budget exhausted");
        auto it = locks_.find(lock_id);
        if (it == locks_.end() && locks_.size() >= limits_.max_locks)
            throw std::length_error("lock queue budget exhausted");
        if (it != locks_.end() && it->second.size() >= limits_.max_entries_per_lock)
            throw std::length_error("per-lock queue budget exhausted");
        if (next_sequence_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("queue sequence exhausted");

        // Insert into live-operations index first, rollback on any exception.
        live_operations_.insert(operation_id);
        try {
            auto pair = locks_.try_emplace(lock_id);
            it = pair.first;
            try {
                const Ticket ticket{generation_, lock_id, next_sequence_, operation_id};
                const bool runnable = it->second.empty();
                it->second.push_back(Entry{ticket, payload_bytes});
                ++next_sequence_;
                ++entries_;
                payload_bytes_ += payload_bytes;
                return {ticket, runnable};
            } catch (...) {
                if (pair.second && it->second.empty()) locks_.erase(it);
                throw;
            }
        } catch (...) {
            live_operations_.erase(operation_id);
            throw;
        }
    }

    // Complete or abort the current owner. The next ticket, if any, must be
    // dispatched as a separate scheduler turn (never recursively inline).
    std::optional<Ticket> release(Ticket ticket) {
        std::lock_guard<std::mutex> guard(mu_);
        auto it = checked_front(ticket);
        erase_front(it);
        if (it->second.empty()) { locks_.erase(it); return std::nullopt; }
        return it->second.front().ticket;
    }

    // Cancellation is only permitted for not-yet-running queued turns.
    // A current owner must release after its physical execution has retired.
    void cancel_waiter(Ticket ticket) {
        std::lock_guard<std::mutex> guard(mu_);
        auto it = locks_.find(ticket.lock_id);
        if (ticket.isolate_generation != generation_ || it == locks_.end())
            throw std::logic_error("foreign or stale wait ticket");
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
        throw std::logic_error("ticket is not a queued waiter");
    }

    bool owns(Ticket ticket) const {
        std::lock_guard<std::mutex> guard(mu_);
        auto it = locks_.find(ticket.lock_id);
        return ticket.isolate_generation == generation_ && it != locks_.end() &&
               !it->second.empty() && it->second.front().ticket == ticket;
    }
    void quiesce() {
        std::lock_guard<std::mutex> guard(mu_);
        accepting_ = false;
    }
    Snapshot snapshot() const {
        std::lock_guard<std::mutex> guard(mu_);
        return {locks_.size(), entries_, payload_bytes_, accepting_};
    }
    bool drained() const {
        std::lock_guard<std::mutex> guard(mu_);
        return !accepting_ && entries_ == 0;
    }

private:
    struct Entry { Ticket ticket; std::size_t payload_bytes; };
    using LockMap = std::unordered_map<std::uint64_t, std::deque<Entry>>;
    LockMap::iterator checked_front(const Ticket& ticket) {
        auto it = locks_.find(ticket.lock_id);
        if (ticket.isolate_generation != generation_ || it == locks_.end() ||
            it->second.empty() || !(it->second.front().ticket == ticket))
            throw std::logic_error("release requires exact current owner ticket");
        return it;
    }
    void erase_front(LockMap::iterator it) {
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
    std::uint64_t next_sequence_ = 1;
    std::size_t entries_ = 0;
    std::size_t payload_bytes_ = 0;
    bool accepting_ = true;
};

} // namespace oreslang::runtime
