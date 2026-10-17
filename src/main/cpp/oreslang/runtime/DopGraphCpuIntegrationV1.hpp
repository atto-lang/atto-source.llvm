#pragma once

#include "oreslang/runtime/DopKernelIR.hpp"
#include "oreslang/runtime/ExecutionGraphV1.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <condition_variable>
#include <thread>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace oreslang::dop {

/**
 * Host-only composition of one owned f32 CPU region and an independent
 * CPU-only i64 execution graph. No source AST lowering, device launch,
 * implicit GPU fallback, actor scheduling, or f32->i64 data transfer.
 *
 * Callers must supply a bounded, trusted worker executor. Strong shared
 * owners deliberately keep both native allocations alive across dispatch:
 * capturing C++ HostBuffer references would allow use-after-free when the
 * caller releases an owner before a queued task is claimed.
 */
class OwnedCpuGraphFuture {
    struct State {
        // The scheduler may start a worker before schedule(...) returns, then
        // throw. Neither a claimed worker nor a retained closure may touch
        // native buffers until external task admission has committed.
        struct Admission {
            std::mutex mutex;
            std::condition_variable cv;
            bool resolved = false;
            bool accepted = false;
            void resolve(bool ok) noexcept {
                {
                    std::lock_guard<std::mutex> guard(mutex);
                    resolved = true;
                    accepted = ok;
                }
                cv.notify_all();
            }
        };
        mutable std::mutex mutex;
        std::atomic<bool> cancelled{false};
        std::atomic<bool> started{false};
        bool settled = false;
        std::promise<std::int64_t> promise;
        std::shared_future<std::int64_t> result = promise.get_future().share();

        bool claim() noexcept {
            return !started.exchange(true, std::memory_order_acq_rel) &&
                !cancelled.load(std::memory_order_acquire);
        }
        bool cancel() {
            std::lock_guard<std::mutex> guard(mutex);
            if (settled) return false;
            cancelled.store(true, std::memory_order_release);
            settled = true;
            promise.set_exception(std::make_exception_ptr(
                execution_v1::Cancellation()));
            return true;
        }
        void fail(std::exception_ptr reason) {
            std::lock_guard<std::mutex> guard(mutex);
            if (settled) return;
            settled = true;
            promise.set_exception(std::move(reason));
        }
        void complete(std::int64_t value) {
            std::lock_guard<std::mutex> guard(mutex);
            if (settled) return;
            settled = true;
            promise.set_value(value);
        }
        void reject(std::exception_ptr reason) {
            // A nonconforming executor might queue a closure and then throw.
            // Invalidate the delayed closure even if the scheduler rejected.
            cancelled.store(true, std::memory_order_release);
            fail(std::move(reason));
        }
    };
    std::shared_ptr<State> state_;
    explicit OwnedCpuGraphFuture(std::shared_ptr<State> state)
        : state_(std::move(state)) {}
public:
    bool cancel() { return state_->cancel(); }
    bool ready() const {
        return state_->result.wait_for(std::chrono::seconds(0)) ==
            std::future_status::ready;
    }
    std::int64_t get() const { return state_->result.get(); }

    friend OwnedCpuGraphFuture submit_owned_cpu_graph(
        const Kernel&, std::shared_ptr<HostBuffer>,
        std::shared_ptr<HostBuffer>, RegionAccess, RegionAccess,
        execution_v1::Plan,
        const std::function<void(std::function<void()>)>&);
};

/**
 * Only a host worker can invoke this API; no native actor carrier is
 * implicitly selected. Preflight is structural and NOT an execution loan:
 * execute_cpu_region rechecks ownership and holds both physical loans.
 * Cancellation before the worker claims the closure prevents all writes.
 * After the worker claims it, cancellation cannot roll back kernel writes.
 */
inline OwnedCpuGraphFuture submit_owned_cpu_graph(
        const Kernel& kernel,
        std::shared_ptr<HostBuffer> input,
        std::shared_ptr<HostBuffer> output,
        RegionAccess source,
        RegionAccess destination,
        execution_v1::Plan graph,
        const std::function<void(std::function<void()>)>& schedule) {
    auto state = std::make_shared<OwnedCpuGraphFuture::State>();
    auto admission = std::make_shared<OwnedCpuGraphFuture::State::Admission>();
    const auto submitting_thread = std::this_thread::get_id();
    auto inline_attempted = std::make_shared<std::atomic<bool>>(false);
    OwnedCpuGraphFuture future(state);
    try {
        if (!input || !output) throw std::invalid_argument("missing owned CPU buffer");
        if (!graph.cpu_only() ||
                graph.result_type() != execution_v1::Kind::HostI64) {
            throw std::invalid_argument(
                "GPU graph/transfer or non-i64 result not supported; no CPU fallback");
        }
        if (!schedule) throw std::invalid_argument("missing trusted CPU executor");
        if (input->identical_storage(*output) ||
                !matching_layout(kernel.layout, input->layout()) ||
                !matching_layout(kernel.layout, output->layout()) ||
                source.mode != RegionMode::Read ||
                destination.mode != RegionMode::Write ||
                source.field != destination.field ||
                source.begin != destination.begin ||
                source.end != destination.end ||
                !source.root.same_root(input->region_root()) ||
                !destination.root.same_root(output->region_root())) {
            throw std::invalid_argument("invalid owned CPU graph region footprint");
        }
        require_live_owned(RegionTask("owned-graph-cpu",
            std::vector<RegionAccess>{source, destination}));

        // Capture owners by value: queued work must never retain raw pointers
        // or references to caller-owned C++ wrappers.
        schedule([state, admission, inline_attempted, submitting_thread, kernel, input = std::move(input),
                  output = std::move(output), source = std::move(source),
                  destination = std::move(destination),
                  graph = std::move(graph)]() mutable {
            if (!state->claim()) return;
            try {
                {
                    std::unique_lock<std::mutex> guard(admission->mutex);
                    if (!admission->resolved &&
                            std::this_thread::get_id() == submitting_thread) {
                        // Defer settlement until schedule returns. If the
                        // scheduler invokes inline and then throws, report
                        // its actual rejection, not a misleading inline error.
                        inline_attempted->store(true, std::memory_order_release);
                        return;
                    }
                    if (!admission->cv.wait_for(guard, std::chrono::seconds(30),
                            [&] { return admission->resolved; })) {
                        state->reject(std::make_exception_ptr(std::runtime_error(
                            "owned CPU graph executor admission timed out")));
                        return;
                    }
                    if (!admission->accepted) return;
                }
                if (state->cancelled.load(std::memory_order_acquire)) return;
                execute_cpu_region(kernel, *input, *output, source, destination);
                if (state->cancelled.load(std::memory_order_acquire)) return;
                const auto value = graph.execute_cpu();
                if (!state->cancelled.load(std::memory_order_acquire)) {
                    state->complete(std::get<std::int64_t>(value));
                }
            } catch (...) {
                state->fail(std::current_exception());
            }
        });
        // Inline callbacks are not asynchronous. Their first invocation
        // is a failed admission, even if schedule() returned normally.
        if (inline_attempted->load(std::memory_order_acquire)) {
            state->reject(std::make_exception_ptr(std::logic_error(
                "owned CPU graph requires queued executor, not inline dispatch")));
            admission->resolve(false);
        } else {
            admission->resolve(true);
        }
    } catch (...) {
        // Even when schedule started a worker and then threw, the pending
        // closure is released solely in rejected state, without CPU writes.
        state->reject(std::current_exception());
        admission->resolve(false);
    }
    return future;
}

} // namespace oreslang::dop
