#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace oreslang::dop {

// Conservative v0 storage ABI: no pointer-bearing or nontrivial elements.
// This is an ACTOR-INDEPENDENT host reference, not Oreslang source lowering.
template<class T>
inline constexpr bool shared_immutable_cell_v =
    (std::is_arithmetic_v<T> || std::is_enum_v<T>) &&
    !std::is_same_v<T, bool> && std::is_trivially_copyable_v<T> &&
    (sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8);

template<class T> class SharedImmutableArray;
template<class T> class WeakImmutableArray;

template<class T>
class PinnedImmutableView {
    static_assert(shared_immutable_cell_v<T>, "only fixed-width arithmetic/enum POD cells are admitted");
    std::shared_ptr<const std::vector<T>> owner_;
    std::size_t offset_ = 0;
    std::size_t length_ = 0;

    PinnedImmutableView(std::shared_ptr<const std::vector<T>> owner,
                        std::size_t offset, std::size_t length)
        : owner_(std::move(owner)), offset_(offset), length_(length) {}
    friend class SharedImmutableArray<T>;
public:
    PinnedImmutableView() = default;

    std::size_t size() const noexcept { return length_; }
    bool empty() const noexcept { return length_ == 0; }
    // The pinned shared_ptr outlives temporary array handles. The returned span
    // is a *borrow*; callers must not retain it after the view is destroyed.
    std::span<const T> span() const noexcept {
        if (!owner_ || length_ == 0) return {};
        return {owner_->data() + offset_, length_};
    }
    const T& at(std::size_t row) const {
        if (row >= length_) throw std::out_of_range("shared view row out of bounds");
        return (*owner_)[offset_ + row];
    }
    PinnedImmutableView subview(std::size_t first, std::size_t count) const {
        if (first > length_ || count > length_ - first)
            throw std::out_of_range("shared view range out of bounds");
        return PinnedImmutableView(owner_, offset_ + first, count);
    }
};

template<class T>
class SharedImmutableArray {
    static_assert(shared_immutable_cell_v<T>, "only fixed-width arithmetic/enum POD cells are admitted");
    std::shared_ptr<const std::vector<T>> owner_;
    explicit SharedImmutableArray(std::shared_ptr<const std::vector<T>> owner)
        : owner_(std::move(owner)) {}
    friend class WeakImmutableArray<T>;
public:
    SharedImmutableArray() = default;

    // Move-adopt the backing vector. With std::allocator this preserves the
    // element allocation and performs no element copies; a control block is
    // still allocated. No mutation is exposed after publication.
    static SharedImmutableArray adopt(std::vector<T>&& unique) {
        return SharedImmutableArray(std::make_shared<const std::vector<T>>(std::move(unique)));
    }

    std::size_t size() const noexcept { return owner_ ? owner_->size() : 0; }
    bool valid() const noexcept { return static_cast<bool>(owner_); }
    PinnedImmutableView<T> view() const noexcept {
        return PinnedImmutableView<T>(owner_, 0, size());
    }
    PinnedImmutableView<T> view(std::size_t first, std::size_t count) const {
        return view().subview(first, count);
    }
    WeakImmutableArray<T> downgrade() const noexcept;
};

template<class T>
class WeakImmutableArray {
    static_assert(shared_immutable_cell_v<T>, "only fixed-width arithmetic/enum POD cells are admitted");
    std::weak_ptr<const std::vector<T>> owner_;
    explicit WeakImmutableArray(const std::shared_ptr<const std::vector<T>>& owner) : owner_(owner) {}
    friend class SharedImmutableArray<T>;
public:
    WeakImmutableArray() = default;
    bool expired() const noexcept { return owner_.expired(); }
    std::optional<SharedImmutableArray<T>> upgrade() const noexcept {
        auto owner = owner_.lock();
        if (!owner) return std::nullopt;
        return SharedImmutableArray<T>(std::move(owner));
    }
};

template<class T>
WeakImmutableArray<T> SharedImmutableArray<T>::downgrade() const noexcept {
    return WeakImmutableArray<T>(owner_);
}

} // namespace oreslang::dop
