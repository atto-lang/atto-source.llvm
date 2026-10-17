#pragma once

#include "oreslang/runtime/ActorHeapIdentity.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace oreslang::runtime {

/**
 * Host-only substrate for an eventual explicit UNSAFE native pointer escape hatch.
 * This does not grant source-language pointer arithmetic. A trusted supervisor
 * must authorize the actor, backing allocation/IO mapping, and access mode
 * before constructing this object; native host C++ is inside the TCB.
 *
 * Caller owns the bytes: keep them mapped/alive until revoke() has returned.
 * A guest cannot manufacture this window, obtain its addresses, or transport it.
 */
class TrustedPointerWindow final {
public:
    enum class Mode { ReadOnly, ReadWrite, MmioReadOnly, MmioReadWrite };
private:
    struct State final {
        ActorHeapIdentity owner;
        std::byte* base;
        std::size_t bytes;
        Mode mode;
        bool live = true;
        std::mutex mu;
        State(ActorHeapIdentity a, void* p, std::size_t n, Mode m)
            : owner(a), base(static_cast<std::byte*>(p)), bytes(n), mode(m) {}
    };
    std::shared_ptr<State> state_;

    static bool writable(Mode mode) noexcept {
        return mode == Mode::ReadWrite || mode == Mode::MmioReadWrite;
    }
    static bool mmio(Mode mode) noexcept {
        return mode == Mode::MmioReadOnly || mode == Mode::MmioReadWrite;
    }
    static void require_live(const State& s, ActorHeapIdentity actor) {
        if (actor != s.owner) throw std::logic_error("foreign actor/epoch native pointer");
        if (!s.live) throw std::logic_error("revoked native pointer region");
    }
    template<class T> static void validate(const State& s, std::size_t offset, bool access) {
        static_assert(std::is_trivially_copyable<T>::value && !std::is_pointer<T>::value,
                      "pointer access requires a trivially-copyable nonpointer type");
        if (offset > s.bytes || (access && sizeof(T) > s.bytes - offset))
            throw std::out_of_range("native pointer out of region bounds");
        const auto base = reinterpret_cast<std::uintptr_t>(s.base);
        // The host must supply a genuine mapped object/region, not an integer
        // address. Validate arithmetic *before* constructing a C++ pointer.
        if (offset > std::numeric_limits<std::uintptr_t>::max() - base ||
            ((base + offset) % alignof(T)) != 0)
            throw std::invalid_argument("unaligned/overflowing native pointer");
    }
    template<class T> static constexpr bool valid_mmio_type() {
        return std::is_integral<T>::value && std::is_unsigned<T>::value &&
               !std::is_same<T, bool>::value &&
               (sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8);
    }
public:
    TrustedPointerWindow(ActorHeapIdentity owner, void* base, std::size_t bytes, Mode mode) {
        if (!base || !bytes) throw std::invalid_argument("empty native region");
        const auto address = reinterpret_cast<std::uintptr_t>(base);
        if (bytes > std::numeric_limits<std::uintptr_t>::max() - address)
            throw std::invalid_argument("overflowing native region");
        state_ = std::make_shared<State>(owner, base, bytes, mode);
    }
    TrustedPointerWindow(const TrustedPointerWindow&) = delete;
    TrustedPointerWindow& operator=(const TrustedPointerWindow&) = delete;
    TrustedPointerWindow(TrustedPointerWindow&&) noexcept = default;
    TrustedPointerWindow& operator=(TrustedPointerWindow&&) = delete;
    ~TrustedPointerWindow() { revoke(); }

    void revoke() noexcept {
        if (auto s = state_) {
            std::lock_guard<std::mutex> lock(s->mu);
            s->live = false;
        }
    }

    template<class T> class Cursor final {
        friend class TrustedPointerWindow;
        std::weak_ptr<State> state_;
        std::size_t byte_offset_;
        Cursor(const std::shared_ptr<State>& s, std::size_t off)
            : state_(s), byte_offset_(off) {}
        template<class Fn> auto inspect(ActorHeapIdentity actor, bool access, bool writing, Fn&& fn) const
            -> decltype(fn(std::declval<State&>())) {
            auto s = state_.lock();
            if (!s) throw std::logic_error("expired native pointer region");
            std::lock_guard<std::mutex> lock(s->mu);
            require_live(*s, actor);
            validate<T>(*s, byte_offset_, access);
            if (writing && !writable(s->mode))
                throw std::logic_error("read-only native pointer region");
            return fn(*s);
        }
    public:
        // Arithmetic is in elements, not bytes. One-past is legal but not readable.
        Cursor offset(ActorHeapIdentity actor, std::ptrdiff_t elements) const {
            return inspect(actor, false, false, [&](State& s) -> Cursor {
                const auto size = sizeof(T);
                std::size_t next = byte_offset_;
                if (elements >= 0) {
                    const auto u = static_cast<std::uintmax_t>(elements);
                    if (u > (s.bytes - next) / size)
                        throw std::out_of_range("native pointer positive offset overflow");
                    next += static_cast<std::size_t>(u) * size;
                } else {
                    // Avoid negating PTRDIFF_MIN, which is undefined behavior.
                    const auto magnitude = std::uintmax_t{0} - static_cast<std::uintmax_t>(elements);
                    if (magnitude > next / size)
                        throw std::out_of_range("native pointer negative offset underflow");
                    next -= static_cast<std::size_t>(magnitude) * size;
                }
                validate<T>(s, next, false);
                return Cursor{state_.lock(), next};
            });
        }
        T load(ActorHeapIdentity actor) const {
            return inspect(actor, true, false, [&](State& s) -> T {
                if (mmio(s.mode)) {
                    if constexpr (valid_mmio_type<T>())
                        return *reinterpret_cast<volatile T*>(s.base + byte_offset_);
                    throw std::logic_error("MMIO requires unsigned native scalar");
                }
                T result;
                std::memcpy(&result, s.base + byte_offset_, sizeof(T));
                return result;
            });
        }
        void store(ActorHeapIdentity actor, T value) const {
            inspect(actor, true, true, [&](State& s) {
                if (mmio(s.mode)) {
                    if constexpr (valid_mmio_type<T>())
                        *reinterpret_cast<volatile T*>(s.base + byte_offset_) = value;
                    else
                        throw std::logic_error("MMIO requires unsigned native scalar");
                } else {
                    std::memcpy(s.base + byte_offset_, &value, sizeof(T));
                }
            });
        }
        // Intentionally unsafe escape hatch: synchronous, nonreentrant FFI only.
        // Native caller must not retain/return the address, suspend, or unmap
        // during the callback. Runtime revocation waits for callback completion.
        template<class F> auto with_ffi_address(ActorHeapIdentity actor, F&& f) const
            -> decltype(f(static_cast<T*>(nullptr))) {
            return inspect(actor, true, false, [&](State& s) {
                if (mmio(s.mode))
                    throw std::logic_error("MMIO address cannot escape into ordinary FFI");
                return f(reinterpret_cast<T*>(s.base + byte_offset_));
            });
        }
    };

    template<class T> Cursor<T> at(ActorHeapIdentity actor, std::size_t byte_offset = 0) const {
        auto s = state_;
        if (!s) throw std::logic_error("moved-from native pointer region");
        std::lock_guard<std::mutex> lock(s->mu);
        require_live(*s, actor);
        validate<T>(*s, byte_offset, false);
        return Cursor<T>{s, byte_offset};
    }
};

} // namespace oreslang::runtime
