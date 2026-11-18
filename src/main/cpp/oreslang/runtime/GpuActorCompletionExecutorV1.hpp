#pragma once

#include "oreslang/runtime/GpuActorProviderBridgeV1.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace oreslang::gpu_actor {

/*
 * Bounded, supervisor-owned GPU device-event completion executor.
 *
 * WHY NATIVE: device callbacks and OpenCL/CUDA/Vulkan event polling are
 * privileged native operations; a .atto actor must never receive raw GPU
 * queue/event capabilities or execute provider polling on its CPU carrier.
 * This executor does NOT launch kernels, check physical device provenance,
 * implement a source Future<T>, or grant permissions to untrusted actors.
 *
 * Each instance is confined to one trusted provider tenant/device/queue
 * generation cohort. The VM may create one executor per authenticated queue
 * and assign its host worker to a CPU set separate from actor/CONTROL pools.
 * Its single worker has no per-GPU-task thread creation, and each request
 * polls exactly one host lease instead of scanning all actors in a registry.
 * The OpenCL Api exposes poll_event(), not a completion callback. In its
 * opt-in timed-poll mode, this worker rechecks ONLY accepted, still-submitted
 * events after a bounded delay. Unknown enqueue, device loss, STOP recovery
 * and buffer release failures never infer quiescence or auto-retry physical
 * recovery. Explicit notifications can expedite the next scheduled poll.
 * Default event-driven mode retains the existing one-pass-per-request API.
 *
 * A request that arrives while the same lease is being polled is coalesced
 * into ONE additional pass, so no terminal event notification is lost.
 * The retained shared_ptr keeps the supervisor-owned registry alive until
 * that pass finishes. Blocking/recovery driver operations belong to the
 * separate, privileged OpenClActorRecoveryWorkerV1 and eventually a process.
 *
 * Destructor drains outstanding passes and joins: a wedged native driver may
 * still hang shutdown. This class is NOT a process-isolation boundary and
 * requires the VM to fence concurrent request() calls before destruction.
 */
class GpuActorCompletionExecutorV1 final {
public:
    using Registry = GpuActorQuarantineRegistryV1;
    using Binding = ProviderPortV1::Binding;

    struct Cohort {
        std::string tenant;
        std::string device;
        std::string queue;
        std::uint64_t device_generation;
        std::uint64_t queue_generation;

        explicit Cohort(const Binding& binding)
            : tenant(binding.tenant), device(binding.device),
              queue(binding.queue),
              device_generation(binding.device_generation),
              queue_generation(binding.queue_generation) {
            if (tenant.empty() || device.empty() || queue.empty()
                    || device_generation == 0 || queue_generation == 0)
                throw std::invalid_argument("GPU_COMPLETION_INVALID_COHORT");
        }

        bool matches(const Binding& binding) const noexcept {
            return tenant == binding.tenant
                && device == binding.device
                && queue == binding.queue
                && device_generation == binding.device_generation
                && queue_generation == binding.queue_generation;
        }
    };

    struct Stats {
        std::uint64_t accepted;
        std::uint64_t rearmed;
        std::uint64_t processed;
        std::uint64_t failed;
        std::uint64_t rejected;
        std::uint64_t timed_repolls;
        std::size_t queued;
        std::size_t retained;
        bool active;
    };

    explicit GpuActorCompletionExecutorV1(
            const Binding& binding, std::size_t capacity,
            std::chrono::milliseconds poll_interval = std::chrono::milliseconds{0})
        : cohort_(binding), capacity_(capacity), poll_interval_(poll_interval) {
        if (capacity == 0 || capacity > 4096)
            throw std::invalid_argument("GPU_COMPLETION_INVALID_CAPACITY");
        // Zero means purely request-driven. A positive interval is host
        // OpenCL polling (not a timer per GPU task or a source-level await).
        if (poll_interval_.count() < 0 || poll_interval_.count() > 30000)
            throw std::invalid_argument("GPU_COMPLETION_INVALID_POLL_INTERVAL");
        worker_ = std::thread([this] { run(); });
    }
    GpuActorCompletionExecutorV1(const GpuActorCompletionExecutorV1&) = delete;
    GpuActorCompletionExecutorV1& operator=(const GpuActorCompletionExecutorV1&) = delete;

    ~GpuActorCompletionExecutorV1() {
        {
            std::lock_guard<std::mutex> guard(mu_);
            stopping_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    // Only trusted native host code can supply a registry and its lease ID.
    // This checks the provider binding BEFORE reserving a queue slot.
    // A false return means it was already queued or its single rerun was
    // already reserved; false does NOT mean a device event was completed.
    bool request(const std::shared_ptr<Registry>& registry,
                 std::uint64_t lease_id) {
        if (!registry || lease_id == 0)
            throw std::invalid_argument("GPU_COMPLETION_INVALID_LEASE");
        const auto binding = registry->completion_binding(lease_id);
        if (!cohort_.matches(binding))
            throw std::logic_error("GPU_COMPLETION_FOREIGN_QUEUE_COHORT");

        const Key key{registry.get(), lease_id};
        std::lock_guard<std::mutex> guard(mu_);
        if (stopping_)
            throw std::logic_error("GPU_COMPLETION_EXECUTOR_STOPPED");
        const auto found = by_key_.find(key);
        if (found != by_key_.end()) {
            // Already executing: one additional pass handles a notification
            // that races with provider.observe() returning Pending.
            if (found->second->running && !found->second->rerun) {
                found->second->rerun = true;
                bump(rearmed_);
                return true;
            }
            // Explicit host provider notification can wake an outstanding
            // delayed OpenCL poll early, without consuming another queue slot.
            if (!found->second->running
                    && found->second->due > Clock::now()) {
                found->second->due = Clock::now();
                bump(rearmed_);
                cv_.notify_one();
                return true;
            }
            return false;
        }
        // Capacity includes the active job: a blocked driver cannot turn
        // a bounded queue into unlimited retained GPU/tenant resources.
        if (by_key_.size() >= capacity_) {
            bump(rejected_);
            throw std::logic_error("GPU_COMPLETION_QUEUE_CAPACITY_EXHAUSTED");
        }
        queued_.push_back(Job{registry, lease_id, key, false, false,
                              Clock::now()});
        auto it = std::prev(queued_.end());
        try {
            by_key_.emplace(key, it);
        } catch (...) {
            queued_.pop_back();
            throw;
        }
        bump(accepted_);
        cv_.notify_one();
        return true;
    }

    Stats stats() const {
        std::lock_guard<std::mutex> guard(mu_);
        return {accepted_, rearmed_, processed_, failed_, rejected_,
                timed_repolls_, queued_.size(), by_key_.size(),
                !active_.empty()};
    }

    // TEST/HOST barrier: it is NOT a device event, GPU fence, cancellation
    // acknowledgement, or proof that device buffers are quiescent.
    void await_idle() {
        std::unique_lock<std::mutex> guard(mu_);
        idle_cv_.wait(guard, [this] {
            return queued_.empty() && active_.empty();
        });
    }

private:
    using Clock = std::chrono::steady_clock;

    struct Key {
        Registry* registry;
        std::uint64_t lease_id;
    };
    struct KeyLess {
        bool operator()(const Key& a, const Key& b) const noexcept {
            if (a.registry != b.registry)
                return std::less<Registry*>{}(a.registry, b.registry);
            return a.lease_id < b.lease_id;
        }
    };
    struct Job {
        std::shared_ptr<Registry> registry;
        std::uint64_t lease_id;
        Key key;
        bool rerun;
        bool running; // avoids comparing iterators from distinct lists
        Clock::time_point due;
    };

    static void bump(std::uint64_t& value) noexcept {
        if (value != std::numeric_limits<std::uint64_t>::max()) ++value;
    }

    void run() noexcept {
        for (;;) {
            std::shared_ptr<Registry> registry;
            std::uint64_t lease = 0;
            {
                std::unique_lock<std::mutex> guard(mu_);
                for (;;) {
                    if (stopping_) {
                        // Cancel *poll subscriptions*, not GPU commands.
                        // The supervisor-rooted registry still owns any
                        // physically outstanding device loan and WAKE escrow.
                        queued_.clear();
                        by_key_.clear();
                        idle_cv_.notify_all();
                        return;
                    }
                    if (queued_.empty()) {
                        cv_.wait(guard, [this] {
                            return stopping_ || !queued_.empty();
                        });
                        continue;
                    }
                    const auto next = std::min_element(
                        queued_.begin(), queued_.end(),
                        [](const Job& a, const Job& b) {
                            return a.due < b.due;
                        });
                    const auto now = Clock::now();
                    if (next->due > now) {
                        // This wait is confined to the GPU service worker,
                        // never an actor carrier or the supervisor CONTROL
                        // lane. Any new notification wakes and reorders it.
                        cv_.wait_until(guard, next->due);
                        continue;
                    }
                    active_.splice(active_.end(), queued_, next);
                    active_.front().running = true;
                    registry = active_.front().registry;
                    lease = active_.front().lease_id;
                    break;
                }
            }

            bool success = true;
            bool needs_poll = false;
            try {
                needs_poll = registry->progress_one(lease);
            } catch (...) {
                // Exception is not a successful provider event or proof
                // of physical quiescence. Quarantine stays supervisor-owned.
                success = false;
            }
            {
                std::lock_guard<std::mutex> guard(mu_);
                if (success) bump(processed_);
                else bump(failed_);
                if (!stopping_ && (active_.front().rerun
                        || (needs_poll && poll_interval_.count() != 0))) {
                    const bool explicit_notification = active_.front().rerun;
                    active_.front().rerun = false;
                    active_.front().running = false;
                    active_.front().due = Clock::now() +
                        (explicit_notification ? std::chrono::milliseconds{0}
                                               : poll_interval_);
                    if (!explicit_notification) bump(timed_repolls_);
                    queued_.splice(queued_.end(), active_, active_.begin());
                    cv_.notify_one();
                } else {
                    by_key_.erase(active_.front().key);
                    active_.pop_front();
                }
                if (queued_.empty() && active_.empty())
                    idle_cv_.notify_all();
            }
        }
    }

    const Cohort cohort_;
    const std::size_t capacity_;
    const std::chrono::milliseconds poll_interval_;
    mutable std::mutex mu_;
    std::condition_variable cv_, idle_cv_;
    std::list<Job> queued_, active_;
    std::map<Key, std::list<Job>::iterator, KeyLess> by_key_;
    std::thread worker_;
    bool stopping_ = false;
    std::uint64_t accepted_ = 0, rearmed_ = 0, processed_ = 0;
    std::uint64_t failed_ = 0, rejected_ = 0, timed_repolls_ = 0;
};

} // namespace oreslang::gpu_actor
