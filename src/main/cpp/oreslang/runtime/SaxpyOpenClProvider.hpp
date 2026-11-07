#pragma once

#include "oreslang/frontend/AttoGpuSaxpyV1.hpp"
#include "oreslang/runtime/OpenClProvider.hpp"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::opencl {

// A trusted host interface; no device pointer or native event is guest-visible.
// The old two-buffer DOP Provider continues to use Api independently.
class SaxpyApi : public Api {
public:
    virtual Handle prepare_saxpy(
            const gpu_source_v1::Artifact& artifact) = 0;
    virtual Handle enqueue_saxpy_kernel(
            Handle program, Handle x, Handle y, Handle out,
            float alpha, std::uint64_t elements) = 0;
    virtual std::string authenticated_device_name() const = 0;
    virtual std::uint64_t authenticated_max_allocation() const = 0;
};

struct SaxpyProviderIdentity {
    std::string tenant;
    std::string device;
    std::uint64_t queue_id;
    std::uint64_t queue_generation;
};
struct SaxpyProviderLimits {
    std::uint64_t resident_bytes;
    std::uint64_t host_staging_bytes;
    std::uint64_t maximum_elements;
};
struct SaxpyProviderUsage {
    std::uint64_t resident_bytes = 0;
    std::uint64_t host_staging_bytes = 0;
    std::uint32_t queue_slots = 0;
};

// This intentionally serial, single-queue G4 provider guarantees that no
// second call may race mutable kernel arguments on the same OpenCL program.
// Multi-queue support requires independent programs/queues and fair quotas.
class SaxpyProvider final {
public:
    enum class State {
        UploadX, UploadY, Kernel, Download,
        Succeeded, Cancelled, Failed, Quarantined, ReleaseFailed
    };

    class Operation final {
    public:
        Operation(const Operation&) = delete;
        Operation& operator=(const Operation&) = delete;
        Operation(Operation&&) = delete;
        Operation& operator=(Operation&&) = delete;

        State state() const noexcept {
            return state_.load(std::memory_order_acquire);
        }
        bool cancellation_requested() const noexcept {
            return cancellation_.load(std::memory_order_acquire);
        }
        std::uint64_t allocation_generation() const noexcept {
            return generation_;
        }
        std::vector<float> result() const {
            if (state() != State::Succeeded)
                throw std::logic_error("SAXPY output unavailable before authenticated completion");
            return output_;
        }

    private:
        friend class SaxpyProvider;
        Operation(std::vector<float> x, std::vector<float> y,
                  std::uint64_t generation, float alpha)
            : x_(std::move(x)), y_(std::move(y)),
              output_(x_.size(), 0.0f),
              generation_(generation), alpha_(alpha) {}

        void set_state(State state) noexcept {
            state_.store(state, std::memory_order_release);
        }

        std::vector<float> x_, y_, output_;
        std::uint64_t generation_ = 0;
        float alpha_ = 0.0f;
        Api::Handle program_ = nullptr;
        Api::Handle x_device_ = nullptr;
        Api::Handle y_device_ = nullptr;
        Api::Handle output_device_ = nullptr;
        Api::Handle event_ = nullptr;
        std::atomic<State> state_{State::Quarantined};
        std::atomic<bool> cancellation_{false};
        bool attempted_enqueue_ = false;
        bool quiescent_ = false;
        State terminal_after_cleanup_ = State::Failed;
    };

    SaxpyProvider(SaxpyApi& api, SaxpyProviderIdentity identity,
                  SaxpyProviderLimits limits)
        : api_(api), identity_(std::move(identity)), limits_(limits) {
        if (identity_.tenant.empty() || identity_.device.empty()
                || identity_.queue_id == 0 || identity_.queue_generation == 0
                || identity_.device != api_.authenticated_device_name()
                || api_.authenticated_max_allocation() == 0
                || limits_.resident_bytes == 0
                || limits_.host_staging_bytes == 0
                || limits_.maximum_elements == 0)
            throw std::invalid_argument("SAXPY provider identity/capability incomplete");
    }

    // Never invoke provider destruction while an operation is active: device
    // handles must remain owned until a trusted terminal event or clFinish
    // recovery proves all three are quiescent. A destruction-time implicit
    // cancellation would be unsafe.
    bool closed_for_release() const {
        std::lock_guard<std::mutex> guard(lock_);
        return !active_;
    }

    SaxpyProviderUsage usage() const {
        std::lock_guard<std::mutex> guard(lock_);
        return usage_;
    }

    std::shared_ptr<Operation> begin(
            const gpu_source_v1::Artifact& artifact,
            const std::vector<float>& x, const std::vector<float>& y,
            float alpha) {
        require_artifact(artifact);
        if (!std::isfinite(alpha) || x.empty() || x.size() != y.size())
            throw std::invalid_argument("SAXPY requires finite coefficient and equal nonempty f32 inputs");
        for (float value : x)
            if (!std::isfinite(value))
                throw std::invalid_argument("SAXPY v1 rejects nonfinite input x");
        for (float value : y)
            if (!std::isfinite(value))
                throw std::invalid_argument("SAXPY v1 rejects nonfinite input y");

        const std::uint64_t count = x.size();
        if (count > limits_.maximum_elements
                || count > std::numeric_limits<std::uint64_t>::max() / sizeof(float)
                || count > std::numeric_limits<std::size_t>::max() / sizeof(float))
            throw std::invalid_argument("SAXPY element count exceeds admitted device/host limits");
        const std::uint64_t bytes = count * sizeof(float);
        if (bytes > api_.authenticated_max_allocation()
                || bytes > limits_.resident_bytes / 3
                || bytes > limits_.host_staging_bytes / 3)
            throw std::invalid_argument("SAXPY three-buffer allocation exceeds provider quotas");

        std::lock_guard<std::mutex> guard(lock_);
        if (active_)
            throw std::logic_error("SAXPY provider queue has an in-flight or quarantined operation");
        if (next_generation_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("SAXPY allocation generation exhausted");
        // Copy complete owned staging before touching the driver or reserving
        // quota. These vectors are kept stable through asynchronous OpenCL I/O.
        auto operation = std::shared_ptr<Operation>(new Operation(
                x, y, ++next_generation_, alpha));
        usage_ = {bytes * 3, bytes * 3, 1};
        active_ = operation;

        try {
            operation->program_ = api_.prepare_saxpy(artifact);
            if (!operation->program_)
                throw std::runtime_error("SAXPY driver returned null program");
            operation->x_device_ = api_.allocate(bytes);
            if (!operation->x_device_)
                throw std::runtime_error("SAXPY driver returned null x buffer");
            operation->y_device_ = api_.allocate(bytes);
            if (!operation->y_device_)
                throw std::runtime_error("SAXPY driver returned null y buffer");
            operation->output_device_ = api_.allocate(bytes);
            if (!operation->output_device_)
                throw std::runtime_error("SAXPY driver returned null output buffer");
            if (operation->x_device_ == operation->y_device_
                    || operation->x_device_ == operation->output_device_
                    || operation->y_device_ == operation->output_device_) {
                // Unexpected alias provenance cannot be blindly released
                // three times or treated as exclusive memory.
                operation->attempted_enqueue_ = true;
                throw std::runtime_error("SAXPY driver returned aliased physical buffers");
            }
            operation->attempted_enqueue_ = true;
            operation->event_ = api_.enqueue_upload(
                    operation->x_device_,
                    reinterpret_cast<const unsigned char*>(operation->x_.data()),
                    static_cast<std::size_t>(bytes));
            if (!operation->event_)
                throw std::runtime_error("SAXPY upload did not return a trusted event");
            operation->set_state(State::UploadX);
            return operation;
        } catch (...) {
            // If a driver threw during an enqueue attempt, it may have
            // submitted work without returning the cl_event. Never reclaim
            // those buffers without an explicit queue-quiescence proof.
            if (operation->attempted_enqueue_) {
                operation->set_state(State::Quarantined);
            } else {
                operation->quiescent_ = true;
                release_locked(*operation);
            }
            throw;
        }
    }

    void request_cancel(Operation& operation) {
        std::lock_guard<std::mutex> guard(lock_);
        require_active(operation);
        if (!is_terminal(operation.state()))
            operation.cancellation_.store(true, std::memory_order_release);
        // Cancellation is not device quiescence and never releases a buffer.
    }

    State poll(Operation& operation) {
        std::lock_guard<std::mutex> guard(lock_);
        require_active(operation);
        const auto state = operation.state();
        if (state == State::Quarantined || state == State::ReleaseFailed)
            return state;
        if (is_terminal(state))
            return state;
        if (!operation.event_) {
            operation.set_state(State::Quarantined);
            return State::Quarantined;
        }
        EventStatus event;
        try {
            event = api_.poll_event(operation.event_);
        } catch (...) {
            operation.set_state(State::Quarantined);
            return State::Quarantined;
        }
        if (event == EventStatus::Pending) return state;
        if (event == EventStatus::Failed || event == EventStatus::DeviceLost) {
            // Failed event queries do not prove physical quiescence. The
            // original event, program and all allocations remain pinned.
            operation.set_state(State::Quarantined);
            return State::Quarantined;
        }

        operation.quiescent_ = true; // exact native completion event
        try {
            api_.release_event(operation.event_);
            operation.event_ = nullptr;
        } catch (...) {
            operation.terminal_after_cleanup_ = State::Failed;
            operation.set_state(State::ReleaseFailed);
            return State::ReleaseFailed;
        }

        if (operation.cancellation_.load(std::memory_order_acquire)) {
            operation.terminal_after_cleanup_ = State::Cancelled;
            release_locked(operation);
            return operation.state();
        }

        const auto bytes = operation.x_.size() * sizeof(float);
        try {
            operation.quiescent_ = false;
            operation.attempted_enqueue_ = true;
            switch (state) {
            case State::UploadX:
                operation.event_ = api_.enqueue_upload(
                        operation.y_device_,
                        reinterpret_cast<const unsigned char*>(operation.y_.data()),
                        bytes);
                if (!operation.event_)
                    throw std::runtime_error("SAXPY y upload returned no event");
                operation.set_state(State::UploadY);
                break;
            case State::UploadY:
                operation.event_ = api_.enqueue_saxpy_kernel(
                        operation.program_, operation.x_device_,
                        operation.y_device_, operation.output_device_,
                        operation.alpha_, operation.x_.size());
                if (!operation.event_)
                    throw std::runtime_error("SAXPY kernel returned no event");
                operation.set_state(State::Kernel);
                break;
            case State::Kernel:
                operation.event_ = api_.enqueue_download(
                        operation.output_device_,
                        reinterpret_cast<unsigned char*>(operation.output_.data()),
                        bytes);
                if (!operation.event_)
                    throw std::runtime_error("SAXPY download returned no event");
                operation.set_state(State::Download);
                break;
            case State::Download:
                // Terminal output was already written to the stable staging
                // buffer before event status reached Complete.
                operation.quiescent_ = true;
                operation.terminal_after_cleanup_ = State::Succeeded;
                release_locked(operation);
                break;
            default:
                throw std::logic_error("unreachable SAXPY transition");
            }
        } catch (...) {
            operation.set_state(State::Quarantined);
            // Even if the throwing enqueue returned no event, clFinish is
            // required before safe release. Keep quota and all raw handles.
            return State::Quarantined;
        }
        return operation.state();
    }

    // Recovery is a *trusted* device-wide clFinish witness (or equivalent).
    // No guest, actor or arbitrary timeout may invoke it with a forged bool.
    void recover_quarantined() {
        std::lock_guard<std::mutex> guard(lock_);
        if (!active_ || active_->state() != State::Quarantined)
            throw std::logic_error("no quarantined SAXPY operation");
        if (!api_.prove_device_loss_quiescence())
            throw std::runtime_error("SAXPY queue not physically quiescent");
        active_->quiescent_ = true;
        active_->terminal_after_cleanup_ = active_->cancellation_requested()
            ? State::Cancelled : State::Failed;
        release_locked(*active_);
    }

    void retry_release() {
        std::lock_guard<std::mutex> guard(lock_);
        if (!active_ || active_->state() != State::ReleaseFailed
                || !active_->quiescent_)
            throw std::logic_error("SAXPY release retry requires quiescent retained operation");
        release_locked(*active_);
    }

private:
    static bool is_terminal(State state) noexcept {
        return state == State::Succeeded || state == State::Cancelled
            || state == State::Failed;
    }
    static const gpu_source_v1::Artifact& canonical_artifact() {
        static const auto artifact = gpu_source_v1::compile_saxpy(
            "compute fnc saxpy(x: DeviceRead<f32>, y: DeviceRead<f32>, "
            "out: DeviceWrite<f32>, alpha: f32, n: u64) device { "
            "const i = gpu.global_id.x; if i < n { "
            "out[i] = alpha * x[i] + y[i]; } }");
        return artifact;
    }
    static void require_artifact(const gpu_source_v1::Artifact& artifact) {
        const auto& canonical = canonical_artifact();
        if (artifact.kernel_name != canonical.kernel_name
                || artifact.device_source != canonical.device_source
                || artifact.abi_version != canonical.abi_version
                || artifact.required_address_bits != canonical.required_address_bits
                || artifact.requires_f32 != canonical.requires_f32)
            throw std::invalid_argument("untrusted or unsupported SAXPY device artifact");
    }
    void require_active(const Operation& operation) const {
        if (!active_ || active_.get() != &operation)
            throw std::logic_error("SAXPY operation foreign, stale or already released");
    }
    void release_locked(Operation& operation) {
        if (!operation.quiescent_)
            throw std::logic_error("SAXPY physical release requires device quiescence");
        try {
            if (operation.event_) {
                api_.release_event(operation.event_);
                operation.event_ = nullptr;
            }
            if (operation.x_device_) {
                const auto physical = operation.x_device_;
                api_.release_buffer(physical);
                operation.x_device_ = nullptr;
                // Even a faulty adapter reporting duplicate raw handles
                // cannot induce a second physical release.
                if (operation.y_device_ == physical) operation.y_device_ = nullptr;
                if (operation.output_device_ == physical) operation.output_device_ = nullptr;
            }
            if (operation.y_device_) {
                const auto physical = operation.y_device_;
                api_.release_buffer(physical);
                operation.y_device_ = nullptr;
                if (operation.output_device_ == physical) operation.output_device_ = nullptr;
            }
            if (operation.output_device_) {
                api_.release_buffer(operation.output_device_);
                operation.output_device_ = nullptr;
            }
            if (operation.program_) {
                api_.release_program(operation.program_);
                operation.program_ = nullptr;
            }
        } catch (...) {
            operation.set_state(State::ReleaseFailed);
            // Full quota remains pinned until all releases succeed.
            return;
        }
        operation.set_state(operation.terminal_after_cleanup_);
        usage_ = {};
        active_.reset();
    }

    SaxpyApi& api_;
    const SaxpyProviderIdentity identity_;
    const SaxpyProviderLimits limits_;
    mutable std::mutex lock_;
    std::shared_ptr<Operation> active_;
    SaxpyProviderUsage usage_;
    std::uint64_t next_generation_ = 0;
};

} // namespace oreslang::opencl
