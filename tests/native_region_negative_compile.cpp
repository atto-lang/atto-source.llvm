// Build this fixture with exactly one ORESLANG_TEST_NEGATIVE_* define.
// Each configuration MUST fail compilation, not execute.
#include "oreslang/runtime/NativeRegionRegistry.hpp"
using namespace oreslang::runtime;

#if defined(ORESLANG_TEST_NEGATIVE_PUBLIC_ISSUE)
void attempt_public_mint(NativeRegionRegistry& registry,
                         ActorHeapIdentity owner, void* data) {
    (void)registry.issue(NativeRegionRegistry::Trust::Trusted, owner, 1, 1,
                         data, 16, TrustedPointerWindow::Mode::ReadWrite);
}
#elif defined(ORESLANG_TEST_NEGATIVE_FLOAT)
void attempt_float(TrustedPointerWindow& window, ActorHeapIdentity owner) {
    (void)window.at<float>(owner);
}
#elif defined(ORESLANG_TEST_NEGATIVE_BOOL)
void attempt_bool(TrustedPointerWindow& window, ActorHeapIdentity owner) {
    (void)window.at<bool>(owner);
}
#else
#error "Select a negative compile fixture"
#endif
