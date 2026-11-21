#pragma once
// Trusted host-only request/NIO registration bridge. The returned watch
// identity and request lease must travel together until the broker's physical
// terminal completion has been consumed and scheduler cleanup has finished.
// No guest callbacks, source await lowering, or broker ACK is inferred here.
#include "oreslang/runtime/NativeRequestPhysicalWork.hpp"
#include "oreslang/runtime/NativeOresVMNio.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <utility>

namespace oreslang::runtime {

template<std::size_t Capacity>
class NativeRequestNioWatch final {
    using Work=NativeRequestPhysicalWork<Capacity>;
public:
    using Context=typename Work::Context;
    using Handle=typename Work::Handle;

    struct AcceptedWatch final {
        const OresNativeNioWatch watch;
        const int fd;
        const AsyncIoTicket operation;
        PinnedContinuationGate* operation_gate=nullptr;
        typename Work::IoLease physical;

        AcceptedWatch(OresNativeNioWatch w,int file,AsyncIoTicket op,
                      PinnedContinuationGate& gate,
                      typename Work::IoLease&& pin) noexcept
            :watch(w),fd(file),operation(op),operation_gate(&gate),
             physical(std::move(pin)) {}
        AcceptedWatch(const AcceptedWatch&)=delete;
        AcceptedWatch& operator=(const AcceptedWatch&)=delete;
        AcceptedWatch(AcceptedWatch&&)=default;
        AcceptedWatch& operator=(AcceptedWatch&&)=delete;

        // A completion from a different reactor, watch generation, or FD
        // cannot provide evidence about this request's broker ownership.
        // This comparison alone is NOT physical-quiescence acknowledgement.
        bool matches_completion(const OresNativeNioCompletion& event) const noexcept {
            return watch.ticket != 0 &&
                event.watch.ticket==watch.ticket &&
                event.watch.reactor==watch.reactor &&
                event.fd==fd;
        }
        // Observing one exact terminal NIO record arbitrates the operation's
        // logical outcome, never the physical broker ACK. The broker must
        // independently certify that it cannot touch buffers or callback
        // again before the request lease may physically retire.
        bool observe_terminal_completion(
                const OresNativeNioCompletion& event) noexcept {
            if(!operation_gate || !matches_completion(event)) return false;
            AsyncIoTerminal outcome;
            switch(event.outcome) {
                case ORES_NATIVE_NIO_READY:
                    outcome=AsyncIoTerminal::success;break;
                case ORES_NATIVE_NIO_CANCELLED:
                    outcome=AsyncIoTerminal::cancelled;break;
                case ORES_NATIVE_NIO_STOPPED:
                    outcome=AsyncIoTerminal::closed;break;
                default:return false;
            }
            return operation_gate->settle(operation,outcome);
        }
    };

    static std::optional<AcceptedWatch> register_watch(
            Context& context,Handle request,OresNativeActorRef owner,
            PinnedContinuationGate& gate,AsyncIoTicket operation,
            OresNativeVM* vm,int fd,std::uint32_t events) {
        if(!vm || fd<0 || !events) return std::nullopt;
        OresNativeNioWatch accepted{};
        auto physical=Work::register_async_io(
            context,request,owner,gate,operation,[&]() noexcept -> bool {
                const bool ok=ores_native_vm_nio_try_watch(
                    vm,fd,events,&accepted)!=0;
                // Successful watch admission with a missing identity is
                // an impossible backend contract violation. Cannot safely
                // roll back: its broker may already own the watch.
                if(ok && !accepted.ticket) std::terminate();
                return ok;
            });
        if(!physical) return std::nullopt;
        return AcceptedWatch(accepted,fd,operation,gate,std::move(*physical));
    }
};

} // namespace oreslang::runtime
