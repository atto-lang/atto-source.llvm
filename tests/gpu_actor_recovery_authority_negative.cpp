// This MUST fail to compile in a normal runtime translation unit.
// Only the native supervisor's friend recovery worker can call these methods.
#include "oreslang/runtime/GpuActorOpenClProviderV2.hpp"
using oreslang::gpu_actor::OpenClActorProviderPortV2;
void forbidden_native_recovery(OpenClActorProviderPortV2& p) {
    (void)p.recover_unknown_begin_on_worker();
    (void)p.recover_unknown_kernel_on_worker();
    (void)p.recover_device_loss_on_worker();
    (void)p.recover_release_failure_on_worker();
    (void)p.matches_recovery_turn({3,1});
}
