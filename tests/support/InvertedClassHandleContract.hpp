#pragma once
// Host-only zero-GC, bounded reference oracle for future inverted class MIR.
// NOT an .atto parser, compiler ABI, actor arena implementation or guest pointer.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace oreslang::dop::contract {

template<std::size_t Capacity>
class InvertedClassHandleContract final {
    static_assert(Capacity > 0 && Capacity <= 1024,
                  "host reference contract supports 1..1024 rows");
public:
    struct Value {
        std::int64_t x;
        std::int64_t y;
    };

    // Guest-visible identity must be opaque: no public coordinate constructor.
    // A copied handle does not keep the owner alive and cannot become a pointer.
    class Handle final {
        std::uint64_t issuer_;
        std::uint64_t actor_;
        std::uint32_t slot_;
        std::uint32_t generation_;
        Handle(std::uint64_t issuer, std::uint64_t actor,
               std::uint32_t slot, std::uint32_t generation) noexcept
            : issuer_(issuer), actor_(actor), slot_(slot),
              generation_(generation) {}
        friend class InvertedClassHandleContract;
    public:
        Handle(const Handle&) noexcept = default;
        Handle& operator=(const Handle&) noexcept = default;
    };

private:
    struct Slot {
        std::uint32_t row = 0;
        std::uint32_t generation = 1;
        bool live = false;
    };
    // Fixed-size inline metadata and physical columns: no new/delete, shared
    // ownership, collector, raw row pointer, or unbounded allocation.
    std::array<std::int64_t, Capacity> x_{};
    std::array<std::int64_t, Capacity> y_{};
    std::array<Slot, Capacity> slots_{};
    std::array<std::uint32_t, Capacity> row_slots_{};
    std::array<std::uint32_t, Capacity> free_slots_{};
    mutable std::mutex mutex_;
    std::size_t length_ = 0;
    std::size_t slot_count_ = 0;
    std::size_t free_count_ = 0;
    const std::uint64_t actor_;
    const std::uint64_t issuer_;
    bool closed_ = false;
    inline static std::atomic<std::uint64_t> next_issuer_{1};

    static std::uint64_t fresh_issuer() {
        auto current = next_issuer_.load(std::memory_order_relaxed);
        for (;;) {
            if (current == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("inverted class issuer exhausted");
            if (next_issuer_.compare_exchange_weak(
                    current, current + 1, std::memory_order_relaxed))
                return current;
        }
    }
    void open() const {
        if (closed_) throw std::logic_error("inverted class owner is closed");
    }
    std::uint32_t checked_row(const Handle& handle) const {
        open();
        if (handle.issuer_ != issuer_ || handle.actor_ != actor_ ||
                handle.slot_ >= slot_count_)
            throw std::out_of_range("foreign inverted class handle");
        const Slot& slot = slots_[handle.slot_];
        if (!slot.live || slot.generation != handle.generation_ ||
                slot.row >= length_ || row_slots_[slot.row] != handle.slot_)
            throw std::out_of_range("stale inverted class handle");
        return slot.row;
    }
    static std::int64_t add_checked(std::int64_t a, std::int64_t b) {
        constexpr auto max = std::numeric_limits<std::int64_t>::max();
        constexpr auto min = std::numeric_limits<std::int64_t>::min();
        if ((b > 0 && a > max - b) || (b < 0 && a < min - b))
            throw std::overflow_error("inverted class receiver arithmetic overflow");
        return a + b;
    }
    void scrub_row(std::size_t row) noexcept {
        // Physical erasure should not be optimized away on owner teardown.
        volatile std::int64_t* left = &x_[row];
        volatile std::int64_t* right = &y_[row];
        *left = 0;
        *right = 0;
    }
public:
    explicit InvertedClassHandleContract(std::uint64_t actor_id)
        : actor_(actor_id), issuer_(fresh_issuer()) {
        if (!actor_id) throw std::invalid_argument("actor identity must be nonzero");
    }
    InvertedClassHandleContract(const InvertedClassHandleContract&) = delete;
    InvertedClassHandleContract& operator=(const InvertedClassHandleContract&) = delete;
    ~InvertedClassHandleContract() { close(); }

    Handle push(std::int64_t x, std::int64_t y) {
        std::lock_guard<std::mutex> lock(mutex_);
        open();
        if (length_ == Capacity || (free_count_ == 0 && slot_count_ == Capacity))
            throw std::out_of_range("inverted class capacity or identity exhausted");
        const std::uint32_t slot = free_count_ ? free_slots_[--free_count_]
                                               : static_cast<std::uint32_t>(slot_count_++);
        Slot& metadata = slots_[slot];
        metadata.row = static_cast<std::uint32_t>(length_);
        metadata.live = true;
        row_slots_[length_] = slot;
        x_[length_] = x;
        y_[length_] = y;
        ++length_;
        return Handle(issuer_, actor_, slot, metadata.generation);
    }
    Value snapshot(const Handle& handle) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto row = checked_row(handle);
        return Value{x_[row], y_[row]};
    }
    // A receiver-style update executes atomically under owner lock. Compute
    // *both* checked values before committing either field (no partial write).
    void move_by(const Handle& handle, std::int64_t dx, std::int64_t dy) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto row = checked_row(handle);
        const auto x = add_checked(x_[row], dx);
        const auto y = add_checked(y_[row], dy);
        x_[row] = x;
        y_[row] = y;
    }
    void erase_swap(const Handle& handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto row = checked_row(handle);
        const auto last = length_ - 1;
        if (row != last) {
            x_[row] = x_[last];
            y_[row] = y_[last];
            const auto displaced = row_slots_[last];
            row_slots_[row] = displaced;
            slots_[displaced].row = row;
        }
        scrub_row(last);
        --length_;
        Slot& removed = slots_[handle.slot_];
        removed.live = false;
        // Never wrap a generation: permanently retire exhausted identities.
        if (removed.generation != std::numeric_limits<std::uint32_t>::max()) {
            ++removed.generation;
            free_slots_[free_count_++] = handle.slot_;
        }
    }
    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        open();
        return length_;
    }
    void close() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return;
        closed_ = true;
        for (std::size_t row = 0; row < Capacity; ++row) scrub_row(row);
        for (auto& slot : slots_) slot.live = false;
        length_ = 0;
        free_count_ = 0;
    }
};

} // namespace oreslang::dop::contract
