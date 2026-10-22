#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace oreslang::runtime {

// Actor-carrier-confined, fixed-capacity, non-moving arena with typed drop glue.
// No malloc, refcount allocation or tracing during emplace(), with() or reset().
// This primitive is single-threaded and MUST NOT cross actor carriers.
// Source compiler lifetime verification is still needed to prevent a callback
// from saving an untracked raw reference beyond a guarded invocation.
template<std::size_t Capacity, std::size_t MaxObjects>
class EpochArena final {
    static_assert(Capacity > 0 && MaxObjects > 0, "arena bounds must be positive");

    using Destructor = void (*)(void*) noexcept;
    struct Record {
        void* location = nullptr;
        Destructor drop = nullptr;
    };
    alignas(std::max_align_t) std::array<std::byte, Capacity> storage_{};
    std::array<Record, MaxObjects> records_{};
    std::size_t used_ = 0;
    std::size_t count_ = 0;
    std::size_t pins_ = 0;
    std::uint64_t generation_ = 1;
    bool retired_ = false;
    bool reclaiming_ = false;
    bool constructing_ = false;

    void assert_live(std::uint64_t generation) const {
        if (generation != generation_ || retired_ || reclaiming_)
            throw std::logic_error("stale actor-epoch reference");
    }

    void release_pin() noexcept {
        // All Ref instances are move-only and carry exactly one pin.
        --pins_;
    }

public:
    EpochArena() = default;
    EpochArena(const EpochArena&) = delete;
    EpochArena& operator=(const EpochArena&) = delete;
    EpochArena(EpochArena&&) = delete;
    EpochArena& operator=(EpochArena&&) = delete;

    template<class T>
    class Ref final {
        EpochArena* arena_ = nullptr;
        T* value_ = nullptr;
        std::uint64_t generation_ = 0;
        Ref(EpochArena* arena, T* value, std::uint64_t generation) noexcept
            : arena_(arena), value_(value), generation_(generation) {}
        friend class EpochArena;
    public:
        Ref() = default;
        Ref(const Ref&) = delete;
        Ref& operator=(const Ref&) = delete;
        Ref(Ref&& other) noexcept
            : arena_(std::exchange(other.arena_, nullptr)),
              value_(std::exchange(other.value_, nullptr)),
              generation_(std::exchange(other.generation_, 0)) {}
        Ref& operator=(Ref&& other) noexcept {
            if (this != &other) {
                reset();
                arena_ = std::exchange(other.arena_, nullptr);
                value_ = std::exchange(other.value_, nullptr);
                generation_ = std::exchange(other.generation_, 0);
            }
            return *this;
        }
        ~Ref() noexcept { reset(); }
        void reset() noexcept {
            if (!arena_) return;
            arena_->release_pin();
            arena_ = nullptr;
            value_ = nullptr;
            generation_ = 0;
        }
        explicit operator bool() const noexcept { return arena_ != nullptr; }
        template<class Fn>
        void with(Fn&& action) const {
            if (!arena_) throw std::logic_error("empty actor-epoch reference");
            auto* arena = arena_;
            auto* value = value_;
            arena->assert_live(generation_);
            if (arena->pins_ == std::numeric_limits<std::size_t>::max())
                throw std::overflow_error("actor epoch pin count exhausted");
            ++arena->pins_;
            struct CallbackPin {
                EpochArena* arena;
                ~CallbackPin() noexcept { arena->release_pin(); }
            } pin{arena};
            std::forward<Fn>(action)(*value);
        }
    };

    template<class T, class... Args>
    Ref<T> emplace(Args&&... args) {
        static_assert(std::is_nothrow_destructible<T>::value,
                      "arena object destructors must be noexcept");
        static_assert(alignof(T) <= alignof(std::max_align_t),
                      "overaligned arena types require a dedicated allocator");
        if (retired_ || reclaiming_ || constructing_)
            throw std::logic_error("actor epoch not accepting allocations");
        if (count_ == MaxObjects)
            throw std::bad_alloc();
        const auto base = reinterpret_cast<std::uintptr_t>(storage_.data());
        if (used_ > Capacity || base > std::numeric_limits<std::uintptr_t>::max() - used_)
            throw std::bad_alloc();
        const auto current = base + used_;
        const auto mask = static_cast<std::uintptr_t>(alignof(T) - 1);
        if (current > std::numeric_limits<std::uintptr_t>::max() - mask)
            throw std::bad_alloc();
        const auto aligned = (current + mask) & ~mask;
        const auto padding = static_cast<std::size_t>(aligned - current);
        if (padding > Capacity - used_ || sizeof(T) > Capacity - used_ - padding)
            throw std::bad_alloc();

        // Only commit accounting after construction succeeds. A constructor
        // failure leaves the arena cursor and destructor stack unchanged.
        void* location = reinterpret_cast<void*>(aligned);
        constructing_ = true;
        struct ConstructionGuard {
            bool& constructing;
            ~ConstructionGuard() noexcept { constructing = false; }
        } guard{constructing_};
        T* value = ::new (location) T(std::forward<Args>(args)...);
        records_[count_++] = Record{location, [](void* p) noexcept {
            static_cast<T*>(p)->~T();
        }};
        used_ += padding + sizeof(T);
        ++pins_;
        return Ref<T>(this, value, generation_);
    }

    std::size_t used() const noexcept { return used_; }
    std::size_t live_objects() const noexcept { return count_; }
    std::size_t live_refs() const noexcept { return pins_; }
    std::uint64_t generation() const noexcept { return generation_; }

    // Reclaim in reverse construction order, including nontrivial resources.
    // No old epoch storage is freed while a typed reference/lease remains.
    // All external I/O/continuation references must be represented by leases;
    // source lifetime checks are responsible for preventing untracked escapes.
    void reset() {
        if (retired_ || reclaiming_ || constructing_)
            throw std::logic_error("actor epoch is retired, reclaiming or constructing");
        if (pins_ != 0)
            throw std::logic_error("cannot reclaim actor epoch with live references");
        if (generation_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("actor epoch generation exhausted");
        reclaiming_ = true;
        while (count_ > 0) {
            Record record = records_[--count_];
            records_[count_] = {};
            record.drop(record.location);
        }
        used_ = 0;
        ++generation_;
        reclaiming_ = false;
    }

    void close() {
        if (retired_) return;
        reset();
        retired_ = true;
    }

    ~EpochArena() noexcept {
        // A dangling Ref would otherwise call release_pin on dead arena
        // storage. Fail closed instead of silently introducing a UAF.
        if (pins_ != 0 || reclaiming_ || constructing_) std::terminate();
        if (!retired_) {
            reclaiming_ = true;
            retired_ = true;
            while (count_ > 0) {
                const Record record = records_[--count_];
                record.drop(record.location);
            }
        }
    }
};

} // namespace oreslang::runtime
