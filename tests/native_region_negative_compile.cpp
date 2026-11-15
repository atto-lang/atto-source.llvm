// Build this fixture with exactly one ORESLANG_TEST_NEGATIVE_* define.
// Each configuration MUST fail compilation, not execute.
#include "oreslang/runtime/NativeRegionRegistry.hpp"
#include "oreslang/runtime/NativeRegionRuntimeAuthority.hpp"
using namespace oreslang::runtime;

#if defined(ORESLANG_TEST_NEGATIVE_PUBLIC_ISSUE)
void attempt_public_mint(NativeRegionRegistry& registry,
                         ActorHeapIdentity owner, void* data) {
    (void)registry.issue(NativeRegionRegistry::Trust::Trusted, owner, 1, 1,
                         data, 16, TrustedPointerWindow::Mode::ReadWrite);
}
#elif defined(ORESLANG_TEST_NEGATIVE_PUBLIC_FENCE)
void attempt_public_actor_fence(NativeRegionRegistry& registry,
                                ActorHeapIdentity owner) {
    registry.fence_actor(owner);
}
#elif defined(ORESLANG_TEST_NEGATIVE_PUBLIC_AUTHORITY_CONSTRUCT)
void attempt_broker_construction(NativeRegionRegistry& registry) {
    NativeRegionRuntimeAuthority authority(registry, 1);
}
#elif defined(ORESLANG_TEST_NEGATIVE_PUBLIC_AUTHORITY_ADMIT)
void attempt_public_native_admission(NativeRegionRuntimeAuthority& authority,
                                     ActorHeapIdentity owner) {
    authority.admit_actor(owner, NativeRegionRuntimeAuthority::Admission::TrustedNative);
}
#elif defined(ORESLANG_TEST_NEGATIVE_PUBLIC_AUTHORITY_ISSUE)
void attempt_public_native_issuance(NativeRegionRuntimeAuthority& authority,
                                   ActorHeapIdentity owner, void* data) {
    (void)authority.issue_region(owner, 1, 1, data, 16,
                                 TrustedPointerWindow::Mode::ReadWrite);
}
#elif defined(ORESLANG_TEST_NEGATIVE_PUBLIC_AUTHORITY_RETIRE)
void attempt_public_native_retirement(NativeRegionRuntimeAuthority& authority,
                                     ActorHeapIdentity owner) {
    authority.retire_actor(owner);
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
