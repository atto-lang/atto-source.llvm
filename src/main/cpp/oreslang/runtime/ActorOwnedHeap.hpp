#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <type_traits>

// Prototype for actor-owned, NON-GC native storage. This is a fixed-capacity
// byte heap, not yet compiler-lowered Oreslang guest objects or a sandbox.
// Actor execution must externally enforce exclusive access and quiescence.
namespace oreslang::runtime {

template<std::size_t Bytes, std::size_t Slots>
class ActorOwnedHeap final {
    static_assert(Bytes > 0 && Slots > 0, "bounded positive heap required");

public:
    class Handle final {
        friend class ActorOwnedHeap;
        std::uint64_t heap_ = 0;
        std::uint64_t incarnation_ = 0;
        std::size_t slot_ = 0;
        std::uint64_t generation_ = 0;

        Handle(std::uint64_t heap, std::uint64_t incarnation,
               std::size_t slot, std::uint64_t generation) noexcept
            : heap_(heap), incarnation_(incarnation),
              slot_(slot), generation_(generation) {}
    public:
        Handle() = default; // default handle is invalid
        bool operator==(const Handle& rhs) const noexcept {
            return heap_ == rhs.heap_ && incarnation_ == rhs.incarnation_
                && slot_ == rhs.slot_ && generation_ == rhs.generation_;
        }
    };

private:
    struct Slot {
        std::size_t offset = 0;
        std::size_t length = 0;
        std::uint64_t generation = 1;
        bool live = false;
    };

    // The byte array itself lives in native memory, not a Java object heap.
    alignas(std::max_align_t) std::array<std::byte, Bytes> memory_{};
    std::array<Slot, Slots> slots_{};
    std::array<std::size_t, Slots> free_slots_{};
    std::size_t next_slot_ = 0;
    std::size_t free_count_ = 0;
    std::size_t used_ = 0;
    std::size_t live_count_ = 0;
    const std::uint64_t actor_id_;
    const std::uint64_t heap_identity_;
    std::uint64_t incarnation_ = 1;
    bool retired_ = false;

    static std::uint64_t new_identity() {
        static std::atomic<std::uint64_t> next{1};
        const auto id = next.fetch_add(1, std::memory_order_relaxed);
        if (id == 0 || id == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("actor heap identity exhausted");
        return id;
    }

    bool valid(const Handle& handle) const noexcept {
        if (retired_ || handle.heap_ != heap_identity_
                || handle.incarnation_ != incarnation_
                || handle.slot_ >= next_slot_)
            return false;
        const auto& slot = slots_[handle.slot_];
        return slot.live && slot.generation == handle.generation_;
    }

public:
    explicit ActorOwnedHeap(std::uint64_t actor_id)
        : actor_id_(actor_id), heap_identity_(new_identity()) {
        if (actor_id == 0) throw std::invalid_argument("zero actor id");
    }

    ActorOwnedHeap(const ActorOwnedHeap&) = delete;
    ActorOwnedHeap& operator=(const ActorOwnedHeap&) = delete;
    ActorOwnedHeap(ActorOwnedHeap&&) = delete;
    ActorOwnedHeap& operator=(ActorOwnedHeap&&) = delete;

    std::uint64_t actor_id() const noexcept { return actor_id_; }
    std::size_t used() const noexcept { return used_; }
    std::size_t live_allocations() const noexcept { return live_count_; }
    bool retired() const noexcept { return retired_; }
    static constexpr std::size_t capacity = Bytes;

    // Allocation does not call malloc or invoke any GC. It fails closed under
    // fixed slot/byte limits. Never expose a raw pointer to actor memory.
    std::optional<Handle> allocate(
            std::size_t count, std::size_t alignment = alignof(std::max_align_t)) noexcept {
        if (retired_ || count == 0 || count > Bytes || used_ > Bytes
                || alignment == 0 || (alignment & (alignment - 1))
                || alignment > alignof(std::max_align_t)
                || (free_count_ == 0 && next_slot_ >= Slots))
            return std::nullopt;

        const auto base = reinterpret_cast<std::uintptr_t>(memory_.data());
        if (base > std::numeric_limits<std::uintptr_t>::max() - used_)
            return std::nullopt;
        const auto address = base + used_;
        const auto mask = static_cast<std::uintptr_t>(alignment - 1);
        if (address > std::numeric_limits<std::uintptr_t>::max() - mask)
            return std::nullopt;
        const auto aligned = (address + mask) & ~mask;
        const auto padding = static_cast<std::size_t>(aligned - address);
        if (padding > Bytes - used_ || count > Bytes - used_ - padding)
            return std::nullopt;

        const std::size_t slot_id = free_count_ != 0
            ? free_slots_[--free_count_] : next_slot_++;
        auto& slot = slots_[slot_id];
        slot.offset = used_ + padding;
        slot.length = count;
        slot.live = true;
        used_ += padding + count;
        ++live_count_;
        return Handle(heap_identity_, incarnation_, slot_id, slot.generation);
    }

    bool remove(const Handle& handle) noexcept {
        if (!valid(handle)) return false;
        auto& slot = slots_[handle.slot_];
        // Refuse ABA if the slot-generation space is exhausted.
        if (slot.generation == std::numeric_limits<std::uint64_t>::max())
            return false;
        std::fill_n(memory_.data() + slot.offset, slot.length, std::byte{0});
        slot.live = false;
        ++slot.generation;
        free_slots_[free_count_++] = handle.slot_;
        --live_count_;
        return true;
    }

    template<class T>
    bool write(const Handle& handle, const T& value,
               std::size_t offset = 0) noexcept {
        static_assert(std::is_trivially_copyable<T>::value,
                      "only trivially copyable native values; no hidden drops");
        if (!valid(handle)) return false;
        const auto& slot = slots_[handle.slot_];
        if (offset > slot.length || sizeof(T) > slot.length - offset)
            return false;
        std::memcpy(memory_.data() + slot.offset + offset, &value, sizeof(T));
        return true;
    }

    template<class T>
    std::optional<T> read(
            const Handle& handle, std::size_t offset = 0) const noexcept {
        static_assert(std::is_trivially_copyable<T>::value
                      && std::is_nothrow_default_constructible<T>::value,
                      "only trivially copyable values with noexcept default construction");
        if (!valid(handle)) return std::nullopt;
        const auto& slot = slots_[handle.slot_];
        if (offset > slot.length || sizeof(T) > slot.length - offset)
            return std::nullopt;
        T value{};
        std::memcpy(&value, memory_.data() + slot.offset + offset, sizeof(T));
        return value;
    }

    // Supervisor must first prove no active actor handlers, pending wakers,
    // FFI pins, in-flight operations or escaped references. This class does
    // not itself implement the global quiescence protocol.
    void retire_quiesced() noexcept {
        if (retired_) return;
        std::fill(memory_.begin(), memory_.end(), std::byte{0});
        retired_ = true;
        used_ = 0;
        live_count_ = 0;
        free_count_ = 0;
        next_slot_ = 0;
        // Permanent retirement: new actors construct a fresh unique heap.
        // Existing handles can never resolve again, even at the same address.
    }
};

} // namespace oreslang::runtime
