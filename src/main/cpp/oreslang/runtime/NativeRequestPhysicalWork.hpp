#pragma once
// Trusted native bridge between request-context leases and independently
// executing source Tasks / asynchronous I/O. No guest capability or compiler
// lowering is provided here. The runtime must call finish_* only AFTER the
// physical worker / I/O cleanup has actually completed.
#include "oreslang/runtime/NativeRequestContext.hpp"
#include "oreslang/runtime/PinnedContinuationGate.hpp"
#include "oreslang/runtime/TaskCancellation.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <type_traits>
#include <utility>

namespace oreslang::runtime {

template<std::size_t Capacity>
class NativeRequestPhysicalWork final {
public:
    using Context = NativeRequestContext<Capacity>;
    using Handle = typename Context::Handle;
    using Lease = typename Context::Lease;

    // A structured Task's physical frame lease. TaskCancellation::retire()
    // refuses live structured descendants; mere cancellation never releases
    // this request pin. This object must outlive the actual worker execution.
    class TaskLease final {
        friend class NativeRequestPhysicalWork;
        TaskCancellation::Ptr task_;
        Lease pin_;
        TaskLease(TaskCancellation::Ptr task, Lease&& pin) noexcept
            :task_(std::move(task)),pin_(std::move(pin)) {}
    public:
        TaskLease(const TaskLease&)=delete;
        TaskLease& operator=(const TaskLease&)=delete;
        TaskLease(TaskLease&&)=default;
        TaskLease& operator=(TaskLease&&)=delete;
        ~TaskLease() { if(pin_) std::terminate(); }

        bool finish_after_physical_exit() noexcept {
            if(!pin_ || !task_) return false;
            // May reject retirement with children still physically active.
            // Retain the request pin on ANY failure; fail closed.
            try {task_->retire();}
            catch(...) {return false;}
            pin_.reset();
            task_.reset();
            return true;
        }
        explicit operator bool() const noexcept { return bool(pin_); }
    };

    // An async I/O completion's physical frame lease. The existing
    // PinnedContinuationGate owns terminal/cancel vs broker ACK vs finished
    // resume arbitration. Do not drop the request pin on cancel/timeout alone.
    // The exact gate and its broker must outlive this host-only binding.
    class IoLease final {
        friend class NativeRequestPhysicalWork;
        PinnedContinuationGate* gate_;
        AsyncIoTicket ticket_;
        Lease pin_;
        IoLease(PinnedContinuationGate& gate, AsyncIoTicket ticket, Lease&& pin) noexcept
            :gate_(&gate),ticket_(ticket),pin_(std::move(pin)) {}
    public:
        IoLease(const IoLease&)=delete;
        IoLease& operator=(const IoLease&)=delete;
        IoLease(IoLease&&)=default;
        IoLease& operator=(IoLease&&)=delete;
        ~IoLease() { if(pin_) std::terminate(); }

        bool finish_after_broker_and_resume_quiescence() noexcept {
            if(!pin_ || !gate_ ||
                !gate_->try_retire_bound_request(ticket_)) return false;
            pin_.reset();
            gate_=nullptr;
            return true;
        }
        explicit operator bool() const noexcept { return bool(pin_); }
    };

    static std::optional<TaskLease> retain_child_task(
            Context& context, Handle request, OresNativeActorRef owner,
            const TaskCancellation::Ptr& expected_parent,
            TaskCancellation::Ptr child_authority) {
        if(!expected_parent || !child_authority) return std::nullopt;
        std::optional<Lease> retained;
        // Exact structured parent and cancellation/retirement share ONE Task
        // graph-lock admission boundary. No unrelated Task can silently
        // acquire another request's physical lease through this bridge.
        // The callback only pins registry storage, never executes guest code.
        const bool admitted=child_authority->with_active_child_of(expected_parent,[&] {
            retained=context.retain(request,owner);
            return bool(retained);
        });
        if(!admitted || !retained) return std::nullopt;
        return TaskLease(std::move(child_authority),std::move(*retained));
    }

    // The registration callback is the trusted host's actual NIO watch
    // admission call (e.g. ores_native_vm_nio_try_watch), not a guest callback.
    // It MUST be nonthrowing and return true only once a physical broker
    // watch has been accepted, and false only when no broker is able to
    // touch the provided buffers. An exception after successful physical
    // registration cannot be translated into a safe rollback.
    // An immediately delivered readiness/cancellation completion may settle
    // the gate before registration returns; RESERVED prevents retirement
    // until the physical request lease is irreversibly PUBLISHED.
    //
    // No allocation or blocking is introduced by this bridge; the host's
    // chosen registrar must independently satisfy those constraints.
    template<typename RegisterWatch>
    static std::optional<IoLease> register_async_io(
            Context& context, Handle request, OresNativeActorRef owner,
            PinnedContinuationGate& gate, AsyncIoTicket ticket,
            RegisterWatch&& register_watch) {
        static_assert(std::is_nothrow_invocable_v<RegisterWatch&>,
            "Native NIO watch admission must be a nonthrowing host callback");
        static_assert(std::is_convertible_v<
            std::invoke_result_t<RegisterWatch&>,bool>,
            "Native NIO watch admission must return acceptance status");
        if(!gate.matches_request_owner({
                owner.scheduler_domain,owner.actor_slot,owner.generation}))
            return std::nullopt;
        if(!gate.try_bind_request(ticket)) return std::nullopt;
        auto pin=context.retain(request,owner);
        if(!pin || gate.terminal()!=AsyncIoTerminal::pending) {
            if(!gate.abort_request_binding(ticket)) std::terminate();
            return std::nullopt;
        }
        // Throwing here after accepting a kernel watch could otherwise
        // release the physical request pin under a live broker callback.
        const bool accepted=static_cast<bool>(
            std::forward<RegisterWatch>(register_watch)());
        if(!accepted) {
            if(!gate.abort_request_binding(ticket)) std::terminate();
            return std::nullopt;
        }
        if(!gate.commit_request_binding(ticket)) std::terminate();
        return IoLease(gate,ticket,std::move(*pin));
    }
};

} // namespace oreslang::runtime
