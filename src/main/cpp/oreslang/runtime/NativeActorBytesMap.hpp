#pragma once
#include "oreslang/runtime/ActorHeapIdentity.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace oreslang::runtime {

// One allocation-domain sequence shared across ALL template instantiations.
// A domain ID is a stale-instance fence, NOT guest-side authentication.
namespace detail {
inline std::atomic<std::uint64_t> next_native_actor_bytes_map_domain{1};
inline std::uint64_t new_native_actor_bytes_map_domain() {
    auto current = next_native_actor_bytes_map_domain.load(std::memory_order_relaxed);
    while (current != 0 && current != std::numeric_limits<std::uint64_t>::max()) {
        if (next_native_actor_bytes_map_domain.compare_exchange_weak(
                current, current + 1, std::memory_order_relaxed,
                std::memory_order_relaxed))
            return current;
    }
    throw std::overflow_error("native actor Bytes map domain exhausted");
}
} // namespace detail

// Native, fixed-budget backing primitive for a future Map<Bytes, Bytes> lowering.
//
// This class deliberately implements NO Redis commands, WAL, networking,
// mailbox dispatch or source-level ownership rules. The source compiler still
// must check Bytes move/borrow lifetimes and ensure that each invocation comes
// from the scheduler's authenticated actor turn, never guest-supplied IDs.
//
// The byte payloads are inline, actor-owned, binary-exact and deterministic:
// no string decoding, gc, per-operation malloc, pointers returned to callers,
// unbounded resize, or lock parking on an actor carrier. All reads copy out;
// a loan cannot escape the short non-suspending call. A busy result means the
// actor scheduler must retry later, not spin on a carrier.
template<std::size_t Slots, std::size_t MaxKeyBytes, std::size_t MaxValueBytes>
class NativeActorBytesMap final {
    static_assert(Slots > 0 && Slots <= 65536, "invalid Bytes map slot count");
    static_assert(MaxKeyBytes > 0 && MaxKeyBytes <= (1u << 20),
                  "invalid Bytes map key byte limit");
    static_assert(MaxValueBytes > 0 && MaxValueBytes <= (1u << 20),
                  "invalid Bytes map value byte limit");
    static_assert(Slots <= ((64ull << 20) / (MaxKeyBytes + MaxValueBytes + 64)),
                  "actor Bytes map exceeds 64 MiB bounded arena budget");

public:
    enum class Status : std::uint8_t {
        ok, absent, busy, denied, invalid, full, too_small, retired, overflow
    };
    struct ReadResult {
        Status status;
        std::size_t length;
        std::uint64_t version;
    };

private:
    struct Slot {
        std::array<std::byte, MaxKeyBytes> key{};
        std::array<std::byte, MaxValueBytes> value{};
        std::size_t key_length = 0;
        std::size_t value_length = 0;
        std::uint64_t version = 0;
        bool live = false;
    };

    const ActorHeapIdentity owner_;
    const std::uint64_t domain_;
    // Every operation is a short, fail-fast critical section. Never park the
    // calling carrier behind another actor/continuation.
    mutable std::mutex gate_;
    std::atomic<bool> closing_{false};
    std::array<Slot, Slots> slots_{};
    std::uint64_t sequence_ = 0;
    std::size_t live_ = 0;
    bool retired_ = false;

    static bool valid_bytes(const void* p, std::size_t n, std::size_t cap) noexcept {
        return n <= cap && (n == 0 || p != nullptr);
    }
    static bool same(const Slot& slot, const void* key, std::size_t key_length) noexcept {
        return slot.live && slot.key_length == key_length &&
               (key_length == 0 ||
                std::memcmp(slot.key.data(), key, key_length) == 0);
    }
    static void wipe(void* address, std::size_t size) noexcept {
        auto* p = reinterpret_cast<volatile unsigned char*>(address);
        while (size-- != 0) *p++ = 0;
    }
    static void scrub(Slot& slot) noexcept {
        wipe(slot.key.data(), slot.key.size());
        wipe(slot.value.data(), slot.value.size());
        slot.key_length = 0;
        slot.value_length = 0;
        slot.version = 0;
        slot.live = false;
    }
    bool authorized(ActorHeapIdentity requester) const noexcept {
        return requester == owner_;
    }

public:
    explicit NativeActorBytesMap(ActorHeapIdentity owner)
        : owner_(owner), domain_(detail::new_native_actor_bytes_map_domain()) {
        if (!owner.runtime || !owner.epoch)
            throw std::invalid_argument("native Bytes map requires valid actor domain");
    }
    NativeActorBytesMap(const NativeActorBytesMap&) = delete;
    NativeActorBytesMap& operator=(const NativeActorBytesMap&) = delete;
    NativeActorBytesMap(NativeActorBytesMap&&) = delete;
    NativeActorBytesMap& operator=(NativeActorBytesMap&&) = delete;
    ~NativeActorBytesMap() noexcept {
        // An actor may only release this storage AFTER its last turn, pending
        // FFI callback and continuation has quiesced. The supervisor must
        // explicitly retire; silent destructor cleanup masks lifecycle bugs.
        if (!retired_) std::terminate();
        for (auto& slot : slots_) scrub(slot);
    }
    std::uint64_t domain() const noexcept { return domain_; }
    static constexpr std::size_t capacity() noexcept { return Slots; }

    Status put(ActorHeapIdentity requester, const void* key,
               std::size_t key_length, const void* value,
               std::size_t value_length) {
        if (!authorized(requester)) return Status::denied;
        if (!valid_bytes(key, key_length, MaxKeyBytes) ||
            !valid_bytes(value, value_length, MaxValueBytes))
            return Status::invalid;
        if (closing_.load(std::memory_order_acquire)) return Status::retired;
        std::unique_lock<std::mutex> hold(gate_, std::try_to_lock);
        if (!hold) return Status::busy;
        if (closing_.load(std::memory_order_acquire)) return Status::retired;
        if (sequence_ == std::numeric_limits<std::uint64_t>::max())
            return Status::overflow;
        Slot* found = nullptr;
        Slot* unused = nullptr;
        for (auto& slot : slots_) {
            if (same(slot, key, key_length)) { found = &slot; break; }
            if (!slot.live && !unused) unused = &slot;
        }
        Slot* target = found ? found : unused;
        if (!target) return Status::full;
        // All argument/capacity checks happen before any mutation. A failed
        // SET never makes a previous complete value partially visible.
        scrub(*target);
        if (key_length) std::memcpy(target->key.data(), key, key_length);
        if (value_length) std::memcpy(target->value.data(), value, value_length);
        target->key_length = key_length;
        target->value_length = value_length;
        target->version = ++sequence_;
        target->live = true;
        if (!found) ++live_;
        return Status::ok;
    }

    ReadResult get(ActorHeapIdentity requester, const void* key,
                   std::size_t key_length, void* destination,
                   std::size_t output_capacity) const {
        if (!authorized(requester)) return {Status::denied, 0, 0};
        if (!valid_bytes(key, key_length, MaxKeyBytes) ||
            (output_capacity && !destination))
            return {Status::invalid, 0, 0};
        if (closing_.load(std::memory_order_acquire))
            return {Status::retired, 0, 0};
        std::unique_lock<std::mutex> hold(gate_, std::try_to_lock);
        if (!hold) return {Status::busy, 0, 0};
        if (closing_.load(std::memory_order_acquire))
            return {Status::retired, 0, 0};
        for (const auto& slot : slots_) {
            if (!same(slot, key, key_length)) continue;
            if (slot.value_length > output_capacity)
                return {Status::too_small, slot.value_length, slot.version};
            if (slot.value_length) {
                if (!destination) return {Status::invalid, 0, 0};
                std::memcpy(destination, slot.value.data(), slot.value_length);
            }
            return {Status::ok, slot.value_length, slot.version};
        }
        return {Status::absent, 0, 0};
    }

    Status erase(ActorHeapIdentity requester, const void* key,
                 std::size_t key_length) {
        if (!authorized(requester)) return Status::denied;
        if (!valid_bytes(key, key_length, MaxKeyBytes)) return Status::invalid;
        if (closing_.load(std::memory_order_acquire)) return Status::retired;
        std::unique_lock<std::mutex> hold(gate_, std::try_to_lock);
        if (!hold) return Status::busy;
        if (closing_.load(std::memory_order_acquire)) return Status::retired;
        for (auto& slot : slots_) {
            if (!same(slot, key, key_length)) continue;
            if (sequence_ == std::numeric_limits<std::uint64_t>::max())
                return Status::overflow;
            ++sequence_;
            scrub(slot);
            --live_;
            return Status::ok;
        }
        return Status::absent;
    }

    Status count(ActorHeapIdentity requester, std::size_t* result) const {
        if (!authorized(requester)) return Status::denied;
        if (!result) return Status::invalid;
        if (closing_.load(std::memory_order_acquire)) return Status::retired;
        std::unique_lock<std::mutex> hold(gate_, std::try_to_lock);
        if (!hold) return Status::busy;
        if (closing_.load(std::memory_order_acquire)) return Status::retired;
        *result = live_;
        return Status::ok;
    }

    // CONTROL/supervisor ONLY: seals admission before trying a nonparking
    // lock; retry only after all already-running operations have completed.
    Status try_retire(ActorHeapIdentity requester) noexcept {
        if (!authorized(requester)) return Status::denied;
        closing_.store(true, std::memory_order_release);
        std::unique_lock<std::mutex> hold(gate_, std::try_to_lock);
        if (!hold) return Status::busy;
        if (!retired_) {
            for (auto& slot : slots_) scrub(slot);
            live_ = 0;
            retired_ = true;
        }
        return Status::ok;
    }
};
} // namespace oreslang::runtime
