#include "oreslang/runtime/ActorStateReadWriteContinuations.hpp"

#include <concepts>
#include <cstdint>

using Registry = oreslang::runtime::ActorStateReadWriteContinuations;

// Compile as an ordinary production consumer, without the internal test
// hook. Keeping these capabilities unnameable is part of actor isolation.
template <typename T>
concept HasHostCompletion = requires(T& registry, std::uint64_t incarnation) {
    registry.complete(incarnation, 1);
};

template <typename T>
concept HasTestCompletion = requires(T& registry, std::uint64_t incarnation) {
    registry.complete_undispatched_for_test(incarnation, 1);
};

template <typename T>
concept HasRawDispatchClaim =
        requires(T& registry, const typename T::Notification& wake, typename T::Mode mode) {
    { registry.try_claim_ready(wake, mode) } -> std::same_as<bool>;
};

template <typename T>
concept HasRawDispatchedCompletion =
        requires(T& registry, std::uint64_t incarnation) {
    registry.complete_dispatched(incarnation, 1);
};

static_assert(!HasHostCompletion<Registry>,
              "production callers must never release state tickets directly");
static_assert(!HasTestCompletion<Registry>,
              "test-only completion capability must not leak into production");
static_assert(!HasRawDispatchClaim<Registry>,
              "only the scheduler bridge may claim a READY continuation");
static_assert(!HasRawDispatchedCompletion<Registry>,
              "only the owning scheduler bridge may complete a guest loan");

int main() { return 0; }
