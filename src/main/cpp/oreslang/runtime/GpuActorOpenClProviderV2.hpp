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

class OpenClActorRecoveryWorkerV1;

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
    friend class OpenClActorRecoveryWorkerV1;
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
        // Pre-allocate the local event capability BEFORE the first GPU API
        // call. No host allocation/issuer exception after native submission
        // may be mistaken for a safely aborted physical command.
        EventHandle candidate;
        try { candidate = issue_event(turn); }
        catch (...) { return {EnqueueState::DefinitelyNotSubmitted, nullptr}; }
        active_turn_ = turn;
        try {
            operation_ = provider_->begin(*prepared_, input_, output_, host_input_);
            active_ = std::move(candidate); // noexcept shared_ptr move
        } catch (...) {
            ambiguous_ = true;
            throw;
        }
        return {EnqueueState::Submitted, active_};
    }

    EnqueueResult reconcile_unknown(Turn turn) override {
        std::lock_guard<std::mutex> guard(mu_);
        if (!recovered_unknown_begin_ ||
                turn.actor_generation != active_turn_.actor_generation ||
                turn.sequence != active_turn_.sequence)
            return {EnqueueState::Unknown, nullptr};
        if (!active_) {
            // ISSUE BEFORE returning the native recovery receipt. This
            // represents an authenticated failed/quiescent actor command,
            // NEVER a successful device kernel or an unsubmitted upload.
            active_ = issue_event(turn);
            event_terminal_ = true;
            event_success_ = false;
        }
        return {EnqueueState::Submitted, active_};
    }

    // Explicit privileged RECOVERY WORKER entrypoints. These may call
    // clFinish and must never be called from the scheduler polling path.
    // Failure to establish physical queue quiescence cannot free buffers.
private:
    // These recovery entrypoints run ONLY on the supervised worker thread.
    // The native call can block indefinitely in clFinish: never hold mu_
    // while invoking it. Actor scheduler polling must remain responsive.
    bool recover_unknown_begin_on_worker() {
        {
            std::lock_guard<std::mutex> guard(mu_);
            if (!ambiguous_ || operation_ || closed_ || recovery_in_progress_)
                return false;
            recovery_in_progress_ = true;
        }
        bool proven = false;
        try { proven = provider_->recover_ambiguous_begin_after_quiescence(); }
        catch (...) { /* failed proof preserves quarantine */ }
        std::lock_guard<std::mutex> guard(mu_);
        recovery_in_progress_ = false;
        if (proven) {
            ambiguous_ = false;
            recovered_unknown_begin_ = true;
        }
        return proven;
    }

    bool recover_unknown_kernel_on_worker() {
        std::shared_ptr<opencl::Provider::Operation> operation;
        {
            std::lock_guard<std::mutex> guard(mu_);
            if (!operation_ || closed_ || event_terminal_ || recovery_in_progress_
                    || operation_->state() != opencl::Provider::OperationState::UnknownEnqueue)
                return false;
            operation = operation_;
            recovery_in_progress_ = true;
        }
        bool proven = false;
        try { proven = provider_->recover_ambiguous_kernel_after_quiescence(*operation); }
        catch (...) { /* no speculative state commit or resource release */ }
        std::lock_guard<std::mutex> guard(mu_);
        recovery_in_progress_ = false;
        if (proven && operation_ == operation) {
            event_terminal_ = true;
            event_success_ = false;
        }
        return proven;
    }

    bool recover_device_loss_on_worker() {
        std::shared_ptr<opencl::Provider::Operation> operation;
        {
            std::lock_guard<std::mutex> guard(mu_);
            if (!operation_ || closed_ || event_terminal_ || recovery_in_progress_
                    || operation_->state() != opencl::Provider::OperationState::DeviceLost)
                return false;
            operation = operation_;
            recovery_in_progress_ = true;
        }
        bool proven = false;
        try {
            provider_->recover_device_loss(*operation);
            proven = provider_->usage() == opencl::Usage{0,0,0,0};
        } catch (...) { /* failed or ambiguous native recovery remains quarantined */ }
        std::lock_guard<std::mutex> guard(mu_);
        recovery_in_progress_ = false;
        if (proven && operation_ == operation) {
            event_terminal_ = true;
            event_success_ = false;
        }
        return proven;
    }

    bool recover_release_failure_on_worker() {
        std::shared_ptr<opencl::Provider::Operation> operation;
        {
            std::lock_guard<std::mutex> guard(mu_);
            if (!operation_ || closed_ || event_terminal_ || recovery_in_progress_
                    || operation_->state() != opencl::Provider::OperationState::ReleaseFailed)
                return false;
            operation = operation_;
            recovery_in_progress_ = true;
        }
        bool proven = false;
        try {
            provider_->retry_release(*operation);
            proven = provider_->usage() == opencl::Usage{0,0,0,0};
        } catch (...) {}
        std::lock_guard<std::mutex> guard(mu_);
        recovery_in_progress_ = false;
        if (proven && operation_ == operation) {
            event_terminal_ = true;
            event_success_ = false; // recovery never asserts GPU success
        }
        return proven;
    }

    bool matches_recovery_turn(Turn turn) const {
        std::lock_guard<std::mutex> guard(mu_);
        return turn.actor_generation == active_turn_.actor_generation
            && turn.sequence == active_turn_.sequence
            && turn.sequence != 0 && !closed_;
    }

public:
    Observation observe(const EventHandle& event) override {
        std::lock_guard<std::mutex> guard(mu_);
        if (closed_ || !active_ || event != active_ ||
                !authentic_local_event(event, active_turn_))
            return Observation::Unknown;
        // A verified failed upload may be reconciled via clFinish without
        // ever producing an Operation handle. Its locally minted terminal
        // failure receipt is valid *only after* physical queue quiescence.
        if (event_terminal_)
            return event_success_ ? Observation::CompleteOk : Observation::CompleteFailed;
        if (recovery_in_progress_) return Observation::Unknown;
        if (!operation_) return Observation::Unknown;

        using State = opencl::Provider::OperationState;
        State state = operation_->state();
        try {
            if (state == State::DeviceLost || state == State::UnknownEnqueue)
                return Observation::DeviceLost;
            if (state == State::ReleaseFailed)
                return Observation::Unknown;
            state = provider_->poll(*operation_);
        } catch (...) {
            // Native provider error may be after a physical enqueue and
            // MUST NOT be converted into a fake completed event.
            return Observation::Unknown;
        }

        if (state == State::DeviceLost ||
                state == State::UnknownEnqueue)
            return Observation::DeviceLost; // uncertainty is NOT quiescence
        if (state == State::ReleaseFailed)
            return Observation::Unknown;
        if (state == State::Succeeded) {
            try {
                verified_output_bytes_ = operation_->output_bytes();
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
        if (ambiguous_ || recovery_in_progress_) return false;
        if (operation_ && !event_terminal_) return false;
        if (!(provider_->usage() == opencl::Usage{0,0,0,0})) return false;
        try {
            prepared_->close(); // physically releases program/kernel
        } catch (...) {
            return false;
        }
        operation_.reset();
        active_.reset(); // retired local token cannot be replayed
        closed_ = true;
        return true;
    }

    std::vector<unsigned char> verified_output() const {
        std::lock_guard<std::mutex> guard(mu_);
        if (!event_terminal_ || !event_success_)
            throw std::logic_error("GPU_V2_OUTPUT_NOT_VERIFIED");
        return verified_output_bytes_;
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
    std::vector<unsigned char> verified_output_bytes_;
    bool ambiguous_ = false;
    bool recovered_unknown_begin_ = false;
    bool recovery_in_progress_ = false;
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
