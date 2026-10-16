#include <cassert>
#include <memory>
#include <optional>
#include <type_traits>
#include "WrapperOptionHost.hpp"

using oreslang::runtime::flatten_option;
using oreslang::runtime::flattened_option_t;

int main() {
    using I = std::optional<int>;
    using II = std::optional<I>;
    using III = std::optional<II>;
    static_assert(std::is_same_v<flattened_option_t<II>, I>);
    static_assert(std::is_same_v<flattened_option_t<III>, I>);
    const III nested = III{II{I{42}}};
    assert(nested.has_value());
    assert(flatten_option(nested) == I{42});
    assert(flatten_option(III{}) == std::nullopt);
    assert(flatten_option(III{II{I{}}}) == std::nullopt);
    const II stored_none = II{I{}};
    assert(stored_none.has_value());  // Must NOT flatten implicitly.
    assert(flatten_option(stored_none) == std::nullopt);
    assert(flatten_option(I{7}) == I{7});
    assert(flatten_option(I{}) == std::nullopt);

    using Owned = std::unique_ptr<int>;
    static_assert(!std::is_copy_constructible_v<Owned>);
    std::optional<std::optional<Owned>> owned{
        std::optional<Owned>{std::make_unique<int>(99)}
    };
    auto flattened = flatten_option(std::move(owned));
    assert(flattened.has_value());
    assert(**flattened == 99);
    assert(flattened_option_t<std::optional<Owned>>{} == std::nullopt);
    return 0;
}
