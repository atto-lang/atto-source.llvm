#pragma once

#include "oreslang/backend/GpuG1DeviceLeaseRegistry.hpp"
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace oreslang::gpu::g1 {

// Returned only by an authenticated driver. Guests cannot manufacture this
// result: callers never pass a completion Boolean to EventProvider.
enum class HardwareEventState : std::uint8_t { Pending, Completed, DeviceError };
enum class DispatchState : std::uint8_t { Pending, Completed, CancelledQuiescent, DeviceLost };

// Driver is a trusted host implementation. Production must use the actual
// CUDA driver (GpuG1CudaDriver.hpp), NOT test fakes or guest callbacks.
// No blocking waits occur in poll(); production calls it on a control/event
// lane, never while holding an actor critical section or scheduler carrier.
template<class Driver>
class GpuG1EventProvider final {
public:
    using Registry = DeviceLeaseRegistry;
    using QueueHandle = Registry::QueueHandle;
    using AllocationHandle = Registry::AllocationHandle;
    using Ticket = Registry::Ticket;
    using Kernel = typename Driver::Kernel;

    struct Slice { AllocationHandle allocation; std::uint64_t offset_bytes; };
    struct Launch {
        QueueHandle queue;
        Kernel kernel;
        Slice x, y, out;
        float scale;
        std::uint64_t n;
        std::uint32_t block_threads;
    };

    GpuG1EventProvider(Registry& ledger, Driver& driver,
                       std::uint64_t tenant, std::uint64_t device)
        : ledger_(ledger), driver_(driver), tenant_(tenant), device_(device) {
        if (!tenant_ || !device_) fail("GPU-G1-CUDA-001: invalid provider tenant/device");
        const auto caps = driver_.limits();
        if (!caps.maximum_threads_per_block || !caps.maximum_grid_x)
            fail("GPU-G1-CUDA-001: unusable driver launch capabilities");
    }
    GpuG1EventProvider(const GpuG1EventProvider&) = delete;
    GpuG1EventProvider& operator=(const GpuG1EventProvider&) = delete;

    QueueHandle create_queue() {
        std::lock_guard<std::mutex> lock(mutex_);
        require_live();
        if (next_queue_id_ == max()) fail("GPU-G1-CUDA-002: queue identity exhausted");
        auto stream = driver_.create_stream();
        std::optional<QueueHandle> registered;
        try {
            const auto id = next_queue_id_;
            registered.emplace(ledger_.provider_register_queue(tenant_, device_, id));
            queues_.emplace(id, QueueEntry{*registered, stream});
            ++next_queue_id_;
            return *registered;
        } catch (...) {
            // Failed host tracking is rolled back BEFORE destroying the
            // stream. No submission can exist on this freshly minted queue.
            if (registered) {
                try { ledger_.provider_retire_queue(*registered); }
                catch (...) { quarantine(); }
            }
            try { driver_.destroy_stream(stream); } catch (...) { quarantine(); }
            throw;
        }
    }

    AllocationHandle allocate(std::uint64_t bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        require_live();
        if (!bytes || bytes % sizeof(float) || bytes > static_cast<std::uint64_t>(SIZE_MAX) ||
            next_allocation_id_ == max())
            fail("GPU-G1-CUDA-003: invalid allocation size/identity");
        auto buffer = driver_.allocate(static_cast<std::size_t>(bytes));
        std::optional<AllocationHandle> registered;
        try {
            const auto id = next_allocation_id_;
            registered.emplace(ledger_.provider_register_allocation(tenant_, device_, id, bytes));
            allocations_.emplace(id, AllocationEntry{*registered, buffer, bytes});
            ++next_allocation_id_;
            return *registered;
        } catch (...) {
            if (registered) {
                try { ledger_.provider_retire_allocation(*registered); }
                catch (...) { quarantine(); }
            }
            try { driver_.free(buffer); } catch (...) { quarantine(); }
            throw;
        }
    }

    // Returns ticket 0 for a valid empty logical extent. No physical kernel
    // or event is enqueued. Ownership/domain/driver limits still checked.
    Ticket launch_saxpy(const Launch& launch) {
        std::lock_guard<std::mutex> lock(mutex_);
        require_live();
        auto& queue = lookup_queue(launch.queue);
        const auto& x = lookup_allocation(launch.x.allocation);
        const auto& y = lookup_allocation(launch.y.allocation);
        const auto& out = lookup_allocation(launch.out.allocation);
        const auto caps = driver_.limits();
        if (!launch.block_threads || launch.block_threads > caps.maximum_threads_per_block ||
            !std::isfinite(launch.scale) || !driver_.valid_kernel(launch.kernel))
            fail("GPU-G1-CUDA-004: invalid kernel/scale/block");
        if (launch.n > max()/sizeof(float))
            fail("GPU-G1-CUDA-005: logical byte count overflow");
        const auto bytes = launch.n * sizeof(float);
        for (const auto& ref : {std::pair<const Slice*,const AllocationEntry*>{&launch.x,&x},
                                {&launch.y,&y}, {&launch.out,&out}}) {
            if (ref.first->offset_bytes % sizeof(float) ||
                ref.first->offset_bytes > ref.second->bytes ||
                bytes > ref.second->bytes - ref.first->offset_bytes)
                fail("GPU-G1-CUDA-005: out-of-bounds typed kernel arguments");
        }
        const auto blocks = launch.n / launch.block_threads +
                            (launch.n % launch.block_threads != 0);
        if (blocks > caps.maximum_grid_x || blocks > UINT32_MAX)
            fail("GPU-G1-CUDA-006: physical grid limit exceeded");
        if (launch.n == 0) return 0;

        // Event is created BEFORE claiming loans or enqueuing a kernel.
        // Any error after admission conservatively quarantines the device.
        auto event = driver_.create_event();
        std::vector<Registry::View> views{
            {launch.x.allocation, launch.x.offset_bytes, bytes, Registry::Access::Read},
            {launch.y.allocation, launch.y.offset_bytes, bytes, Registry::Access::Read},
            {launch.out.allocation, launch.out.offset_bytes, bytes, Registry::Access::Write}
        };
        Ticket ticket = 0;
        try {
            ticket = ledger_.provider_submit(launch.queue, views);
        } catch (...) {
            try { driver_.destroy_event(event); } catch (...) { quarantine(); }
            throw;
        }
        try {
            pending_.emplace(ticket, Pending{launch.queue, event, false});
            // Driver may enqueue work before reporting an error. Any launch,
            // record, or tracking failure therefore permanently quarantines.
            driver_.launch_saxpy(launch.kernel, queue.stream, launch.scale,
                driver_.offset_ptr(x.buffer, launch.x.offset_bytes),
                driver_.offset_ptr(y.buffer, launch.y.offset_bytes),
                driver_.offset_ptr(out.buffer, launch.out.offset_bytes),
                launch.n, static_cast<std::uint32_t>(blocks), launch.block_threads);
            driver_.record_event(event, queue.stream);
        } catch (...) {
            quarantine(); // NEVER ack the ticket or release the loan.
            throw;
        }
        return ticket;
    }

    void request_cancel(const QueueHandle& queue, Ticket ticket) {
        std::lock_guard<std::mutex> lock(mutex_);
        require_live();
        (void)lookup_queue(queue);
        auto& p = lookup_pending(queue, ticket);
        ledger_.provider_request_cancel(queue, ticket);
        p.cancel_requested = true;
        // Cancellation marks intent. It does not issue a fake completion or
        // destroy the event, stream, or device allocations.
    }

    DispatchState poll(const QueueHandle& queue, Ticket ticket) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (quarantined_) return DispatchState::DeviceLost;
        (void)lookup_queue(queue);
        auto& p = lookup_pending(queue, ticket);
        HardwareEventState result = HardwareEventState::DeviceError;
        try { result = driver_.query_event(p.event); }
        catch (...) { quarantine(); return DispatchState::DeviceLost; }
        if (result == HardwareEventState::Pending) return DispatchState::Pending;
        if (result != HardwareEventState::Completed) {
            quarantine();
            return DispatchState::DeviceLost;
        }
        // Completion comes ONLY from driver event query, never from a
        // guest-provided status, caller callback, or cancellation request.
        try {
            ledger_.provider_ack_event(queue, ticket,
                p.cancel_requested ? Registry::Event::CancelledQuiescent
                                   : Registry::Event::Completed);
        } catch (...) { quarantine(); return DispatchState::DeviceLost; }
        const bool cancelled = p.cancel_requested;
        try { driver_.destroy_event(p.event); }
        catch (...) { quarantine(); return DispatchState::DeviceLost; }
        pending_.erase(ticket);
        return cancelled ? DispatchState::CancelledQuiescent : DispatchState::Completed;
    }

    void release_allocation(const AllocationHandle& handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        require_live();
        const auto entry = lookup_allocation(handle);
        // Registry rejects any outstanding, including cancelled, device loans.
        ledger_.provider_retire_allocation(handle);
        try { driver_.free(entry.buffer); }
        catch (...) { quarantine(); throw; }
        allocations_.erase(handle.id());
    }

    void close_queue(const QueueHandle& handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        require_live();
        const auto entry = lookup_queue(handle);
        ledger_.provider_retire_queue(handle);
        try { driver_.destroy_stream(entry.stream); }
        catch (...) { quarantine(); throw; }
        queues_.erase(handle.id());
    }

    void quarantine_device() {
        std::lock_guard<std::mutex> lock(mutex_);
        quarantine();
    }
    bool is_quarantined() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return quarantined_;
    }
    std::size_t pending_events() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pending_.size();
    }
    void require_quiescent_shutdown() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (quarantined_ || !pending_.empty() || !allocations_.empty() || !queues_.empty())
            fail("GPU-G1-CUDA-007: cannot shut down with live GPU resources");
    }

private:
    struct QueueEntry { QueueHandle handle; typename Driver::Stream stream; };
    struct AllocationEntry { AllocationHandle handle; typename Driver::Buffer buffer; std::uint64_t bytes; };
    struct Pending { QueueHandle queue; typename Driver::Event event; bool cancel_requested; };
    static constexpr std::uint64_t max() { return std::numeric_limits<std::uint64_t>::max(); }
    [[noreturn]] static void fail(const char* error) { throw std::invalid_argument(error); }
    void require_live() const { if (quarantined_) fail("GPU-G1-CUDA-008: quarantined driver"); }
    QueueEntry& lookup_queue(const QueueHandle& h) {
        auto it = queues_.find(h.id());
        if (it == queues_.end() || it->second.handle.generation() != h.generation() ||
            it->second.handle.id() != h.id() || it->second.handle.tenant() != h.tenant() ||
            it->second.handle.device() != h.device() ||
            it->second.handle.registry() != h.registry())
            fail("GPU-G1-CUDA-009: unknown/stale/foreign queue");
        return it->second;
    }
    const AllocationEntry& lookup_allocation(const AllocationHandle& h) const {
        auto it = allocations_.find(h.id());
        if (it == allocations_.end() || it->second.handle.generation() != h.generation() ||
            it->second.handle.id() != h.id() || it->second.handle.tenant() != h.tenant() ||
            it->second.handle.device() != h.device() ||
            it->second.handle.registry() != h.registry())
            fail("GPU-G1-CUDA-010: unknown/stale/foreign allocation");
        return it->second;
    }
    Pending& lookup_pending(const QueueHandle& queue, Ticket ticket) {
        auto it = pending_.find(ticket);
        if (!ticket || it == pending_.end() || it->second.queue.id() != queue.id() ||
            it->second.queue.generation() != queue.generation() ||
            it->second.queue.tenant() != queue.tenant() ||
            it->second.queue.device() != queue.device())
            fail("GPU-G1-CUDA-011: stale/foreign event ticket");
        return it->second;
    }
    void quarantine() {
        quarantined_ = true;
        ledger_.provider_quarantine_device(device_);
    }

    Registry& ledger_;
    Driver& driver_;
    const std::uint64_t tenant_, device_;
    mutable std::mutex mutex_;
    bool quarantined_ = false;
    std::uint64_t next_queue_id_ = 1;
    std::uint64_t next_allocation_id_ = 1;
    std::map<std::uint64_t,QueueEntry> queues_;
    std::map<std::uint64_t,AllocationEntry> allocations_;
    std::map<Ticket,Pending> pending_;
};

} // namespace oreslang::gpu::g1
