#pragma once
#if !defined(__linux__)
#error "NativeOresVMActorNio requires Linux"
#endif

#include "oreslang/runtime/NativeActorNioCallbacks.hpp"
#include "oreslang/runtime/NativeOresVM.h"

namespace oreslang::runtime {

// Trusted C++ host-only actor NIO interface. No functions here are guest
// capabilities, and the legacy C NIO APIs deliberately cannot consume these
// completions. Caller keeps the VM and actor-owned gate alive until all
// accepted terminal completions have been acknowledged.
using ActorNioCompletion = NativeActorNioCallbacks::Completion;

// Direct native NIO backend admission (exactly one irreversible mode per
// backend: legacy C records OR actor-owned completions; never both).
bool nio_actor_try_watch(OresNativeNio* backend,
    NativeTransportCallbackGate& owner, int fd, std::uint32_t events,
    OresNativeNioWatch* output) noexcept;
bool nio_actor_try_cancel(OresNativeNio* backend, OresNativeNioWatch watch) noexcept;
// Trusted host shutdown only: may join Linux reactor pthreads. Afterwards
// pending watches emit STOPPED terminal records; they still require ack.
bool nio_actor_stop(OresNativeNio* backend) noexcept;
bool nio_actor_try_pop(OresNativeNio* backend, ActorNioCompletion* output) noexcept;
bool nio_actor_acknowledge(OresNativeNio* backend,
    const ActorNioCompletion& completion) noexcept;

// VM-lifecycle-pinned entrypoints. Each operation borrows its installed NIO
// backend under existing VM retirement fencing. For graceful VM close, the
// trusted host must stop/cancel the watches, consume terminal records and
// acknowledge every lease BEFORE permitting VM retirement to free the backend.
// Calls after retirement reject; pending leases cause backend teardown to
// fail-stop instead of silently freeing memory still accessed by callbacks.
bool vm_nio_actor_try_watch(OresNativeVM* vm,
    NativeTransportCallbackGate& owner, int fd, std::uint32_t events,
    OresNativeNioWatch* output) noexcept;
bool vm_nio_actor_try_cancel(OresNativeVM* vm, OresNativeNioWatch watch) noexcept;
bool vm_nio_actor_stop(OresNativeVM* vm) noexcept;
bool vm_nio_actor_try_pop(OresNativeVM* vm, ActorNioCompletion* output) noexcept;
bool vm_nio_actor_acknowledge(OresNativeVM* vm,
    const ActorNioCompletion& completion) noexcept;

} // namespace oreslang::runtime
