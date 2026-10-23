#pragma once

#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace oreslang::runtime {

// Native, tracing-GC-free allocation for uniquely owned resources.
// This is a runtime building block, NOT proof that source .ores ownership
// syntax or physical actor/tenant isolation is implemented by the compiler.
//
// A heap accounts for its own live objects. Each allocation is freed by its
// last RAII owner; a borrow is an explicit short-lived reference-counted lease
// so a buggy host caller cannot turn owner destruction into a UAF.
//
// Borrow leases MUST NOT escape source lifetimes once the borrow checker is
// lowered to native code. They deliberately keep the allocation alive as a
// defense-in-depth fallback; they are not a cycle collector.
// Moving/resetting the same Owned/Borrow handle requires external serialization.
// Independently held leases share protected slot admission; with() retains its
// own callback pin, and one mutable lease cannot run overlapping callbacks.
class OwnedHeap final {
    struct State final {
        explicit State(std::size_t limit) : byte_limit(limit) {}
        const std::size_t byte_limit;
        std::mutex mutex;
        std::size_t live_bytes = 0;
        std::size_t live_objects = 0;
        std::size_t peak_bytes = 0;
        bool closed = false;

        void reserve(std::size_t bytes) {
            std::lock_guard<std::mutex> guard(mutex);
            if (closed) throw std::logic_error("owned heap is closed");
            if (bytes > byte_limit - live_bytes)
                throw std::bad_alloc();
            live_bytes += bytes;
            ++live_objects;
            if (live_bytes > peak_bytes) peak_bytes = live_bytes;
        }

        void release(std::size_t bytes) noexcept {
            std::lock_guard<std::mutex> guard(mutex);
            // Only a successfully reserved allocation reaches this path.
            live_bytes -= bytes;
            --live_objects;
        }
    };

    template<class T> struct Slot final {
        std::shared_ptr<State> state;
        mutable std::mutex borrow_mutex;
        std::size_t readers = 0;
        bool writer = false;
        std::size_t active_read_callbacks = 0;
        bool active_write_callback = false;
        std::optional<T> value;

        template<class... Args>
        Slot(std::shared_ptr<State> owner, Args&&... args)
            : state(std::move(owner)), value(std::in_place, std::forward<Args>(args)...) {}

        // Destroy the user object before lowering live-byte accounting.
        // Otherwise another thread could close a heap while T::~T is active.
        ~Slot() noexcept {
            value.reset();
            state->release(sizeof(T));
        }

        void acquire(bool exclusive) {
            std::lock_guard<std::mutex> guard(borrow_mutex);
            if (writer || active_write_callback || (exclusive && (readers != 0 || active_read_callbacks != 0)))
                throw std::logic_error("conflicting owned heap borrow");
            if (exclusive) writer = true;
            else {
                if (readers == std::numeric_limits<std::size_t>::max())
                    throw std::overflow_error("owned heap reader count exhausted");
                ++readers;
            }
        }

        void relinquish(bool exclusive) noexcept {
            std::lock_guard<std::mutex> guard(borrow_mutex);
            if (exclusive) writer = false;
            else --readers;
        }

        // A callback has its own lifetime/borrow admission. Resetting the
        // external lease during with() must not admit reclamation or aliases.
        void enter_callback(bool exclusive) {
            std::lock_guard<std::mutex> guard(borrow_mutex);
            if (exclusive) {
                if (!writer || active_write_callback || active_read_callbacks != 0)
                    throw std::logic_error("conflicting owned heap callback");
                active_write_callback = true;
            } else {
                if (writer || active_write_callback || readers == 0)
                    throw std::logic_error("conflicting owned heap callback");
                if (active_read_callbacks == std::numeric_limits<std::size_t>::max())
                    throw std::overflow_error("owned heap callback count exhausted");
                ++active_read_callbacks;
            }
        }

        void leave_callback(bool exclusive) noexcept {
            std::lock_guard<std::mutex> guard(borrow_mutex);
            if (exclusive) active_write_callback = false;
            else --active_read_callbacks;
        }

        void require_unborrowed() const {
            std::lock_guard<std::mutex> guard(borrow_mutex);
            if (readers != 0 || writer || active_read_callbacks != 0 || active_write_callback)
                throw std::logic_error("cannot drop an actively borrowed value");
        }
    };

public:
    struct Stats {
        std::size_t live_bytes;
        std::size_t live_objects;
        std::size_t peak_bytes;
        std::size_t byte_limit;
        bool closed;
    };

    // No naked heap access: use borrow_mut()/borrow() leases or with_*().
    template<class T, bool Mutable> class Borrow final {
        std::shared_ptr<Slot<T>> slot_;
        explicit Borrow(std::shared_ptr<Slot<T>> slot) : slot_(std::move(slot)) {
            if (slot_) slot_->acquire(Mutable);
        }
        friend class OwnedHeap;
        template<class U> friend class Owned;
    public:
        Borrow(const Borrow&) = delete;
        Borrow& operator=(const Borrow&) = delete;

        Borrow(Borrow&& other) noexcept : slot_(std::move(other.slot_)) {}
        Borrow& operator=(Borrow&& other) noexcept {
            if (this == &other) return *this;
            reset();
            slot_ = std::move(other.slot_);
            return *this;
        }

        ~Borrow() noexcept { reset(); }
        void reset() noexcept {
            if (slot_) {
                slot_->relinquish(Mutable);
                slot_.reset();
            }
        }

        template<class Fn>
        void with(Fn&& fn) const {
            auto slot = slot_;
            if (!slot) throw std::logic_error("borrow lease is empty");
            slot->enter_callback(Mutable);
            struct CallbackPin {
                std::shared_ptr<Slot<T>> slot;
                ~CallbackPin() noexcept { slot->leave_callback(Mutable); }
            } pin{std::move(slot)};
            if constexpr (Mutable) {
                std::forward<Fn>(fn)(*pin.slot->value);
            } else {
                std::forward<Fn>(fn)(static_cast<const T&>(*pin.slot->value));
            }
        }
    };

    template<class T> class Owned final {
        std::shared_ptr<Slot<T>> slot_;
        explicit Owned(std::shared_ptr<Slot<T>> slot) : slot_(std::move(slot)) {}
        friend class OwnedHeap;
    public:
        Owned() = default;
        Owned(const Owned&) = delete;
        Owned& operator=(const Owned&) = delete;
        Owned(Owned&&) noexcept = default;
        Owned& operator=(Owned&&) noexcept = default;
        ~Owned() = default;

        explicit operator bool() const noexcept { return static_cast<bool>(slot_); }

        Borrow<T, false> borrow() const {
            if (!slot_) throw std::logic_error("borrow from moved/dropped owner");
            return Borrow<T, false>(slot_);
        }

        Borrow<T, true> borrow_mut() {
            if (!slot_) throw std::logic_error("borrow from moved/dropped owner");
            return Borrow<T, true>(slot_);
        }

        // Explicit drop can be checked and failed without consuming ownership.
        // Ordinary RAII destruction is safe if host code violates a borrow
        // lifetime: outstanding leases defer reclamation, rather than UAF.
        void drop() {
            if (!slot_) return;
            slot_->require_unborrowed();
            slot_.reset();
        }
    };

    explicit OwnedHeap(std::size_t byte_limit)
        : state_(std::make_shared<State>(byte_limit)) {}

    OwnedHeap(const OwnedHeap&) = delete;
    OwnedHeap& operator=(const OwnedHeap&) = delete;
    OwnedHeap(OwnedHeap&&) = delete;
    OwnedHeap& operator=(OwnedHeap&&) = delete;

    template<class T, class... Args>
    Owned<T> make(Args&&... args) {
        static_assert(sizeof(T) > 0, "owned allocations require a complete type");
        static_assert(std::is_nothrow_destructible<T>::value,
                      "owned object destructors must be noexcept");
        auto state = state_;
        state->reserve(sizeof(T));
        try {
            auto slot = std::make_shared<Slot<T>>(
                std::move(state), std::forward<Args>(args)...);
            return Owned<T>(std::move(slot));
        } catch (...) {
            // A failed Slot constructor never reaches Slot::~Slot.
            state_->release(sizeof(T));
            throw;
        }
    }

    Stats stats() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return {state_->live_bytes, state_->live_objects,
                state_->peak_bytes, state_->byte_limit, state_->closed};
    }

    // Actor/supervisor teardown must quiesce all owners and borrow leases.
    // A failed close leaves the heap usable so the supervisor can clean up.
    void close() {
        std::lock_guard<std::mutex> guard(state_->mutex);
        if (state_->live_objects != 0)
            throw std::logic_error("cannot retire owned heap with live allocations");
        state_->closed = true;
    }

private:
    std::shared_ptr<State> state_;
};

} // namespace oreslang::runtime
