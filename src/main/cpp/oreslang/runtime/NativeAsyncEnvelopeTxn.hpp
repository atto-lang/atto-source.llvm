#pragma once

#include "oreslang/runtime/NativeAsyncMailboxAdmission.hpp"
#include "oreslang/runtime/PriorityActorInbox.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>

namespace oreslang::runtime {

/**
 * Move-only ownership transaction for one native async actor send.
 *
 * The caller MUST acquire NativeAsyncMailboxAdmission::begin_preparation()
 * before expensive payload graph validation/freeze/copy. This transaction then
 * binds that bounded descriptor to:
 *   - one runtime-owned frozen payload snapshot,
 *   - one exact NativeActorRef physical resource loan,
 *   - one generation-fenced scheduler wake ticket,
 *   - and, after TRANSFER, one real PriorityActorInbox envelope.
 *
 * While WAITING/TRANSFERRED the sender no longer owns the frozen snapshot.
 * Successful envelope publication transfers the already-retained resource loan
 * to the inbox Delivery; physical ACK/terminal retirement releases it exactly
 * once. No second target loan is acquired at publication.
 *
 * This is host-runtime substrate, not source send_async lowering.
 */
template <typename T>
class NativeAsyncEnvelopeTxn final {
public:
    using Admission = NativeAsyncMailboxAdmission;
    using Handle = typename Admission::Handle;
    using Registration = typename Admission::Registration;
    using DescriptorState = typename Admission::DescriptorState;
    using Inbox = PriorityActorInbox<T>;
    using Lane = typename Inbox::Lane;

    enum class PublishResult {
        NotTransferred,
        Published,
        ProducerRetired,
        Failed
    };

    NativeAsyncEnvelopeTxn(
            Admission& admission,
            Handle handle,
            const NativeActorRef& target,
            Lane lane,
            std::size_t bytes)
        : admission_(&admission),
          handle_(handle),
          target_(target),
          lane_(lane),
          bytes_(bytes) {
        if (!handle_.generation || !bytes_)
            throw std::invalid_argument("valid async descriptor and byte charge required");
        if (admission_->descriptor_state(handle_) != DescriptorState::Preparing)
            throw std::logic_error("async envelope transaction requires PREPARING descriptor");
    }

    NativeAsyncEnvelopeTxn(const NativeAsyncEnvelopeTxn&) = delete;
    NativeAsyncEnvelopeTxn& operator=(const NativeAsyncEnvelopeTxn&) = delete;
    NativeAsyncEnvelopeTxn(NativeAsyncEnvelopeTxn&&) = delete;
    NativeAsyncEnvelopeTxn& operator=(NativeAsyncEnvelopeTxn&&) = delete;

    ~NativeAsyncEnvelopeTxn() noexcept {
        cleanup_noexcept();
    }

    /**
     * Bind the already-frozen runtime payload to this descriptor and reserve
     * producer continuation escrow. The payload remains with the caller on
     * rejection. On Waiting/Transferred ownership moves into this transaction.
     */
    Registration reserve_and_arm_owned(
            std::unique_ptr<T>& frozen_payload,
            OresNativeActorScheduler* scheduler,
            OresNativeActorRef producer,
            OresNativeActorRef scheduler_target,
            std::uint64_t continuation_slot,
            std::uint64_t continuation_generation,
            std::uint64_t code_generation) {
        if (state_ != LocalState::Preparing)
            throw std::logic_error("async envelope transaction already armed");
        if (!frozen_payload || !scheduler || !producer.scheduler_domain
                || !scheduler_target.scheduler_domain)
            throw std::invalid_argument("complete async envelope ownership required");

        target_.retain_resource_loan();
        loan_owned_ = true;

        OresNativeWakeTicket wake{};
        if (!ores_native_actor_scheduler_reserve_wake(
                scheduler, producer, scheduler_target,
                continuation_slot, continuation_generation, code_generation,
                handle_.generation, &wake)) {
            release_loan_noexcept();
            if (!admission_->cancel_preparation(handle_)
                    && admission_->descriptor_state(handle_)
                        != DescriptorState::Inactive)
                std::terminate();
            state_ = LocalState::Failed;
            return Registration::Rejected;
        }

        payload_ = std::move(frozen_payload);
        const auto result = admission_->register_prepared(
            handle_, bytes_, scheduler, wake);

        if (result == Registration::Waiting) {
            state_ = LocalState::Waiting;
            return result;
        }
        if (result == Registration::Transferred) {
            state_ = LocalState::Transferred;
            return result;
        }

        // Rejected leaves the wake RESERVED; Failed consumed it as FAILED.
        if (result == Registration::Rejected) {
            const auto descriptor = admission_->descriptor_state(handle_);
            if (descriptor == DescriptorState::Preparing) {
                if (!ores_native_actor_scheduler_cancel_wake(scheduler, wake))
                    std::terminate();
                if (!admission_->cancel_preparation(handle_))
                    std::terminate();
            } else if (descriptor != DescriptorState::Inactive) {
                std::terminate();
            }
        }

        frozen_payload = std::move(payload_);
        release_loan_noexcept();
        state_ = LocalState::Failed;
        return result;
    }

    /**
     * Publish only after mailbox credit has been irreversibly TRANSFERRED.
     * The pre-loaned inbox entry point consumes the exact target loan already
     * held by this transaction; no duplicate retain occurs.
     */
    PublishResult publish_if_transferred(Inbox& inbox) {
        refresh_transfer_state();
        if (state_ != LocalState::Transferred)
            return state_ == LocalState::Failed
                ? PublishResult::Failed
                : PublishResult::NotTransferred;
        if (!payload_ || !loan_owned_)
            throw std::logic_error("transferred async envelope lost ownership");

        bool accepted = false;
        try {
            accepted = inbox.offer_owned_preloaned_to(
                typename Inbox::PreloanedPublicationTag{},
                target_, lane_, payload_, bytes_);
        } catch (...) {
            rollback_transferred_or_terminate();
            throw;
        }

        if (!accepted) {
            rollback_transferred_or_terminate();
            return PublishResult::Failed;
        }

        // The envelope now owns the physical target loan. From here a failed
        // producer wake may not retract mailbox admission.
        loan_owned_ = false;
        const auto status = admission_->commit(handle_);
        if (status == ORES_NATIVE_WAKE_INVALID) {
            // Envelope publication is already visible and cannot be rolled
            // back safely. Treat descriptor/wake corruption as fatal.
            std::terminate();
        }
        payload_.reset();
        state_ = LocalState::Committed;
        return status == ORES_NATIVE_WAKE_PRODUCER_RETIRED
            ? PublishResult::ProducerRetired
            : PublishResult::Published;
    }

    bool cancel_waiting() {
        refresh_transfer_state();
        if (state_ != LocalState::Waiting) return false;
        if (!admission_->cancel(handle_)) {
            refresh_transfer_state();
            return false;
        }
        payload_.reset();
        release_loan_noexcept();
        state_ = LocalState::Cancelled;
        return true;
    }

    bool abort_transferred() {
        refresh_transfer_state();
        if (state_ != LocalState::Transferred) return false;
        if (!admission_->abort_transferred(handle_)) return false;
        payload_.reset();
        release_loan_noexcept();
        state_ = LocalState::Failed;
        return true;
    }

    bool owns_target_loan() const noexcept { return loan_owned_; }
    bool owns_payload() const noexcept { return bool(payload_); }
    Handle handle() const noexcept { return handle_; }

private:
    enum class LocalState {
        Preparing,
        Waiting,
        Transferred,
        Committed,
        Failed,
        Cancelled
    };

    void refresh_transfer_state() {
        if (state_ != LocalState::Waiting) return;
        const auto descriptor = admission_->descriptor_state(handle_);
        if (descriptor == DescriptorState::Transferred) {
            state_ = LocalState::Transferred;
        } else if (descriptor == DescriptorState::Inactive) {
            // Target STOP/seal may have consumed the RESERVED wake as FAILED.
            payload_.reset();
            release_loan_noexcept();
            state_ = LocalState::Failed;
        } else if (descriptor != DescriptorState::Waiting) {
            throw std::logic_error("async envelope descriptor state mismatch");
        }
    }

    void rollback_transferred_or_terminate() noexcept {
        if (!admission_->abort_transferred(handle_))
            std::terminate();
        payload_.reset();
        release_loan_noexcept();
        state_ = LocalState::Failed;
    }

    void release_loan_noexcept() noexcept {
        if (!loan_owned_) return;
        target_.release_resource_loan();
        loan_owned_ = false;
    }

    void cleanup_noexcept() noexcept {
        if (!loan_owned_ && !payload_) return;

        auto descriptor = admission_->descriptor_state(handle_);
        if (descriptor == DescriptorState::Waiting) {
            if (!admission_->cancel(handle_)) {
                descriptor = admission_->descriptor_state(handle_);
                if (descriptor == DescriptorState::Transferred) {
                    if (!admission_->abort_transferred(handle_))
                        std::terminate();
                } else if (descriptor != DescriptorState::Inactive) {
                    std::terminate();
                }
            }
        } else if (descriptor == DescriptorState::Transferred) {
            if (!admission_->abort_transferred(handle_))
                std::terminate();
        } else if (descriptor == DescriptorState::Preparing) {
            if (!admission_->cancel_preparation(handle_))
                std::terminate();
        } else if (descriptor == DescriptorState::Publishing) {
            std::terminate();
        }

        payload_.reset();
        release_loan_noexcept();
        if (state_ != LocalState::Committed)
            state_ = LocalState::Cancelled;
    }

    Admission* admission_;
    Handle handle_;
    NativeActorRef target_;
    Lane lane_;
    std::size_t bytes_;
    std::unique_ptr<T> payload_;
    bool loan_owned_ = false;
    LocalState state_ = LocalState::Preparing;
};

} // namespace oreslang::runtime
