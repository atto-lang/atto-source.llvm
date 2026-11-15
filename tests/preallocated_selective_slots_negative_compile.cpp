#include "oreslang/runtime/PreallocatedSelectiveSlots.hpp"

#if defined(ATTO_NEGATIVE_DELETER_DESTRUCTOR)
struct UnsafeDeleter {
    void operator()(int* value) const noexcept { delete value; }
    ~UnsafeDeleter() noexcept {} // even an empty hook can hide re-entry
};
#elif defined(ATTO_NEGATIVE_DELETER_MOVE)
struct UnsafeDeleter {
    UnsafeDeleter() = default;
    UnsafeDeleter(UnsafeDeleter&&) noexcept {} // may invoke guest code under lock
    void operator()(int* value) const noexcept { delete value; }
};
#else
#error "select exactly one negative deleter test"
#endif

// Expected to FAIL compilation due to the exact class static_assert.
using UnsafeSlots = oreslang::runtime::PreallocatedSelectiveSlots<int, UnsafeDeleter>;
static_assert(sizeof(UnsafeSlots) > 0);
