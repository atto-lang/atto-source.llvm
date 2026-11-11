#pragma once

#include "oreslang/runtime/GpuActorProviderBridgeV1.hpp"
#include "oreslang/runtime/OpenClProvider.hpp"
#include "oreslang/runtime/OpenClSystemApi.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::gpu_actor {

/**
 * V2 physical GPU adapter for the already-existing OpenCL provider pipeline.
 *
 * The production choice is opencl::SystemApi, which authenticates GPU-only
 * OpenCL selection, manages native queue/event objects and forbids CPU fallback.
 * An injected opencl::Api is useful for deterministic host-only tests, but
 * those tests MUST NEVER be represented as physical GPU evidence.
 *
 * This adapter owns ONE immutable typed DOP f32 kernel and its corresponding
 * input/output device-binding ledgers, serving ONE actor incarnation.
 * The native OpenCL provider performs upload -> kernel -> download, using
 * cl_event and physically retiring buffers BEFORE reporting success.
 *
 * IMPORTANT: an exception from opencl::Provider::begin() may happen after
 * an OpenCL enqueue; without its receipt it cannot safely be deemed aborted.
 * reconcile_unknown intentionally fails closed in that case: root quarantine
 * retains resources pending a future provider-level recovery mechanism.
 * This is a draft, not a production-ready actor GPU dispatch path.
 */
class OpenClActorProviderPortV2 final : public ProviderPortV1 {
public:
    using Base = ProviderPortV1;
    using Turn = Base::Turn;
    using EnqueueResult = Base::EnqueueResult;
    using EnqueueState = Base::EnqueueState;
    using Observation = Base::Observation;

    OpenClActorProviderPortV2(
            Binding binding,
            std::shared_ptr<opencl::Api> api,
            opencl::ProviderIdentity provider_identity,
            opencl::Limits limits,
            dop::GpuTarget target,
            dop::Kernel kernel,
            std::vector<unsigned char> host_input)
        : Base(std::move(binding)),
          api_(std::move(api)),
          kernel_(std::move(kernel)),
          input_fence_(make_identity(this->binding(), false),
                       kernel_.layout.byte_length()),
          output_fence_(make_identity(this->binding(), true),
                        kernel_.layout.byte_length()),
          input_(kernel_.layout, input_fence_),
          output_(kernel_.layout, output_fence_),
          host_input_(std::move(host_input)) {
        if (!api_) throw std::invalid_argument("GPU_V2_MISSING_TRUSTED_OPENCL_API");
        if (this->binding().tenant != provider_identity.tenant ||
                this->binding().device != provider_identity.device ||
                this->binding().queue != std::to_string(provider_identity.queue_id) ||
                this->binding().queue_generation != provider_identity.queue_generation ||
                this->binding().allocation_generation
                    == std::numeric_limits<std::uint64_t>::max()) {
            throw std::invalid_argument("GPU_V2_PROVIDER_AUTHORITY_MISMATCH");
        }
        if (kernel_.layout.byte_length() == 0 ||
                host_input_.size() != kernel_.layout.byte_length()) {
            throw std::invalid_argument("GPU_V2_INVALID_INPUT_LAYOUT");
        }
        provider_ = std::make_unique<opencl::Provider>(
            *api_, std::move(provider_identity), limits, target);
        prepared_.emplace(provider_->prepare(kernel_));
    }

    OpenClActorProviderPortV2(const OpenClActorProviderPortV2&) = delete;
    OpenClActorProviderPortV2& operator=(const OpenClActorProviderPortV2&) = delete;

    EnqueueResult enqueue(Turn turn) override {
        std::lock_guard<std::mutex> guard(mu_);
        if (closed_ || active_ || turn.actor_generation != binding().actor_generation
                || turn.sequence == 0 || ambiguous_) {
            throw std::logic_error("GPU_V2_ENQUEUE_NOT_ADMITTED");
        }
        // The real provider must own all physical allocations/queue claims.
        // Any exception is UNKNOWN, not proof of no enqueue.
        try {
            operation_ = provider_->begin(*prepared_, input_, output_, host_input_);
            active_turn_ = turn;
            active_ = issue_event(turn);
        } catch (...) {
            ambiguous_ = true;
            throw;
        }
        return {EnqueueState::Submitted, active_};
    }

    EnqueueResult reconcile_unknown(Turn) override {
        // No source-visible Boolean or timer can clear an unknown physical
        // enqueue. The existing OpenCL provider can retain a quarantined
        // operation internally, but does not expose an authenticated recovery
        // receipt for begin() exceptions. Require a future explicit adapter.
        return {EnqueueState::Unknown, nullptr};
    }

    Observation observe(const EventHandle& event) override {
        std::lock_guard<std::mutex> guard(mu_);
        if (!active_ || event != active_ ||
                !authentic_local_event(event, active_turn_) || !operation_)
            return Observation::Unknown;
        if (event_terminal_)
            return event_success_ ? Observation::CompleteOk : Observation::CompleteFailed;

        using State = opencl::Provider::OperationState;
        State state = operation_->state();
        try {
            if (state == State::DeviceLost) {
                try { provider_->recover_device_loss(*operation_); }
                catch (...) { return Observation::DeviceLost; }
                state = operation_->state();
            } else if (state == State::ReleaseFailed) {
                try { provider_->retry_release(*operation_); }
                catch (...) { return Observation::Unknown; }
                state = operation_->state();
            } else {
                state = provider_->poll(*operation_);
            }
        } catch (...) {
            // Native provider error may be after a physical enqueue and
            // MUST NOT be converted into a fake completed event.
            return Observation::Unknown;
        }

        if (state == State::DeviceLost)
            return Observation::DeviceLost;
        if (state == State::ReleaseFailed)
            return Observation::Unknown;
        if (state == State::Succeeded) {
            try {
                output_ = operation_->output_bytes();
            } catch (...) {
                return Observation::Unknown;
            }
            event_terminal_ = true;
            event_success_ = true;
            return Observation::CompleteOk;
        }
        if (state == State::Failed || state == State::Cancelled) {
            // opencl::Provider only reports these after event terminalization
            // and successful physical buffer cleanup.
            event_terminal_ = true;
            event_success_ = false;
            return Observation::CompleteFailed;
        }
        return Observation::Pending;
    }

    bool try_release_buffer() override {
        std::lock_guard<std::mutex> guard(mu_);
        if (closed_) return true; // idempotent only after native release
        if (ambiguous_) return false;
        if (operation_ && !event_terminal_) return false;
        if (!(provider_->usage() == opencl::Usage{0,0,0,0})) return false;
        try {
            prepared_->close(); // physically releases program/kernel
        } catch (...) {
            return false;
        }
        operation_.reset();
        closed_ = true;
        return true;
    }

    std::vector<unsigned char> verified_output() const {
        std::lock_guard<std::mutex> guard(mu_);
        if (!event_terminal_ || !event_success_)
            throw std::logic_error("GPU_V2_OUTPUT_NOT_VERIFIED");
        return output_;
    }

    bool native_opencl_success_observed() const {
        std::lock_guard<std::mutex> guard(mu_);
        return event_terminal_ && event_success_;
    }

private:
    static gpu::Identity make_identity(const Binding& b, bool output) {
        if (b.allocation_generation ==
                std::numeric_limits<std::uint64_t>::max())
            throw std::invalid_argument("GPU_V2_ALLOCATION_GENERATION_EXHAUSTED");
        return gpu::Identity{b.tenant, b.device,
                b.allocation_generation + (output ? 1ULL : 0ULL)};
    }

    std::shared_ptr<opencl::Api> api_;
    const dop::Kernel kernel_;
    gpu::BufferFence input_fence_;
    gpu::BufferFence output_fence_;
    dop::DeviceBuffer input_;
    dop::DeviceBuffer output_;
    std::vector<unsigned char> host_input_;
    std::unique_ptr<opencl::Provider> provider_;
    std::optional<opencl::Provider::PreparedKernel> prepared_;
    std::shared_ptr<opencl::Provider::Operation> operation_;
    mutable std::mutex mu_;
    Turn active_turn_{0,0};
    EventHandle active_;
    std::vector<unsigned char> output_;
    bool ambiguous_ = false;
    bool event_terminal_ = false;
    bool event_success_ = false;
    bool closed_ = false;
};

/**
 * Runtime-only hardware entry: absence of a selected actual OpenCL GPU is a
 * hard failure, never an implicit CPU substitute.
 */
inline std::shared_ptr<opencl::Api> require_physical_opencl_gpu() {
    auto gpu = opencl::SystemApi::try_create_gpu();
    if (!gpu)
        throw std::runtime_error("GPU_V2_PHYSICAL_OPENCL_GPU_REQUIRED_NO_CPU_FALLBACK");
    return std::shared_ptr<opencl::Api>(std::move(gpu));
}

} // namespace oreslang::gpu_actor
