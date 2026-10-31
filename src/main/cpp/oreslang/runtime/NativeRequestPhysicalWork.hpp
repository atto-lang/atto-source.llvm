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
            if(!pin_ || !gate_ || !gate_->try_retire(ticket_)) return false;
            pin_.reset();
            gate_=nullptr;
            return true;
        }
        explicit operator bool() const noexcept { return bool(pin_); }
    };

    static std::optional<TaskLease> retain_child_task(
            Context& context, Handle request, OresNativeActorRef owner,
            TaskCancellation::Ptr child_authority) {
        if(!child_authority) return std::nullopt;
        std::optional<Lease> retained;
        // Admission and cancellation/retirement share the Task graph lock.
        // The callback only acquires a request-registry pin; it does not
        // execute guest code, invoke Task methods, or suspend.
        const bool admitted=child_authority->with_active_authority([&] {
            retained=context.retain(request,owner);
            return bool(retained);
        });
        if(!admitted || !retained) return std::nullopt;
        return TaskLease(std::move(child_authority),std::move(*retained));
    }

    static std::optional<IoLease> retain_async_io(
            Context& context, Handle request, OresNativeActorRef owner,
            PinnedContinuationGate& gate, AsyncIoTicket ticket) {
        if(!gate.matches_ticket(ticket) ||
            gate.terminal()!=AsyncIoTerminal::pending || gate.retired())
            return std::nullopt;
        auto pin=context.retain(request,owner);
        if(!pin || gate.terminal()!=AsyncIoTerminal::pending ||
            gate.retired()) return std::nullopt;
        // A concurrent settle after the final check is safe: it will still
        // require broker ACK, finished resume, zero loans and try_retire().
        return IoLease(gate,ticket,std::move(*pin));
    }
};

} // namespace oreslang::runtime
