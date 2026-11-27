#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace oreslang::runtime {
// HOST ONLY: fixed-size exclusive lease reference. No scheduler identity,
// actor transfer, loans, I/O pins, secure erasure or guest integration.
// Pool lifetime must enclose all concurrent calls; handles never expose pointers.
template<std::size_t Slots, std::size_t Bytes>
class FixedBufferReusePrototype final {
    static_assert(Slots > 0 && Bytes > 0);
    struct Slot {
        std::array<std::uint8_t, Bytes> data{};
        std::uint64_t generation = 0;
        bool leased = false;
    };
    inline static std::atomic<std::uint64_t> next_id_{1};
    static std::uint64_t new_id() {
        auto n = next_id_.load(std::memory_order_relaxed);
        for (;;) {
            if (n == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("pool identities exhausted");
            if (next_id_.compare_exchange_weak(n, n + 1, std::memory_order_relaxed)) return n;
        }
    }
public:
    class Lease final {
        friend class FixedBufferReusePrototype;
        std::uint64_t pool_, generation_;
        std::size_t slot_;
        Lease(std::uint64_t p, std::uint64_t g, std::size_t s) noexcept
            : pool_(p), generation_(g), slot_(s) {}
    public:
        Lease(const Lease&) = default; // host ticket; aliases invalidated on release
        Lease& operator=(const Lease&) = default;
    };
    FixedBufferReusePrototype() : id_(new_id()) {}
    FixedBufferReusePrototype(const FixedBufferReusePrototype&) = delete;
    FixedBufferReusePrototype& operator=(const FixedBufferReusePrototype&) = delete;
    std::optional<Lease> try_acquire() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return std::nullopt;
        for (std::size_t i = 0; i < Slots; ++i) {
            auto& s = slots_[i];
            if (s.leased || s.generation == std::numeric_limits<std::uint64_t>::max()) continue;
            ++s.generation;
            s.leased = true;
            ++live_;
            return Lease(id_, s.generation, i);
        }
        return std::nullopt;
    }
    bool release(const Lease& h) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* s = checked(h);
        if (!s) return false;
        s->data.fill(0); // ordinary logical scrub; not cryptographic erasure
        s->leased = false;
        --live_;
        return true;
    }
    bool write(const Lease& h, std::size_t offset, std::uint8_t v) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* s = checked(h);
        if (!s || offset >= Bytes) return false;
        s->data[offset] = v;
        return true;
    }
    std::optional<std::uint8_t> read(const Lease& h, std::size_t offset) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* s = checked(h);
        if (!s || offset >= Bytes) return std::nullopt;
        return s->data[offset];
    }
    // Opaque storage identity within this pool, not an address or authority.
    std::optional<std::size_t> backing_slot(const Lease& h) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!checked(h)) return std::nullopt;
        return h.slot_;
    }
    void close_admission() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }
    std::size_t outstanding() {
        std::lock_guard<std::mutex> lock(mutex_);
        return live_;
    }
    static constexpr std::size_t backing_bytes() noexcept { return Slots * Bytes; }
private:
    Slot* checked(const Lease& h) noexcept {
        if (h.pool_ != id_ || h.slot_ >= Slots) return nullptr;
        auto& s = slots_[h.slot_];
        return s.leased && s.generation == h.generation_ ? &s : nullptr;
    }
    const std::uint64_t id_;
    std::array<Slot, Slots> slots_{};
    std::mutex mutex_;
    std::size_t live_ = 0;
    bool closed_ = false;
};
} // namespace oreslang::runtime
