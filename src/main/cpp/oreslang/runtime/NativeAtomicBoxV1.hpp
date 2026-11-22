#pragma once

// Native Atto backend building blocks, NOT guest-language instruction lowering.
// No allocation, GC, worker threads or OS synchronization are created here.
// Callers must provide compiler-verified Send/Sync and allocator provenance.

#include <atomic>
#include <cstddef>
#include <concepts>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace oreslang::runtime::native_ownership_v1 {

enum class Ordering : std::uint8_t {
    Relaxed, Acquire, Release, AcqRel, SeqCst
};
enum class AtomicStatus : std::uint8_t {
    Ok, InvalidOrder, InvalidOutput, Mismatch, Overflow
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

    // Checked arithmetic is required by the guest Atomic<int> contract.
    // An overflow return is linearized at an observed atomic value and does
    // not mutate the cell or the caller's previous-value output. Every failed
    // CAS retries arithmetic from the latest atomically observed value.
    // LLVM/Clang/GCC support the builtins for signed and unsigned scalars.
    AtomicStatus fetch_add(T delta, T* previous,
                           Ordering order = Ordering::SeqCst) noexcept
        requires (!std::is_same_v<T, bool>) {
        if (!previous) return AtomicStatus::InvalidOutput;
        if (!is_rmw_order(order)) return AtomicStatus::InvalidOrder;
        T observed = value_.load(std::memory_order_relaxed);
        for (;;) {
            T next = 0;
            if (__builtin_add_overflow(observed, delta, &next))
                return AtomicStatus::Overflow;
            if (value_.compare_exchange_weak(
                    observed, next, native_order(order),
                    std::memory_order_relaxed)) {
                *previous = observed;
                return AtomicStatus::Ok;
            }
        }
    }
    AtomicStatus fetch_sub(T delta, T* previous,
                           Ordering order = Ordering::SeqCst) noexcept
        requires (!std::is_same_v<T, bool>) {
        if (!previous) return AtomicStatus::InvalidOutput;
        if (!is_rmw_order(order)) return AtomicStatus::InvalidOrder;
        T observed = value_.load(std::memory_order_relaxed);
        for (;;) {
            T next = 0;
            if (__builtin_sub_overflow(observed, delta, &next))
                return AtomicStatus::Overflow;
            if (value_.compare_exchange_weak(
                    observed, next, native_order(order),
                    std::memory_order_relaxed)) {
                *previous = observed;
                return AtomicStatus::Ok;
            }
        }
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

// Compiler/runtime-owned allocator issues exactly one outstanding ticket
// per live slot. Guest source cannot manufacture a ticket from a raw pointer.
// The allocator itself is responsible for physical identity and lifetime.
template <typename T, std::size_t Slots> class FixedBoxArena;
template <typename T> class UniqueBox;

template <typename T>
class UniqueAllocationTicket final {
public:
    using Destroy = void (*)(void*, T*) noexcept;

    UniqueAllocationTicket() noexcept = default; // invalid/empty ticket
    UniqueAllocationTicket(const UniqueAllocationTicket&) = delete;
    UniqueAllocationTicket& operator=(const UniqueAllocationTicket&) = delete;
    UniqueAllocationTicket(UniqueAllocationTicket&&) = delete;
    UniqueAllocationTicket& operator=(UniqueAllocationTicket&&) = delete;

    ~UniqueAllocationTicket() noexcept {
        // Unconsumed allocation still has exactly one lifetime owner.
        // Its allocator slot is released through trusted drop glue.
        if (!claimed_.load(std::memory_order_acquire) && pointer_ && destroy_)
            destroy_(context_, pointer_);
    }

    bool valid() const noexcept {
        return pointer_ && destroy_ && context_ && domain_ &&
            generation_ && !claimed_.load(std::memory_order_acquire);
    }

private:
    template <typename U, std::size_t N> friend class FixedBoxArena;
    template <typename U> friend class UniqueBox;

    // Private constructor: generic callers cannot mint ownership from an
    // arbitrary pointer, even with a fabricated domain/generation.
    UniqueAllocationTicket(T* pointer, void* context, Destroy destroy,
                           std::uint64_t domain, std::uint32_t generation) noexcept
        : pointer_(pointer), context_(context), destroy_(destroy),
          domain_(domain), generation_(generation) { }

    bool take(T*& pointer, void*& context, Destroy& destroy,
              std::uint64_t& domain, std::uint32_t& generation) noexcept {
        if (!pointer_ || !context_ || !destroy_ || !domain_ || !generation_)
            return false;
        bool expected = false;
        if (!claimed_.compare_exchange_strong(expected, true,
                std::memory_order_acq_rel, std::memory_order_acquire))
            return false;
        pointer = pointer_;
        context = context_;
        destroy = destroy_;
        domain = domain_;
        generation = generation_;
        return true;
    }

    T* pointer_ = nullptr;
    void* context_ = nullptr;
    Destroy destroy_ = nullptr;
    std::uint64_t domain_ = 0;
    std::uint32_t generation_ = 0;
    std::atomic<bool> claimed_{false};
};

template <typename T>
class UniqueBox final {
public:
    using Destroy = typename UniqueAllocationTicket<T>::Destroy;

    UniqueBox() noexcept = default;
    UniqueBox(const UniqueBox&) = delete;
    UniqueBox& operator=(const UniqueBox&) = delete;
    UniqueBox(UniqueBox&& other) noexcept { steal(other); }
    UniqueBox& operator=(UniqueBox&& other) noexcept {
        if (this != &other) {
            reset();
            steal(other);
        }
        return *this;
    }
    ~UniqueBox() noexcept { reset(); }

    static bool adopt(UniqueAllocationTicket<T>& ticket,
                      UniqueBox& destination) noexcept {
        if (destination.pointer_) return false;
        T* pointer = nullptr;
        void* context = nullptr;
        Destroy destroy = nullptr;
        std::uint64_t domain = 0;
        std::uint32_t generation = 0;
        if (!ticket.take(pointer, context, destroy, domain, generation))
            return false;
        destination.pointer_ = pointer;
        destination.context_ = context;
        destination.destroy_ = destroy;
        destination.domain_ = domain;
        destination.generation_ = generation;
        return true;
    }

    // Host must supply current allocator domain/generation from independent
    // authority; the box's self-reported metadata cannot prove authorization.
    T* get_checked(std::uint64_t current_domain,
                   std::uint32_t current_generation) const noexcept {
        return pointer_ && current_domain == domain_ &&
                current_generation == generation_ ? pointer_ : nullptr;
    }
    bool is_owned() const noexcept { return pointer_ != nullptr; }
    std::uint64_t owner_domain() const noexcept { return domain_; }
    std::uint32_t owner_generation() const noexcept { return generation_; }

    void reset() noexcept {
        T* pointer = std::exchange(pointer_, nullptr);
        void* context = std::exchange(context_, nullptr);
        Destroy destroy = std::exchange(destroy_, nullptr);
        domain_ = 0;
        generation_ = 0;
        if (pointer) destroy(context, pointer);
    }

private:
    void steal(UniqueBox& other) noexcept {
        pointer_ = std::exchange(other.pointer_, nullptr);
        context_ = std::exchange(other.context_, nullptr);
        destroy_ = std::exchange(other.destroy_, nullptr);
        domain_ = std::exchange(other.domain_, 0);
        generation_ = std::exchange(other.generation_, 0);
    }
    T* pointer_ = nullptr;
    void* context_ = nullptr;
    Destroy destroy_ = nullptr;
    std::uint64_t domain_ = 0;
    std::uint32_t generation_ = 0;
};

} // namespace oreslang::runtime::native_ownership_v1
