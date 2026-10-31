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
        OresNativeNioWatch watch{};
        int fd=-1;
        typename Work::IoLease physical;

        AcceptedWatch(OresNativeNioWatch w,int file,
                      typename Work::IoLease&& pin) noexcept
            :watch(w),fd(file),physical(std::move(pin)) {}
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
        return AcceptedWatch(accepted,fd,std::move(*physical));
    }
};

} // namespace oreslang::runtime
