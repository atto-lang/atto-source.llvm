#pragma once
#include "oreslang/runtime/NativeOresVM.h"
#include "oreslang/runtime/NativeEpollReactorABI.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Trusted host-only VM NIO integration. All of these functions require the
 * caller to retain a live VM pointer and externally fence producer threads
 * before VM retirement/close. They never run guest code.
 *
 * NIO startup is an explicit bootstrap operation. Throughput mode starts one
 * dedicated epoll thread; preallocated RT mode starts its fixed configured
 * reactor set before source guest admission. No hard-RT WCET is claimed.
 */
int ores_native_vm_enable_nio(OresNativeVM* vm, size_t max_reactors,
                              int preallocated_realtime, size_t rt_reactors);
size_t ores_native_vm_nio_started(OresNativeVM* vm);
int ores_native_vm_nio_try_watch(OresNativeVM* vm, int fd, uint32_t events,
                                 OresNativeNioWatch* output);
int ores_native_vm_nio_try_cancel(OresNativeVM* vm, OresNativeNioWatch watch);
int ores_native_vm_nio_try_pop(OresNativeVM* vm, OresNativeNioCompletion* output);
int ores_native_vm_nio_completion_fd(OresNativeVM* vm, size_t reactor);
void ores_native_vm_nio_drain_signal(OresNativeVM* vm, size_t reactor);

#ifdef __cplusplus
}
#endif
