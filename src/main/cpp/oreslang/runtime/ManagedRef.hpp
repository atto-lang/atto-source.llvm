#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>

namespace oreslang::runtime {

enum class ManagedRefMode : std::uint8_t {
    Compressed32,
    Native64,
};

#if defined(ORES_MANAGED_REF_DEFAULT_NATIVE64)
inline constexpr ManagedRefMode kDefaultManagedRefMode = ManagedRefMode::Native64;
#else
// Native Oreslang defaults to compact actor-local references even on 64-bit
// targets. Native ABI/FFI/code pointers are intentionally unaffected.
inline constexpr ManagedRefMode kDefaultManagedRefMode = ManagedRefMode::Compressed32;
#endif

namespace detail {
// A valid region must fit in the native address domain without wrapping.
// Merely subtracting an address from base is insufficient when the *declared*
// span itself crosses UINTPTR_MAX (e.g., a corrupt region descriptor).
inline bool valid_arena_span(std::uintptr_t base, std::size_t bytes) noexcept {
    return base != 0 && bytes != 0
        && bytes - 1 <= std::numeric_limits<std::uintptr_t>::max() - base;
}
inline bool address_in_span(std::uintptr_t base, std::size_t bytes,
                            std::uintptr_t address) noexcept {
    if (!valid_arena_span(base, bytes) || address < base) return false;
    const auto delta = address - base;
    return delta < bytes;
}
} // namespace detail

template<ManagedRefMode Mode, unsigned AlignmentShift = 3>
class ActorManagedRef;

/**
 * Actor-relative managed reference stored in exactly 32 bits.
 *
 * Zero is null. Non-zero values encode (byte_offset >> AlignmentShift) + 1.
 * The reference is meaningful only together with its owning actor arena base.
 * It is not an ABI pointer, FFI pointer, ActorRef, or cross-actor capability.
 */
template<unsigned AlignmentShift>
class ActorManagedRef<ManagedRefMode::Compressed32, AlignmentShift> final {
    static_assert(AlignmentShift < 32, "compressed reference alignment shift is too large");
    static constexpr std::uint64_t kQuantum = UINT64_C(1) << AlignmentShift;
    std::uint32_t encoded_ = 0;

    explicit constexpr ActorManagedRef(std::uint32_t encoded) noexcept : encoded_(encoded) {}

public:
    using storage_type = std::uint32_t;
    static constexpr ManagedRefMode mode = ManagedRefMode::Compressed32;
    static constexpr std::size_t storage_bytes = sizeof(storage_type);
    static constexpr std::uint64_t alignment = kQuantum;
    static constexpr std::uint64_t max_index =
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) - 1;
    static constexpr std::uint64_t max_offset = max_index * kQuantum;
    static constexpr std::uint64_t max_addressable_span =
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) * kQuantum;

    constexpr ActorManagedRef() noexcept = default;

    static std::optional<ActorManagedRef> encode(std::uintptr_t arena_base,
                                                  std::size_t arena_bytes,
                                                  std::uintptr_t address) noexcept {
        // Validate the arena descriptor even for null. Null is a zero value,
        // not permission to bypass a malformed/overflowing region descriptor.
        if (!detail::valid_arena_span(arena_base, arena_bytes)
                || static_cast<std::uint64_t>(arena_bytes) > max_addressable_span
                || (arena_base & (kQuantum - 1)) != 0)
            return std::nullopt;
        if (address == 0) return ActorManagedRef{};
        if (!detail::address_in_span(arena_base, arena_bytes, address)) return std::nullopt;
        const auto offset = static_cast<std::uint64_t>(address - arena_base);
        if ((offset & (kQuantum - 1)) != 0 || offset > max_offset) return std::nullopt;
        const auto index = offset >> AlignmentShift;
        return ActorManagedRef{static_cast<std::uint32_t>(index + 1)};
    }

    static std::optional<ActorManagedRef> encode(const void* arena_base,
                                                  std::size_t arena_bytes,
                                                  const void* address) noexcept {
        return encode(reinterpret_cast<std::uintptr_t>(arena_base), arena_bytes,
                      reinterpret_cast<std::uintptr_t>(address));
    }

    std::optional<std::uintptr_t> decode(std::uintptr_t arena_base,
                                          std::size_t arena_bytes) const noexcept {
        if (!detail::valid_arena_span(arena_base, arena_bytes)
                || static_cast<std::uint64_t>(arena_bytes) > max_addressable_span
                || (arena_base & (kQuantum - 1)) != 0) return std::nullopt;
        if (encoded_ == 0) return std::uintptr_t{0};
        const auto index = static_cast<std::uint64_t>(encoded_ - 1);
        const auto offset = index << AlignmentShift;
        if (offset >= arena_bytes
                || arena_base > std::numeric_limits<std::uintptr_t>::max() - offset) {
            return std::nullopt;
        }
        return arena_base + static_cast<std::uintptr_t>(offset);
    }

    std::optional<std::uintptr_t> decode(const void* arena_base,
                                          std::size_t arena_bytes) const noexcept {
        return decode(reinterpret_cast<std::uintptr_t>(arena_base), arena_bytes);
    }

    constexpr bool is_null() const noexcept { return encoded_ == 0; }
    constexpr storage_type storage() const noexcept { return encoded_; }

    friend constexpr bool operator==(ActorManagedRef a, ActorManagedRef b) noexcept {
        return a.encoded_ == b.encoded_;
    }
};

/**
 * Full-width actor managed reference mode for large heaps and workloads where
 * compression is undesirable. The stored value is the native address on the
 * currently supported <=64-bit native targets, but arena bounds are still
 * validated so actor ownership does not become an ambient raw-pointer escape.
 */
template<unsigned AlignmentShift>
class ActorManagedRef<ManagedRefMode::Native64, AlignmentShift> final {
    static_assert(AlignmentShift < 32, "managed reference alignment shift is too large");
    static_assert(sizeof(std::uintptr_t) <= sizeof(std::uint64_t),
                  "native64 managed references require <=64-bit native addresses");
    static constexpr std::uintptr_t kQuantum =
        static_cast<std::uintptr_t>(UINT64_C(1) << AlignmentShift);
    std::uint64_t address_ = 0;

    explicit constexpr ActorManagedRef(std::uint64_t address) noexcept : address_(address) {}

public:
    using storage_type = std::uint64_t;
    static constexpr ManagedRefMode mode = ManagedRefMode::Native64;
    static constexpr std::size_t storage_bytes = sizeof(storage_type);
    static constexpr std::uint64_t alignment = UINT64_C(1) << AlignmentShift;

    constexpr ActorManagedRef() noexcept = default;

    static std::optional<ActorManagedRef> encode(std::uintptr_t arena_base,
                                                  std::size_t arena_bytes,
                                                  std::uintptr_t address) noexcept {
        // Null does not bypass validation of the arena descriptor.
        if (!detail::valid_arena_span(arena_base, arena_bytes)
                || (arena_base & (kQuantum - 1)) != 0)
            return std::nullopt;
        if (address == 0) return ActorManagedRef{};
        if ((address & (kQuantum - 1)) != 0
                || !detail::address_in_span(arena_base, arena_bytes, address)) {
            return std::nullopt;
        }
        return ActorManagedRef{static_cast<std::uint64_t>(address)};
    }

    static std::optional<ActorManagedRef> encode(const void* arena_base,
                                                  std::size_t arena_bytes,
                                                  const void* address) noexcept {
        return encode(reinterpret_cast<std::uintptr_t>(arena_base), arena_bytes,
                      reinterpret_cast<std::uintptr_t>(address));
    }

    std::optional<std::uintptr_t> decode(std::uintptr_t arena_base,
                                          std::size_t arena_bytes) const noexcept {
        if (!detail::valid_arena_span(arena_base, arena_bytes)
                || (arena_base & (kQuantum - 1)) != 0)
            return std::nullopt;
        if (address_ == 0) return std::uintptr_t{0};
        if (address_ > std::numeric_limits<std::uintptr_t>::max()) return std::nullopt;
        const auto address = static_cast<std::uintptr_t>(address_);
        if ((address & (kQuantum - 1)) != 0
                || !detail::address_in_span(arena_base, arena_bytes, address)) {
            return std::nullopt;
        }
        return address;
    }

    std::optional<std::uintptr_t> decode(const void* arena_base,
                                          std::size_t arena_bytes) const noexcept {
        return decode(reinterpret_cast<std::uintptr_t>(arena_base), arena_bytes);
    }

    constexpr bool is_null() const noexcept { return address_ == 0; }
    constexpr storage_type storage() const noexcept { return address_; }

    friend constexpr bool operator==(ActorManagedRef a, ActorManagedRef b) noexcept {
        return a.address_ == b.address_;
    }
};

using CompressedActorRef = ActorManagedRef<ManagedRefMode::Compressed32>;
using NativeActorManagedRef = ActorManagedRef<ManagedRefMode::Native64>;
using DefaultActorManagedRef = ActorManagedRef<kDefaultManagedRefMode>;

/**
 * Typed compact dense-table handle for data-oriented layouts.
 *
 * ECS component/entity columns, SoA and AoSoA storage should prefer an index
 * when object identity is a row/slot rather than materializing a pointer.
 * A tag type prevents accidentally mixing unrelated tables. Zero is null.
 */
template<class Tag>
class DenseIndex32 final {
    std::uint32_t encoded_ = 0;
    explicit constexpr DenseIndex32(std::uint32_t encoded) noexcept : encoded_(encoded) {}
public:
    using tag_type = Tag;
    using storage_type = std::uint32_t;
    static constexpr std::size_t storage_bytes = sizeof(storage_type);
    static constexpr std::uint64_t max_index =
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) - 1;

    constexpr DenseIndex32() noexcept = default;

    static constexpr std::optional<DenseIndex32> from_index(std::uint64_t index) noexcept {
        if (index > max_index) return std::nullopt;
        return DenseIndex32{static_cast<std::uint32_t>(index + 1)};
    }

    constexpr std::optional<std::uint32_t> index() const noexcept {
        if (encoded_ == 0) return std::nullopt;
        return encoded_ - 1;
    }

    constexpr bool is_null() const noexcept { return encoded_ == 0; }
    constexpr storage_type storage() const noexcept { return encoded_; }

    friend constexpr bool operator==(DenseIndex32 a, DenseIndex32 b) noexcept {
        return a.encoded_ == b.encoded_;
    }
};

static_assert(sizeof(CompressedActorRef) == 4,
              "compressed actor references must remain exactly 32 bits");
static_assert(sizeof(NativeActorManagedRef) == 8,
              "native64 actor references must remain exactly 64 bits");
static_assert(std::is_trivially_copyable<CompressedActorRef>::value,
              "managed references must remain POD-friendly for SoA/AoSoA storage");

} // namespace oreslang::runtime
