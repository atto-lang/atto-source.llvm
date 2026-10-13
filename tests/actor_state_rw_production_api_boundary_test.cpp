#include "oreslang/runtime/ActorStateReadWriteContinuations.hpp"

#include <cstdint>
#include <type_traits>
#include <utility>

using Registry = oreslang::runtime::ActorStateReadWriteContinuations;

// C++17 SFINAE verification from an ordinary production translation unit.
// Private dispatch methods and the explicitly test-only completion helper
// must be inaccessible, even if the implementation is refactored.
template <typename T, typename = void>
struct HasHostCompletion : std::false_type {};
template <typename T>
struct HasHostCompletion<T, std::void_t<decltype(
        std::declval<T&>().complete(std::uint64_t{}, std::uint64_t{}))>>
        : std::true_type {};

template <typename T, typename = void>
struct HasTestCompletion : std::false_type {};
template <typename T>
struct HasTestCompletion<T, std::void_t<decltype(
        std::declval<T&>().complete_undispatched_for_test(
                std::uint64_t{}, std::uint64_t{}))>>
        : std::true_type {};

template <typename T, typename = void>
struct HasRawDispatchClaim : std::false_type {};
template <typename T>
struct HasRawDispatchClaim<T, std::void_t<decltype(
        std::declval<T&>().try_claim_ready(
                std::declval<const typename T::Notification&>(),
                std::declval<typename T::Mode>()))>>
        : std::true_type {};

template <typename T, typename = void>
struct HasRawDispatchedCompletion : std::false_type {};
template <typename T>
struct HasRawDispatchedCompletion<T, std::void_t<decltype(
        std::declval<T&>().complete_dispatched(
                std::uint64_t{}, std::uint64_t{}))>>
        : std::true_type {};

static_assert(!HasHostCompletion<Registry>::value,
              "production callers must never release state tickets directly");
static_assert(!HasTestCompletion<Registry>::value,
              "test-only completion must not leak into production");
static_assert(!HasRawDispatchClaim<Registry>::value,
              "only the scheduler bridge may claim a READY continuation");
static_assert(!HasRawDispatchedCompletion<Registry>::value,
              "only the scheduler bridge may complete a dispatched loan");

int main() { return 0; }
