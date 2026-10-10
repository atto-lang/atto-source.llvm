#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>

namespace oreslang::runtime {

// One non-repeating identity sequence shared by every ActorHeap specialization.
inline std::atomic<std::uint64_t> next_actor_heap_identity{1};

// Experimental per-actor fixed-capacity native object store. No malloc/free,
// tracing, reference counting, or collector safepoint in the read/write/free
// path. Only the one bound carrier may mutate it. Handles are generation-
// checked and tied to this exact heap; raw pointers never leave the API.
//
// This is a lower-level runtime prototype, not compiler-enforced ownership
// and not an OS security boundary. Objects are bytes only: general Oreslang
// references/destructors will need lowering and explicit lifetime proofs.
template<std::size_t Slots = 64, std::size_t SlotBytes = 256>
class ActorHeap final {
    static_assert(Slots > 0 && SlotBytes > 0, "actor heap must have capacity");
    static_assert(Slots <= std::numeric_limits<std::uint32_t>::max(),
                  "slot index exceeds handle width");
public:
    struct Handle {
        std::uint64_t heap = 0;
        std::uint32_t slot = 0;
        std::uint64_t generation = 0;
    };

private:
    struct Slot {
        alignas(std::max_align_t) std::array<std::byte, SlotBytes> bytes{};
        std::uint64_t generation = 1;
        std::size_t length = 0;
        bool live = false;
    };
    static void scrub(Slot& item) noexcept {
        // Volatile writes prevent the compiler from deleting an end-of-life
        // wipe as dead stores. This is an actor cleanup path, not hot tracing GC.
        volatile std::byte* bytes = item.bytes.data();
        for (std::size_t n = 0; n < SlotBytes; ++n)
            bytes[n] = std::byte{0};
    }
    std::array<Slot, Slots> slots_{};
    const std::uint64_t identity_ =
        next_actor_heap_identity.fetch_add(1, std::memory_order_relaxed);
    // Binding may be attempted concurrently by startup/admission code.
    // The mutex is used ONLY during admission, never in the heap hot path.
    std::mutex bind_mutex_;
    std::thread::id owner_{};
    std::atomic<bool> bound_{false};
    std::size_t live_bytes_ = 0;
    std::size_t live_blocks_ = 0;

    bool owner_thread() const noexcept {
        // acquire synchronizes visibility of the immutable owner_ assignment.
        return bound_.load(std::memory_order_acquire) &&
               std::this_thread::get_id() == owner_;
    }

    Slot* find(Handle handle) noexcept {
        if (!owner_thread() || handle.heap != identity_ ||
            handle.slot >= Slots || !handle.generation)
            return nullptr;
        auto& item = slots_[handle.slot];
        return item.live && item.generation == handle.generation ? &item : nullptr;
    }

public:
    ActorHeap() {
        // Zero is reserved as an invalid identity. Never silently wrap.
        if (identity_ == 0 || identity_ == std::numeric_limits<std::uint64_t>::max())
            std::terminate();
    }
    ~ActorHeap() noexcept {
        for (auto& item : slots_) scrub(item);
    }
    ActorHeap(const ActorHeap&) = delete;
    ActorHeap& operator=(const ActorHeap&) = delete;
    ActorHeap(ActorHeap&&) = delete;
    ActorHeap& operator=(ActorHeap&&) = delete;

    bool bind_carrier() noexcept {
        std::lock_guard<std::mutex> lock(bind_mutex_);
        if (!bound_.load(std::memory_order_relaxed)) {
            owner_ = std::this_thread::get_id();
            bound_.store(true, std::memory_order_release);
        }
        return owner_thread();
    }
    std::uint64_t identity() const noexcept { return identity_; }
    std::size_t live_bytes() const noexcept { return owner_thread() ? live_bytes_ : 0; }
    std::size_t live_blocks() const noexcept { return owner_thread() ? live_blocks_ : 0; }

    std::optional<Handle> allocate(std::size_t bytes) noexcept {
        if (!owner_thread() || bytes == 0 || bytes > SlotBytes) return std::nullopt;
        for (std::size_t i = 0; i < Slots; ++i) {
            auto& item = slots_[i];
            if (item.live || item.generation == 0) continue;
            item.live = true;
            item.length = bytes;
            item.bytes.fill(std::byte{0}); // do not disclose contents of prior generation
            live_bytes_ += bytes;
            ++live_blocks_;
            return Handle{identity_, static_cast<std::uint32_t>(i), item.generation};
        }
        return std::nullopt;
    }

    bool write(Handle handle, std::size_t offset, const void* data,
               std::size_t bytes) noexcept {
        Slot* item = find(handle);
        if (!item || (!data && bytes) || offset > item->length ||
            bytes > item->length - offset) return false;
        if (bytes) std::memcpy(item->bytes.data() + offset, data, bytes);
        return true;
    }

    bool read(Handle handle, std::size_t offset, void* data,
              std::size_t bytes) noexcept {
        Slot* item = find(handle);
        if (!item || (!data && bytes) || offset > item->length ||
            bytes > item->length - offset) return false;
        if (bytes) std::memcpy(data, item->bytes.data() + offset, bytes);
        return true;
    }

    bool release(Handle handle) noexcept {
        Slot* item = find(handle);
        if (!item) return false;
        live_bytes_ -= item->length;
        --live_blocks_;
        scrub(*item);
        item->length = 0;
        item->live = false;
        // Never reuse a generation after integer wraparound.
        item->generation = item->generation == std::numeric_limits<std::uint64_t>::max()
                ? 0 : item->generation + 1;
        return true;
    }
};

} // namespace oreslang::runtime
