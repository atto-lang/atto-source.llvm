#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace oreslang::runtime {

/** Opaque timer identity. Both the owner domain and id must match. */
struct NativeTimerHandle {
    std::uint64_t domain = 0;
    std::uint64_t id = 0;
    friend bool operator==(NativeTimerHandle, NativeTimerHandle) = default;
};

/**
 * Bounded, monotonic, host-CONTROL timer queue.
 *
 * This queue never calls guest code or parks a carrier. Its owner supplies
 * timestamps from one steady/monotonic clock, polls expired handles, and
 * enqueues the matching pinned continuation onto its owning scheduler lane.
 * An expired handle by itself is NOT permission to execute a continuation.
 */
class NativeTimerQueue final {
public:
    struct Expired {
        NativeTimerHandle handle;
        std::uint64_t deadline_ns;
    };

    explicit NativeTimerQueue(std::size_t capacity = 4096)
        : capacity_(capacity), domain_(next_domain()) {
        if (capacity == 0) throw std::invalid_argument("timer capacity must be positive");
    }

    NativeTimerQueue(const NativeTimerQueue&) = delete;
    NativeTimerQueue& operator=(const NativeTimerQueue&) = delete;

    NativeTimerHandle arm_after(std::uint64_t now_ns, std::uint64_t delay_ns) {
        if (delay_ns > std::numeric_limits<std::uint64_t>::max() - now_ns) {
            throw std::overflow_error("timer deadline overflow");
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) throw std::logic_error("timer owner closed");
        if (by_id_.size() >= capacity_) throw std::length_error("timer admission full");
        if (next_id_ == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("timer identity exhausted");
        }
        const auto id = next_id_++;
        const auto deadline = now_ns + delay_ns;
        auto it = by_deadline_.emplace(Key{deadline, id}, 0).first;
        try {
            by_id_.emplace(id, it);
        } catch (...) {
            by_deadline_.erase(it);
            throw;
        }
        return NativeTimerHandle{domain_, id};
    }

    bool cancel(NativeTimerHandle handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (handle.domain != domain_ || closed_) return false;
        const auto found = by_id_.find(handle.id);
        if (found == by_id_.end()) return false;
        by_deadline_.erase(found->second);
        by_id_.erase(found);
        return true;
    }

    /** Returns at most max_items expired handles; does NOT execute callbacks. */
    std::vector<Expired> poll(std::uint64_t now_ns, std::size_t max_items) {
        std::vector<Expired> result;
        if (max_items == 0) return result;
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return result;
        const auto count = max_items < by_deadline_.size()
                ? max_items : by_deadline_.size();
        result.reserve(count);
        while (result.size() < max_items && !by_deadline_.empty()) {
            const auto it = by_deadline_.begin();
            if (it->first.first > now_ns) break;
            const auto deadline = it->first.first;
            const auto id = it->first.second;
            result.push_back(Expired{NativeTimerHandle{domain_, id}, deadline});
            by_id_.erase(id);
            by_deadline_.erase(it);
        }
        return result;
    }

    std::optional<std::uint64_t> next_deadline_ns() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || by_deadline_.empty()) return std::nullopt;
        return by_deadline_.begin()->first.first;
    }

    std::size_t pending() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return by_id_.size();
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        by_id_.clear();
        by_deadline_.clear();
    }

private:
    using Key = std::pair<std::uint64_t, std::uint64_t>;
    using DeadlineMap = std::map<Key, unsigned char>;

    static std::uint64_t next_domain() {
        static std::atomic<std::uint64_t> value{1};
        auto candidate = value.load(std::memory_order_relaxed);
        for (;;) {
            if (candidate == 0 ||
                candidate == std::numeric_limits<std::uint64_t>::max()) {
                throw std::overflow_error("timer owner domain exhausted");
            }
            if (value.compare_exchange_weak(candidate, candidate + 1,
                                            std::memory_order_relaxed)) {
                return candidate;
            }
        }
    }

    const std::size_t capacity_;
    const std::uint64_t domain_;
    mutable std::mutex mutex_;
    bool closed_ = false;
    std::uint64_t next_id_ = 1;
    DeadlineMap by_deadline_;
    std::map<std::uint64_t, DeadlineMap::iterator> by_id_;
};

}  // namespace oreslang::runtime
