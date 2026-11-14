#pragma once

// Native Atto backend building blocks, NOT guest-language instruction lowering.
// No allocation, GC, worker threads or OS synchronization are created here.
// Callers must provide compiler-verified Send/Sync and allocator provenance.

#include <atomic>
#include <concepts>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace oreslang::runtime::native_ownership_v1 {

enum class Ordering : std::uint8_t {
    Relaxed, Acquire, Release, AcqRel, SeqCst
};
enum class AtomicStatus : std::uint8_t {
    Ok, InvalidOrder, InvalidOutput, Mismatch
};

inline constexpr bool is_load_order(Ordering order) noexcept {
    return order == Ordering::Relaxed || order == Ordering::Acquire
        || order == Ordering::SeqCst;
}
inline constexpr bool is_store_order(Ordering order) noexcept {
    return order == Ordering::Relaxed || order == Ordering::Release
        || order == Ordering::SeqCst;
}
inline constexpr bool is_rmw_order(Ordering order) noexcept {
    return order == Ordering::Relaxed || order == Ordering::Acquire
        || order == Ordering::Release || order == Ordering::AcqRel
        || order == Ordering::SeqCst;
}
inline constexpr bool is_cas_order(Ordering success, Ordering failure) noexcept {
    switch (success) {
    case Ordering::Relaxed: return failure == Ordering::Relaxed;
    case Ordering::Acquire:
        return failure == Ordering::Relaxed || failure == Ordering::Acquire;
    case Ordering::Release: return failure == Ordering::Relaxed;
    case Ordering::AcqRel:
        return failure == Ordering::Relaxed || failure == Ordering::Acquire;
    case Ordering::SeqCst:
        return failure == Ordering::Relaxed || failure == Ordering::Acquire
            || failure == Ordering::SeqCst;
    }
    return false;
}
inline constexpr std::memory_order native_order(Ordering order) noexcept {
    switch (order) {
    case Ordering::Relaxed: return std::memory_order_relaxed;
    case Ordering::Acquire: return std::memory_order_acquire;
    case Ordering::Release: return std::memory_order_release;
    case Ordering::AcqRel: return std::memory_order_acq_rel;
    case Ordering::SeqCst: return std::memory_order_seq_cst;
    }
    // This fallback is never executed by a valid public operation.
    return std::memory_order_seq_cst;
}

template <typename T>
concept SupportedAtomicScalar =
    std::is_integral_v<T> && !std::is_const_v<T> &&
    (sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8) &&
    std::atomic<T>::is_always_lock_free;

// Lock-free is intentionally a target compile-time requirement. Do not
// substitute an OS mutex or an ordinary read/write on unsupported targets.
template <SupportedAtomicScalar T>
class AtomicScalar final {
public:
    explicit AtomicScalar(T initial) noexcept : value_(initial) { }
    AtomicScalar(const AtomicScalar&) = delete;
    AtomicScalar& operator=(const AtomicScalar&) = delete;
    AtomicScalar(AtomicScalar&&) = delete;
    AtomicScalar& operator=(AtomicScalar&&) = delete;

    AtomicStatus load(T* out, Ordering order = Ordering::SeqCst) const noexcept {
        if (!out) return AtomicStatus::InvalidOutput;
        if (!is_load_order(order)) return AtomicStatus::InvalidOrder;
        *out = value_.load(native_order(order));
        return AtomicStatus::Ok;
    }
    AtomicStatus store(T next, Ordering order = Ordering::SeqCst) noexcept {
        if (!is_store_order(order)) return AtomicStatus::InvalidOrder;
        value_.store(next, native_order(order));
        return AtomicStatus::Ok;
    }
    AtomicStatus swap(T next, T* previous,
                      Ordering order = Ordering::SeqCst) noexcept {
        if (!previous) return AtomicStatus::InvalidOutput;
        if (!is_rmw_order(order)) return AtomicStatus::InvalidOrder;
        *previous = value_.exchange(next, native_order(order));
        return AtomicStatus::Ok;
    }
    AtomicStatus compare_exchange(T* expected, T desired, Ordering success,
                                  Ordering failure, bool weak = false) noexcept {
        if (!expected) return AtomicStatus::InvalidOutput;
        if (!is_cas_order(success, failure)) return AtomicStatus::InvalidOrder;
        const bool matched = weak
            ? value_.compare_exchange_weak(*expected, desired,
                                           native_order(success), native_order(failure))
            : value_.compare_exchange_strong(*expected, desired,
                                             native_order(success), native_order(failure));
        return matched ? AtomicStatus::Ok : AtomicStatus::Mismatch;
    }

    // All integer fetch arithmetic is the explicitly wrapping two's-complement
    // atomic operation, distinct from checked scalar arithmetic.
    AtomicStatus fetch_add(T delta, T* previous,
                           Ordering order = Ordering::SeqCst) noexcept
        requires (!std::is_same_v<T, bool>) {
        if (!previous) return AtomicStatus::InvalidOutput;
        if (!is_rmw_order(order)) return AtomicStatus::InvalidOrder;
        *previous = value_.fetch_add(delta, native_order(order));
        return AtomicStatus::Ok;
    }
    AtomicStatus fetch_sub(T delta, T* previous,
                           Ordering order = Ordering::SeqCst) noexcept
        requires (!std::is_same_v<T, bool>) {
        if (!previous) return AtomicStatus::InvalidOutput;
        if (!is_rmw_order(order)) return AtomicStatus::InvalidOrder;
        *previous = value_.fetch_sub(delta, native_order(order));
        return AtomicStatus::Ok;
    }
    AtomicStatus fetch_and(T mask, T* previous,
                           Ordering order = Ordering::SeqCst) noexcept
        requires (!std::is_same_v<T, bool>) {
        if (!previous) return AtomicStatus::InvalidOutput;
        if (!is_rmw_order(order)) return AtomicStatus::InvalidOrder;
        *previous = value_.fetch_and(mask, native_order(order));
        return AtomicStatus::Ok;
    }
    AtomicStatus fetch_or(T mask, T* previous,
                          Ordering order = Ordering::SeqCst) noexcept
        requires (!std::is_same_v<T, bool>) {
        if (!previous) return AtomicStatus::InvalidOutput;
        if (!is_rmw_order(order)) return AtomicStatus::InvalidOrder;
        *previous = value_.fetch_or(mask, native_order(order));
        return AtomicStatus::Ok;
    }
    AtomicStatus fetch_xor(T mask, T* previous,
                           Ordering order = Ordering::SeqCst) noexcept
        requires (!std::is_same_v<T, bool>) {
        if (!previous) return AtomicStatus::InvalidOutput;
        if (!is_rmw_order(order)) return AtomicStatus::InvalidOrder;
        *previous = value_.fetch_xor(mask, native_order(order));
        return AtomicStatus::Ok;
    }

private:
    std::atomic<T> value_;
};

// Move-only native ownership capability. The allocation/deallocation policy is
// supplied by a trusted allocator/actor runtime. No implicit new/delete or
// ARC/GC. A raw actor-private pointer cannot be exposed across domain
// boundaries by this interface: checked access requires domain + generation.
template <typename T>
class UniqueBox final {
public:
    using Destroy = void (*)(T*) noexcept;

    UniqueBox() noexcept = default;
    UniqueBox(const UniqueBox&) = delete;
    UniqueBox& operator=(const UniqueBox&) = delete;

    UniqueBox(UniqueBox&& other) noexcept {
        steal(other);
    }
    UniqueBox& operator=(UniqueBox&& other) noexcept {
        if (this != &other) {
            reset();
            steal(other);
        }
        return *this;
    }
    ~UniqueBox() noexcept { reset(); }

    static bool adopt(T* pointer, Destroy destroy, std::uint64_t domain,
                      std::uint32_t generation, UniqueBox& destination) noexcept {
        if (!pointer || !destroy || !domain || !generation || destination.pointer_)
            return false;
        destination.pointer_ = pointer;
        destination.destroy_ = destroy;
        destination.domain_ = domain;
        destination.generation_ = generation;
        return true;
    }

    // The compiler/runtime must ensure there are no outstanding borrows
    // when a move/reset occurs. This is not a standalone loan checker.
    T* get_checked(std::uint64_t domain, std::uint32_t generation) const noexcept {
        return pointer_ && domain == domain_ && generation == generation_
            ? pointer_ : nullptr;
    }
    bool is_owned() const noexcept { return pointer_ != nullptr; }
    std::uint64_t owner_domain() const noexcept { return domain_; }
    std::uint32_t owner_generation() const noexcept { return generation_; }

    void reset() noexcept {
        T* pointer = std::exchange(pointer_, nullptr);
        Destroy destroy = std::exchange(destroy_, nullptr);
        domain_ = 0;
        generation_ = 0;
        if (pointer) destroy(pointer); // exactly once, after revoking capability
    }

private:
    void steal(UniqueBox& other) noexcept {
        pointer_ = std::exchange(other.pointer_, nullptr);
        destroy_ = std::exchange(other.destroy_, nullptr);
        domain_ = std::exchange(other.domain_, 0);
        generation_ = std::exchange(other.generation_, 0);
    }
    T* pointer_ = nullptr;
    Destroy destroy_ = nullptr;
    std::uint64_t domain_ = 0;
    std::uint32_t generation_ = 0;
};

} // namespace oreslang::runtime::native_ownership_v1
