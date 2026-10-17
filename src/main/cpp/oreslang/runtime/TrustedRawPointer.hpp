#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace oreslang::runtime {

enum class RawPointerAccess : std::uint8_t { ReadOnly, ReadWrite };

/**
 * Host/compiler-minted authority for the trusted native unsafe-memory lane.
 *
 * Guest source never constructs this token directly. The source compiler must
 * additionally prove that the current execution domain is not UNTRUSTED and
 * that the callable admits the unsafe.memory effect.
 */
class TrustedUnsafeMemoryAuthority final {
public:
    static TrustedUnsafeMemoryAuthority compiler_trusted() noexcept {
        return TrustedUnsafeMemoryAuthority();
    }

private:
    TrustedUnsafeMemoryAuthority() noexcept = default;
};

/**
 * Native proof object for the future source-level RawPointer<T> capability.
 *
 * It is deliberately distinct from the owning Pointer<T> contract. This value
 * never owns or extends the pointee lifetime; a compiler-created raw lease must
 * remain rooted in the original owner/arena until all descendants are dead.
 *
 * Bounded pointers remember their provenance and permit one-past-end addresses
 * for comparison/difference, but never dereference one-past-end. Integer-made
 * pointers are unbounded and therefore receive only overflow/alignment checks.
 */
template<class T>
class TrustedRawPointer final {
    static_assert(!std::is_void<T>::value, "RawPointer<void> is not a typed dereference capability");
    static_assert(std::is_trivially_copyable<T>::value,
                  "raw pointer runtime slice requires trivially copyable T");

public:
    using element_type = T;

    static TrustedRawPointer from_span(
            const T* base,
            std::size_t count,
            TrustedUnsafeMemoryAuthority,
            std::uintptr_t provenance = 0,
            std::uint32_t address_space = 0) {
        if (base == nullptr || count == 0)
            throw std::invalid_argument("trusted raw span requires non-null non-empty storage");
        const auto address = reinterpret_cast<std::uintptr_t>(base);
        if ((address % alignof(T)) != 0)
            throw std::invalid_argument("trusted raw span base is misaligned");
        if (count > std::numeric_limits<std::uintptr_t>::max() / sizeof(T))
            throw std::overflow_error("trusted raw span byte extent overflow");
        const auto bytes = static_cast<std::uintptr_t>(count) * sizeof(T);
        if (address > std::numeric_limits<std::uintptr_t>::max() - bytes)
            throw std::overflow_error("trusted raw span address extent overflow");
        if (provenance == 0) provenance = address;
        return TrustedRawPointer(address, address, count, 0, true, provenance, address_space,
                                 RawPointerAccess::ReadOnly);
    }

    static TrustedRawPointer from_mut_span(
            T* base,
            std::size_t count,
            TrustedUnsafeMemoryAuthority authority,
            std::uintptr_t provenance = 0,
            std::uint32_t address_space = 0) {
        auto result = from_span(base, count, authority, provenance, address_space);
        result.access_ = RawPointerAccess::ReadWrite;
        return result;
    }

    static TrustedRawPointer from_address(
            std::uintptr_t address,
            TrustedUnsafeMemoryAuthority,
            std::uint32_t address_space = 0) {
        if (address == 0)
            throw std::invalid_argument("trusted raw address cannot be null");
        if ((address % alignof(T)) != 0)
            throw std::invalid_argument("trusted raw address is misaligned for T");
        return TrustedRawPointer(address, address, 0, 0, false, address, address_space,
                                 RawPointerAccess::ReadOnly);
    }

    static TrustedRawPointer from_mut_address(
            std::uintptr_t address,
            TrustedUnsafeMemoryAuthority authority,
            std::uint32_t address_space = 0) {
        auto result = from_address(address, authority, address_space);
        result.access_ = RawPointerAccess::ReadWrite;
        return result;
    }

    [[nodiscard]] TrustedRawPointer offset_elements(std::ptrdiff_t delta) const {
        if (bounded_) {
            std::size_t next = index_;
            if (delta >= 0) {
                const auto amount = static_cast<std::size_t>(delta);
                if (amount > count_ - index_)
                    throw std::out_of_range("trusted raw pointer offset exceeds bounded provenance");
                next += amount;
            } else {
                const auto amount = negative_magnitude(delta);
                if (amount > index_)
                    throw std::out_of_range("trusted raw pointer offset precedes bounded provenance");
                next -= amount;
            }
            const auto byte_offset = checked_element_bytes(next);
            return TrustedRawPointer(
                checked_add(base_address_, byte_offset),
                base_address_,
                count_,
                next,
                true,
                provenance_,
                address_space_,
                access_);
        }

        const auto byte_delta = checked_delta_bytes(delta);
        const auto next_address = delta >= 0
            ? checked_add(address_, byte_delta)
            : checked_sub(address_, byte_delta);
        return TrustedRawPointer(
            next_address,
            base_address_,
            0,
            0,
            false,
            provenance_,
            address_space_,
            access_);
    }

    [[nodiscard]] std::ptrdiff_t distance_elements(const TrustedRawPointer& other) const {
        if (address_space_ != other.address_space_ || provenance_ != other.provenance_)
            throw std::invalid_argument("trusted raw pointer subtraction requires common provenance");
        if (bounded_ != other.bounded_)
            throw std::invalid_argument("trusted raw pointer subtraction requires compatible provenance mode");

        if (bounded_) {
            if (index_ >= other.index_) {
                const auto d = index_ - other.index_;
                if (d > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()))
                    throw std::overflow_error("trusted raw pointer distance overflow");
                return static_cast<std::ptrdiff_t>(d);
            }
            const auto d = other.index_ - index_;
            if (d > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()))
                throw std::overflow_error("trusted raw pointer distance overflow");
            return -static_cast<std::ptrdiff_t>(d);
        }

        if (address_ >= other.address_) {
            const auto bytes = address_ - other.address_;
            if (bytes % sizeof(T) != 0)
                throw std::invalid_argument("trusted raw pointer distance is not element-aligned");
            const auto d = bytes / sizeof(T);
            if (d > static_cast<std::uintptr_t>(std::numeric_limits<std::ptrdiff_t>::max()))
                throw std::overflow_error("trusted raw pointer distance overflow");
            return static_cast<std::ptrdiff_t>(d);
        }
        const auto bytes = other.address_ - address_;
        if (bytes % sizeof(T) != 0)
            throw std::invalid_argument("trusted raw pointer distance is not element-aligned");
        const auto d = bytes / sizeof(T);
        if (d > static_cast<std::uintptr_t>(std::numeric_limits<std::ptrdiff_t>::max()))
            throw std::overflow_error("trusted raw pointer distance overflow");
        return -static_cast<std::ptrdiff_t>(d);
    }

    [[nodiscard]] T load() const {
        require_dereferenceable();
        T value{};
        std::memcpy(&value, reinterpret_cast<const void*>(address_), sizeof(T));
        return value;
    }

    void store(const T& value) const {
        require_writable();
        require_dereferenceable();
        std::memcpy(reinterpret_cast<void*>(address_), &value, sizeof(T));
    }

    [[nodiscard]] T load_volatile() const {
        require_dereferenceable();
        return *reinterpret_cast<volatile const T*>(address_);
    }

    void store_volatile(const T& value) const {
        require_writable();
        require_dereferenceable();
        *reinterpret_cast<volatile T*>(address_) = value;
    }

    [[nodiscard]] std::uintptr_t address() const noexcept { return address_; }
    [[nodiscard]] std::uintptr_t provenance() const noexcept { return provenance_; }
    [[nodiscard]] std::uint32_t address_space() const noexcept { return address_space_; }
    [[nodiscard]] bool bounded() const noexcept { return bounded_; }
    [[nodiscard]] bool writable() const noexcept {
        return access_ == RawPointerAccess::ReadWrite;
    }
    [[nodiscard]] bool one_past_end() const noexcept {
        return bounded_ && index_ == count_;
    }

private:
    TrustedRawPointer(
            std::uintptr_t address,
            std::uintptr_t base_address,
            std::size_t count,
            std::size_t index,
            bool bounded,
            std::uintptr_t provenance,
            std::uint32_t address_space,
            RawPointerAccess access) noexcept
        : address_(address),
          base_address_(base_address),
          count_(count),
          index_(index),
          bounded_(bounded),
          provenance_(provenance),
          address_space_(address_space),
          access_(access) {}

    static std::size_t negative_magnitude(std::ptrdiff_t value) {
        // Avoid signed overflow for PTRDIFF_MIN.
        return static_cast<std::size_t>(-(value + 1)) + 1u;
    }

    static std::uintptr_t checked_add(std::uintptr_t value, std::uintptr_t delta) {
        if (value > std::numeric_limits<std::uintptr_t>::max() - delta)
            throw std::overflow_error("trusted raw pointer address overflow");
        return value + delta;
    }

    static std::uintptr_t checked_sub(std::uintptr_t value, std::uintptr_t delta) {
        if (delta > value)
            throw std::overflow_error("trusted raw pointer address underflow");
        return value - delta;
    }

    static std::uintptr_t checked_element_bytes(std::size_t elements) {
        if (elements > std::numeric_limits<std::uintptr_t>::max() / sizeof(T))
            throw std::overflow_error("trusted raw pointer element offset overflow");
        return static_cast<std::uintptr_t>(elements) * sizeof(T);
    }

    static std::uintptr_t checked_delta_bytes(std::ptrdiff_t delta) {
        const std::size_t magnitude = delta >= 0
            ? static_cast<std::size_t>(delta)
            : negative_magnitude(delta);
        return checked_element_bytes(magnitude);
    }

    void require_writable() const {
        if (access_ != RawPointerAccess::ReadWrite)
            throw std::logic_error("trusted raw pointer store requires mutable raw authority");
    }

    void require_dereferenceable() const {
        if ((address_ % alignof(T)) != 0)
            throw std::runtime_error("trusted raw pointer dereference is misaligned");
        if (bounded_ && index_ >= count_)
            throw std::out_of_range("trusted raw pointer cannot dereference one-past-end");
    }

    std::uintptr_t address_;
    std::uintptr_t base_address_;
    std::size_t count_;
    std::size_t index_;
    bool bounded_;
    std::uintptr_t provenance_;
    std::uint32_t address_space_;
    RawPointerAccess access_;
};

} // namespace oreslang::runtime
