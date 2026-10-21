#include "oreslang/runtime/NativeEntry.hpp"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

using oreslang::runtime::NativeEntry;
using oreslang::runtime::NativeEntryI64Abi;
using oreslang::runtime::native_entry_guest_equal;

#define REQUIRE(condition) do { \
    if (!(condition)) throw std::runtime_error("requirement failed: " #condition); \
} while (false)

namespace {

template<class T, class = void>
struct has_host_equality : std::false_type {};

template<class T>
struct has_host_equality<T, std::void_t<decltype(
        std::declval<const T&>() == std::declval<const T&>())>>
    : std::true_type {};

struct MoveOnly {
    explicit MoveOnly(int value) : value(std::make_unique<int>(value)) {}
    MoveOnly(const MoveOnly&) = delete;
    MoveOnly& operator=(const MoveOnly&) = delete;
    MoveOnly(MoveOnly&&) noexcept = default;
    MoveOnly& operator=(MoveOnly&&) noexcept = default;

    std::unique_ptr<int> value;
};

} // namespace

int main() {
    using Pair = NativeEntry<std::int64_t, std::int64_t>;

    static_assert(!std::is_default_constructible<Pair>::value,
                  "Entry must be factory-only");
    static_assert(!std::is_constructible<Pair, std::int64_t, std::int64_t>::value,
                  "Entry constructor must not be public");
    static_assert(std::is_copy_constructible<Pair>::value,
                  "Copyable components should make Entry copy-constructible");
    static_assert(std::is_move_constructible<Pair>::value,
                  "Movable components should make Entry move-constructible");
    static_assert(!std::is_copy_assignable<Pair>::value,
                  "immutable Entry slots cannot be reassigned");
    static_assert(!std::is_move_assignable<Pair>::value,
                  "immutable Entry slots cannot be move-assigned");
    static_assert(!has_host_equality<Pair>::value,
                  "host equality must not substitute for guest Entry equality");
    static_assert(std::is_same<decltype(std::declval<const Pair&>().key()),
                               const std::int64_t&>::value,
                  "key projection must be readonly");
    static_assert(std::is_same<decltype(std::declval<const Pair&>().value()),
                               const std::int64_t&>::value,
                  "value projection must be readonly");

    const Pair pair = Pair::of(7, 9);
    REQUIRE(pair.key() == 7);
    REQUIRE(pair.value() == 9);

    std::vector<std::int64_t> visited;
    pair.visit_children([&](const std::int64_t& item) {
        visited.push_back(item);
    });
    REQUIRE(visited.size() == 2);
    REQUIRE(visited[0] == 7);
    REQUIRE(visited[1] == 9);

    const Pair same = Pair::of(7, 9);
    const Pair different_key = Pair::of(8, 9);
    const Pair different_value = Pair::of(7, 10);
    auto scalar_equal = [](std::int64_t left, std::int64_t right) {
        return left == right;
    };
    REQUIRE(native_entry_guest_equal(pair, same, scalar_equal));
    REQUIRE(!native_entry_guest_equal(pair, different_key, scalar_equal));
    REQUIRE(!native_entry_guest_equal(pair, different_value, scalar_equal));

    NativeEntryI64Abi abi{11, 13};
    REQUIRE(abi.key == 11);
    REQUIRE(abi.value == 13);
    REQUIRE(reinterpret_cast<const unsigned char*>(&abi.value)
            - reinterpret_cast<const unsigned char*>(&abi.key) == 8);

    using MoveEntry = NativeEntry<MoveOnly, MoveOnly>;
    static_assert(!std::is_copy_constructible<MoveEntry>::value,
                  "Entry must not invent Copy for move-only components");
    static_assert(std::is_move_constructible<MoveEntry>::value,
                  "Entry should preserve component move construction");
    static_assert(!std::is_copy_assignable<MoveEntry>::value
                  && !std::is_move_assignable<MoveEntry>::value,
                  "Entry slot identity remains immutable for move-only values");

    MoveEntry owned = MoveEntry::of(MoveOnly(21), MoveOnly(34));
    REQUIRE(*owned.key().value == 21);
    REQUIRE(*owned.value().value == 34);

    return 0;
}
