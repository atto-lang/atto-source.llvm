#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace oreslang::runtime {

/**
 * Native host substrate for Oreslang Entry<K,V>.
 *
 * This is a nominal immutable two-slot carrier. It intentionally does not
 * define operator==/hashing: guest value equality is a language operation and
 * must not silently become C++ host equality. Source-level Entry admission is
 * still gated in the native frontend until ownership/drop, actor transport,
 * and LLVM lowering consume this substrate end to end.
 */
template<class K, class V>
class NativeEntry final {
public:
    static NativeEntry of(K key, V value)
            noexcept(std::is_nothrow_move_constructible<K>::value
                     && std::is_nothrow_move_constructible<V>::value) {
        return NativeEntry(std::move(key), std::move(value));
    }

    NativeEntry(const NativeEntry&) = default;
    NativeEntry(NativeEntry&&) noexcept(
            std::is_nothrow_move_constructible<K>::value
            && std::is_nothrow_move_constructible<V>::value) = default;
    NativeEntry& operator=(const NativeEntry&) = delete;
    NativeEntry& operator=(NativeEntry&&) = delete;
    ~NativeEntry() = default;

    const K& key() const noexcept { return key_; }
    const V& value() const noexcept { return value_; }

    template<class Visitor>
    void visit_children(Visitor&& visitor) const {
        visitor(key_);
        visitor(value_);
    }

private:
    NativeEntry(K key, V value)
            noexcept(std::is_nothrow_move_constructible<K>::value
                     && std::is_nothrow_move_constructible<V>::value)
        : key_(std::move(key)), value_(std::move(value)) {}

    K key_;
    V value_;
};

/**
 * Stable first native LLVM product layout for Entry<int,int>.
 *
 * The source compiler still rejects Entry syntax today; this layout is the
 * ABI target the later lowerer must use rather than flattening Entry to one
 * scalar or a structural record. Keeping the ABI record separate from the
 * high-level carrier prevents host representation conveniences from becoming
 * guest nominal identity.
 */
struct NativeEntryI64Abi final {
    std::int64_t key;
    std::int64_t value;
};

static_assert(std::is_standard_layout<NativeEntryI64Abi>::value,
              "Entry<int,int> ABI must be standard-layout");
static_assert(std::is_trivially_copyable<NativeEntryI64Abi>::value,
              "Entry<int,int> ABI must be trivially copyable");
static_assert(sizeof(NativeEntryI64Abi) == 16,
              "Entry<int,int> ABI must occupy exactly two i64 slots");
static_assert(offsetof(NativeEntryI64Abi, key) == 0,
              "Entry key must occupy slot 0");
static_assert(offsetof(NativeEntryI64Abi, value) == 8,
              "Entry value must occupy slot 1");

template<class K, class V, class Equal>
bool native_entry_guest_equal(
        const NativeEntry<K, V>& left,
        const NativeEntry<K, V>& right,
        Equal&& equal) {
    auto&& compare = equal;
    return compare(left.key(), right.key())
        && compare(left.value(), right.value());
}

} // namespace oreslang::runtime
