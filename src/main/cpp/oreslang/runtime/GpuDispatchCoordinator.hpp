#pragma once

#include "oreslang/runtime/DopKernelIR.hpp"
#include "oreslang/runtime/GpuBufferFence.hpp"

#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace oreslang::gpu {

/**
 * Host-owned bounded async GPU queue. No OpenCL device driver is included.
 * Only an authenticated trusted provider can turn these tickets into actual
 * device work; caller identities are descriptive rather than credentials.
 *
 * Completion callbacks are host control events, NEVER actor user-code calls.
 * Actor continuations must be re-admitted to their scheduler-owned lane.
 */
class GpuDispatchCoordinator {
public:
    using Completion = std::function<void(std::exception_ptr)>;
    struct Provider {
        virtual ~Provider() = default;
        // Notify completion only after the real device event, not queueing.
        virtual void launch(const dop::OpenClArtifact& artifact,
                            const Identity& identity, Ticket ticket,
                            Completion finished) = 0;
        // Notify completion only after real physical device storage release.
        virtual void release(const Identity& identity, std::uint64_t bytes,
                             Completion finished) = 0;
    };
    struct Limits {
        std::size_t max_frames;
        std::uint64_t max_bytes;
    };

private:
    struct Core {
        std::shared_ptr<Provider> provider;
        Limits limits;
        std::mutex credit_lock;
        std::size_t used_frames = 0;
        std::uint64_t used_bytes = 0;
        bool closed = false;
        Core(std::shared_ptr<Provider> p, Limits l)
            : provider(std::move(p)), limits(l) {
            if (!provider || limits.max_frames == 0 || limits.max_bytes == 0)
                throw std::invalid_argument("GPU provider and positive quota required");
        }
    };
    struct State {
        std::shared_ptr<Core> core;
        std::shared_ptr<BufferFence> fence;
        Ticket ticket;
        Identity identity;
        std::uint64_t reserved_bytes;
        std::mutex lock;
        bool cancelled = false;
        bool event_observed = false;
        bool release_observed = false;
        bool observer_registered = false;
        bool settled = false;
        std::exception_ptr terminal_error;
        Completion host_observer;
        std::promise<void> ready_promise, done_promise;
        std::shared_future<void> ready, done;

        State(std::shared_ptr<Core> core_arg, std::shared_ptr<BufferFence> fence_arg,
              Ticket t, Identity id, std::uint64_t bytes)
            : core(std::move(core_arg)), fence(std::move(fence_arg)), ticket(t),
              identity(std::move(id)), reserved_bytes(bytes),
              ready(ready_promise.get_future().share()),
              done(done_promise.get_future().share()) {}
        void settle(std::exception_ptr error = {}) noexcept {
            Completion notify;
            {
                std::lock_guard<std::mutex> guard(lock);
                if (settled) return;
                settled = true;
                terminal_error = error;
                notify = std::move(host_observer);
            }
            try {
                if (error) done_promise.set_exception(error);
                else done_promise.set_value();
            } catch (...) { }
            // Host callback must enqueue, NEVER call actor user code.
            if (notify) {
                try { notify(error); } catch (...) { /* supervisor sees timed-out routing */ }
            }
        }
    };
public:
    class Submission {
    public:
        std::shared_future<void> get_ready() const { return state_->ready; }
        std::shared_future<void> get_done() const { return state_->done; }
        /**
         * Register a single nonblocking host CONTROL-notification hook.
         * Invoked exactly once after provider release (or on an uncertain
         * launch/release failure), never under internal locks. Caller MUST
         * post an actor CONTROL event, not execute actor user code here.
         */
        void on_done(Completion notify) {
            if (!notify) throw std::invalid_argument("host notification required");
            bool already_settled;
            std::exception_ptr terminal;
            {
                std::lock_guard<std::mutex> guard(state_->lock);
                if (state_->observer_registered)
                    throw std::logic_error("GPU host completion already registered");
                state_->observer_registered = true;
                already_settled = state_->settled;
                terminal = state_->terminal_error;
                if (!already_settled) state_->host_observer = std::move(notify);
            }
            if (already_settled) notify(terminal);
        }
        bool cancel() {
            std::lock_guard<std::mutex> guard(state_->lock);
            if (state_->event_observed || state_->cancelled) return false;
            state_->fence->cancel_pending(state_->ticket);
            state_->cancelled = true;
            return true;
        }
    private:
        friend class GpuDispatchCoordinator;
        explicit Submission(std::shared_ptr<State> state)
            : state_(std::move(state)) {}
        std::shared_ptr<State> state_;
    };

    explicit GpuDispatchCoordinator(std::shared_ptr<Provider> provider, Limits limits)
        : core_(std::make_shared<Core>(std::move(provider), limits)) {}

    Submission submit(const dop::OpenClArtifact& artifact, Identity identity) {
        if (artifact.required_bytes == 0 ||
            artifact.required_bytes > std::numeric_limits<std::uint64_t>::max() / 2)
            throw std::invalid_argument("invalid or overflowing GPU byte reservation");
        const auto reserved = artifact.required_bytes * 2;
        auto core = core_;
        {
            std::lock_guard<std::mutex> guard(core->credit_lock);
            if (core->closed) throw std::logic_error("GPU coordinator closed");
            if (core->used_frames >= core->limits.max_frames
                || reserved > core->limits.max_bytes - core->used_bytes)
                throw std::overflow_error("GPU byte/frame admission exhausted");
            ++core->used_frames;
            core->used_bytes += reserved;
        }
        auto fence = std::make_shared<BufferFence>(identity, reserved);
        const auto ticket = fence->begin_command(identity);
        auto state = std::make_shared<State>(core, fence, ticket, std::move(identity), reserved);
        try {
            core->provider->launch(artifact, state->identity, ticket,
                    [state](std::exception_ptr error) { after_device_event(state, error); });
            state->ready_promise.set_value();
        } catch (...) {
            // Launch acceptance uncertainty is a leak-safe terminal error.
            // Never return credits without a real device completion event.
            const auto failure = std::current_exception();
            try { state->ready_promise.set_exception(failure); } catch (...) { }
            state->settle(failure);
        }
        return Submission(std::move(state));
    }

    std::size_t in_flight_frames() const {
        std::lock_guard<std::mutex> lock(core_->credit_lock);
        return core_->used_frames;
    }
    std::uint64_t reserved_bytes() const {
        std::lock_guard<std::mutex> lock(core_->credit_lock);
        return core_->used_bytes;
    }
    void close() {
        std::lock_guard<std::mutex> lock(core_->credit_lock);
        core_->closed = true;
        if (core_->used_frames != 0)
            throw std::logic_error("GPU device reservations not quiescent");
    }

private:
    static void after_device_event(std::shared_ptr<State> state, std::exception_ptr error) noexcept {
        bool cancelled = false;
        std::exception_ptr validation_error;
        {
            std::lock_guard<std::mutex> lock(state->lock);
            if (state->event_observed) return;
            state->event_observed = true;
            cancelled = state->cancelled;
            try {
                if (cancelled) {
                    state->fence->acknowledge_cancelled_device_event(
                            state->ticket, state->identity);
                } else {
                    state->fence->complete(state->ticket, !error);
                }
            } catch (...) {
                validation_error = std::current_exception();
            }
        }
        if (validation_error) {
            state->settle(validation_error);
            return; // No physical free or refund on identity uncertainty.
        }
        try {
            state->core->provider->release(state->identity, state->reserved_bytes,
                    [state, cancelled, error](std::exception_ptr release_error) {
                {
                    std::lock_guard<std::mutex> guard(state->lock);
                    // A faulty/native driver might invoke a completion
                    // callback twice. Resource accounting is exactly once.
                    if (state->release_observed) return;
                    state->release_observed = true;
                }
                if (release_error) {
                    state->settle(release_error); // Credits intentionally retained.
                    return;
                }
                try {
                    state->fence->retire_after_backend_release(state->identity);
                    state->fence->close_after_retirement();
                    {
                        std::lock_guard<std::mutex> lock(state->core->credit_lock);
                        --state->core->used_frames;
                        state->core->used_bytes -= state->reserved_bytes;
                    }
                    if (cancelled)
                        state->settle(std::make_exception_ptr(
                            std::runtime_error("GPU cancelled after event and physical release")));
                    else if (error) state->settle(error);
                    else state->settle();
                } catch (...) {
                    state->settle(std::current_exception());
                }
            });
        } catch (...) {
            state->settle(std::current_exception()); // Keep reservations.
        }
    }

    std::shared_ptr<Core> core_;
};

} // namespace oreslang::gpu
