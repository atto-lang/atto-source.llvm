// Compile-negative production access gate. This source MUST NOT compile.
// The only production path is the supervisor-owned V1 registry.
#include "oreslang/runtime/GpuActorHostTurnBridgeV0.hpp"

using oreslang::gpu_actor::HostTurnBridgeV0;

void forbidden_completion(HostTurnBridgeV0& host, HostTurnBridgeV0::Turn turn) {
    host.finish_after_provider_event(turn, true, true);
}
void forbidden_cancel(HostTurnBridgeV0& host, HostTurnBridgeV0::Turn turn) {
    host.acknowledge_cancelled_provider_event(turn, true);
}
void forbidden_release(HostTurnBridgeV0& host) {
    host.acknowledge_backend_buffer_release(true);
}
void forbidden_retire(HostTurnBridgeV0& host) {
    host.retire_gracefully_after_backend_release(true);
}
