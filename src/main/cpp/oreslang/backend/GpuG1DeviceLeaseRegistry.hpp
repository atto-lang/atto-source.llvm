#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace oreslang::gpu::g1 {

// Host-provider-owned admission/loan ledger, not a device driver or a proof
// of physical GPU event completion. The provider MUST withhold its handles and
// event acknowledgements from untrusted guest source and must authenticate
// device events before invoking provider_ack_event().
class DeviceLeaseRegistry final {
public:
    struct Limits {
        std::uint64_t tenant_resident_bytes;
        std::uint64_t tenant_allocations;
        std::uint64_t tenant_inflight;
        std::uint64_t views_per_submission;
        std::uint64_t referenced_bytes_per_submission;
    };
    enum class Access : std::uint8_t { Read, Write };
    enum class Event : std::uint8_t { Completed, CancelledQuiescent };

    class QueueHandle final {
        friend class DeviceLeaseRegistry;
        std::uint64_t registry_, tenant_, device_, id_, generation_;
        QueueHandle(std::uint64_t r, std::uint64_t t, std::uint64_t d,
                    std::uint64_t id, std::uint64_t g)
            : registry_(r), tenant_(t), device_(d), id_(id), generation_(g) {}
    public:
        std::uint64_t generation() const noexcept { return generation_; }
    };
    class AllocationHandle final {
        friend class DeviceLeaseRegistry;
        std::uint64_t registry_, tenant_, device_, id_, generation_;
        AllocationHandle(std::uint64_t r, std::uint64_t t, std::uint64_t d,
                         std::uint64_t id, std::uint64_t g)
            : registry_(r), tenant_(t), device_(d), id_(id), generation_(g) {}
    public:
        std::uint64_t generation() const noexcept { return generation_; }
    };
    struct View {
        AllocationHandle allocation;
        std::uint64_t offset_bytes;
        std::uint64_t length_bytes;
        Access access;
    };
    using Ticket = std::uint64_t;
    struct Snapshot {
        std::uint64_t resident_bytes;
        std::uint64_t allocations;
        std::uint64_t inflight;
        std::uint64_t cancelled_pending;
        std::uint64_t quarantined_queues;
    };

    explicit DeviceLeaseRegistry(Limits limits) : limits_(limits), identity_(next_identity()) {
        static_assert(sizeof(float) == 4, "GPU G1 requires f32 storage ABI");
        if (!limits_.tenant_resident_bytes || !limits_.tenant_allocations ||
            !limits_.tenant_inflight || !limits_.views_per_submission ||
            !limits_.referenced_bytes_per_submission)
            deny("GPU-G1-LEASE-001: invalid positive provider quota");
    }
    DeviceLeaseRegistry(const DeviceLeaseRegistry&) = delete;
    DeviceLeaseRegistry& operator=(const DeviceLeaseRegistry&) = delete;

    QueueHandle provider_register_queue(std::uint64_t tenant, std::uint64_t device,
                                        std::uint64_t queue) {
        if (!tenant || !device || !queue) deny("GPU-G1-LEASE-001: zero queue identity");
        std::lock_guard<std::mutex> guard(mutex_);
        if (quarantined_devices_.count(device))
            deny("GPU-G1-LEASE-017: quarantined device domain");
        const Key key{tenant, device, queue};
        auto [it, inserted] = queues_.try_emplace(key);
        auto& state = it->second;
        if (state.active || state.quarantined || state.generation == max())
            deny("GPU-G1-LEASE-002: queue active, quarantined or exhausted");
        ++state.generation;
        state.active = true;
        return QueueHandle(identity_, tenant, device, queue, state.generation);
    }
    void provider_retire_queue(const QueueHandle& handle) {
        std::lock_guard<std::mutex> guard(mutex_);
        auto& state = require_queue(handle);
        for (const auto& [ticket, submission] : submissions_)
            if (submission.queue == key(handle))
                deny("GPU-G1-LEASE-003: queue still has device events");
        state.active = false;
    }
    AllocationHandle provider_register_allocation(std::uint64_t tenant, std::uint64_t device,
                                                  std::uint64_t allocation, std::uint64_t bytes) {
        if (!tenant || !device || !allocation || !bytes || bytes % sizeof(float) != 0)
            deny("GPU-G1-LEASE-001: invalid provider allocation");
        std::lock_guard<std::mutex> guard(mutex_);
        if (quarantined_devices_.count(device))
            deny("GPU-G1-LEASE-017: quarantined device domain");
        auto& usage = usages_[tenant];
        if (usage.allocations >= limits_.tenant_allocations ||
            bytes > limits_.tenant_resident_bytes - usage.resident_bytes)
            deny("GPU-G1-LEASE-004: tenant resident quota exceeded");
        const Key id{tenant, device, allocation};
        auto [it, inserted] = allocations_.try_emplace(id);
        auto& state = it->second;
        if (state.active || state.quarantined || state.generation == max())
            deny("GPU-G1-LEASE-005: allocation active, quarantined or exhausted");
        ++state.generation;
        state.active = true;
        state.bytes = bytes;
        usage.resident_bytes += bytes;
        ++usage.allocations;
        return AllocationHandle(identity_, tenant, device, allocation, state.generation);
    }
    void provider_retire_allocation(const AllocationHandle& handle) {
        std::lock_guard<std::mutex> guard(mutex_);
        auto& state = require_allocation(handle);
        for (const auto& [ticket, submission] : submissions_)
            for (const auto& view : submission.views)
                if (key(view.allocation) == key(handle))
                    deny("GPU-G1-LEASE-006: allocation still borrowed by device");
        auto& usage = usages_.at(handle.tenant_);
        usage.resident_bytes -= state.bytes;
        --usage.allocations;
        state.active = false;
    }
    Ticket provider_submit(const QueueHandle& queue, const std::vector<View>& views) {
        std::lock_guard<std::mutex> guard(mutex_);
        (void)require_queue(queue);
        if (views.empty() || views.size() > limits_.views_per_submission || next_ticket_ == max())
            deny("GPU-G1-LEASE-007: invalid or oversized submission");
        auto& usage = usages_[queue.tenant_];
        if (usage.inflight >= limits_.tenant_inflight)
            deny("GPU-G1-LEASE-008: tenant inflight quota exceeded");
        std::uint64_t referenced = 0;
        for (const auto& view : views) {
            const auto& h = view.allocation;
            if (h.tenant_ != queue.tenant_ || h.device_ != queue.device_)
                deny("GPU-G1-LEASE-009: foreign allocation domain");
            const auto& allocation = require_allocation(h);
            if ((view.access != Access::Read && view.access != Access::Write) ||
                !view.length_bytes || view.offset_bytes % sizeof(float) ||
                view.length_bytes % sizeof(float) ||
                view.offset_bytes > allocation.bytes ||
                view.length_bytes > allocation.bytes - view.offset_bytes)
                deny("GPU-G1-LEASE-010: invalid typed view bounds");
            if (view.length_bytes > limits_.referenced_bytes_per_submission - referenced)
                deny("GPU-G1-LEASE-011: referenced byte quota exceeded");
            referenced += view.length_bytes;
        }
        for (std::size_t i = 0; i < views.size(); ++i)
            for (std::size_t j = i + 1; j < views.size(); ++j)
                if (conflicts(views[i], views[j]))
                    deny("GPU-G1-LEASE-012: conflicting views in one submission");
        for (const auto& [ticket, inflight] : submissions_)
            for (const auto& owned : inflight.views)
                for (const auto& requested : views)
                    if (conflicts(owned, requested))
                        deny("GPU-G1-LEASE-013: conflicting inflight device loan");
        // No owner/state mutation until every check and map insertion succeeds.
        const Ticket ticket = next_ticket_;
        submissions_.emplace(ticket, Submission{key(queue), queue.generation_, views, false});
        ++next_ticket_;
        ++usage.inflight;
        return ticket;
    }
    void provider_request_cancel(const QueueHandle& queue, Ticket ticket) {
        std::lock_guard<std::mutex> guard(mutex_);
        require_queue(queue);
        require_submission(queue, ticket).cancel_requested = true;
        // Cancellation does NOT imply quiescence, loan release or event ack.
    }
    void provider_ack_event(const QueueHandle& queue, Ticket ticket, Event event) {
        std::lock_guard<std::mutex> guard(mutex_);
        require_queue(queue);
        const auto& submission = require_submission(queue, ticket);
        if (event != Event::Completed && event != Event::CancelledQuiescent)
            deny("GPU-G1-LEASE-014: invalid provider event status");
        if (event == Event::CancelledQuiescent && !submission.cancel_requested)
            deny("GPU-G1-LEASE-014: cancellation event without request");
        // Only the trusted provider may call this after actual hardware
        // quiescence. An untrusted guest must NEVER be able to mint this ack.
        auto& usage = usages_.at(queue.tenant_);
        --usage.inflight;
        submissions_.erase(ticket);
    }
    void provider_quarantine_device(std::uint64_t device) {
        if (!device) deny("GPU-G1-LEASE-001: zero device identity");
        std::lock_guard<std::mutex> guard(mutex_);
        // A physical device can serve many tenants; loss quarantines all of them.
        quarantined_devices_.insert(device);
        for (auto& [k, queue] : queues_)
            if (std::get<1>(k) == device && queue.active)
                queue.quarantined = true;
        for (auto& [k, allocation] : allocations_)
            if (std::get<1>(k) == device && allocation.active)
                allocation.quarantined = true;
        // Device loss never invents completion events or returns any loans.
    }
    Snapshot snapshot() const {
        std::lock_guard<std::mutex> guard(mutex_);
        Snapshot s{};
        for (const auto& [tenant, usage] : usages_) {
            if (usage.resident_bytes > max() - s.resident_bytes ||
                usage.allocations > max() - s.allocations)
                deny("GPU-G1-LEASE-016: aggregate statistics overflow");
            s.resident_bytes += usage.resident_bytes;
            s.allocations += usage.allocations;
        }
        s.inflight = submissions_.size();
        for (const auto& [ticket, submission] : submissions_)
            if (submission.cancel_requested) ++s.cancelled_pending;
        for (const auto& [key, queue] : queues_)
            if (queue.active && queue.quarantined) ++s.quarantined_queues;
        return s;
    }

private:
    using Key = std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>;
    struct QueueState { std::uint64_t generation = 0; bool active = false; bool quarantined = false; };
    struct AllocationState {
        std::uint64_t generation = 0;
        std::uint64_t bytes = 0;
        bool active = false;
        bool quarantined = false;
    };
    struct Usage { std::uint64_t resident_bytes = 0; std::uint64_t allocations = 0; std::uint64_t inflight = 0; };
    struct Submission {
        Key queue;
        std::uint64_t queue_generation;
        std::vector<View> views;
        bool cancel_requested;
    };
    static constexpr std::uint64_t max() noexcept { return std::numeric_limits<std::uint64_t>::max(); }
    static std::uint64_t next_identity() {
        static std::atomic<std::uint64_t> counter{0};
        auto previous = counter.load(std::memory_order_relaxed);
        for (;;) {
            if (previous == max()) deny("GPU-G1-LEASE-001: registry identity exhausted");
            if (counter.compare_exchange_weak(previous, previous + 1,
                                              std::memory_order_relaxed))
                return previous + 1;
        }
    }
    [[noreturn]] static void deny(const char* reason) { throw std::invalid_argument(reason); }
    static Key key(const QueueHandle& h) { return {h.tenant_, h.device_, h.id_}; }
    static Key key(const AllocationHandle& h) { return {h.tenant_, h.device_, h.id_}; }
    QueueState& require_queue(const QueueHandle& h) {
        auto it = queues_.find(key(h));
        if (h.registry_ != identity_ || it == queues_.end() || !it->second.active ||
            it->second.generation != h.generation_ || it->second.quarantined)
            deny("GPU-G1-LEASE-002: stale or quarantined queue handle");
        return it->second;
    }
    AllocationState& require_allocation(const AllocationHandle& h) {
        auto it = allocations_.find(key(h));
        if (h.registry_ != identity_ || it == allocations_.end() || !it->second.active ||
            it->second.generation != h.generation_ || it->second.quarantined)
            deny("GPU-G1-LEASE-005: stale or quarantined allocation handle");
        return it->second;
    }
    Submission& require_submission(const QueueHandle& h, Ticket ticket) {
        auto it = submissions_.find(ticket);
        if (!ticket || it == submissions_.end() || it->second.queue != key(h) ||
            it->second.queue_generation != h.generation_)
            deny("GPU-G1-LEASE-015: stale or foreign event ticket");
        return it->second;
    }
    static bool conflicts(const View& a, const View& b) {
        if (key(a.allocation) != key(b.allocation) ||
            a.allocation.generation_ != b.allocation.generation_ ||
            (a.access == Access::Read && b.access == Access::Read)) return false;
        const auto aend = a.offset_bytes + a.length_bytes;
        const auto bend = b.offset_bytes + b.length_bytes;
        return a.offset_bytes < bend && b.offset_bytes < aend;
    }
    Limits limits_;
    const std::uint64_t identity_;
    mutable std::mutex mutex_;
    std::set<std::uint64_t> quarantined_devices_;
    std::map<Key, QueueState> queues_;
    std::map<Key, AllocationState> allocations_;
    std::map<std::uint64_t, Usage> usages_;
    std::map<Ticket, Submission> submissions_;
    Ticket next_ticket_ = 1;
};

} // namespace oreslang::gpu::g1
