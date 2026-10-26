#include "oreslang/runtime/NativeAsyncEnvelopeTxn.hpp"
#include "oreslang/runtime/NativeScheduledActor.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>

using oreslang::runtime::NativeActorGroup;
using oreslang::runtime::NativeScheduledActor;
using oreslang::runtime::NativeAsyncEnvelopeTxn;
using oreslang::runtime::NativeAsyncMailboxAdmission;
using oreslang::runtime::PriorityActorInbox;

#define CHECK(x) do { if (!(x)) throw std::runtime_error("native async envelope ownership: " #x); } while (false)

struct Payload {
    int value = 0;
};

struct WakeFixture {
    OresNativeActorScheduler* scheduler = nullptr;
    OresNativeActorRef producer{};
    OresNativeActorRef target{};
    std::atomic<int> ready{0};
    std::atomic<int> failed{0};
    std::atomic<int> errors{0};
};

static void on_data(void*, std::size_t, std::uint64_t) {}

static void on_wake(
        void* raw, std::size_t actor, OresNativeWakeTicket ticket,
        OresNativeWakeOutcome outcome) {
    auto* f = static_cast<WakeFixture*>(raw);
    if (actor != 0
            || !ores_native_actor_scheduler_is_current_actor(
                f->scheduler, f->producer)
            || !ores_native_actor_scheduler_is_current_wake(
                f->scheduler, ticket))
        ++f->errors;
    if (outcome == ORES_NATIVE_WAKE_READY) ++f->ready;
    else if (outcome == ORES_NATIVE_WAKE_FAILED) ++f->failed;
    else ++f->errors;
}

static void setup_scheduler(WakeFixture& f) {
    f.scheduler = ores_native_actor_scheduler_open_with_wakes(
        2, 1, 1, on_data, nullptr, nullptr, on_wake, &f);
    CHECK(f.scheduler);
    f.producer = ores_native_actor_scheduler_spawn(f.scheduler, 0);
    CHECK(f.producer.scheduler_domain);
}

static void finish_scheduler(WakeFixture& f, bool target_already_stopped = false) {
    CHECK(ores_native_actor_scheduler_drain(f.scheduler));
    CHECK(!f.errors.load());
    if (!target_already_stopped)
        CHECK(ores_native_actor_scheduler_stop(f.scheduler, f.target));
    CHECK(ores_native_actor_scheduler_stop(f.scheduler, f.producer));
    CHECK(ores_native_actor_scheduler_wait_done(f.scheduler, f.target));
    CHECK(ores_native_actor_scheduler_wait_done(f.scheduler, f.producer));
    ores_native_actor_scheduler_retire(f.scheduler);
    CHECK(ores_native_actor_scheduler_try_destroy(f.scheduler));
}

using Inbox = PriorityActorInbox<Payload>;
using Txn = NativeAsyncEnvelopeTxn<Payload>;
using Lane = Inbox::Lane;

static bool sync_offer_owned(
        NativeAsyncMailboxAdmission& admission,
        Inbox& inbox,
        const oreslang::runtime::NativeActorRef& target,
        std::unique_ptr<Payload>& payload,
        std::size_t bytes) {
    if (!admission.try_acquire(bytes)) return false;
    try {
        if (inbox.offer_owned_to(target, Lane::Normal, payload, bytes))
            return true;
    } catch (...) {
        admission.release_acquired(bytes);
        throw;
    }
    admission.release_acquired(bytes);
    return false;
}


static void malformed_scheduler_binding_rejects_before_group_publication() {
    NativeActorGroup group(7010, 1);
    WakeFixture f;
    setup_scheduler(f);
    f.target = ores_native_actor_scheduler_spawn(f.scheduler, 1);
    CHECK(f.target.scheduler_domain);
    CHECK(f.target.generation);

    bool threw = false;
    try {
        (void)group.adopt(
            7010, 50, f.target.generation, [] {},
            nullptr, f.target);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(group.worker_count() == 0);

    OresNativeActorRef incomplete = f.target;
    incomplete.scheduler_domain = 0;
    threw = false;
    try {
        (void)group.adopt(
            7010, 50, f.target.generation, [] {},
            f.scheduler, incomplete);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(group.worker_count() == 0);

    finish_scheduler(f);
}

static void unarmed_transaction_destruction_refunds_preparation_descriptor() {
    NativeActorGroup group(7000, 1);
    WakeFixture f;
    setup_scheduler(f);
    auto target = NativeScheduledActor::spawn(group, f.scheduler, 1, 40);
    CHECK(target.exact_scheduler_ref(f.scheduler, &f.target));
    CHECK(group.mark_ready(target));
    NativeAsyncMailboxAdmission admission(1, 8, 1, 8);

    NativeAsyncMailboxAdmission::Handle handle;
    CHECK(admission.begin_preparation(&handle));
    CHECK(admission.active_descriptors() == 1);
    {
        Txn txn(admission, handle, target, Lane::Normal, 8);
        CHECK(!txn.owns_payload());
        CHECK(!txn.owns_target_loan());
    }
    CHECK(admission.active_descriptors() == 0);
    CHECK(admission.descriptor_state(handle)
          == NativeAsyncMailboxAdmission::DescriptorState::Inactive);
    CHECK(target.outstanding_resource_loans() == 0);

    CHECK(group.finalized(target));
    target.get_done().get();
    finish_scheduler(f);
}

static void terminal_target_rejects_before_payload_or_wake_transfer() {
    NativeActorGroup group(7005, 1);
    WakeFixture f;
    setup_scheduler(f);
    auto target = NativeScheduledActor::spawn(group, f.scheduler, 1, 45);
    CHECK(target.exact_scheduler_ref(f.scheduler, &f.target));
    CHECK(group.mark_ready(target));
    CHECK(group.finalized(target));
    target.get_done().get();

    NativeAsyncMailboxAdmission admission(1, 8, 1, 8);
    NativeAsyncMailboxAdmission::Handle handle;
    CHECK(admission.begin_preparation(&handle));
    auto frozen = std::make_unique<Payload>();
    frozen->value = 11;

    {
        Txn txn(admission, handle, target, Lane::Normal, 8);
        bool threw = false;
        try {
            (void)txn.reserve_and_arm_owned(
                frozen, f.scheduler, f.producer, f.target,
                501, 1, 1);
        } catch (const std::logic_error&) {
            threw = true;
        }
        CHECK(threw);
        CHECK(frozen && frozen->value == 11);
        CHECK(!txn.owns_payload());
        CHECK(!txn.owns_target_loan());
    }
    CHECK(admission.active_descriptors() == 0);
    CHECK(target.outstanding_resource_loans() == 0);
    CHECK(!ores_native_actor_scheduler_outstanding_wake_loans(
        f.scheduler, f.producer));
    finish_scheduler(f);
}

static void waiting_target_stop_fails_future_and_releases_owned_state() {
    NativeActorGroup group(7006, 1);
    WakeFixture f;
    setup_scheduler(f);
    auto target = NativeScheduledActor::spawn(group, f.scheduler, 1, 46);
    CHECK(target.exact_scheduler_ref(f.scheduler, &f.target));
    CHECK(group.mark_ready(target));

    Inbox inbox({1, 8}, {1, 8}, {1, 8}, 8);
    inbox.bind_target(target);
    NativeAsyncMailboxAdmission admission(1, 8, 2, 16);

    auto blocker = std::make_unique<Payload>();
    blocker->value = 3;
    CHECK(sync_offer_owned(admission, inbox, target, blocker, 8));

    NativeAsyncMailboxAdmission::Handle handle;
    CHECK(admission.begin_preparation(&handle));
    auto frozen = std::make_unique<Payload>();
    frozen->value = 12;
    Txn txn(admission, handle, target, Lane::Normal, 8);
    CHECK(txn.reserve_and_arm_owned(
        frozen, f.scheduler, f.producer, f.target,
        601, 1, 1) == NativeAsyncMailboxAdmission::Registration::Waiting);
    CHECK(target.outstanding_resource_loans() == 2);

    CHECK(ores_native_actor_scheduler_stop(f.scheduler, f.target));
    auto first = inbox.try_receive();
    CHECK(first && first->acknowledge_released());
    admission.release_acquired(8);

    CHECK(txn.publish_if_transferred(inbox) == Txn::PublishResult::Failed);
    CHECK(!txn.owns_payload());
    CHECK(!txn.owns_target_loan());
    CHECK(target.outstanding_resource_loans() == 0);
    CHECK(admission.active_descriptors() == 0);
    CHECK(admission.pending_waiters() == 0);
    CHECK(admission.used_frames() == 0);

    CHECK(ores_native_actor_scheduler_drain(f.scheduler));
    CHECK(f.failed.load() == 1);
    CHECK(f.ready.load() == 0);
    CHECK(!f.errors.load());

    CHECK(group.finalized(target));
    target.get_done().get();
    inbox.close();

    CHECK(ores_native_actor_scheduler_stop(f.scheduler, f.producer));
    CHECK(ores_native_actor_scheduler_wait_done(f.scheduler, f.target));
    CHECK(ores_native_actor_scheduler_wait_done(f.scheduler, f.producer));
    ores_native_actor_scheduler_retire(f.scheduler);
    CHECK(ores_native_actor_scheduler_try_destroy(f.scheduler));
}

static void transaction_move_transfers_descriptor_payload_and_loan_once() {
    NativeActorGroup group(7007, 1);
    WakeFixture f;
    setup_scheduler(f);
    auto target = NativeScheduledActor::spawn(group, f.scheduler, 1, 47);
    CHECK(target.exact_scheduler_ref(f.scheduler, &f.target));
    CHECK(group.mark_ready(target));

    NativeAsyncMailboxAdmission admission(1, 8, 1, 8);

    NativeAsyncMailboxAdmission::Handle handle;
    CHECK(admission.begin_preparation(&handle));
    auto frozen = std::make_unique<Payload>();
    frozen->value = 13;

    {
        Txn original(admission, handle, target, Lane::Normal, 8);
        CHECK(original.reserve_and_arm_owned(
            frozen, f.scheduler, f.producer, f.target,
            701, 1, 1) == NativeAsyncMailboxAdmission::Registration::Transferred);
        CHECK(target.outstanding_resource_loans() == 1);
        CHECK(original.owns_payload() && original.owns_target_loan());

        Txn moved(std::move(original));
        CHECK(moved.owns_payload() && moved.owns_target_loan());
        CHECK(target.outstanding_resource_loans() == 1);
        CHECK(moved.abort_transferred());
        CHECK(!moved.owns_payload() && !moved.owns_target_loan());
    }

    CHECK(target.outstanding_resource_loans() == 0);
    CHECK(admission.active_descriptors() == 0);
    CHECK(admission.used_frames() == 0);
    CHECK(!ores_native_actor_scheduler_outstanding_wake_loans(
        f.scheduler, f.producer));
    CHECK(group.finalized(target));
    target.get_done().get();
    finish_scheduler(f);
}

static void waiting_send_transfers_frozen_payload_and_exact_resource_loan() {
    NativeActorGroup group(7001, 1);
    WakeFixture f;
    setup_scheduler(f);
    auto target = NativeScheduledActor::spawn(group, f.scheduler, 1, 41);
    CHECK(target.exact_scheduler_ref(f.scheduler, &f.target));
    CHECK(group.mark_ready(target));

    Inbox inbox({1, 8}, {1, 8}, {1, 8}, 8);
    inbox.bind_target(target);
    NativeAsyncMailboxAdmission admission(1, 8, 4, 32);


    // Occupy the real and shadow NORMAL credit with an ordinary accepted send.
    auto blocker = std::make_unique<Payload>();
    blocker->value = 1;
    CHECK(sync_offer_owned(admission, inbox, target, blocker, 8));
    CHECK(!blocker);
    CHECK(target.outstanding_resource_loans() == 1);

    NativeAsyncMailboxAdmission::Handle handle;
    CHECK(admission.begin_preparation(&handle));

    // Preparation is bounded before the potentially expensive copy. Mutating
    // the sender afterwards must not change the runtime-owned frozen snapshot.
    Payload source{7};
    auto frozen = std::make_unique<Payload>(source);
    source.value = 99;

    Txn txn(admission, handle, target, Lane::Normal, 8);
    CHECK(txn.reserve_and_arm_owned(
        frozen, f.scheduler, f.producer, f.target,
        101, 1, 1) == NativeAsyncMailboxAdmission::Registration::Waiting);
    CHECK(!frozen);
    CHECK(txn.owns_payload());
    CHECK(txn.owns_target_loan());
    CHECK(target.outstanding_resource_loans() == 2);
    CHECK(admission.pending_waiters() == 1);
    CHECK(admission.pending_bytes() == 8);

    auto first = inbox.try_receive();
    CHECK(first && first->value().value == 1);
    CHECK(first->acknowledge_released());
    CHECK(target.outstanding_resource_loans() == 1);

    // Physical ACK frees the real port first; releasing the mirrored async
    // credit then transfers that exact slot to the FIFO waiter.
    admission.release_acquired(8);
    CHECK(admission.transferred(handle));
    CHECK(admission.used_frames() == 1);
    CHECK(admission.pending_waiters() == 0);
    CHECK(target.outstanding_resource_loans() == 1);

    CHECK(txn.publish_if_transferred(inbox) == Txn::PublishResult::Published);
    CHECK(!txn.owns_payload());
    CHECK(!txn.owns_target_loan());
    // Loan ownership moved from txn to the actual envelope, so count is stable.
    CHECK(target.outstanding_resource_loans() == 1);

    CHECK(ores_native_actor_scheduler_drain(f.scheduler));
    CHECK(f.ready.load() == 1);
    CHECK(f.failed.load() == 0);

    auto delivered = inbox.try_receive();
    CHECK(delivered);
    CHECK(delivered->value().value == 7);
    CHECK(delivered->acknowledge_released());
    CHECK(target.outstanding_resource_loans() == 0);
    admission.release_acquired(8);
    CHECK(admission.used_frames() == 0);
    CHECK(admission.active_descriptors() == 0);
    CHECK(admission.pending_bytes() == 0);

    CHECK(group.finalized(target));
    target.get_done().get();
    inbox.close();
    finish_scheduler(f);
}

static void waiting_cancellation_releases_payload_loan_and_wake() {
    NativeActorGroup group(7002, 1);
    WakeFixture f;
    setup_scheduler(f);
    auto target = NativeScheduledActor::spawn(group, f.scheduler, 1, 42);
    CHECK(target.exact_scheduler_ref(f.scheduler, &f.target));
    CHECK(group.mark_ready(target));

    Inbox inbox({1, 8}, {1, 8}, {1, 8}, 8);
    inbox.bind_target(target);
    NativeAsyncMailboxAdmission admission(1, 8, 2, 16);

    auto blocker = std::make_unique<Payload>();
    blocker->value = 2;
    CHECK(sync_offer_owned(admission, inbox, target, blocker, 8));

    NativeAsyncMailboxAdmission::Handle handle;
    CHECK(admission.begin_preparation(&handle));
    auto frozen = std::make_unique<Payload>();
    frozen->value = 8;
    Txn txn(admission, handle, target, Lane::Normal, 8);
    CHECK(txn.reserve_and_arm_owned(
        frozen, f.scheduler, f.producer, f.target,
        201, 1, 1) == NativeAsyncMailboxAdmission::Registration::Waiting);
    CHECK(target.outstanding_resource_loans() == 2);
    CHECK(txn.cancel_waiting());
    CHECK(!txn.owns_payload());
    CHECK(!txn.owns_target_loan());
    CHECK(target.outstanding_resource_loans() == 1);
    CHECK(admission.pending_waiters() == 0);
    CHECK(admission.active_descriptors() == 0);

    auto first = inbox.try_receive();
    CHECK(first && first->acknowledge_released());
    admission.release_acquired(8);
    CHECK(target.outstanding_resource_loans() == 0);
    CHECK(ores_native_actor_scheduler_drain(f.scheduler));
    CHECK(f.ready.load() == 0 && f.failed.load() == 0);

    CHECK(group.finalized(target));
    target.get_done().get();
    inbox.close();
    finish_scheduler(f);
}

static void publication_exception_aborts_transfer_and_releases_target_loan() {
    NativeActorGroup group(7003, 1);
    WakeFixture f;
    setup_scheduler(f);
    auto target = NativeScheduledActor::spawn(group, f.scheduler, 1, 43);
    CHECK(target.exact_scheduler_ref(f.scheduler, &f.target));
    CHECK(group.mark_ready(target));

    Inbox inbox({1, 8}, {1, 8}, {1, 8}, 8);
    inbox.bind_target(target);
    NativeAsyncMailboxAdmission admission(1, 8, 2, 16);

    NativeAsyncMailboxAdmission::Handle handle;
    CHECK(admission.begin_preparation(&handle));
    auto frozen = std::make_unique<Payload>();
    frozen->value = 9;
    Txn txn(admission, handle, target, Lane::Normal, 8);
    CHECK(txn.reserve_and_arm_owned(
        frozen, f.scheduler, f.producer, f.target,
        301, 1, 1) == NativeAsyncMailboxAdmission::Registration::Transferred);
    CHECK(target.outstanding_resource_loans() == 1);
    CHECK(admission.used_frames() == 1);

    // No real envelope exists yet, so sealing the underlying inbox succeeds.
    // Publication must then roll back transferred wake/credit + target loan.
    inbox.close();
    bool threw = false;
    try { (void)txn.publish_if_transferred(inbox); }
    catch (const std::logic_error&) { threw = true; }
    CHECK(threw);
    CHECK(!txn.owns_payload());
    CHECK(!txn.owns_target_loan());
    CHECK(target.outstanding_resource_loans() == 0);
    CHECK(admission.used_frames() == 0);
    CHECK(admission.active_descriptors() == 0);
    CHECK(admission.pending_bytes() == 0);
    CHECK(ores_native_actor_scheduler_drain(f.scheduler));
    CHECK(f.ready.load() == 0 && f.failed.load() == 0);

    CHECK(group.finalized(target));
    target.get_done().get();
    finish_scheduler(f);
}

static void producer_stop_after_transfer_does_not_retract_envelope() {
    NativeActorGroup group(7004, 1);
    WakeFixture f;
    setup_scheduler(f);
    auto target = NativeScheduledActor::spawn(group, f.scheduler, 1, 44);
    CHECK(target.exact_scheduler_ref(f.scheduler, &f.target));
    CHECK(group.mark_ready(target));

    Inbox inbox({1, 8}, {1, 8}, {1, 8}, 8);
    inbox.bind_target(target);
    NativeAsyncMailboxAdmission admission(1, 8, 2, 16);

    NativeAsyncMailboxAdmission::Handle handle;
    CHECK(admission.begin_preparation(&handle));
    auto frozen = std::make_unique<Payload>();
    frozen->value = 10;
    Txn txn(admission, handle, target, Lane::Normal, 8);
    CHECK(txn.reserve_and_arm_owned(
        frozen, f.scheduler, f.producer, f.target,
        401, 1, 1) == NativeAsyncMailboxAdmission::Registration::Transferred);

    CHECK(ores_native_actor_scheduler_stop(f.scheduler, f.producer));
    CHECK(txn.publish_if_transferred(inbox)
          == Txn::PublishResult::ProducerRetired);
    CHECK(target.outstanding_resource_loans() == 1);

    auto delivered = inbox.try_receive();
    CHECK(delivered && delivered->value().value == 10);
    CHECK(delivered->acknowledge_released());
    admission.release_acquired(8);
    CHECK(target.outstanding_resource_loans() == 0);
    CHECK(f.ready.load() == 0 && f.failed.load() == 0);

    CHECK(group.finalized(target));
    target.get_done().get();
    inbox.close();

    CHECK(ores_native_actor_scheduler_stop(f.scheduler, f.target));
    CHECK(ores_native_actor_scheduler_wait_done(f.scheduler, f.target));
    CHECK(ores_native_actor_scheduler_wait_done(f.scheduler, f.producer));
    ores_native_actor_scheduler_retire(f.scheduler);
    CHECK(ores_native_actor_scheduler_try_destroy(f.scheduler));
}

static void sealed_preparation_rejection_releases_reserved_wake() {
    NativeActorGroup group(7008, 1);
    WakeFixture f;
    setup_scheduler(f);
    auto target = NativeScheduledActor::spawn(group, f.scheduler, 1, 48);
    CHECK(target.exact_scheduler_ref(f.scheduler, &f.target));
    CHECK(group.mark_ready(target));
    NativeAsyncMailboxAdmission admission(1, 8, 1, 8);

    NativeAsyncMailboxAdmission::Handle handle;
    CHECK(admission.begin_preparation(&handle));
    auto frozen = std::make_unique<Payload>();
    frozen->value = 14;
    {
        Txn txn(admission, handle, target, Lane::Normal, 8);
        // STOP/seal may detach PREPARING between transaction construction
        // and register_prepared(). A rejection still leaves wake ownership
        // with the transaction, even though the descriptor is now inactive.
        admission.seal();
        CHECK(txn.reserve_and_arm_owned(
            frozen, f.scheduler, f.producer, f.target,
            801, 1, 1) == NativeAsyncMailboxAdmission::Registration::Rejected);
        CHECK(frozen && frozen->value == 14);
        CHECK(!txn.owns_payload() && !txn.owns_target_loan());
        CHECK(target.outstanding_resource_loans() == 0);
        CHECK(admission.active_descriptors() == 0);
        CHECK(admission.pending_bytes() == 0);
        CHECK(admission.used_frames() == 0);
        CHECK(!ores_native_actor_scheduler_outstanding_wake_loans(
            f.scheduler, f.producer));
    }
    CHECK(group.finalized(target));
    target.get_done().get();
    finish_scheduler(f);
}

static void unbound_and_mismatched_scheduler_incarnations_reject_before_loans() {
    NativeActorGroup group(7009, 1);
    WakeFixture f;
    setup_scheduler(f);
    auto target = NativeScheduledActor::spawn(group, f.scheduler, 1, 49);
    CHECK(target.exact_scheduler_ref(f.scheduler, &f.target));
    CHECK(group.mark_ready(target));

    auto rejected = [&](const oreslang::runtime::NativeActorRef& control,
                        OresNativeActorScheduler* scheduler,
                        OresNativeActorRef producer,
                        OresNativeActorRef candidate) {
        NativeAsyncMailboxAdmission admission(1, 8, 1, 8);
        NativeAsyncMailboxAdmission::Handle handle;
        CHECK(admission.begin_preparation(&handle));
        auto payload = std::make_unique<Payload>();
        payload->value = 23;
        {
            Txn txn(admission, handle, control, Lane::Normal, 8);
            bool threw = false;
            try {
                (void)txn.reserve_and_arm_owned(
                    payload, scheduler, producer, candidate, 901, 1, 1);
            } catch (const std::invalid_argument&) {
                threw = true;
            }
            CHECK(threw);
            CHECK(payload && payload->value == 23);
            CHECK(!txn.owns_payload() && !txn.owns_target_loan());
            CHECK(control.outstanding_resource_loans() == 0);
            CHECK(!ores_native_actor_scheduler_outstanding_wake_loans(
                f.scheduler, f.producer));
            CHECK(admission.pending_bytes() == 0);
            CHECK(admission.used_frames() == 0);
        }
        CHECK(admission.active_descriptors() == 0);
    };

    auto altered = f.target;
    ++altered.scheduler_domain;
    rejected(target, f.scheduler, f.producer, altered);
    altered = f.target;
    ++altered.generation;
    rejected(target, f.scheduler, f.producer, altered);
    rejected(target, f.scheduler, f.producer, f.producer); // wrong actor slot
    altered = f.producer;
    ++altered.scheduler_domain;
    rejected(target, f.scheduler, altered, f.target);

    auto* foreign = ores_native_actor_scheduler_open(
        2, 1, 1, on_data, nullptr);
    CHECK(foreign);
    const auto foreign_producer = ores_native_actor_scheduler_spawn(foreign, 0);
    const auto foreign_target = ores_native_actor_scheduler_spawn(foreign, 1);
    CHECK(foreign_target.actor_slot == f.target.actor_slot);
    CHECK(foreign_target.generation == f.target.generation);
    CHECK(foreign_target.scheduler_domain != f.target.scheduler_domain);
    rejected(target, foreign, foreign_producer, foreign_target);
    OresNativeActorRef no_projection = f.target;
    CHECK(!target.exact_scheduler_ref(foreign, &no_projection));
    CHECK(!no_projection.scheduler_domain);
    ores_native_actor_scheduler_retire(foreign);
    CHECK(ores_native_actor_scheduler_try_destroy(foreign));

    CHECK(group.finalized(target));
    target.get_done().get();
    // Same routing labels and generation, but a new opaque control ticket.
    // Ordinary adoption cannot forge the old scheduler binding.
    auto replacement = group.adopt(7009, 49, target.generation(), [] {});
    CHECK(replacement == target && !replacement.same_incarnation(target));
    CHECK(group.mark_ready(replacement));
    rejected(replacement, f.scheduler, f.producer, f.target);
    CHECK(group.finalized(replacement));
    replacement.get_done().get();
    finish_scheduler(f);
}

int main() {
    malformed_scheduler_binding_rejects_before_group_publication();
    unbound_and_mismatched_scheduler_incarnations_reject_before_loans();
    sealed_preparation_rejection_releases_reserved_wake();
    unarmed_transaction_destruction_refunds_preparation_descriptor();
    terminal_target_rejects_before_payload_or_wake_transfer();
    waiting_target_stop_fails_future_and_releases_owned_state();
    transaction_move_transfers_descriptor_payload_and_loan_once();
    waiting_send_transfers_frozen_payload_and_exact_resource_loan();
    waiting_cancellation_releases_payload_loan_and_wake();
    publication_exception_aborts_transfer_and_releases_target_loan();
    producer_stop_after_transfer_does_not_retract_envelope();
    return 0;
}
