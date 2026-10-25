#pragma once

#include "oreslang/runtime/DopLayout.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace oreslang::dop {

/**
 * Host-only, fixed-shape POD storage with logical row identity independent of
 * AoS/SoA/AoSoA byte layout. No raw guest pointer or mutable span escapes.
 *
 * This is NOT an actor-turn permission, a general ECS allocator, a borrow
 * checker or source-language lowering. Caller must already own its execution
 * domain; this type is intentionally NOT thread-safe. An actor must not share
 * the object between concurrently executing domains.
 */
class CheckedRegion final {
public:
    class RowHandle final {
        friend class CheckedRegion;
        std::uint64_t owner_;
        std::uint64_t epoch_;
        std::uint64_t row_;
        constexpr RowHandle(std::uint64_t owner, std::uint64_t epoch,
                            std::uint64_t row) noexcept
            : owner_(owner), epoch_(epoch), row_(row) {}
    public:
        RowHandle(const RowHandle&) = default;
        RowHandle& operator=(const RowHandle&) = default;
    };

    explicit CheckedRegion(Plan plan)
        : plan_(std::move(plan)), owner_(new_owner_id()),
          storage_(checked_size(plan_.byte_length())) {}

    CheckedRegion(const CheckedRegion&) = delete;
    CheckedRegion& operator=(const CheckedRegion&) = delete;
    CheckedRegion(CheckedRegion&&) = delete;
    CheckedRegion& operator=(CheckedRegion&&) = delete;

    Layout layout() const noexcept { return plan_.layout(); }
    std::uint64_t rows() const noexcept { return plan_.rows(); }
    std::uint32_t fields() const noexcept { return plan_.fields(); }
    std::uint64_t epoch() const noexcept { return epoch_; }
    std::size_t storage_bytes() const noexcept { return storage_.size(); }

    RowHandle row(std::uint64_t index) const {
        if (index >= plan_.rows())
            throw std::out_of_range("DOP row handle index out of range");
        return RowHandle(owner_, epoch_, index);
    }

    template<typename T>
    T read(RowHandle handle, std::uint32_t field) const {
        check_scalar<T>();
        const std::size_t offset = checked_cell(handle, field, sizeof(T));
        T result{};
        std::memcpy(&result, storage_.data() + offset, sizeof(T));
        return result;
    }

    template<typename T>
    void write(RowHandle handle, std::uint32_t field, T value) {
        check_scalar<T>();
        const std::size_t offset = checked_cell(handle, field, sizeof(T));
        std::memcpy(storage_.data() + offset, &value, sizeof(T));
    }

    /**
     * Explicit epoch-changing migration with a strong exception guarantee.
     * Moved or stale handles never turn into addresses in the new layout.
     * Raw pointers are never returned, including during migration.
     */
    void relayout(Plan next) {
        if (next.rows() != plan_.rows() || next.fields() != plan_.fields()
                || next.scalar_bytes() != plan_.scalar_bytes())
            throw std::invalid_argument("DOP relayout changes logical shape");
        if (epoch_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("DOP region epoch exhausted");
        std::vector<std::byte> replacement(checked_size(next.byte_length()));
        for (std::uint64_t r = 0; r < plan_.rows(); ++r) {
            for (std::uint32_t f = 0; f < plan_.fields(); ++f) {
                const auto source = checked_offset(plan_, storage_.size(), r, f,
                                                  plan_.scalar_bytes());
                const auto target = checked_offset(next, replacement.size(), r, f,
                                                  next.scalar_bytes());
                std::memcpy(replacement.data() + target,
                            storage_.data() + source, plan_.scalar_bytes());
            }
        }
        // No allocation or checked arithmetic after this commit point.
        plan_ = next;
        storage_.swap(replacement);
        ++epoch_;
    }

private:
    static std::uint64_t new_owner_id() {
        // Saturate instead of wrapping owner IDs; a stale RowHandle must
        // never become valid on a newly constructed region.
        static std::atomic<std::uint64_t> next{1};
        auto value = next.load(std::memory_order_relaxed);
        while (true) {
            if (value == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("DOP region owner IDs exhausted");
            if (next.compare_exchange_weak(value, value + 1,
                    std::memory_order_relaxed, std::memory_order_relaxed))
                return value;
        }
    }

    static std::size_t checked_size(std::uint64_t bytes) {
        if (bytes > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
            throw std::length_error("DOP storage exceeds host size_t");
        return static_cast<std::size_t>(bytes);
    }

    template<typename T>
    void check_scalar() const {
        using V = std::remove_cv_t<T>;
        static_assert((std::is_integral_v<V> || std::is_floating_point_v<V>
                       || std::is_enum_v<V>) && !std::is_same_v<V, bool>
                       && !std::is_pointer_v<V> && std::is_trivially_copyable_v<V>,
                      "DOP checked region accepts only POD numeric/enum scalars, not pointers/bool");
        if (sizeof(V) != plan_.scalar_bytes())
            throw std::invalid_argument("DOP scalar size does not match storage ABI");
    }

    static std::size_t checked_offset(const Plan& plan, std::size_t storage_bytes,
                                      std::uint64_t r, std::uint32_t field,
                                      std::size_t scalar_width) {
        const auto offset = plan.byte_offset(r, field);
        if (offset > storage_bytes || scalar_width > storage_bytes - offset)
            throw std::out_of_range("DOP physical address exceeds storage");
        return static_cast<std::size_t>(offset);
    }

    std::size_t checked_cell(RowHandle handle, std::uint32_t field,
                             std::size_t scalar_width) const {
        if (handle.owner_ != owner_)
            throw std::invalid_argument("foreign DOP row handle");
        if (handle.epoch_ != epoch_)
            throw std::invalid_argument("stale DOP row handle after relayout");
        return checked_offset(plan_, storage_.size(), handle.row_, field, scalar_width);
    }

    Plan plan_;
    const std::uint64_t owner_;
    std::vector<std::byte> storage_;
    std::uint64_t epoch_ = 1;
};

} // namespace oreslang::dop
