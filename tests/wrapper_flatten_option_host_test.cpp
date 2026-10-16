#include <cstdlib>
#include <memory>
#include <optional>
#include <type_traits>
#include "WrapperOptionHost.hpp"

using oreslang::runtime::flatten_option;
using oreslang::runtime::flattened_option_t;

static void require(bool condition) { if (!condition) std::abort(); }

// Tracks ownership, not moved-from destructors. Only the final live payload
// may release the affine resource, including after multiple Option moves.
struct AffineDropProbe {
    int* releases;
    bool owns;
    explicit AffineDropProbe(int* target) : releases(target), owns(true) { }
    AffineDropProbe(const AffineDropProbe&) = delete;
    AffineDropProbe& operator=(const AffineDropProbe&) = delete;
    AffineDropProbe(AffineDropProbe&& other) noexcept
        : releases(other.releases), owns(other.owns) {
        other.owns = false;
    }
    AffineDropProbe& operator=(AffineDropProbe&&) = delete;
    ~AffineDropProbe() {
        if (owns) ++*releases;
    }
};

int main() {
    using I = std::optional<int>;
    using II = std::optional<I>;
    using III = std::optional<II>;
    static_assert(std::is_same_v<flattened_option_t<II>, I>);
    static_assert(std::is_same_v<flattened_option_t<III>, I>);
    const III nested = III{II{I{42}}};
    require(nested.has_value());
    require(flatten_option(nested) == I{42});
    require(flatten_option(III{}) == std::nullopt);
    require(flatten_option(III{II{I{}}}) == std::nullopt);
    const II stored_none = II{I{}};
    require(stored_none.has_value());  // Must NOT flatten implicitly.
    require(flatten_option(stored_none) == std::nullopt);
    require(flatten_option(I{7}) == I{7});
    require(flatten_option(I{}) == std::nullopt);

    using Owned = std::unique_ptr<int>;
    static_assert(!std::is_copy_constructible_v<Owned>);
    std::optional<std::optional<Owned>> owned{
        std::optional<Owned>{std::make_unique<int>(99)}
    };
    auto flattened = flatten_option(std::move(owned));
    require(flattened.has_value());
    require(**flattened == 99);
    require(flattened_option_t<std::optional<Owned>>{} == std::nullopt);

    int releases = 0;
    {
        using P = std::optional<AffineDropProbe>;
        using PP = std::optional<P>;
        using PPP = std::optional<PP>;
        PPP deep{PP{P{std::in_place, &releases}}};
        require(deep.has_value());
        auto extracted = flatten_option(std::move(deep));
        require(extracted.has_value());
        require(releases == 0);
    }
    require(releases == 1);  // Exactly one owned resource release.
    return 0;
}
